#include "DatabaseStore.h"

#include <SD_MMC.h>
#include <sqlite3.h>

#include <cctype>
#include <cstdio>
#include <ctime>

namespace {

constexpr const char* kDefaultBootstrapSql = R"SQL(
CREATE TABLE IF NOT EXISTS peers (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    mac TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    description TEXT DEFAULT '',
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE TABLE IF NOT EXISTS command_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    command TEXT NOT NULL,
    source TEXT NOT NULL DEFAULT 'serial',
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE TABLE IF NOT EXISTS command_log_output (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    log_id INTEGER NOT NULL,
    output TEXT NOT NULL,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    FOREIGN KEY(log_id) REFERENCES command_log(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS espnow_incoming_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    peer_id INTEGER NOT NULL,
    payload TEXT NOT NULL,
    payload_type INTEGER NOT NULL DEFAULT 0,
    received_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    FOREIGN KEY(peer_id) REFERENCES peers(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS espnow_outgoing_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    peer_id INTEGER,
    mac TEXT NOT NULL,
    payload TEXT NOT NULL,
    payload_type INTEGER NOT NULL DEFAULT 0,
    delivered INTEGER NOT NULL DEFAULT 0,
    sent_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    FOREIGN KEY(peer_id) REFERENCES peers(id) ON DELETE SET NULL
);

CREATE TABLE IF NOT EXISTS boot_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    reason TEXT NOT NULL,
    boot_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE TABLE IF NOT EXISTS kv_store (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);
)SQL";

struct QueryTextContext {
    String* out;
    size_t rowCount;
    size_t maxRows;
    bool truncated;
};

int queryTextCallback(void* rawContext, int argc, char** argv, char** colNames) {
    if (rawContext == nullptr) {
        return 0;
    }

    auto* context = static_cast<QueryTextContext*>(rawContext);
    if (context->rowCount >= context->maxRows) {
        context->truncated = true;
        return 1;
    }

    context->out->concat("[");
    context->out->concat(static_cast<unsigned long>(context->rowCount + 1));
    context->out->concat("] ");

    for (int i = 0; i < argc; ++i) {
        if (i > 0) {
            context->out->concat(" | ");
        }

        const char* columnName = (colNames != nullptr && colNames[i] != nullptr) ? colNames[i] : "col";
        const char* value = (argv != nullptr && argv[i] != nullptr) ? argv[i] : "NULL";

        context->out->concat(columnName);
        context->out->concat("=");
        context->out->concat(value);
    }

    context->out->concat("\n");
    ++context->rowCount;
    return 0;
}

struct IntQueryContext {
    bool hasValue;
    int32_t value;
};

int queryIntCallback(void* rawContext, int argc, char** argv, char**) {
    if (rawContext == nullptr || argc <= 0 || argv == nullptr || argv[0] == nullptr) {
        return 0;
    }

    auto* context = static_cast<IntQueryContext*>(rawContext);
    context->value = static_cast<int32_t>(std::strtol(argv[0], nullptr, 10));
    context->hasValue = true;
    return 0;
}

String safeColumnText(sqlite3_stmt* stmt, int columnIndex) {
    if (stmt == nullptr) {
        return "";
    }

    const unsigned char* value = sqlite3_column_text(stmt, columnIndex);
    if (value == nullptr) {
        return "";
    }

    return String(reinterpret_cast<const char*>(value));
}

String epochToDateTimeText(int64_t epoch) {
    if (epoch <= 0) {
        return "(sem-data)";
    }

    const time_t raw = static_cast<time_t>(epoch);
    std::tm localTime = {};
    if (localtime_r(&raw, &localTime) == nullptr) {
        return "(sem-data)";
    }

    char out[24] = {0};
    std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &localTime);
    return String(out);
}

String sanitizeAndTruncateField(const String& input, size_t maxLen) {
    String out;
    out.reserve((maxLen > 0 ? maxLen : 1) + 8);

    const size_t inputLen = input.length();
    for (size_t i = 0; i < inputLen; ++i) {
        char ch = input[i];
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        } else if (ch == '|') {
            ch = '/';
        }

        out += ch;

        if (maxLen > 0 && out.length() >= maxLen) {
            if (i + 1 < inputLen) {
                out += "...";
            }
            break;
        }
    }

    out.trim();
    if (out.isEmpty()) {
        return "(vazio)";
    }

    return out;
}

} // namespace

DatabaseStore::DatabaseStore()
    : db_(nullptr),
      io_(nullptr),
      ready_(false),
      dbMutex_(xSemaphoreCreateRecursiveMutex()) {
}

bool DatabaseStore::lockDb(uint32_t timeoutMs) {
    if (dbMutex_ == nullptr) {
        return true;
    }

    const TickType_t waitTicks = (timeoutMs == 0)
        ? portMAX_DELAY
        : pdMS_TO_TICKS(timeoutMs);

    return xSemaphoreTakeRecursive(dbMutex_, waitTicks) == pdTRUE;
}

