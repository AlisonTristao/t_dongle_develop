#include "DongleCommands.h"

#include "SerialMux.h"
#include "ShellCommandSupport.h"
#include "SudoManager.h"
#include "error_codes.h"

#include <Esp.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <SD_MMC.h>

namespace {

using std::string;
using ShellCommandSupport::clampByte;
using ShellCommandSupport::context;
using ShellCommandSupport::currentUserId;
using ShellCommandSupport::failWithCode;
using ShellCommandSupport::parseDateTimeText;
using ShellCommandSupport::printLine;
using ShellCommandSupport::stripOuterQuotes;
using ShellCommandSupport::warnWithCode;

// Normalizes a user-supplied path into an absolute SD path (leading '/').
string normalizeSdPath(const string& rawPath) {
    string cleanPath = stripOuterQuotes(rawPath);
    if (cleanPath.empty()) {
        cleanPath = "/";
    } else if (cleanPath.front() != '/') {
        cleanPath = "/" + cleanPath;
    }
    return cleanPath;
}

uint8_t wrapper_dongle_run(string command) {
    if (context().shell == nullptr) {
        return failWithCode(AppError::Code::SHELL_NOT_READY, "shell nao configurada para executar comando local");
    }

    const string localCommand = stripOuterQuotes(command);
    context().shell->run_command_line(localCommand);
    return RESULT_OK;
}

uint8_t wrapper_dongle_ping() {
    printLine("[dongle] pong");
    return RESULT_OK;
}

uint8_t wrapper_dongle_clock() {
    const time_t nowEpoch = time(nullptr);
    if (nowEpoch <= 0) {
        printLine("[dongle] clock sem ajuste (epoch invalido)");
        return RESULT_OK;
    }

    std::tm localTime = {};
    if (localtime_r(&nowEpoch, &localTime) == nullptr) {
        return failWithCode(AppError::Code::RTC_READ_FAILED, "falha ao ler horario local");
    }

    char dateTime[32] = {0};
    std::strftime(dateTime, sizeof(dateTime), "%Y-%m-%d %H:%M:%S", &localTime);

    char line[120] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[dongle] clock=%s epoch=%lld",
        dateTime,
        static_cast<long long>(nowEpoch)
    );
    printLine(line);
    return RESULT_OK;
}

uint8_t wrapper_dongle_set_clock(string dateTimeText) {
    time_t epoch = 0;
    if (!parseDateTimeText(dateTimeText, epoch)) {
        return failWithCode(AppError::Code::CLOCK_FORMAT_INVALID, "formato invalido. Use: YYYY-MM-DD HH:MM:SS");
    }

    timeval tv = {};
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
        return failWithCode(AppError::Code::RTC_SET_FAILED, "falha ao ajustar clock");
    }

    std::tm adjusted = {};
    if (localtime_r(&epoch, &adjusted) == nullptr) {
        printLine("[dongle] clock ajustado");
        return RESULT_OK;
    }

    char dateTime[32] = {0};
    std::strftime(dateTime, sizeof(dateTime), "%Y-%m-%d %H:%M:%S", &adjusted);

    char line[120] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[dongle] clock ajustado para %s",
        dateTime
    );
    printLine(line);
    return RESULT_OK;
}

uint8_t wrapper_dongle_led(int32_t r, int32_t g, int32_t b) {
    if (context().peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando led");
    }

    context().peripherals->setLedColor(clampByte(r), clampByte(g), clampByte(b));
    printLine("[dongle] LED atualizado");
    return RESULT_OK;
}

uint8_t wrapper_dongle_led_off() {
    if (context().peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando led_off");
    }

    context().peripherals->ledOff();
    printLine("[dongle] LED desligado");
    return RESULT_OK;
}

uint8_t wrapper_dongle_lcd(string text) {
    if (context().peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando lcd");
    }

    const String content = String(stripOuterQuotes(text).c_str());

    if (context().lcdDashboard != nullptr && context().lcdDashboard->isReady()) {
        context().lcdDashboard->showMessage(content, ST77XX_BLACK);
        printLine("[dongle] texto escrito no painel LCD");
        return RESULT_OK;
    }

    const bool ok = context().peripherals->writeLcd(content, true);
    if (!ok) {
        return failWithCode(AppError::Code::LCD_NOT_READY, "LCD nao inicializado");
    }

    printLine("[dongle] texto escrito no LCD");
    return RESULT_OK;
}

