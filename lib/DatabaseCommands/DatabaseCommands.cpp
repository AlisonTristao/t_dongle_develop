#include "DatabaseCommands.h"

#include "ShellCommandSupport.h"
#include "error_codes.h"

namespace {

using std::string;
using ShellCommandSupport::context;
using ShellCommandSupport::failWithCode;
using ShellCommandSupport::printLine;
using ShellCommandSupport::stripOuterQuotes;
using ShellCommandSupport::warnWithCode;

// Destructive commands require this exact word as their last argument.
// Guards against typos/accidental Enter, not a real access-control boundary.
constexpr const char* kConfirmToken = "CONFIRMAR";

bool isConfirmed(const string& token) {
    return stripOuterQuotes(token) == kConfirmToken;
}

uint8_t wrapper_database_init() {
    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando init");
    }

    if (context().espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para bootstrap do database");
    }

    if (context().database->begin(*context().espNow, context().io)) {
        const bool syncOk = context().database->syncPeersFromManager(*context().espNow);
        if (!syncOk) {
            warnWithCode(AppError::Code::DATABASE_PEER_SYNC_FAILED, "database inicializado, mas falhou ao sincronizar peers");
        }

        if (syncOk) {
            printLine("[database] inicializado e sincronizado com peers");
        } else {
            printLine("[database] inicializado (sync de peers com aviso)");
        }
        return RESULT_OK;
    }

    return failWithCode(AppError::Code::DATABASE_INIT_FAILED, "falha ao inicializar (confira SD e sqlite)");
}

uint8_t wrapper_database_status() {
    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando status");
    }

    String status;
    const bool ok = context().database->getStatus(status);
    printLine(status.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_QUERY_FAILED, "falha ao consultar status do database");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_tables() {
    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando tables");
    }

    String output;
    const bool ok = context().database->listTables(output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_QUERY_FAILED, "falha ao listar tabelas");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_read(string tableName, int32_t limit = 20) {
    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando read");
    }

    String output;
    const size_t boundedLimit = (limit > 0) ? static_cast<size_t>(limit) : 20U;
    const bool ok = context().database->readTable(stripOuterQuotes(tableName).c_str(), boundedLimit, output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_QUERY_FAILED, "falha ao ler tabela");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_drop(string tableName, string confirm = "") {
    if (!isConfirmed(confirm)) {
        printLine(
            "[database] isso apaga a tabela '" + stripOuterQuotes(tableName) +
            "' permanentemente. Para confirmar: database -drop " + stripOuterQuotes(tableName) + ", " + kConfirmToken
        );
        return RESULT_OK;
    }

    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando drop");
    }

    const String safeName = String(stripOuterQuotes(tableName).c_str());
    const bool ok = context().database->dropTable(safeName);
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_DROP_FAILED, "falha ao remover tabela");
    }

    printLine("[database] tabela removida");
    return RESULT_OK;
}

uint8_t wrapper_database_logs(int32_t limit = 20) {
    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando logs");
    }

    const size_t boundedLimit = (limit > 0) ? static_cast<size_t>(limit) : 20U;
    String output;
    const bool ok = context().database->readCommandLogsWithOutput(boundedLimit, output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_QUERY_FAILED, "falha ao ler logs de comandos");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_espnow_history(int32_t limit = 30) {
    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando espnow_history");
    }

    const size_t boundedLimit = (limit > 0) ? static_cast<size_t>(limit) : 30U;
    String output;
    const bool ok = context().database->readEspNowHistory(boundedLimit, output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_QUERY_FAILED, "falha ao ler historico ESP-NOW");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_rebuild(string confirm = "") {
    if (!isConfirmed(confirm)) {
        printLine(string("[database] isso apaga o banco inteiro e recria do zero. Para confirmar: database -rebuild ") + kConfirmToken);
        return RESULT_OK;
    }

    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando rebuild");
    }

    if (context().espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando rebuild");
    }

    if (context().database->rebuild(*context().espNow)) {
        if (!context().database->syncPeersFromManager(*context().espNow)) {
            warnWithCode(AppError::Code::DATABASE_PEER_SYNC_FAILED, "banco recriado, mas falhou ao sincronizar peers");
        }
        printLine("[database] banco recriado com bootstrap.sql");
        return RESULT_OK;
    }

    return failWithCode(AppError::Code::DATABASE_REBUILD_FAILED, "falha ao recriar banco");
}

uint8_t wrapper_database_count(string tableName) {
    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando count");
    }

    const string safeTableName = stripOuterQuotes(tableName);
    int32_t count = 0;
    if (!context().database->countRows(String(safeTableName.c_str()), count)) {
        return failWithCode(AppError::Code::DATABASE_QUERY_FAILED, "falha ao contar linhas (tabela invalida?)");
    }

    char line[96] = {0};
    std::snprintf(line, sizeof(line), "[database] %s: %ld linha(s)", safeTableName.c_str(), static_cast<long>(count));
    printLine(line);
    return RESULT_OK;
}