void DatabaseStore::unlockDb() {
    if (dbMutex_ != nullptr) {
        xSemaphoreGiveRecursive(dbMutex_);
    }
}

bool DatabaseStore::begin(EspNowManager& espNow, Stream* io) {
    io_ = io;
    ready_ = false;

    closeDatabase();

    if (!ensureBootstrapAssets()) {
        logLine("[database] falha ao preparar bootstrap no SD");
        return false;
    }

    if (!openDatabase()) {
        logLine("[database] falha ao abrir arquivo sqlite");
        return false;
    }

    if (!applyBootstrapScript()) {
        closeDatabase();
        logLine("[database] falha ao aplicar bootstrap SQL");
        return false;
    }

    if (!applyRuntimeMigrations()) {
        closeDatabase();
        logLine("[database] falha ao aplicar migracoes da base");
        return false;
    }

    ready_ = true;

    if (!ensureDefaultBroadcastPeer()) {
        logLine("[database] aviso: nao foi possivel garantir peer default 000");
    }

    if (!loadPeersFromDatabase(espNow)) {
        logLine("[database] banco aberto, mas falhou carga inicial de peers");
    }

    return true;
}

bool DatabaseStore::rebuild(EspNowManager& espNow) {
    closeDatabase();

    if (SD_MMC.exists(kFsDatabasePath) && !SD_MMC.remove(kFsDatabasePath)) {
        logLine("[database] falha ao remover banco anterior");
        return false;
    }

    return begin(espNow, io_);
}

void DatabaseStore::end() {
    closeDatabase();
}

bool DatabaseStore::isReady() const {
    return ready_;
}

bool DatabaseStore::upsertPeer(const uint8_t mac[6], const char* name, const char* description) {
    if (!ready_ || mac == nullptr) {
        return false;
    }

    const int64_t now = currentEpochSeconds();
    const String macText = macToText(mac);
    const String peerName = (name != nullptr) ? name : "";
    const String peerDescription = (description != nullptr) ? description : "";

    String sql;
    sql.reserve(520);
    sql += "UPDATE peers SET name='";
    sql += escapeSqlText(peerName);
    sql += "', description='";
    sql += escapeSqlText(peerDescription);
    sql += "', updated_at=";
    sql += String(static_cast<long long>(now));
    sql += " WHERE mac='";
    sql += escapeSqlText(macText);
    sql += "';";

    sql += "INSERT OR IGNORE INTO peers(mac,name,description,created_at,updated_at) VALUES('";
    sql += escapeSqlText(macText);
    sql += "','";
    sql += escapeSqlText(peerName);
    sql += "','";
    sql += escapeSqlText(peerDescription);
    sql += "',";
    sql += String(static_cast<long long>(now));
    sql += ",";
    sql += String(static_cast<long long>(now));
    sql += ");";

    return executeNoResult(sql);
}

bool DatabaseStore::removePeer(const uint8_t mac[6]) {
    if (!ready_ || mac == nullptr) {
        return false;
    }

    String sql = "DELETE FROM peers WHERE mac='";
    sql += escapeSqlText(macToText(mac));
    sql += "';";

    return executeNoResult(sql);
}

bool DatabaseStore::updatePeerMetadata(const uint8_t mac[6], const char* name, const char* description) {
    if (!ready_ || mac == nullptr) {
        return false;
    }

    int32_t existingId = -1;
    if (!peerIdByMac(mac, existingId)) {
        return false;
    }

    const int64_t now = currentEpochSeconds();
    String sql;
    sql.reserve(320);
    sql += "UPDATE peers SET name='";
    sql += escapeSqlText(String((name != nullptr) ? name : ""));
    sql += "', description='";
    sql += escapeSqlText(String((description != nullptr) ? description : ""));
    sql += "', updated_at=";
    sql += String(static_cast<long long>(now));
    sql += " WHERE mac='";
    sql += escapeSqlText(macToText(mac));
    sql += "';";

    return executeNoResult(sql);
}

bool DatabaseStore::logCommand(const char* command, const char* source) {
    if (!ready_ || command == nullptr || command[0] == '\0') {
        return false;
    }

    return logCommandWithOutput(command, "", source);
}