uint8_t wrapper_dongle_lcd_clear() {
    if (context().peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando lcd_clear");
    }

    if (context().lcdDashboard != nullptr && context().lcdDashboard->isReady()) {
        context().lcdDashboard->clear();
        printLine("[dongle] painel LCD limpo");
        return RESULT_OK;
    }

    const bool ok = context().peripherals->clearLcd();
    if (!ok) {
        return failWithCode(AppError::Code::LCD_NOT_READY, "LCD nao inicializado");
    }

    printLine("[dongle] LCD limpo");
    return RESULT_OK;
}

uint8_t wrapper_dongle_lcd_bl(int32_t on) {
    if (context().peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando lcd_bl");
    }

    const bool turnOn = (on == 0);
    context().peripherals->setLcdBacklight(turnOn);
    printLine(turnOn ? "[dongle] backlight ligado" : "[dongle] backlight desligado");
    return RESULT_OK;
}

uint8_t wrapper_dongle_lcd_reinit() {
    if (context().peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando lcd_reinit");
    }

    const bool ok = context().peripherals->reinitLcd(context().peripherals->lcdRotation());
    if (!ok) {
        return failWithCode(AppError::Code::LCD_REINIT_FAILED, "falha ao reinicializar LCD");
    }

    if (context().lcdDashboard != nullptr) {
        context().lcdDashboard->begin(*context().peripherals);
    }

    printLine("[dongle] LCD reinicializado");
    return RESULT_OK;
}

uint8_t wrapper_dongle_lcd_rot(int32_t rotation) {
    if (context().peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando lcd_rot");
    }

    const int32_t normalized = ((rotation % 4) + 4) % 4;
    context().peripherals->setLcdRotation(static_cast<uint8_t>(normalized));

    if (context().lcdDashboard != nullptr) {
        context().lcdDashboard->begin(*context().peripherals);
    }

    char line[80] = {0};
    std::snprintf(line, sizeof(line), "[dongle] rotacao LCD = %ld", static_cast<long>(normalized));
    printLine(line);
    return RESULT_OK;
}

uint8_t wrapper_dongle_lcd_rot_get() {
    if (context().peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando lcd_rot_get");
    }

    char line[80] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[dongle] rotacao LCD atual = %u",
        static_cast<unsigned>(context().peripherals->lcdRotation())
    );
    printLine(line);
    return RESULT_OK;
}

uint8_t wrapper_dongle_lcd_bl_inv(int32_t activeHigh) {
    if (context().peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando lcd_bl_inv");
    }

    context().peripherals->setLcdBacklightPolarity(activeHigh != 0);
    if (activeHigh != 0) {
        printLine("[dongle] polaridade backlight: HIGH=ON");
    } else {
        printLine("[dongle] polaridade backlight: LOW=ON");
    }
    return RESULT_OK;
}

uint8_t wrapper_dongle_sd_init() {
    if (context().peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando sd_init");
    }

    const bool ok = context().peripherals->beginSd(false);
    if (!ok) {
        return failWithCode(AppError::Code::SD_INIT_FAILED, "falha ao iniciar SD (verifique cartao, contato e pull-ups)");
    }

    char line[120] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[dongle] SD inicializado (%u-bit @ %lukHz)",
        context().peripherals->sdOneBitMode() ? 1U : 4U,
        static_cast<unsigned long>(context().peripherals->sdFrequencyKHz())
    );
    printLine(line);
    return RESULT_OK;
}

