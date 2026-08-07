#include "ShellConfig.h"

#include "DatabaseCommands.h"
#include "DongleCommands.h"
#include "EspNowCommands.h"
#include "HelpCommands.h"
#include "ShellAliases.h"
#include "ShellCommandSupport.h"
#include "error_codes.h"

#include <cstdio>

namespace {

using std::string;

// Fills in the default broadcast peer (000) when send_to is called with only a
// message and no device number, e.g. "espnow -send_to \"dongle -run status\"".
string applyEspNowSendToDefault(const string& command) {
    const string prefix = "espnow -send_to ";

    if (command.rfind(prefix, 0) == 0) {
        const string args = ShellCommandSupport::trimCopy(command.substr(prefix.length()));
        if (!args.empty() && args.find(',') == string::npos) {
            return "espnow -send_to 000, " + args;
        }
    }

    return command;
}

string normalizeCommand(const string& command) {
    const string trimmed = ShellCommandSupport::trimCopy(command);
    const string aliasResolved = ShellAliases::resolve(trimmed);
    return applyEspNowSendToDefault(aliasResolved);
}

} // namespace

namespace ShellConfig {

bool bind(const Context& context) {
    if (context.shell == nullptr ||
        context.espNow == nullptr ||
        context.peripherals == nullptr ||
        context.lcdDashboard == nullptr ||
        context.database == nullptr ||
        context.io == nullptr) {
        return false;
    }

    ShellCommandSupport::setContext(ShellCommandSupport::Context{
        context.shell,
        context.espNow,
        context.peripherals,
        context.lcdDashboard,
        context.database,
        context.io
    });

    context.shell->set_output_callback([](const string& output) {
        ShellCommandSupport::appendShellResponse(output);
    });

    context.lcdDashboard->begin(*context.peripherals);
    return true;
}

uint8_t registerDefaultModules() {
    if (ShellCommandSupport::context().shell == nullptr) {
        return ShellCommandSupport::failWithCode(AppError::Code::SHELL_NOT_READY, "shell nao configurada para registrar modulos");
    }

    uint8_t result = HelpCommands::registerAll();
    if (result != RESULT_OK) {
        return result;
    }

    result = DongleCommands::registerAll();
    if (result != RESULT_OK) {
        return result;
    }

    result = EspNowCommands::registerAll();
    if (result != RESULT_OK) {
        return result;
    }

    result = DatabaseCommands::registerAll();
    if (result != RESULT_OK) {
        return result;
    }

    return RESULT_OK;
}

std::string runLine(const std::string& command) {
    const ShellCommandSupport::Context& ctx = ShellCommandSupport::context();
    if (ctx.shell == nullptr) {
        char line[96] = {0};
        std::snprintf(
            line,
            sizeof(line),
            "erro(%u/%s) Shell nao configurada.",
            static_cast<unsigned>(AppError::value(AppError::Code::SHELL_NOT_READY)),
            AppError::name(AppError::Code::SHELL_NOT_READY)
        );
        return line;
    }

    const std::string normalized = normalizeCommand(command);
    ShellCommandSupport::resetBuffers();

    std::string output;
    const std::string databaseExecPrefix = "database -exec";
    const std::string databaseExecNoLogPrefix = "database -exec_nolog";
    bool skipCommandPersistence = false;

    if (normalized == databaseExecNoLogPrefix || normalized.rfind(databaseExecNoLogPrefix + " ", 0) == 0) {
        const std::string sql = ShellCommandSupport::trimCopy(normalized.substr(databaseExecNoLogPrefix.length()));
        if (sql.empty()) {
            ShellCommandSupport::failWithCode(AppError::Code::INVALID_ARGUMENT, "uso: database -exec_nolog \"<sql>\"");
        } else {
            DatabaseCommands::execNoLog(sql);
        }
        skipCommandPersistence = true;

    // Bypass TinyShell tokenizer here so SQL can contain commas/quotes safely.
    } else if (normalized == databaseExecPrefix || normalized.rfind(databaseExecPrefix + " ", 0) == 0) {
        const std::string sql = ShellCommandSupport::trimCopy(normalized.substr(databaseExecPrefix.length()));
        if (sql.empty()) {
            ShellCommandSupport::failWithCode(AppError::Code::INVALID_ARGUMENT, "uso: database -exec \"<sql>\"");
        } else {
            DatabaseCommands::exec(sql);
        }
        skipCommandPersistence = true;
    } else {
        ctx.shell->run_command_line(normalized);
        output = ShellCommandSupport::shellResponseBuffer();
    }

    if (!skipCommandPersistence && ctx.database != nullptr && ctx.database->isReady()) {
        std::string persistedOutput = ShellCommandSupport::commandOutputBuffer();
        if (!output.empty()) {
            if (!persistedOutput.empty()) {
                persistedOutput += "\n";
            }
            persistedOutput += output;
        }

        if (persistedOutput.empty()) {
            persistedOutput = "(sem saida textual)";
        }

        if (!ctx.database->logCommandWithOutput(normalized.c_str(), persistedOutput.c_str(), "serial")) {
            ShellCommandSupport::warnWithCode(AppError::Code::DATABASE_COMMAND_LOG_FAILED, "falha ao persistir log de comando");
        }
    }

    ShellCommandSupport::resetBuffers();

    return output;
}

} // namespace ShellConfig