bool DatabaseStore::logCommandWithOutput(const char* command, const char* output, const char* source) {
    if (!ready_ || command == nullptr || command[0] == '\0') {
        return false;
    }

    const int64_t now = currentEpochSeconds();
    const String sourceText = (source != nullptr) ? source : "serial";
    const String outputText = (output != nullptr) ? output : "";

    String sql;
    sql.reserve(760);
    sql += "INSERT INTO command_log(command,source,created_at) VALUES('";
    sql += escapeSqlText(String(command));
    sql += "','";
    sql += escapeSqlText(sourceText);
    sql += "',";
    sql += String(static_cast<long long>(now));
    sql += ");";
    sql += "INSERT INTO command_log_output(log_id,output,created_at) VALUES(last_insert_rowid(),'";
    sql += escapeSqlText(outputText);
    sql += "',";
    sql += String(static_cast<long long>(now));
    sql += ");";

    return executeNoResult(sql);
}

bool DatabaseStore::logIncomingEspNow(const uint8_t mac[6], const EspNowManager::message& incoming) {
    if (!ready_ || mac == nullptr) {
        return false;
    }

    int32_t peerId = -1;
    if (!ensurePeerExistsWithDefaults(mac, peerId)) {
        return false;
    }

    const int64_t now = currentEpochSeconds();
    String sql;
    sql.reserve(640);
    sql += "INSERT INTO espnow_incoming_log(peer_id,payload,payload_type,received_at) VALUES(";
    sql += String(static_cast<long>(peerId));
    sql += ",'";
    sql += escapeSqlText(String(incoming.msg));
    sql += "',";
    sql += String(static_cast<int>(incoming.type));
    sql += ",";
    sql += String(static_cast<long long>(now));
    sql += ");";

    return executeNoResult(sql);
}

bool DatabaseStore::logOutgoingEspNow(const uint8_t mac[6], const EspNowManager::message& outgoing, bool delivered) {
    if (!ready_ || mac == nullptr) {
        return false;
    }

    int32_t peerId = -1;
    if (!peerIdByMac(mac, peerId)) {
        ensurePeerExistsWithDefaults(mac, peerId);
    }

    const int64_t now = currentEpochSeconds();
    String sql;
    sql.reserve(760);
    sql += "INSERT INTO espnow_outgoing_log(peer_id,mac,payload,payload_type,delivered,sent_at) VALUES(";
    if (peerId > 0) {
        sql += String(static_cast<long>(peerId));
    } else {
        sql += "NULL";
    }
    sql += ",'";
    sql += escapeSqlText(macToText(mac));
    sql += "','";
    sql += escapeSqlText(String(outgoing.msg));
    sql += "',";
    sql += String(static_cast<int>(outgoing.type));
    sql += ",";
    sql += delivered ? "1" : "0";
    sql += ",";
    sql += String(static_cast<long long>(now));
    sql += ");";

    return executeNoResult(sql);
}

bool DatabaseStore::logBootEvent(const char* reason) {
    if (!ready_) {
        return false;
    }

    const int64_t now = currentEpochSeconds();

    String sql;
    sql.reserve(280);
    sql += "INSERT INTO boot_events(reason,boot_at) VALUES('";
    sql += escapeSqlText(String((reason != nullptr) ? reason : "power_on"));
    sql += "',";
    sql += String(static_cast<long long>(now));
    sql += ");";

    return executeNoResult(sql);
}

bool DatabaseStore::getDefaultBroadcastMac(uint8_t outMac[6]) {
    if (!ready_ || db_ == nullptr || outMac == nullptr) {
        return false;
    }

    if (!lockDb()) {
        return false;
    }

    struct MacQueryContext {
        uint8_t* out;
        bool found;
    };

    MacQueryContext context = {outMac, false};

    auto callback = [](void* rawContext, int argc, char** argv, char**) -> int {
        if (rawContext == nullptr || argc <= 0 || argv == nullptr || argv[0] == nullptr) {
            return 0;
        }

        auto* context = static_cast<MacQueryContext*>(rawContext);
        unsigned int macBytes[6] = {0, 0, 0, 0, 0, 0};
        if (std::sscanf(
                argv[0],
                "%02x:%02x:%02x:%02x:%02x:%02x",
                &macBytes[0], &macBytes[1], &macBytes[2],
                &macBytes[3], &macBytes[4], &macBytes[5]
            ) != 6) {
            return 0;
        }

        for (size_t i = 0; i < 6; ++i) {
            context->out[i] = static_cast<uint8_t>(macBytes[i]);
        }

        context->found = true;
        return 1;
    };

    char* errorMessage = nullptr;
    const int rc = sqlite3_exec(
        db_,
        "SELECT mac FROM peers "
        "WHERE name='000' OR mac='FF:FF:FF:FF:FF:FF' "
        "ORDER BY CASE WHEN name='000' THEN 0 ELSE 1 END, id ASC LIMIT 1;",
        callback,
        &context,
        &errorMessage
    );

    if (rc != SQLITE_OK && rc != SQLITE_ABORT) {
        if (errorMessage != nullptr) {
            sqlite3_free(errorMessage);
        }
        unlockDb();
        return false;
    }

    unlockDb();

    return context.found;
}