uint8_t wrapper_dongle_sd_status() {
    if (context().peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando sd_status");
    }

    if (!context().peripherals->isSdReady()) {
        printLine("[dongle] SD nao inicializado");
        return RESULT_OK;
    }

    const String type = context().peripherals->sdCardTypeName();
    const uint64_t totalBytes = context().peripherals->sdTotalBytes();
    const uint64_t usedBytes = context().peripherals->sdUsedBytes();
    const uint64_t totalMB = context().peripherals->sdTotalMB();
    const uint64_t usedMB = context().peripherals->sdUsedMB();

    uint64_t percentInt = 0;
    uint64_t percentFrac = 0;
    if (totalBytes > 0) {
        const uint64_t percent100 = (usedBytes * 10000ULL) / totalBytes;
        percentInt = percent100 / 100ULL;
        percentFrac = percent100 % 100ULL;
    }

    const bool dbExists = SD_MMC.exists("/database/dongle.db");
    uint64_t dbBytes = 0;
    if (dbExists) {
        File dbFile = SD_MMC.open("/database/dongle.db", FILE_READ);
        if (dbFile) {
            dbBytes = static_cast<uint64_t>(dbFile.size());
            dbFile.close();
        }
    }

    char line[280] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[dongle] SD %s %u-bit@%lukHz total=%lluMB(%lluB) usado=%lluMB(%lluB) uso=%llu.%02llu%% db=%s(%lluB)",
        type.c_str(),
        context().peripherals->sdOneBitMode() ? 1U : 4U,
        static_cast<unsigned long>(context().peripherals->sdFrequencyKHz()),
        static_cast<unsigned long long>(totalMB),
        static_cast<unsigned long long>(totalBytes),
        static_cast<unsigned long long>(usedMB),
        static_cast<unsigned long long>(usedBytes),
        static_cast<unsigned long long>(percentInt),
        static_cast<unsigned long long>(percentFrac),
        dbExists ? "presente" : "ausente",
        static_cast<unsigned long long>(dbBytes)
    );
    printLine(line);
    return RESULT_OK;
}

uint8_t wrapper_dongle_sd_wipe() {
    if (!SudoManager::isElevated(currentUserId())) {
        return failWithCode(AppError::Code::PERMISSION_DENIED, "isso apaga TUDO do cartao SD, banco de dados incluso. Rode antes: sudo -login <senha>");
    }

    if (context().peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando sd_wipe");
    }

    if (context().espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para rebuild do banco");
    }

    if (!context().peripherals->isSdReady()) {
        return failWithCode(AppError::Code::SD_NOT_READY, "SD nao inicializado");
    }

    if (context().database != nullptr && context().database->isReady()) {
        context().database->end();
    }

    const bool wipeOk = context().peripherals->wipeSdContents();
    if (!wipeOk) {
        return failWithCode(AppError::Code::SD_WIPE_FAILED, "falha ao apagar conteudo do SD");
    }

    const bool sdReinitOk = context().peripherals->beginSd(false);
    if (!sdReinitOk) {
        return failWithCode(AppError::Code::SD_REINIT_FAILED, "SD limpo, mas falhou reinit");
    }

    if (context().database != nullptr) {
        if (context().database->begin(*context().espNow, context().io)) {
            if (!context().database->syncPeersFromManager(*context().espNow)) {
                warnWithCode(AppError::Code::DATABASE_PEER_SYNC_FAILED, "database recriado, mas falhou ao sincronizar peers");
            }
            printLine("[dongle] SD limpo e database recriado");
            return RESULT_OK;
        }

        return failWithCode(AppError::Code::SD_DB_RECREATE_FAILED, "SD limpo, mas falhou ao recriar database");
    }

    printLine("[dongle] SD limpo");
    return RESULT_OK;
}

