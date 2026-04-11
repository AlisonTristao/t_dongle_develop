#include "DatabaseStore.h"

#include <SD_MMC.h>
#include <sqlite3.h>

#include <cctype>
#include <cstdio>

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

} // namespace

DatabaseStore::DatabaseStore()
    : db_(nullptr),
      io_(nullptr),
      ready_(false) {
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

    ready_ = true;

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

    const String macText = macToText(mac);
    const String peerName = (name != nullptr) ? name : "";
    const String peerDescription = (description != nullptr) ? description : "";

    String sql;
    sql.reserve(320);
    sql += "INSERT INTO peers(mac,name,description,updated_at) VALUES('";
    sql += escapeSqlText(macText);
    sql += "','";
    sql += escapeSqlText(peerName);
    sql += "','";
    sql += escapeSqlText(peerDescription);
    sql += "',strftime('%s','now')) ";
    sql += "ON CONFLICT(mac) DO UPDATE SET ";
    sql += "name=excluded.name, ";
    sql += "description=excluded.description, ";
    sql += "updated_at=strftime('%s','now');";

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

bool DatabaseStore::logCommand(const char* command, const char* source) {
    if (!ready_ || command == nullptr || command[0] == '\0') {
        return false;
    }

    const String sourceText = (source != nullptr) ? source : "serial";

    String sql;
    sql.reserve(320);
    sql += "INSERT INTO command_log(command,source,created_at) VALUES('";
    sql += escapeSqlText(String(command));
    sql += "','";
    sql += escapeSqlText(sourceText);
    sql += "',strftime('%s','now'));";

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
    const bool peersOk = querySingleInt("SELECT COUNT(*) FROM peers;", peerRows);
    const bool commandsOk = querySingleInt("SELECT COUNT(*) FROM command_log;", commandRows);

    char line[200] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[database] pronto arquivo=%s peers=%ld comandos=%ld",
        kSqliteDatabasePath,
        peersOk ? static_cast<long>(peerRows) : -1L,
        commandsOk ? static_cast<long>(commandRows) : -1L
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
    if (db_ != nullptr) {
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
        return false;
    }

    return true;
}

void DatabaseStore::closeDatabase() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }

    ready_ = false;
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

bool DatabaseStore::loadPeersFromDatabase(EspNowManager& espNow) {
    if (!ready_ || db_ == nullptr) {
        return false;
    }

    struct PeerLoadContext {
        EspNowManager* manager;
        size_t loaded;
    };

    PeerLoadContext context = {&espNow, 0};

    auto callback = [](void* rawContext, int argc, char** argv, char**) -> int {
        if (rawContext == nullptr || argc < 1 || argv == nullptr || argv[0] == nullptr) {
            return 0;
        }

        auto* context = static_cast<PeerLoadContext*>(rawContext);

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

        const char* name = (argc > 1 && argv[1] != nullptr) ? argv[1] : "peer";
        const char* description = (argc > 2 && argv[2] != nullptr) ? argv[2] : "";

        if (context->manager->addDevice(mac, name, description)) {
            ++context->loaded;
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
        return false;
    }

    char line[96] = {0};
    std::snprintf(line, sizeof(line), "[database] peers carregados: %u", static_cast<unsigned>(context.loaded));
    logLine(line);
    return true;
}

bool DatabaseStore::executeNoResult(const String& sql) {
    if (db_ == nullptr) {
        return false;
    }

    char* errorMessage = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errorMessage);
    if (rc != SQLITE_OK) {
        if (errorMessage != nullptr) {
            logLine(String("[database] SQL error: ") + errorMessage);
            sqlite3_free(errorMessage);
        }
        return false;
    }

    return true;
}

bool DatabaseStore::querySingleInt(const String& sql, int32_t& outValue) {
    if (db_ == nullptr) {
        return false;
    }

    IntQueryContext context = {false, 0};
    char* errorMessage = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), queryIntCallback, &context, &errorMessage);

    if (rc != SQLITE_OK) {
        if (errorMessage != nullptr) {
            sqlite3_free(errorMessage);
        }
        return false;
    }

    if (!context.hasValue) {
        return false;
    }

    outValue = context.value;
    return true;
}

bool DatabaseStore::queryToText(const String& sql, size_t maxRows, String& outText) {
    if (db_ == nullptr) {
        outText = "[database] nao inicializado";
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
        return false;
    }

    if (context.truncated) {
        outText += "... resultado truncado ...\n";
    }

    if (context.rowCount == 0 && !context.truncated) {
        outText = "[database] consulta sem linhas (ou comando executado com sucesso)";
    }

    return true;
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