bool DatabaseStore::syncPeersFromManager(const EspNowManager& espNow) {
    if (!ready_) {
        return false;
    }

    bool allOk = true;
    const size_t total = espNow.deviceCount();
    for (size_t i = 0; i < total; ++i) {
        EspNowManager::deviceInfo item = {};
        if (!espNow.deviceAt(i, item)) {
            allOk = false;
            continue;
        }

        if (!upsertPeer(item.mac, item.name, item.description)) {
            allOk = false;
        }
    }

    return allOk;
}

bool DatabaseStore::getStatus(String& outText) {
    if (!ready_) {
        outText = "[database] nao inicializado";
        return false;
    }

    int32_t peerRows = -1;
    int32_t commandRows = -1;
    int32_t incomingRows = -1;
    int32_t outgoingRows = -1;
    int32_t bootRows = -1;
    const bool peersOk = querySingleInt("SELECT COUNT(*) FROM peers;", peerRows);
    const bool commandsOk = querySingleInt("SELECT COUNT(*) FROM command_log;", commandRows);
    const bool incomingOk = querySingleInt("SELECT COUNT(*) FROM espnow_incoming_log;", incomingRows);
    const bool outgoingOk = querySingleInt("SELECT COUNT(*) FROM espnow_outgoing_log;", outgoingRows);
    const bool bootsOk = querySingleInt("SELECT COUNT(*) FROM boot_events;", bootRows);

    char line[280] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[database] pronto arquivo=%s peers=%ld comandos=%ld rx=%ld tx=%ld boots=%ld",
        kSqliteDatabasePath,
        peersOk ? static_cast<long>(peerRows) : -1L,
        commandsOk ? static_cast<long>(commandRows) : -1L,
        incomingOk ? static_cast<long>(incomingRows) : -1L,
        outgoingOk ? static_cast<long>(outgoingRows) : -1L,
        bootsOk ? static_cast<long>(bootRows) : -1L
    );

    outText = String(line);
    return true;
}

bool DatabaseStore::listTables(String& outText) {
    if (!ready_) {
        outText = "[database] nao inicializado";
        return false;
    }

    return queryToText(
        "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name;",
        64,
        outText
    );
}

bool DatabaseStore::readTable(const String& tableName, size_t limit, String& outText) {
    if (!ready_) {
        outText = "[database] nao inicializado";
        return false;
    }

    if (!isSafeIdentifier(tableName)) {
        outText = "[database] nome de tabela invalido";
        return false;
    }

    if (limit == 0) {
        limit = 20;
    }
    if (limit > 200) {
        limit = 200;
    }

    String sql = "SELECT * FROM ";
    sql += tableName;
    sql += " LIMIT ";
    sql += static_cast<unsigned long>(limit);
    sql += ";";

    return queryToText(sql, limit, outText);
}

bool DatabaseStore::readCommandLogsWithOutput(size_t limit, String& outText) {
        if (!lockDb()) {
            outText = "[database] lock indisponivel";
            return false;
        }

    if (!ready_) {
        outText = "[database] nao inicializado";
        return false;
    }

    if (limit == 0) {
        limit = 20;
    }
    if (limit > 200) {
        limit = 200;
    }

    String sql;
    sql.reserve(220);
    sql += "SELECT c.id, c.created_at, c.source, c.command, o.output ";
    sql += "FROM command_log c ";
    sql += "LEFT JOIN command_log_output o ON o.log_id = c.id ";
    sql += "ORDER BY c.id DESC LIMIT ";
    sql += static_cast<unsigned long>(limit);
    sql += ";";

    sqlite3_stmt* stmt = nullptr;
    const int prepareRc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (prepareRc != SQLITE_OK || stmt == nullptr) {
        outText = String("[database] SQL error: ") + sqlite3_errmsg(db_);
        unlockDb();
        return false;
    }

    outText = "";
    size_t rowCount = 0;
    int stepRc = SQLITE_ROW;
    while ((stepRc = sqlite3_step(stmt)) == SQLITE_ROW) {
        ++rowCount;

        const int32_t id = static_cast<int32_t>(sqlite3_column_int(stmt, 0));
        const int64_t createdAt = static_cast<int64_t>(sqlite3_column_int64(stmt, 1));
        const String source = sanitizeAndTruncateField(safeColumnText(stmt, 2), 24);
        const String command = sanitizeAndTruncateField(safeColumnText(stmt, 3), 72);
        const String output = sanitizeAndTruncateField(safeColumnText(stmt, 4), 96);

        outText += "[";
        outText += static_cast<unsigned long>(rowCount);
        outText += "] id=";
        outText += static_cast<long>(id);
        outText += " | data_hora=";
        outText += epochToDateTimeText(createdAt);
        outText += " | source=";
        outText += source;
        outText += " | command=";
        outText += command;
        outText += " | output=";
        outText += output;
        outText += "\n";
    }

    sqlite3_finalize(stmt);

    if (stepRc != SQLITE_DONE) {
        outText = String("[database] SQL error: ") + sqlite3_errmsg(db_);
        unlockDb();
        return false;
    }

    if (rowCount == 0) {
        outText = "[database] consulta sem linhas (ou comando executado com sucesso)";
    }

    unlockDb();

    return true;
}

