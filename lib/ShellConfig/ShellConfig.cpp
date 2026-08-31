#include "ShellConfig.h"

#include "DatabaseCommands.h"
#include "DongleCommands.h"
#include "EspNowCommands.h"
#include "HelpCommands.h"
#include "ShellAliases.h"
#include "ShellCommandSupport.h"
#include "SudoCommands.h"
#include "error_codes.h"

#include <ShellStyle.h>

#include <Arduino.h>
#include <Esp.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <vector>

namespace {

using std::string;

// Splits on ';' at the top level only (not inside "..." or '...'), so a
// quoted argument containing ';' isn't torn apart, e.g.
// dongle -lcd "a; b"; dongle -ping
std::vector<string> splitTopLevelSemicolons(const string& line) {
    std::vector<string> parts;
    string current;
    bool inDoubleQuote = false;
    bool inSingleQuote = false;

    for (const char ch : line) {
        if (ch == '"' && !inSingleQuote) {
            inDoubleQuote = !inDoubleQuote;
        } else if (ch == '\'' && !inDoubleQuote) {
            inSingleQuote = !inSingleQuote;
        }

        if (ch == ';' && !inDoubleQuote && !inSingleQuote) {
            parts.push_back(current);
            current.clear();
            continue;
        }

        current += ch;
    }
    parts.push_back(current);
    return parts;
}

// Last command line handed to run_command_line, regardless of whether it got
// persisted. Lets runLine() skip logging when the same command is repeated
// back-to-back (e.g. re-sending the same espnow message), without affecting
// whether the command actually runs.
string g_lastCommandLine;

// Redirects to send_all when send_to is called with only a message and no
// device number, e.g. "espnow -send_to \"dongle -run status\"". There is no
// broadcast peer index anymore (a BTP COMMAND_REQUEST needs a real per-peer
// boot_id, see EspNowCommands.cpp), so "no index given" now means "every
// peer we can currently address", same as calling send_all directly.
string applyEspNowSendToDefault(const string& command) {
    const string prefix = "espnow -send_to ";

    if (command.rfind(prefix, 0) == 0) {
        const string args = ShellCommandSupport::trimCopy(command.substr(prefix.length()));
        if (!args.empty() && args.find(',') == string::npos) {
            return "espnow -send_all " + args;
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

    // DIAG (boot-loop, topico 34/35): each module registers dozens of
    // std::string command names/descriptions. If the panic is here it is
    // heap -- the last "reg:" line printed names the module that could not
    // allocate, and `largest` vs `free_heap` says exhaustion vs fragmentation.
    // Compiled out unless -DDIAG_BOOT (platformio.ini) -- see topico 35 D.3.
    #ifdef DIAG_BOOT
    #define DIAG_REG(stage) Serial.printf("! reg:%s free_heap=%u largest=%u\r\n", (stage), \
        (unsigned) ESP.getFreeHeap(), (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))
    #else
    #define DIAG_REG(stage) ((void) 0)
    #endif

    DIAG_REG("start");
    uint8_t result = HelpCommands::registerAll();
    if (result != RESULT_OK) {
        return result;
    }

    DIAG_REG("help");
    result = DongleCommands::registerAll();
    if (result != RESULT_OK) {
        return result;
    }

    DIAG_REG("dongle");
    result = EspNowCommands::registerAll();
    if (result != RESULT_OK) {
        return result;
    }

    DIAG_REG("espnow");
    result = DatabaseCommands::registerAll();
    if (result != RESULT_OK) {
        return result;
    }

    DIAG_REG("database");
    result = SudoCommands::registerAll();
    if (result != RESULT_OK) {
        return result;
    }

    DIAG_REG("sudo");
    #undef DIAG_REG
    return RESULT_OK;
}

std::string runLine(const std::string& command, const std::string& source, std::string* outFullText, const std::string& userId) {
    const std::vector<std::string> segments = splitTopLevelSemicolons(command);
    if (segments.size() > 1) {
        std::string combinedOutput;
        std::string combinedFullText;

        for (const std::string& segment : segments) {
            const std::string trimmedSegment = ShellCommandSupport::trimCopy(segment);
            if (trimmedSegment.empty()) {
                continue;
            }

            std::string segmentFullText;
            const std::string segmentOutput = runLine(trimmedSegment, source, &segmentFullText, userId);

            if (!segmentOutput.empty()) {
                if (!combinedOutput.empty()) {
                    combinedOutput += "\n";
                }
                combinedOutput += segmentOutput;
            }

            if (!combinedFullText.empty()) {
                combinedFullText += "\n";
            }
            combinedFullText += segmentFullText;
        }

        if (outFullText != nullptr) {
            *outFullText = combinedFullText;
        }
        return combinedOutput;
    }

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
        if (outFullText != nullptr) {
            *outFullText = line;
        }
        return line;
    }

    const std::string normalized = normalizeCommand(command);
    const bool isRepeatOfLastCommand = (normalized == g_lastCommandLine);
    g_lastCommandLine = normalized;
    ShellCommandSupport::resetBuffers();
    ShellCommandSupport::setCurrentUserId(userId.empty() ? source : userId);

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

    std::string combinedText = ShellCommandSupport::commandOutputBuffer();
    if (!output.empty()) {
        if (!combinedText.empty()) {
            combinedText += "\n";
        }
        combinedText += output;
    }
    if (combinedText.empty()) {
        combinedText = "(sem saida textual)";
    }

    // TINYSHELL_COLOR: TinyShell colours its own framework messages. runLine is
    // the shell-execution boundary and hands back plain text -- the BTP
    // terminal (ShellOutput::renderResponse) is the single place that
    // re-applies colour, and COMMAND_RESULT / the command_log stay escape-free.
    // Nothing adds SGR in a no-colour build, so skip the two copies then.
    if (shell_color_enabled()) {
        combinedText = shell_strip_sgr(combinedText);
        output = shell_strip_sgr(output);
    }

    if (outFullText != nullptr) {
        *outFullText = combinedText;
    }

    if (!skipCommandPersistence && !isRepeatOfLastCommand && ctx.database != nullptr && ctx.database->isReady()) {
        // The caller (BTP terminal / TraceView) still gets the full text via
        // outFullText above; only the persisted copy is capped. A bare "dongle"
        // -- or any unknown verb on a known module -- yields that module's whole
        // help dump (~1.5 KB), and there is nothing worth keeping in command_log
        // past the first few lines. Bounding it here also bounds the transient
        // footprint of the DB write, which shares the shell's call stack.
        static constexpr size_t kMaxPersistedOutput = 512;
        const std::string persistedText = (combinedText.size() > kMaxPersistedOutput)
            ? combinedText.substr(0, kMaxPersistedOutput) + "\n... (saida truncada no log)"
            : combinedText;
        if (!ctx.database->logCommandWithOutput(normalized.c_str(), persistedText.c_str(), source.c_str())) {
            ShellCommandSupport::warnWithCode(AppError::Code::DATABASE_COMMAND_LOG_FAILED, "falha ao persistir log de comando");
        }
    }

    ShellCommandSupport::resetBuffers();

    return output;
}

} // namespace ShellConfig
