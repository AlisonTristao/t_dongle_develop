#include "DatabaseStore.h"

#include <SD_MMC.h>
#include <sqlite3.h>

#include <cctype>
#include <cstdio>
#include <cstring>
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

// Human-readable preview for the espnow_outgoing_log audit trail: this call
// site only ever logs the shell one-liner text sent by "espnow -send_to/
// -send_all" (see EspNowCommands.cpp), never a raw BTP envelope, but is kept
// byte-safe (control bytes escaped, hard length cap) in case that changes.
String payloadPreviewText(const uint8_t* payload, size_t payloadSize) {
    constexpr size_t kPreviewCap = 256;
    const size_t length = (payloadSize < kPreviewCap) ? payloadSize : kPreviewCap;

    String out;
    out.reserve(length + 4);
    for (size_t i = 0; i < length; ++i) {
        const uint8_t byte = (payload != nullptr) ? payload[i] : 0;
        out += (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
    }
    if (payloadSize > kPreviewCap) {
        out += "...";
    }
    return out;
}

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

bool DatabaseStore::begin(Stream* io) {
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

    return true;
}

bool DatabaseStore::loadPeers(EspNowManager& espNow) {
    return loadPeersFromDatabase(espNow);
}

bool DatabaseStore::rebuild(EspNowManager& espNow) {
    closeDatabase();

    if (SD_MMC.exists(kFsDatabasePath) && !SD_MMC.remove(kFsDatabasePath)) {
        logLine("[database] falha ao remover banco anterior");
        return false;
    }

    if (!begin(io_)) {
        return false;
    }

    // rebuild() runs post-boot with ESP-NOW already up (unlike the split
    // begin()/loadPeers() at startup), so reload peers in the same call.
    if (!loadPeers(espNow)) {
        logLine("[database] banco recriado, mas falhou carga inicial de peers");
    }

    return true;
}

bool DatabaseStore::backup(String& outText) {
    outText = "";

    if (!ready_) {
        outText = "[database] nao inicializado";
        return false;
    }

    File backupDir = SD_MMC.open(kFsBackupDir);
    const bool hasBackupDir = backupDir && backupDir.isDirectory();
    if (backupDir) {
        backupDir.close();
    }

    if (!hasBackupDir && !SD_MMC.mkdir(kFsBackupDir)) {
        outText = "[database] falha ao criar pasta de backup";
        return false;
    }

    const int64_t nowEpoch = currentEpochSeconds();
    const time_t nowRaw = static_cast<time_t>(nowEpoch);
    std::tm localTime = {};
    char timestamp[32] = {0};
    if (nowRaw > 0 && localtime_r(&nowRaw, &localTime) != nullptr) {
        std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &localTime);
    } else {
        std::snprintf(timestamp, sizeof(timestamp), "epoch_%lld", static_cast<long long>(nowEpoch));
    }

    const String baseName = String("dongle_") + timestamp;
    String fsBackupPath = String(kFsBackupDir) + "/" + baseName + ".db";
    int suffix = 1;
    while (SD_MMC.exists(fsBackupPath) && suffix < 1000) {
        char suffixText[8] = {0};
        std::snprintf(suffixText, sizeof(suffixText), "_%03d", suffix);
        fsBackupPath = String(kFsBackupDir) + "/" + baseName + suffixText + ".db";
        ++suffix;
    }

    if (SD_MMC.exists(fsBackupPath)) {
        outText = "[database] falha ao gerar nome unico para backup";
        return false;
    }

    const String sqliteBackupPath = String("/sdcard") + fsBackupPath;

    if (!lockDb(5000)) {
        outText = "[database] lock indisponivel para backup";
        return false;
    }

    if (db_ == nullptr) {
        outText = "[database] nao inicializado";
        unlockDb();
        return false;
    }

    char* checkpointError = nullptr;
    sqlite3_exec(db_, "PRAGMA wal_checkpoint(FULL);", nullptr, nullptr, &checkpointError);
    if (checkpointError != nullptr) {
        sqlite3_free(checkpointError);
    }

    sqlite3* backupDb = nullptr;
    const int openRc = sqlite3_open(sqliteBackupPath.c_str(), &backupDb);
    if (openRc != SQLITE_OK || backupDb == nullptr) {
        if (backupDb != nullptr) {
            sqlite3_close(backupDb);
        }
        unlockDb();
        outText = "[database] falha ao abrir arquivo de backup";
        return false;
    }

    sqlite3_backup* backupHandle = sqlite3_backup_init(backupDb, "main", db_, "main");
    if (backupHandle == nullptr) {
        sqlite3_close(backupDb);
        unlockDb();
        outText = "[database] falha ao iniciar copia do backup";
        return false;
    }

    const int stepRc = sqlite3_backup_step(backupHandle, -1);
    const int finishRc = sqlite3_backup_finish(backupHandle);
    const int destErr = sqlite3_errcode(backupDb);
    sqlite3_close(backupDb);
    unlockDb();

    const bool copied = (stepRc == SQLITE_DONE) && (finishRc == SQLITE_OK) && (destErr == SQLITE_OK);
    if (!copied) {
        SD_MMC.remove(fsBackupPath);
        outText = "[database] falha ao gerar backup";
        return false;
    }

    uint64_t backupBytes = 0;
    File backupFile = SD_MMC.open(fsBackupPath, FILE_READ);
    if (backupFile) {
        backupBytes = static_cast<uint64_t>(backupFile.size());
        backupFile.close();
    }

    char line[256] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[database] backup salvo: %s (%lluB)",
        fsBackupPath.c_str(),
        static_cast<unsigned long long>(backupBytes)
    );
    outText = line;
    return true;
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