bool DatabaseStore::readEspNowHistory(size_t limit, String& outText) {
        if (!lockDb()) {
            outText = "[database] lock indisponivel";
            return false;
        }

    if (!ready_) {
        outText = "[database] nao inicializado";
        return false;
    }

    if (limit == 0) {
        limit = 20;
    }
    if (limit > 200) {
        limit = 200;
    }

    String txSql;
    txSql.reserve(240);
    txSql += "SELECT o.id, o.sent_at, p.name, o.mac, o.payload, o.delivered ";
    txSql += "FROM espnow_outgoing_log o ";
    txSql += "LEFT JOIN peers p ON p.id = o.peer_id ";
    txSql += "ORDER BY o.sent_at DESC LIMIT ";
    txSql += static_cast<unsigned long>(limit);
    txSql += ";";

    String rxSql;
    rxSql.reserve(240);
    rxSql += "SELECT i.id, i.received_at, p.name, p.mac, i.payload ";
    rxSql += "FROM espnow_incoming_log i ";
    rxSql += "LEFT JOIN peers p ON p.id = i.peer_id ";
    rxSql += "ORDER BY i.received_at DESC LIMIT ";
    rxSql += static_cast<unsigned long>(limit);
    rxSql += ";";

    sqlite3_stmt* txStmt = nullptr;
    int prepareRc = sqlite3_prepare_v2(db_, txSql.c_str(), -1, &txStmt, nullptr);
    if (prepareRc != SQLITE_OK || txStmt == nullptr) {
        outText = String("[database] SQL error: ") + sqlite3_errmsg(db_);
        unlockDb();
        return false;
    }

    outText = "[database] ESP-NOW TX\n";
    size_t txRows = 0;
    int txStepRc = SQLITE_ROW;
    while ((txStepRc = sqlite3_step(txStmt)) == SQLITE_ROW) {
        ++txRows;

        const int32_t id = static_cast<int32_t>(sqlite3_column_int(txStmt, 0));
        const int64_t sentAt = static_cast<int64_t>(sqlite3_column_int64(txStmt, 1));
        const String peer = sanitizeAndTruncateField(safeColumnText(txStmt, 2), 24);
        const String mac = sanitizeAndTruncateField(safeColumnText(txStmt, 3), 17);
        const String message = sanitizeAndTruncateField(safeColumnText(txStmt, 4), 96);
        const bool delivered = sqlite3_column_int(txStmt, 5) == 1;

        outText += "[";
        outText += static_cast<unsigned long>(txRows);
        outText += "] id=";
        outText += static_cast<long>(id);
        outText += " | data_hora=";
        outText += epochToDateTimeText(sentAt);
        outText += " | peer=";
        outText += peer;
        outText += " | mac=";
        outText += mac;
        outText += " | mensagem=";
        outText += message;
        outText += " | status=";
        outText += delivered ? "sucesso" : "falha";
        outText += "\n";
    }

    sqlite3_finalize(txStmt);

    if (txStepRc != SQLITE_DONE) {
        outText = String("[database] SQL error: ") + sqlite3_errmsg(db_);
        unlockDb();
        return false;
    }

    if (txRows == 0) {
        outText += "(sem linhas)\n";
    }

    sqlite3_stmt* rxStmt = nullptr;
    prepareRc = sqlite3_prepare_v2(db_, rxSql.c_str(), -1, &rxStmt, nullptr);
    if (prepareRc != SQLITE_OK || rxStmt == nullptr) {
        outText = String("[database] SQL error: ") + sqlite3_errmsg(db_);
        unlockDb();
        return false;
    }

    outText += "\n[database] ESP-NOW RX\n";
    size_t rxRows = 0;
    int rxStepRc = SQLITE_ROW;
    while ((rxStepRc = sqlite3_step(rxStmt)) == SQLITE_ROW) {
        ++rxRows;

        const int32_t id = static_cast<int32_t>(sqlite3_column_int(rxStmt, 0));
        const int64_t receivedAt = static_cast<int64_t>(sqlite3_column_int64(rxStmt, 1));
        const String peer = sanitizeAndTruncateField(safeColumnText(rxStmt, 2), 24);
        const String mac = sanitizeAndTruncateField(safeColumnText(rxStmt, 3), 17);
        const String message = sanitizeAndTruncateField(safeColumnText(rxStmt, 4), 96);

        outText += "[";
        outText += static_cast<unsigned long>(rxRows);
        outText += "] id=";
        outText += static_cast<long>(id);
        outText += " | data_hora=";
        outText += epochToDateTimeText(receivedAt);
        outText += " | peer=";
        outText += peer;
        outText += " | mac=";
        outText += mac;
        outText += " | mensagem=";
        outText += message;
        outText += " | status=recebido\n";
    }

    sqlite3_finalize(rxStmt);

    if (rxStepRc != SQLITE_DONE) {
        outText = String("[database] SQL error: ") + sqlite3_errmsg(db_);
        unlockDb();
        return false;
    }

    if (rxRows == 0) {
        outText += "(sem linhas)\n";
    }

    unlockDb();

    return true;
}