uint8_t wrapper_dongle_sd_ls(string path = "/") {
    if (context().peripherals == nullptr || !context().peripherals->isSdReady()) {
        return failWithCode(AppError::Code::SD_NOT_READY, "SD nao inicializado");
    }

    const string cleanPath = normalizeSdPath(path);
    File dir = SD_MMC.open(cleanPath.c_str());
    if (!dir || !dir.isDirectory()) {
        if (dir) {
            dir.close();
        }
        return failWithCode(AppError::Code::SD_PATH_NOT_FOUND, "diretorio nao encontrado: " + cleanPath);
    }

    printLine("[sd] listando " + cleanPath);
    size_t count = 0;
    File entry = dir.openNextFile();
    while (entry) {
        char line[160] = {0};
        std::snprintf(
            line,
            sizeof(line),
            "%s %s (%lluB)",
            entry.isDirectory() ? "[dir]" : "[file]",
            entry.name(),
            static_cast<unsigned long long>(entry.isDirectory() ? 0ULL : entry.size())
        );
        printLine(line);
        ++count;
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    if (count == 0) {
        printLine("[sd] (vazio)");
    }
    return RESULT_OK;
}

uint8_t wrapper_dongle_sd_cat(string path) {
    if (context().peripherals == nullptr || !context().peripherals->isSdReady()) {
        return failWithCode(AppError::Code::SD_NOT_READY, "SD nao inicializado");
    }

    const string cleanPath = normalizeSdPath(path);
    File file = SD_MMC.open(cleanPath.c_str(), FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) {
            file.close();
        }
        return failWithCode(AppError::Code::SD_PATH_NOT_FOUND, "arquivo nao encontrado: " + cleanPath);
    }

    char header[96] = {0};
    std::snprintf(header, sizeof(header), "[sd] %s (%lluB)", cleanPath.c_str(), static_cast<unsigned long long>(file.size()));
    printLine(header);

    constexpr size_t kMaxPrintBytes = 4096; // safety cap so a huge file doesn't flood the terminal
    size_t printed = 0;
    bool truncated = false;
    String lineBuffer;
    while (file.available()) {
        if (printed >= kMaxPrintBytes) {
            truncated = true;
            break;
        }

        const char ch = static_cast<char>(file.read());
        ++printed;
        if (ch == '\n') {
            printLine(std::string(lineBuffer.c_str()));
            lineBuffer = "";
        } else if (ch != '\r') {
            lineBuffer += ch;
        }
    }
    file.close();

    if (lineBuffer.length() > 0) {
        printLine(std::string(lineBuffer.c_str()));
    }
    if (truncated) {
        printLine("[sd] ... truncado (mostrando so os primeiros 4KB) ...");
    }
    return RESULT_OK;
}

uint8_t wrapper_dongle_sd_rm(string path) {
    if (context().peripherals == nullptr || !context().peripherals->isSdReady()) {
        return failWithCode(AppError::Code::SD_NOT_READY, "SD nao inicializado");
    }

    const string cleanPath = normalizeSdPath(path);
    if (!SD_MMC.exists(cleanPath.c_str())) {
        return failWithCode(AppError::Code::SD_PATH_NOT_FOUND, "arquivo nao encontrado: " + cleanPath);
    }

    File check = SD_MMC.open(cleanPath.c_str());
    const bool isDirectory = check && check.isDirectory();
    if (check) {
        check.close();
    }
    if (isDirectory) {
        return failWithCode(AppError::Code::SD_PATH_IS_DIRECTORY, "sd_rm so remove arquivos; use dongle -sd_wipe para limpar tudo");
    }

    if (!SD_MMC.remove(cleanPath.c_str())) {
        return failWithCode(AppError::Code::SD_FILE_REMOVE_FAILED, "falha ao remover " + cleanPath);
    }

    printLine("[sd] removido: " + cleanPath);
    return RESULT_OK;
}

uint8_t wrapper_dongle_sd_mkdir(string path) {
    if (context().peripherals == nullptr || !context().peripherals->isSdReady()) {
        return failWithCode(AppError::Code::SD_NOT_READY, "SD nao inicializado");
    }

    const string cleanPath = normalizeSdPath(path);
    if (!SD_MMC.mkdir(cleanPath.c_str())) {
        return failWithCode(AppError::Code::SD_MKDIR_FAILED, "falha ao criar diretorio " + cleanPath);
    }

    printLine("[sd] diretorio criado: " + cleanPath);
    return RESULT_OK;
}

uint8_t writeToSdFile(const string& pathArg, const string& textArg, bool append) {
    if (context().peripherals == nullptr || !context().peripherals->isSdReady()) {
        return failWithCode(AppError::Code::SD_NOT_READY, "SD nao inicializado");
    }

    const string cleanPath = normalizeSdPath(pathArg);
    const string text = stripOuterQuotes(textArg);

    File file = SD_MMC.open(cleanPath.c_str(), append ? FILE_APPEND : FILE_WRITE);
    if (!file) {
        return failWithCode(AppError::Code::SD_FILE_WRITE_FAILED, "falha ao abrir " + cleanPath + " para escrita");
    }

    file.print(text.c_str());
    file.print("\n");
    file.close();

    printLine(string("[sd] ") + (append ? "anexado em " : "escrito em ") + cleanPath);
    return RESULT_OK;
}