uint8_t wrapper_database_delete(string tableName, string condition) {
    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando delete");
    }

    const string safeCondition = stripOuterQuotes(condition);
    if (safeCondition.empty()) {
        return failWithCode(AppError::Code::INVALID_ARGUMENT, "condicao obrigatoria. Uso: database -delete <tabela>, <condicao SQL>");
    }

    int32_t deletedCount = 0;
    const bool ok = context().database->deleteRows(
        String(stripOuterQuotes(tableName).c_str()),
        String(safeCondition.c_str()),
        deletedCount
    );
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_EXEC_FAILED, "falha ao deletar linhas (tabela ou condicao invalida)");
    }

    char line[80] = {0};
    std::snprintf(line, sizeof(line), "[database] %ld linha(s) removida(s)", static_cast<long>(deletedCount));
    printLine(line);
    return RESULT_OK;
}

uint8_t wrapper_database_vacuum() {
    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando vacuum");
    }

    String output;
    const bool ok = context().database->vacuum(output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_EXEC_FAILED, "falha ao compactar banco");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_export(string tableName) {
    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando export");
    }

    String output;
    const bool ok = context().database->exportTableToCsv(String(stripOuterQuotes(tableName).c_str()), output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_EXEC_FAILED, "falha ao exportar tabela");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_clear_logs(string confirm = "") {
    if (!isConfirmed(confirm)) {
        printLine(string("[database] isso apaga command_log, saidas e historico de envios ESP-NOW. Para confirmar: database -clear_logs ") + kConfirmToken);
        return RESULT_OK;
    }

    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando clear_logs");
    }

    String output;
    const bool ok = context().database->clearLogs(output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_EXEC_FAILED, "falha ao limpar logs");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_backup() {
    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando backup");
    }

    String output;
    const bool ok = context().database->backup(output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_QUERY_FAILED, "falha ao gerar backup do banco");
    }

    return RESULT_OK;
}

} // namespace

namespace DatabaseCommands {

uint8_t exec(string sql) {
    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando exec");
    }

    String output;
    const bool ok = context().database->executeSql(stripOuterQuotes(sql).c_str(), output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_EXEC_FAILED, "falha ao executar SQL");
    }

    return RESULT_OK;
}

uint8_t execNoLog(string sql) {
    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando exec_nolog");
    }

    String output;
    const bool ok = context().database->executeSql(stripOuterQuotes(sql).c_str(), output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_EXEC_FAILED, "falha ao executar SQL");
    }

    return RESULT_OK;
}

uint8_t registerAll() {
    if (context().shell == nullptr) {
        return failWithCode(AppError::Code::SHELL_NOT_READY, "shell nao configurada para registrar modulo database");
    }

    context().shell->create_module("database", "SQLite on SD: status, reading and maintenance");

    context().shell->add(wrapper_database_init, "init", "open database and apply bootstrap.sql", "database");
    context().shell->add(wrapper_database_status, "status", "overall SQLite status", "database");
    context().shell->add(wrapper_database_tables, "tables", "list tables in database", "database");
    context().shell->add(wrapper_database_read, "read", "read table: <name>, <limit>", "database");
    context().shell->add(wrapper_database_logs, "logs", "command history with outputs: <limit>", "database");
    context().shell->add(wrapper_database_espnow_history, "espnow_history", "ESP-NOW RX/TX history with status: <limit>", "database");
    context().shell->add(wrapper_database_drop, "drop", "remove table: <name>, CONFIRMAR", "database");
    context().shell->add(wrapper_database_rebuild, "rebuild", "recreate database from bootstrap: CONFIRMAR", "database");
    context().shell->add(wrapper_database_backup, "backup", "save database snapshot to /database/backups", "database");
    context().shell->add(wrapper_database_count, "count", "count rows in a table: <name>", "database");
    context().shell->add(wrapper_database_delete, "delete", "delete rows: <table>, <sql where condition>", "database");
    context().shell->add(wrapper_database_vacuum, "vacuum", "compact database file (reclaim space)", "database");
    context().shell->add(wrapper_database_export, "export", "dump table to CSV on SD: <table>", "database");
    context().shell->add(wrapper_database_clear_logs, "clear_logs", "purge command/espnow logs: CONFIRMAR", "database");
    context().shell->add(exec, "exec", "execute raw SQL: <sql>", "database");
    context().shell->add(execNoLog, "exec_nolog", "execute SQL without saving to command_log", "database");

    return RESULT_OK;
}

} // namespace DatabaseCommands