bool DatabaseStore::dropTable(const String& tableName) {
    if (!ready_) {
        return false;
    }

    if (!isSafeIdentifier(tableName)) {
        return false;
    }

    String sql = "DROP TABLE IF EXISTS ";
    sql += tableName;
    sql += ";";

    return executeNoResult(sql);
}

bool DatabaseStore::executeSql(const String& sql, String& outText) {
    if (!ready_) {
        outText = "[database] nao inicializado";
        return false;
    }

    String trimmed = sql;
    trimmed.trim();
    if (trimmed.isEmpty()) {
        outText = "[database] SQL vazio";
        return false;
    }

    return queryToText(trimmed, 80, outText);
}

bool DatabaseStore::openDatabase() {
    if (!lockDb()) {
        return false;
    }

    if (db_ != nullptr) {
        unlockDb();
        return true;
    }

    const int rc = sqlite3_open(kSqliteDatabasePath, &db_);
    if (rc != SQLITE_OK || db_ == nullptr) {
        if (db_ != nullptr) {
            logLine(String("[database] sqlite open error: ") + sqlite3_errmsg(db_));
            sqlite3_close(db_);
            db_ = nullptr;
        } else {
            logLine("[database] sqlite open error: handle nulo");
        }
        unlockDb();
        return false;
    }

    unlockDb();

    return true;
}

void DatabaseStore::closeDatabase() {
    if (!lockDb()) {
        return;
    }

    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }

    ready_ = false;
    unlockDb();
}

bool DatabaseStore::ensureBootstrapAssets() {
    File databaseDir = SD_MMC.open(kFsDatabaseDir);
    const bool hasDirectory = databaseDir && databaseDir.isDirectory();
    if (databaseDir) {
        databaseDir.close();
    }

    if (!hasDirectory && !SD_MMC.mkdir(kFsDatabaseDir)) {
        return false;
    }

    if (!SD_MMC.exists(kFsBootstrapPath)) {
        File bootstrapFile = SD_MMC.open(kFsBootstrapPath, FILE_WRITE);
        if (!bootstrapFile) {
            return false;
        }

        const size_t bytesWritten = bootstrapFile.print(kDefaultBootstrapSql);
        bootstrapFile.close();

        if (bytesWritten == 0) {
            return false;
        }

        logLine("[database] bootstrap.sql criado em /database/bootstrap.sql");
    }

    return true;
}

bool DatabaseStore::applyBootstrapScript() {
    File bootstrapFile = SD_MMC.open(kFsBootstrapPath, FILE_READ);
    if (!bootstrapFile) {
        return false;
    }

    const String script = bootstrapFile.readString();
    bootstrapFile.close();

    if (script.isEmpty()) {
        return false;
    }

    return executeNoResult(script);
}

bool DatabaseStore::applyRuntimeMigrations() {
    // Keep migrations idempotent, so existing SD cards are upgraded safely.
    return executeNoResult(
        "CREATE TABLE IF NOT EXISTS command_log_output ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "log_id INTEGER NOT NULL,"
        "output TEXT NOT NULL,"
        "created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
        "FOREIGN KEY(log_id) REFERENCES command_log(id) ON DELETE CASCADE"
        ");"
        "CREATE TABLE IF NOT EXISTS espnow_incoming_log ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "peer_id INTEGER NOT NULL,"
        "payload TEXT NOT NULL,"
        "payload_type INTEGER NOT NULL DEFAULT 0,"
        "received_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
        "FOREIGN KEY(peer_id) REFERENCES peers(id) ON DELETE CASCADE"
        ");"
        "CREATE TABLE IF NOT EXISTS espnow_outgoing_log ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "peer_id INTEGER,"
        "mac TEXT NOT NULL,"
        "payload TEXT NOT NULL,"
        "payload_type INTEGER NOT NULL DEFAULT 0,"
        "delivered INTEGER NOT NULL DEFAULT 0,"
        "sent_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),"
        "FOREIGN KEY(peer_id) REFERENCES peers(id) ON DELETE SET NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS boot_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "reason TEXT NOT NULL,"
        "boot_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_peers_mac ON peers(mac);"
        "CREATE INDEX IF NOT EXISTS idx_incoming_peer ON espnow_incoming_log(peer_id);"
        "CREATE INDEX IF NOT EXISTS idx_outgoing_peer ON espnow_outgoing_log(peer_id);"
        "CREATE INDEX IF NOT EXISTS idx_log_output_log_id ON command_log_output(log_id);"
    );
}