uint8_t wrapper_dongle_sd_write(string path, string text) {
    return writeToSdFile(path, text, false);
}

uint8_t wrapper_dongle_sd_append(string path, string text) {
    return writeToSdFile(path, text, true);
}

uint8_t wrapper_dongle_history(int32_t limit = 20) {
    if (context().database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando history");
    }

    const size_t boundedLimit = (limit > 0) ? static_cast<size_t>(limit) : 20U;
    String output;
    const bool ok = context().database->readRecentCommands(boundedLimit, output);
    printLine(output.length() > 0 ? std::string(output.c_str()) : std::string("(sem historico)"));
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_QUERY_FAILED, "falha ao ler historico");
    }

    return RESULT_OK;
}

uint8_t wrapper_dongle_run_script(string path) {
    if (context().peripherals == nullptr || !context().peripherals->isSdReady()) {
        return failWithCode(AppError::Code::SD_NOT_READY, "SD nao inicializado");
    }

    if (context().shell == nullptr) {
        return failWithCode(AppError::Code::SHELL_NOT_READY, "shell nao configurada para rodar script");
    }

    const string cleanPath = normalizeSdPath(path);
    File file = SD_MMC.open(cleanPath.c_str(), FILE_READ);
    if (!file || file.isDirectory()) {
        if (file) {
            file.close();
        }
        return failWithCode(AppError::Code::SD_PATH_NOT_FOUND, "script nao encontrado: " + cleanPath);
    }

    printLine("[dongle] executando script: " + cleanPath);

    size_t executedCount = 0;
    size_t skippedCount = 0;
    while (file.available()) {
        String rawLine = file.readStringUntil('\n');
        rawLine.trim();

        if (rawLine.length() == 0 || rawLine.startsWith("#")) {
            ++skippedCount;
            continue;
        }

        context().shell->run_command_line(std::string(rawLine.c_str()));
        ++executedCount;
    }
    file.close();

    char summary[96] = {0};
    std::snprintf(
        summary,
        sizeof(summary),
        "[dongle] script concluido: %u comando(s), %u linha(s) ignorada(s)",
        static_cast<unsigned>(executedCount),
        static_cast<unsigned>(skippedCount)
    );
    printLine(summary);
    return RESULT_OK;
}

uint8_t wrapper_dongle_reboot() {
    printLine("[dongle] reiniciando...");
    delay(200); // give the serial time to flush the message before reset
    ESP.restart();
    return RESULT_OK; // unreachable
}

uint8_t wrapper_dongle_info() {
    char line[220] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[dongle] chip=%s rev=%d cores=%d heap_livre=%uB flash=%uMB uptime=%lus mac=%s",
        ESP.getChipModel(),
        static_cast<int>(ESP.getChipRevision()),
        static_cast<int>(ESP.getChipCores()),
        static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(ESP.getFlashChipSize() / (1024UL * 1024UL)),
        static_cast<unsigned long>(millis() / 1000UL),
        WiFi.macAddress().c_str()
    );
    printLine(line);
    return RESULT_OK;
}

// PASSO 2 (topico 13): manual affordance to negotiate a BTP v1 protocolled
// session on this same port, for a human at a terminal emulator who does not
// want to hand-type the raw "BTP/1 ENTER <16 hex>\r\n" wire line -- an
// automatic client (TraceView) still uses that raw line directly, recognized
// straight off ShellSerial's input (see SerialMux::tryEnterFromConsoleLine,
// called from AppRuntime::handleShellInput). Prints its confirmation BEFORE
// calling SerialMux::enterFromCommand(), never after: once that call
// succeeds, "BTP/1 READY ...\r\n" is already on the wire and no further
// plain-text output may follow it (bally_protocol/docs/TRANSPORT_SERIAL.md
// section 7 -- "impedir qualquer saida textual entre READY e BTP/1 CONSOLE").
uint8_t wrapper_dongle_btp_v1() {
    printLine("[dongle] negociando sessao BTP v1 (aguardando HELLO por 2s)...");

    if (!SerialMux::enterFromCommand(millis())) {
        return failWithCode(AppError::Code::SERIAL_SESSION_BUSY, "sessao serial ja esta negociando ou protocolada");
    }

    return RESULT_OK;
}

} // namespace