bool DatabaseStore::logOutgoingEspNow(const uint8_t mac[6], btp::MessageType type, const uint8_t* payload, size_t payloadSize, bool delivered) {
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
    sql += escapeSqlText(payloadPreviewText(payload, payloadSize));
    sql += "',";
    sql += String(static_cast<int>(type));
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
    int32_t outgoingRows = -1;
    int32_t bootRows = -1;
    const bool peersOk = querySingleInt("SELECT COUNT(*) FROM peers;", peerRows);
    const bool commandsOk = querySingleInt("SELECT COUNT(*) FROM command_log;", commandRows);
    const bool outgoingOk = querySingleInt("SELECT COUNT(*) FROM espnow_outgoing_log;", outgoingRows);
    const bool bootsOk = querySingleInt("SELECT COUNT(*) FROM boot_events;", bootRows);

    char line[256] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[database] pronto arquivo=%s peers=%ld comandos=%ld tx=%ld boots=%ld",
        kSqliteDatabasePath,
        peersOk ? static_cast<long>(peerRows) : -1L,
        commandsOk ? static_cast<long>(commandRows) : -1L,
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

bool DatabaseStore::readRecentCommands(size_t limit, String& outText) {
    if (!lockDb()) {
        outText = "[database] lock indisponivel";
        return false;
    }

    if (!ready_ || db_ == nullptr) {
        outText = "[database] nao inicializado";
        unlockDb();
        return false;
    }

    if (limit == 0) {
        limit = 64;
    }
    if (limit > 256) {
        limit = 256;
    }

    String sql;
    sql.reserve(280);
    sql += "SELECT command FROM (";
    sql += "SELECT id, command FROM command_log ";
    sql += "WHERE source='serial' ORDER BY id DESC LIMIT ";
    sql += static_cast<unsigned long>(limit);
    sql += ") t ORDER BY id ASC;";

    sqlite3_stmt* stmt = nullptr;
    const int prepareRc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (prepareRc != SQLITE_OK || stmt == nullptr) {
        outText = String("[database] SQL error: ") + sqlite3_errmsg(db_);
        unlockDb();
        return false;
    }

    outText = "";
    int stepRc = SQLITE_ROW;
    while ((stepRc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char* command = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (command == nullptr || command[0] == '\0') {
            continue;
        }

        outText += command;
        outText += "\n";
    }

    sqlite3_finalize(stmt);

    if (stepRc != SQLITE_DONE) {
        outText = String("[database] SQL error: ") + sqlite3_errmsg(db_);
        unlockDb();
        return false;
    }

    outText.trim();
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

bool DatabaseStore::countRows(const String& tableName, int32_t& outCount) {
    if (!ready_) {
        return false;
    }

    if (!isSafeIdentifier(tableName)) {
        return false;
    }

    String sql = "SELECT COUNT(*) FROM ";
    sql += tableName;
    sql += ";";

    return querySingleInt(sql, outCount);
}

bool DatabaseStore::deleteRows(const String& tableName, const String& whereClause, int32_t& outDeletedCount) {
    outDeletedCount = 0;

    if (!ready_ || db_ == nullptr) {
        return false;
    }

    if (!isSafeIdentifier(tableName) || whereClause.isEmpty()) {
        return false;
    }

    String sql = "DELETE FROM ";
    sql += tableName;
    sql += " WHERE ";
    sql += whereClause;
    sql += ";";

    if (!lockDb()) {
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

    outDeletedCount = static_cast<int32_t>(sqlite3_changes(db_));
    unlockDb();
    return true;
}

bool DatabaseStore::vacuum(String& outText) {
    if (!ready_ || db_ == nullptr) {
        outText = "[database] nao inicializado";
        return false;
    }

    uint64_t beforeBytes = 0;
    File beforeFile = SD_MMC.open(kFsDatabasePath, FILE_READ);
    if (beforeFile) {
        beforeBytes = static_cast<uint64_t>(beforeFile.size());
        beforeFile.close();
    }

    if (!executeNoResult("VACUUM;")) {
        outText = "[database] falha ao executar VACUUM";
        return false;
    }

    uint64_t afterBytes = 0;
    File afterFile = SD_MMC.open(kFsDatabasePath, FILE_READ);
    if (afterFile) {
        afterBytes = static_cast<uint64_t>(afterFile.size());
        afterFile.close();
    }

    char line[128] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[database] VACUUM concluido: %lluB -> %lluB",
        static_cast<unsigned long long>(beforeBytes),
        static_cast<unsigned long long>(afterBytes)
    );
    outText = line;
    return true;
}

bool DatabaseStore::exportTableToCsv(const String& tableName, String& outText) {
    if (!ready_ || db_ == nullptr) {
        outText = "[database] nao inicializado";
        return false;
    }

    if (!isSafeIdentifier(tableName)) {
        outText = "[database] nome de tabela invalido";
        return false;
    }

    if (!lockDb(2000)) {
        outText = "[database] lock indisponivel para exportar";
        return false;
    }

    String selectSql = "SELECT * FROM ";
    selectSql += tableName;
    selectSql += ";";

    sqlite3_stmt* stmt = nullptr;
    const int prepareRc = sqlite3_prepare_v2(db_, selectSql.c_str(), -1, &stmt, nullptr);
    if (prepareRc != SQLITE_OK || stmt == nullptr) {
        outText = String("[database] SQL error: ") + sqlite3_errmsg(db_);
        unlockDb();
        return false;
    }

    File exportDir = SD_MMC.open("/database/exports");
    const bool hasExportDir = exportDir && exportDir.isDirectory();
    if (exportDir) {
        exportDir.close();
    }
    if (!hasExportDir && !SD_MMC.mkdir("/database/exports")) {
        sqlite3_finalize(stmt);
        unlockDb();
        outText = "[database] falha ao criar pasta de exportacao";
        return false;
    }

    const String csvPath = String("/database/exports/") + tableName + ".csv";
    File csvFile = SD_MMC.open(csvPath, FILE_WRITE);
    if (!csvFile) {
        sqlite3_finalize(stmt);
        unlockDb();
        outText = "[database] falha ao criar arquivo CSV";
        return false;
    }

    const int columnCount = sqlite3_column_count(stmt);
    for (int i = 0; i < columnCount; ++i) {
        if (i > 0) {
            csvFile.print(",");
        }
        csvFile.print(sqlite3_column_name(stmt, i));
    }
    csvFile.print("\n");

    size_t rowCount = 0;
    int stepRc = SQLITE_ROW;
    while ((stepRc = sqlite3_step(stmt)) == SQLITE_ROW) {
        for (int i = 0; i < columnCount; ++i) {
            if (i > 0) {
                csvFile.print(",");
            }

            const unsigned char* text = sqlite3_column_text(stmt, i);
            if (text == nullptr) {
                continue;
            }

            String value = reinterpret_cast<const char*>(text);
            value.replace("\"", "\"\"");
            if (value.indexOf(',') >= 0 || value.indexOf('"') >= 0 || value.indexOf('\n') >= 0) {
                csvFile.print("\"");
                csvFile.print(value);
                csvFile.print("\"");
            } else {
                csvFile.print(value);
            }
        }
        csvFile.print("\n");
        ++rowCount;
    }

    csvFile.close();
    sqlite3_finalize(stmt);
    unlockDb();

    if (stepRc != SQLITE_DONE) {
        outText = String("[database] SQL error durante exportacao: ") + sqlite3_errmsg(db_);
        return false;
    }

    char line[160] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[database] exportado %s: %u linha(s) -> %s",
        tableName.c_str(),
        static_cast<unsigned>(rowCount),
        csvPath.c_str()
    );
    outText = line;
    return true;
}

bool DatabaseStore::clearLogs(String& outText) {
    if (!ready_ || db_ == nullptr) {
        outText = "[database] nao inicializado";
        return false;
    }

    int32_t commandRows = 0;
    int32_t outgoingRows = 0;
    querySingleInt("SELECT COUNT(*) FROM command_log;", commandRows);
    querySingleInt("SELECT COUNT(*) FROM espnow_outgoing_log;", outgoingRows);

    const bool ok = executeNoResult(
        "DELETE FROM command_log_output;"
        "DELETE FROM command_log;"
        "DELETE FROM espnow_outgoing_log;"
    );

    if (!ok) {
        outText = "[database] falha ao limpar logs";
        return false;
    }

    char line[128] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[database] logs limpos: %ld comando(s) e %ld envio(s) removidos",
        static_cast<long>(commandRows),
        static_cast<long>(outgoingRows)
    );
    outText = line;
    return true;
}

bool DatabaseStore::beginTransaction() {
    if (!ready_) {
        return false;
    }

    return executeNoResult("BEGIN IMMEDIATE;");
}

bool DatabaseStore::commitTransaction() {
    if (!ready_) {
        return false;
    }

    return executeNoResult("COMMIT;");
}

bool DatabaseStore::rollbackTransaction() {
    if (!ready_) {
        return false;
    }

    return executeNoResult("ROLLBACK;");
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

    // Sqlite3Esp32's config_ext.h bakes in SQLITE_DEFAULT_LOOKASIDE=512,64,
    // a fixed 32KB-per-connection cache reserved up front. Disabling it here
    // (must happen before any statement prepares, per the SQLite docs on
    // SQLITE_DBCONFIG_LOOKASIDE) makes small internal allocations fall back
    // to general malloc instead -- slower per-statement, irrelevant at this
    // firmware's call volume, and worth 32KB back out of a boot sequence
    // that was leaving only ~6KB free (topico 33 bench log).
    sqlite3_db_config(db_, SQLITE_DBCONFIG_LOOKASIDE, nullptr, 0, 0);

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
    insertSql += "','Default','peer virtual padrao para broadcast',";
    insertSql += String(static_cast<long long>(now));
    insertSql += ",";
    insertSql += String(static_cast<long long>(now));
    insertSql += ");";

    if (!executeNoResult(insertSql)) {
        return false;
    }

    String updateSql;
    updateSql.reserve(240);
    updateSql += "UPDATE peers SET name='Default', description='peer virtual padrao para broadcast', updated_at=";
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