bool DatabaseStore::ensureDefaultBroadcastPeer() {
    if (!ready_) {
        return false;
    }

    const int64_t now = currentEpochSeconds();

    String insertSql;
    insertSql.reserve(280);
    insertSql += "INSERT OR IGNORE INTO peers(mac,name,description,created_at,updated_at) VALUES('";
    insertSql += "FF:FF:FF:FF:FF:FF";
    insertSql += "','000','peer virtual padrao para broadcast',";
    insertSql += String(static_cast<long long>(now));
    insertSql += ",";
    insertSql += String(static_cast<long long>(now));
    insertSql += ");";

    if (!executeNoResult(insertSql)) {
        return false;
    }

    String updateSql;
    updateSql.reserve(240);
    updateSql += "UPDATE peers SET name='000', description='peer virtual padrao para broadcast', updated_at=";
    updateSql += String(static_cast<long long>(now));
    updateSql += " WHERE mac='FF:FF:FF:FF:FF:FF';";

    return executeNoResult(updateSql);
}

bool DatabaseStore::peerIdByMac(const uint8_t mac[6], int32_t& outPeerId) {
    if (!ready_ || mac == nullptr) {
        return false;
    }

    String sql = "SELECT id FROM peers WHERE mac='";
    sql += escapeSqlText(macToText(mac));
    sql += "' LIMIT 1;";

    return querySingleInt(sql, outPeerId);
}

bool DatabaseStore::ensurePeerExistsWithDefaults(const uint8_t mac[6], int32_t& outPeerId) {
    outPeerId = -1;
    if (!ready_ || mac == nullptr) {
        return false;
    }

    if (peerIdByMac(mac, outPeerId)) {
        return true;
    }

    char defaultName[24] = {0};
    std::snprintf(defaultName, sizeof(defaultName), "peer-%02X%02X", mac[4], mac[5]);

    const int64_t now = currentEpochSeconds();
    String sql;
    sql.reserve(360);
    sql += "INSERT OR IGNORE INTO peers(mac,name,description,created_at,updated_at) VALUES('";
    sql += escapeSqlText(macToText(mac));
    sql += "','";
    sql += escapeSqlText(String(defaultName));
    sql += "','";
    sql += "adicionado automaticamente por RX ESP-NOW";
    sql += "',";
    sql += String(static_cast<long long>(now));
    sql += ",";
    sql += String(static_cast<long long>(now));
    sql += ");";

    if (!executeNoResult(sql)) {
        return false;
    }

    return peerIdByMac(mac, outPeerId);
}

bool DatabaseStore::loadPeersFromDatabase(EspNowManager& espNow) {
    if (!ready_ || db_ == nullptr) {
        return false;
    }

    if (!lockDb()) {
        return false;
    }

    struct PeerLoadContext {
        EspNowManager* manager;
        size_t loaded;
        size_t alreadyPresent;
        size_t skippedBroadcast;
        size_t failed;
        size_t processed;
    };

    PeerLoadContext context = {&espNow, 0, 0, 0, 0, 0};

    auto callback = [](void* rawContext, int argc, char** argv, char**) -> int {
        if (rawContext == nullptr || argc < 1 || argv == nullptr || argv[0] == nullptr) {
            return 0;
        }

        auto* context = static_cast<PeerLoadContext*>(rawContext);
        ++context->processed;

        unsigned int macBytes[6] = {0, 0, 0, 0, 0, 0};
        if (std::sscanf(
                argv[0],
                "%02x:%02x:%02x:%02x:%02x:%02x",
                &macBytes[0], &macBytes[1], &macBytes[2],
                &macBytes[3], &macBytes[4], &macBytes[5]
            ) != 6) {
            return 0;
        }

        uint8_t mac[6] = {0};
        for (size_t i = 0; i < 6; ++i) {
            mac[i] = static_cast<uint8_t>(macBytes[i]);
        }

        const bool isBroadcast =
            mac[0] == 0xFF && mac[1] == 0xFF && mac[2] == 0xFF &&
            mac[3] == 0xFF && mac[4] == 0xFF && mac[5] == 0xFF;
        if (isBroadcast) {
            // Keep broadcast peer virtual (000 alias) instead of materializing as real indexed peer.
            ++context->skippedBroadcast;
            return 0;
        }

        const char* name = (argc > 1 && argv[1] != nullptr) ? argv[1] : "peer";
        const char* description = (argc > 2 && argv[2] != nullptr) ? argv[2] : "";

        if (context->manager->deviceIndexByMac(mac) >= 0) {
            ++context->alreadyPresent;
            return 0;
        }

        if (context->manager->addDevice(mac, name, description)) {
            ++context->loaded;
        } else {
            ++context->failed;
        }

        return 0;
    };

    char* errorMessage = nullptr;
    const int rc = sqlite3_exec(
        db_,
        "SELECT mac,name,description FROM peers ORDER BY id ASC LIMIT 64;",
        callback,
        &context,
        &errorMessage
    );

    if (rc != SQLITE_OK) {
        if (errorMessage != nullptr) {
            logLine(String("[database] erro carregando peers: ") + errorMessage);
            sqlite3_free(errorMessage);
        }
        unlockDb();
        return false;
    }

    char line[192] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[database] peers lidos=%u adicionados=%u ja_em_memoria=%u ignorado_000=%u falhas=%u",
        static_cast<unsigned>(context.processed),
        static_cast<unsigned>(context.loaded),
        static_cast<unsigned>(context.alreadyPresent),
        static_cast<unsigned>(context.skippedBroadcast),
        static_cast<unsigned>(context.failed)
    );
    logLine(line);
    unlockDb();
    return true;
}