namespace DongleCommands {

uint8_t registerAll() {
    if (context().shell == nullptr) {
        return failWithCode(AppError::Code::SHELL_NOT_READY, "shell nao configurada para registrar modulo dongle");
    }

    context().shell->create_module("dongle", "commands executed locally on this ESP");

    context().shell->add(wrapper_dongle_ping, "ping", "quick local test", "dongle");
    context().shell->add(wrapper_dongle_clock, "clock", "show current RTC time", "dongle");
    context().shell->add(wrapper_dongle_set_clock, "set_clock", "set RTC: <\"YYYY-MM-DD HH:MM:SS\">", "dongle");
    context().shell->add(wrapper_dongle_run, "run", "execute a command locally: <command>", "dongle");
    context().shell->add(wrapper_dongle_led, "led", "set RGB LED: <r>, <g>, <b>", "dongle");
    context().shell->add(wrapper_dongle_led_off, "led_off", "turn off LED", "dongle");
    context().shell->add(wrapper_dongle_lcd, "lcd", "write text to LCD terminal: <text>", "dongle");
    context().shell->add(wrapper_dongle_lcd_clear, "lcd_clear", "clear LCD terminal", "dongle");
    context().shell->add(wrapper_dongle_lcd_rot, "lcd_rot", "LCD rotation: <0|1|2|3>", "dongle");
    context().shell->add(wrapper_dongle_lcd_rot_get, "lcd_rot_get", "show current LCD rotation", "dongle");
    context().shell->add(wrapper_dongle_lcd_bl, "lcd_bl", "LCD backlight: <0=ON|1=OFF>", "dongle");
    context().shell->add(wrapper_dongle_lcd_bl_inv, "lcd_bl_inv", "backlight polarity: <1=HIGH_ON|0=LOW_ON>", "dongle");
    context().shell->add(wrapper_dongle_lcd_reinit, "lcd_reinit", "reinitialize LCD", "dongle");
    context().shell->add(wrapper_dongle_sd_init, "sd_init", "initialize SD", "dongle");
    context().shell->add(wrapper_dongle_sd_status, "sd_status", "show SD status", "dongle");
    context().shell->add(wrapper_dongle_sd_wipe, "sd_wipe", "wipe all SD content (requires sudo -login)", "dongle");
    context().shell->add(wrapper_dongle_sd_ls, "sd_ls", "list files/dirs: <path>", "dongle");
    context().shell->add(wrapper_dongle_sd_cat, "sd_cat", "print a text file: <path>", "dongle");
    context().shell->add(wrapper_dongle_sd_rm, "sd_rm", "remove one file: <path>", "dongle");
    context().shell->add(wrapper_dongle_sd_mkdir, "sd_mkdir", "create a directory: <path>", "dongle");
    context().shell->add(wrapper_dongle_sd_write, "sd_write", "overwrite a file: <path>, <text>", "dongle");
    context().shell->add(wrapper_dongle_sd_append, "sd_append", "append to a file: <path>, <text>", "dongle");
    context().shell->add(wrapper_dongle_history, "history", "reprint recent serial commands: <limit>", "dongle");
    context().shell->add(wrapper_dongle_run_script, "run_script", "run one command per line from a SD file: <path>", "dongle");
    context().shell->add(wrapper_dongle_reboot, "reboot", "restart the ESP32", "dongle");
    context().shell->add(wrapper_dongle_info, "info", "chip/heap/flash/uptime/mac summary", "dongle");
    context().shell->add(wrapper_dongle_btp_v1, "btp_v1", "negotiate a BTP v1 protocolled session on this port", "dongle");

    return RESULT_OK;
}

} // namespace DongleCommands