bool DatabaseStore::executeNoResult(const String& sql) {
    if (!lockDb()) {
        return false;
    }

    if (db_ == nullptr) {
        unlockDb();
        return false;
    }

    char* errorMessage = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errorMessage);
    if (rc != SQLITE_OK) {
        if (errorMessage != nullptr) {
            logLine(String("[database] SQL error: ") + errorMessage);
            sqlite3_free(errorMessage);
        }
        unlockDb();
        return false;
    }

    unlockDb();

    return true;
}

bool DatabaseStore::querySingleInt(const String& sql, int32_t& outValue) {
    if (!lockDb()) {
        return false;
    }

    if (db_ == nullptr) {
        unlockDb();
        return false;
    }

    IntQueryContext context = {false, 0};
    char* errorMessage = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), queryIntCallback, &context, &errorMessage);

    if (rc != SQLITE_OK) {
        if (errorMessage != nullptr) {
            sqlite3_free(errorMessage);
        }
        unlockDb();
        return false;
    }

    if (!context.hasValue) {
        unlockDb();
        return false;
    }

    outValue = context.value;
    unlockDb();
    return true;
}

bool DatabaseStore::queryToText(const String& sql, size_t maxRows, String& outText) {
    if (!lockDb()) {
        outText = "[database] lock indisponivel";
        return false;
    }

    if (db_ == nullptr) {
        outText = "[database] nao inicializado";
        unlockDb();
        return false;
    }

    QueryTextContext context = {&outText, 0, maxRows, false};
    outText = "";

    char* errorMessage = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), queryTextCallback, &context, &errorMessage);

    if (rc != SQLITE_OK && rc != SQLITE_ABORT) {
        if (errorMessage != nullptr) {
            outText = String("[database] SQL error: ") + errorMessage;
            sqlite3_free(errorMessage);
        } else {
            outText = "[database] SQL error";
        }
        unlockDb();
        return false;
    }

    if (context.truncated) {
        outText += "... resultado truncado ...\n";
    }

    if (context.rowCount == 0 && !context.truncated) {
        outText = "[database] consulta sem linhas (ou comando executado com sucesso)";
    }

    unlockDb();

    return true;
}

int64_t DatabaseStore::currentEpochSeconds() {
    const time_t now = time(nullptr);
    if (now > 0) {
        return static_cast<int64_t>(now);
    }

    return static_cast<int64_t>(millis() / 1000ULL);
}

void DatabaseStore::logLine(const String& text) const {
    if (io_ != nullptr) {
        io_->println(text);
    }
}

bool DatabaseStore::isSafeIdentifier(const String& value) {
    if (value.isEmpty()) {
        return false;
    }

    for (size_t i = 0; i < value.length(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (!(std::isalnum(ch) || ch == '_')) {
            return false;
        }
    }

    if (value.startsWith("sqlite_")) {
        return false;
    }

    return true;
}

String DatabaseStore::escapeSqlText(const String& value) {
    String escaped;
    escaped.reserve(value.length() + 8);

    for (size_t i = 0; i < value.length(); ++i) {
        const char ch = value[i];
        escaped += ch;
        if (ch == '\'') {
            escaped += '\'';
        }
    }

    return escaped;
}

String DatabaseStore::macToText(const uint8_t mac[6]) {
    char macText[18] = {0};
    std::snprintf(
        macText,
        sizeof(macText),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    );
    return String(macText);
}
