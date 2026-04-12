#include "shell_config.h"
#include "espnow_config.h"
#include "error_codes.h"
#include "shell_output.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <sys/time.h>
#include <Esp.h>
#include <SD_MMC.h>

namespace {

using std::string;

ShellConfig::Context g_ctx = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
string g_commandOutputBuffer;
constexpr uint8_t kFallbackBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

string trimCopy(const string& text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });

    if (first == text.end()) {
        return "";
    }

    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();

    return string(first, last);
}

string stripOuterQuotes(const string& text) {
    string s = trimCopy(text);
    if (s.length() >= 2) {
        const bool doubleQuoted = (s.front() == '"' && s.back() == '"');
        const bool singleQuoted = (s.front() == '\'' && s.back() == '\'');
        if (doubleQuoted || singleQuoted) {
            s = s.substr(1, s.length() - 2);
        }
    }
    return s;
}

string normalizeOutputTag(const string& text) {
    string s = trimCopy(text);
    if (s.empty()) {
        return s;
    }

    if (s.front() == '[') {
        return s;
    }

    return string("[shell] ") + s;
}

bool parseMacAddress(const string& text, uint8_t outMac[6]) {
    unsigned int bytes[6] = {0, 0, 0, 0, 0, 0};
    const string clean = stripOuterQuotes(text);

    if (std::sscanf(
            clean.c_str(),
            "%02x:%02x:%02x:%02x:%02x:%02x",
            &bytes[0], &bytes[1], &bytes[2],
            &bytes[3], &bytes[4], &bytes[5]
        ) != 6) {
        return false;
    }

    for (size_t i = 0; i < 6; ++i) {
        outMac[i] = static_cast<uint8_t>(bytes[i]);
    }
    return true;
}

bool parseDateTimeText(const string& text, time_t& outEpoch) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    const string clean = stripOuterQuotes(text);
    if (std::sscanf(clean.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
        return false;
    }

    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }

    std::tm tmValue = {};
    tmValue.tm_year = year - 1900;
    tmValue.tm_mon = month - 1;
    tmValue.tm_mday = day;
    tmValue.tm_hour = hour;
    tmValue.tm_min = minute;
    tmValue.tm_sec = second;

    const time_t epoch = mktime(&tmValue);
    if (epoch <= 0) {
        return false;
    }

    outEpoch = epoch;
    return true;
}

uint16_t lcdColorForLine(const string& text) {
    // This panel is currently running with inverted visual polarity,
    // so semantic colors are compensated before drawing.
    constexpr bool kPanelInvertedColors = true;
    constexpr bool kPanelSwapRedBlue = true;
    const auto toPanelColor = [](uint16_t desiredColor) -> uint16_t {
        auto swapRedBlue565 = [](uint16_t color) -> uint16_t {
            const uint16_t r = static_cast<uint16_t>((color >> 11) & 0x1F);
            const uint16_t g = static_cast<uint16_t>((color >> 5) & 0x3F);
            const uint16_t b = static_cast<uint16_t>(color & 0x1F);
            return static_cast<uint16_t>((b << 11) | (g << 5) | r);
        };

        uint16_t panelColor = desiredColor;
        if (kPanelInvertedColors) {
            panelColor = static_cast<uint16_t>(~panelColor);
        }
        if (kPanelSwapRedBlue) {
            panelColor = swapRedBlue565(panelColor);
        }

        return panelColor;
    };

    string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    const auto hasAny = [&lower](std::initializer_list<const char*> terms) {
        for (const char* term : terms) {
            if (lower.find(term) != string::npos) {
                return true;
            }
        }
        return false;
    };

    if (hasAny({"erro", "error", "falha", "failed", "invalid", "invalido"})) {
        return toPanelColor(ST77XX_RED);
    }

    if (hasAny({"warning", "warn", "aviso", "atencao"}) || lower.rfind("[help]", 0) == 0) {
        return toPanelColor(ST77XX_YELLOW);
    }

    if (hasAny({
            "sucesso",
            "success",
            "inicializado",
            "atualizado",
            "enviado",
            "adicionado",
            "removido",
            "ligado",
            "desligado",
            "limpo",
            "pong"
        })) {
        return toPanelColor(ST77XX_GREEN);
    }

    return toPanelColor(ST77XX_WHITE);
}

void printLine(const string& text) {
    const string normalized = normalizeOutputTag(text);

    g_commandOutputBuffer += normalized;
    g_commandOutputBuffer += "\n";

    if (g_ctx.io != nullptr) {
        ShellOutput::writeLine(*g_ctx.io, normalized.c_str());
    }

    if (g_ctx.lcdTerminal != nullptr && g_ctx.lcdTerminal->isReady()) {
        g_ctx.lcdTerminal->writeText(String(normalized.c_str()), lcdColorForLine(normalized));
    }
}

string normalizeCommand(const string& command) {
    const string input = trimCopy(command);
    const string prefix = "espnow -send_to ";

    if (input.rfind(prefix, 0) == 0) {
        const string args = trimCopy(input.substr(prefix.length()));
        if (!args.empty() && args.find(',') == string::npos) {
            return "espnow -send_to 000, " + args;
        }
    }

    return input;
}

uint8_t clampByte(int32_t value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

void resolveDefaultBroadcastMac(uint8_t outMac[6]) {
    if (outMac == nullptr) {
        return;
    }

    memcpy(outMac, kFallbackBroadcastMac, 6);
    if (g_ctx.database != nullptr && g_ctx.database->isReady()) {
        uint8_t dbMac[6] = {0};
        if (g_ctx.database->getDefaultBroadcastMac(dbMac)) {
            memcpy(outMac, dbMac, 6);
        }
    }
}

uint8_t failWithCode(AppError::Code code, const string& detail) {
    char line[320] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "erro(%u/%s) %s",
        static_cast<unsigned>(AppError::value(code)),
        AppError::name(code),
        detail.c_str()
    );
    printLine(line);
    return RESULT_ERROR;
}

void warnWithCode(AppError::Code code, const string& detail) {
    char line[320] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "aviso(%u/%s) %s",
        static_cast<unsigned>(AppError::value(code)),
        AppError::name(code),
        detail.c_str()
    );
    printLine(line);
}

uint8_t wrapper_espnow_send_all(string command);

uint8_t wrapper_help_h() {
    if (g_ctx.shell == nullptr) {
        return failWithCode(AppError::Code::SHELL_NOT_READY, "shell nao configurada para help -h");
    }

    printLine(g_ctx.shell->get_help(""));
    return RESULT_OK;
}

uint8_t wrapper_help_l(string module = "") {
    if (g_ctx.shell == nullptr) {
        return failWithCode(AppError::Code::SHELL_NOT_READY, "shell nao configurada para help -l");
    }

    printLine(g_ctx.shell->get_help(module));
    return RESULT_OK;
}

uint8_t wrapper_help_e() {
    printLine("[help] comandos gerais");
    printLine("Uso: <module> -<command> [args]");
    printLine("Horario RTC local: dongle -clock");
    printLine("Ajustar RTC local: dongle -set_clock \"YYYY-MM-DD HH:MM:SS\"");
    printLine("Exemplo local LCD: dongle -lcd \"Ola dongle\"");
    printLine("Se LCD nao aparecer: dongle -lcd_bl 1 e depois dongle -lcd_reinit");
    printLine("Se LCD estiver invertido: dongle -lcd_rot 0..3");
    printLine("Exemplo local LED: dongle -led 255, 0, 0");
    printLine("Exemplo espnow unicast: espnow -send_to 1, \"dongle -run status\"");
    printLine("Exemplo espnow broadcast: espnow -send_to 000, \"dongle -run status\"");
    printLine("Status buffer RX->DB: espnow -flush_status");
    printLine("Flush manual RX->DB (modo high frequency): espnow -flush_db [limite]");
    printLine("Alias 000 no send_to = envia para todos os peers");
    printLine("Editar peer: espnow -update 1, \"nome novo\", \"descricao nova\"");
    printLine("Banco sqlite no SD: database -status | database -tables | database -read peers, 20");
    printLine("Logs comando+saida: database -logs 20");
    printLine("Historico ESP-NOW RX/TX: database -espnow_history 30");
    return RESULT_OK;
}

uint8_t wrapper_dongle_run(string command) {
    const string localCommand = stripOuterQuotes(command);
    printLine("[dongle] comando local: " + localCommand);
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
    if (g_ctx.peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando led");
    }

    g_ctx.peripherals->setLedColor(clampByte(r), clampByte(g), clampByte(b));
    printLine("[dongle] LED atualizado");
    return RESULT_OK;
}

uint8_t wrapper_dongle_led_off() {
    if (g_ctx.peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando led_off");
    }

    g_ctx.peripherals->ledOff();
    printLine("[dongle] LED desligado");
    return RESULT_OK;
}

uint8_t wrapper_dongle_lcd(string text) {
    if (g_ctx.peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando lcd");
    }

    const String content = String(stripOuterQuotes(text).c_str());

    if (g_ctx.lcdTerminal != nullptr && g_ctx.lcdTerminal->isReady()) {
        g_ctx.lcdTerminal->writeText(content, ST77XX_BLACK);
        printLine("[dongle] texto escrito no terminal LCD");
        return RESULT_OK;
    }

    const bool ok = g_ctx.peripherals->writeLcd(content, true);
    if (!ok) {
        return failWithCode(AppError::Code::LCD_NOT_READY, "LCD nao inicializado");
    }

    printLine("[dongle] texto escrito no LCD");
    return RESULT_OK;
}

uint8_t wrapper_dongle_lcd_clear() {
    if (g_ctx.peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando lcd_clear");
    }

    if (g_ctx.lcdTerminal != nullptr && g_ctx.lcdTerminal->isReady()) {
        g_ctx.lcdTerminal->clear();
        printLine("[dongle] terminal LCD limpo");
        return RESULT_OK;
    }

    const bool ok = g_ctx.peripherals->clearLcd();
    if (!ok) {
        return failWithCode(AppError::Code::LCD_NOT_READY, "LCD nao inicializado");
    }

    printLine("[dongle] LCD limpo");
    return RESULT_OK;
}

uint8_t wrapper_dongle_lcd_bl(int32_t on) {
    if (g_ctx.peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando lcd_bl");
    }

    const bool turnOn = (on == 0);
    g_ctx.peripherals->setLcdBacklight(turnOn);
    printLine(turnOn ? "[dongle] backlight ligado" : "[dongle] backlight desligado");
    return RESULT_OK;
}

uint8_t wrapper_dongle_lcd_reinit() {
    if (g_ctx.peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando lcd_reinit");
    }

    const bool ok = g_ctx.peripherals->reinitLcd(g_ctx.peripherals->lcdRotation());
    if (!ok) {
        return failWithCode(AppError::Code::LCD_REINIT_FAILED, "falha ao reinicializar LCD");
    }

    if (g_ctx.lcdTerminal != nullptr) {
        g_ctx.lcdTerminal->begin(*g_ctx.peripherals);
    }

    printLine("[dongle] LCD reinicializado");
    return RESULT_OK;
}

uint8_t wrapper_dongle_lcd_rot(int32_t rotation) {
    if (g_ctx.peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando lcd_rot");
    }

    const int32_t normalized = ((rotation % 4) + 4) % 4;
    g_ctx.peripherals->setLcdRotation(static_cast<uint8_t>(normalized));

    if (g_ctx.lcdTerminal != nullptr) {
        g_ctx.lcdTerminal->begin(*g_ctx.peripherals);
    }

    char line[80] = {0};
    std::snprintf(line, sizeof(line), "[dongle] rotacao LCD = %ld", static_cast<long>(normalized));
    printLine(line);
    return RESULT_OK;
}

uint8_t wrapper_dongle_lcd_rot_get() {
    if (g_ctx.peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando lcd_rot_get");
    }

    char line[80] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[dongle] rotacao LCD atual = %u",
        static_cast<unsigned>(g_ctx.peripherals->lcdRotation())
    );
    printLine(line);
    return RESULT_OK;
}

uint8_t wrapper_dongle_lcd_bl_inv(int32_t activeHigh) {
    if (g_ctx.peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando lcd_bl_inv");
    }

    g_ctx.peripherals->setLcdBacklightPolarity(activeHigh != 0);
    if (activeHigh != 0) {
        printLine("[dongle] polaridade backlight: HIGH=ON");
    } else {
        printLine("[dongle] polaridade backlight: LOW=ON");
    }
    return RESULT_OK;
}

uint8_t wrapper_dongle_sd_init() {
    if (g_ctx.peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando sd_init");
    }

    const bool ok = g_ctx.peripherals->beginSd(false);
    if (!ok) {
        return failWithCode(AppError::Code::SD_INIT_FAILED, "falha ao iniciar SD (verifique cartao, contato e pull-ups)");
    }

    char line[120] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[dongle] SD inicializado (%u-bit @ %lukHz)",
        g_ctx.peripherals->sdOneBitMode() ? 1U : 4U,
        static_cast<unsigned long>(g_ctx.peripherals->sdFrequencyKHz())
    );
    printLine(line);
    return RESULT_OK;
}

uint8_t wrapper_dongle_sd_status() {
    if (g_ctx.peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando sd_status");
    }

    if (!g_ctx.peripherals->isSdReady()) {
        printLine("[dongle] SD nao inicializado");
        return RESULT_OK;
    }

    const String type = g_ctx.peripherals->sdCardTypeName();
    const uint64_t totalBytes = g_ctx.peripherals->sdTotalBytes();
    const uint64_t usedBytes = g_ctx.peripherals->sdUsedBytes();
    const uint64_t totalMB = g_ctx.peripherals->sdTotalMB();
    const uint64_t usedMB = g_ctx.peripherals->sdUsedMB();

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
        g_ctx.peripherals->sdOneBitMode() ? 1U : 4U,
        static_cast<unsigned long>(g_ctx.peripherals->sdFrequencyKHz()),
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
    if (g_ctx.peripherals == nullptr) {
        return failWithCode(AppError::Code::PERIPHERALS_NOT_READY, "perifericos indisponiveis para comando sd_wipe");
    }

    if (g_ctx.espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para rebuild do banco");
    }

    if (!g_ctx.peripherals->isSdReady()) {
        return failWithCode(AppError::Code::SD_NOT_READY, "SD nao inicializado");
    }

    if (g_ctx.database != nullptr && g_ctx.database->isReady()) {
        g_ctx.database->end();
    }

    const bool wipeOk = g_ctx.peripherals->wipeSdContents();
    if (!wipeOk) {
        return failWithCode(AppError::Code::SD_WIPE_FAILED, "falha ao apagar conteudo do SD");
    }

    const bool sdReinitOk = g_ctx.peripherals->beginSd(false);
    if (!sdReinitOk) {
        return failWithCode(AppError::Code::SD_REINIT_FAILED, "SD limpo, mas falhou reinit");
    }

    if (g_ctx.database != nullptr) {
        if (g_ctx.database->begin(*g_ctx.espNow, g_ctx.io)) {
            if (!g_ctx.database->syncPeersFromManager(*g_ctx.espNow)) {
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

uint8_t wrapper_espnow_list() {
    if (g_ctx.espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando list");
    }

    printLine("[000] todos - FF:FF:FF:FF:FF:FF (alias broadcast)");

    const size_t total = g_ctx.espNow->deviceCount();
    if (total == 0) {
        printLine("[espnow] nenhum peer real cadastrado");
        return RESULT_OK;
    }

    for (size_t i = 0; i < total; ++i) {
        EspNowManager::deviceInfo item = {};
        if (!g_ctx.espNow->deviceAt(i, item)) {
            continue;
        }

        char macText[18] = {0};
        std::snprintf(
            macText,
            sizeof(macText),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            item.mac[0], item.mac[1], item.mac[2],
            item.mac[3], item.mac[4], item.mac[5]
        );

        char line[256] = {0};
        std::snprintf(
            line,
            sizeof(line),
            "[%03u] %s - %s - %s",
            static_cast<unsigned>(i + 1),
            item.name,
            item.description,
            macText
        );
        printLine(line);
    }

    return RESULT_OK;
}

uint8_t wrapper_espnow_add(string macText, string name, string description) {
    if (g_ctx.espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando add");
    }

    uint8_t mac[6] = {0};
    if (!parseMacAddress(macText, mac)) {
        return failWithCode(AppError::Code::INVALID_MAC_FORMAT, "MAC invalido. Use formato AA:BB:CC:DD:EE:FF");
    }

    const bool ok = g_ctx.espNow->addDevice(
        mac,
        stripOuterQuotes(name).c_str(),
        stripOuterQuotes(description).c_str()
    );

    if (!ok) {
        return failWithCode(AppError::Code::PEER_ADD_FAILED, "falha ao adicionar dispositivo");
    }

    if (g_ctx.database != nullptr && g_ctx.database->isReady()) {
        if (!g_ctx.database->upsertPeer(mac, stripOuterQuotes(name).c_str(), stripOuterQuotes(description).c_str())) {
            warnWithCode(AppError::Code::DATABASE_PEER_NOT_PERSISTED, "peer adicionado, mas nao persistido");
        }
    }

    printLine("[espnow] dispositivo adicionado");
    return RESULT_OK;
}

uint8_t wrapper_espnow_remove(int32_t deviceNumber) {
    if (g_ctx.espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando remove");
    }

    if (deviceNumber <= 0) {
        return failWithCode(AppError::Code::INVALID_DEVICE_INDEX, "indice invalido. Use valores >= 1");
    }

    const size_t index = static_cast<size_t>(deviceNumber - 1);
    EspNowManager::deviceInfo removed = {};
    const bool hadDevice = g_ctx.espNow->deviceAt(index, removed);

    const bool ok = g_ctx.espNow->removeDeviceByIndex(index);
    if (!ok) {
        return failWithCode(AppError::Code::PEER_REMOVE_FAILED, "indice invalido");
    }

    if (hadDevice && g_ctx.database != nullptr && g_ctx.database->isReady()) {
        if (!g_ctx.database->removePeer(removed.mac)) {
            warnWithCode(AppError::Code::DATABASE_PEER_REMOVE_NOT_PERSISTED, "peer removido, mas persistencia nao atualizada");
        }
    }

    printLine("[espnow] dispositivo removido");
    return RESULT_OK;
}

uint8_t wrapper_espnow_remove_mac(string macText) {
    if (g_ctx.espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando remove_mac");
    }

    uint8_t mac[6] = {0};
    if (!parseMacAddress(macText, mac)) {
        return failWithCode(AppError::Code::INVALID_MAC_FORMAT, "MAC invalido. Use formato AA:BB:CC:DD:EE:FF");
    }

    const bool ok = g_ctx.espNow->removeDeviceByMac(mac);
    if (!ok) {
        return failWithCode(AppError::Code::PEER_NOT_FOUND, "MAC nao encontrado");
    }

    if (g_ctx.database != nullptr && g_ctx.database->isReady()) {
        if (!g_ctx.database->removePeer(mac)) {
            warnWithCode(AppError::Code::DATABASE_PEER_REMOVE_NOT_PERSISTED, "peer removido, mas persistencia nao atualizada");
        }
    }

    printLine("[espnow] dispositivo removido por MAC");
    return RESULT_OK;
}

uint8_t wrapper_espnow_update(int32_t deviceNumber, string name, string description) {
    if (g_ctx.espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando update");
    }

    if (deviceNumber <= 0) {
        return failWithCode(AppError::Code::INVALID_DEVICE_INDEX, "indice invalido. Use valores >= 1");
    }

    const size_t index = static_cast<size_t>(deviceNumber - 1);
    const string safeName = stripOuterQuotes(name);
    const string safeDescription = stripOuterQuotes(description);

    const bool ok = g_ctx.espNow->updateDeviceByIndex(index, safeName.c_str(), safeDescription.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::PEER_UPDATE_FAILED, "indice invalido para update");
    }

    EspNowManager::deviceInfo updated = {};
    if (g_ctx.espNow->deviceAt(index, updated) && g_ctx.database != nullptr && g_ctx.database->isReady()) {
        if (!g_ctx.database->updatePeerMetadata(updated.mac, safeName.c_str(), safeDescription.c_str())) {
            warnWithCode(AppError::Code::DATABASE_PEER_UPDATE_NOT_PERSISTED, "peer atualizado em memoria, mas nao persistido");
        }
    }

    printLine("[espnow] peer atualizado");
    return RESULT_OK;
}

uint8_t wrapper_espnow_send_to(int32_t deviceNumber, string command) {
    if (g_ctx.espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando send_to");
    }

    if (deviceNumber < 0) {
        return failWithCode(AppError::Code::INVALID_DEVICE_INDEX, "indice invalido. Use 000 ou valores >= 1");
    }

    EspNowManager::message outgoing = {};
    outgoing.timer = millis();
    outgoing.type = EspNowManager::logType::INFO;

    const string msg = stripOuterQuotes(command);
    std::strncpy(outgoing.msg, msg.c_str(), sizeof(outgoing.msg) - 1);
    outgoing.msg[sizeof(outgoing.msg) - 1] = '\0';

    if (deviceNumber == 0) {
        // Peer virtual 000: route to default broadcast MAC (stored in DB, with FF fallback).
        uint8_t broadcastMac[6] = {0, 0, 0, 0, 0, 0};
        resolveDefaultBroadcastMac(broadcastMac);

        const bool queued = g_ctx.espNow->sendToMac(broadcastMac, outgoing);

        if (g_ctx.database != nullptr && g_ctx.database->isReady()) {
            g_ctx.database->logOutgoingEspNow(broadcastMac, outgoing, queued);
        }

        if (!queued) {
            return failWithCode(AppError::Code::BROADCAST_QUEUE_FAILED, "000 status=false");
        }

        printLine("[espnow] 000 status=true");
        return RESULT_OK;
    }

    const size_t index = static_cast<size_t>(deviceNumber - 1);

    bool delivered = false;
    const bool gotStatus = g_ctx.espNow->sendToDeviceWithStatus(index, outgoing, delivered, 700);

    EspNowManager::deviceInfo target = {};
    if (g_ctx.database != nullptr && g_ctx.database->isReady() && g_ctx.espNow->deviceAt(index, target)) {
        g_ctx.database->logOutgoingEspNow(target.mac, outgoing, gotStatus && delivered);
    }

    if (!gotStatus) {
        return failWithCode(AppError::Code::SEND_STATUS_TIMEOUT, "status=false (sem callback/timeout)");
    }

    if (!delivered) {
        return failWithCode(AppError::Code::SEND_DELIVERY_FAILED, "status=false");
    }

    printLine("[espnow] status=true");
    return RESULT_OK;
}

uint8_t wrapper_espnow_send_all(string command) {
    if (g_ctx.espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando send_all");
    }

    EspNowManager::message outgoing = {};
    outgoing.timer = millis();
    outgoing.type = EspNowManager::logType::INFO;

    const string msg = stripOuterQuotes(command);
    std::strncpy(outgoing.msg, msg.c_str(), sizeof(outgoing.msg) - 1);
    outgoing.msg[sizeof(outgoing.msg) - 1] = '\0';

    size_t deliveredCount = 0;
    size_t triedCount = 0;
    const bool attempted = g_ctx.espNow->sendToAllWithStatus(outgoing, deliveredCount, triedCount, 700);

    if (!attempted || triedCount == 0) {
        uint8_t broadcastMac[6] = {0, 0, 0, 0, 0, 0};
        resolveDefaultBroadcastMac(broadcastMac);

        const bool queued = g_ctx.espNow->sendToMac(broadcastMac, outgoing);
        if (g_ctx.database != nullptr && g_ctx.database->isReady()) {
            g_ctx.database->logOutgoingEspNow(broadcastMac, outgoing, queued);
        }

        if (!queued) {
            return failWithCode(AppError::Code::BROADCAST_QUEUE_FAILED, "status=false (nenhum peer e broadcast falhou)");
        }

        printLine("[espnow] status=true (broadcast 000)");
        return RESULT_OK;
    }

    char line[120] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[espnow] status=%s delivered=%u/%u",
        (deliveredCount == triedCount) ? "true" : "false",
        static_cast<unsigned>(deliveredCount),
        static_cast<unsigned>(triedCount)
    );
    printLine(line);

    if (deliveredCount != triedCount) {
        return failWithCode(AppError::Code::SEND_PARTIAL_DELIVERY, "entrega parcial no envio para todos os peers");
    }

    return RESULT_OK;
}

uint8_t wrapper_espnow_flush_db(int32_t limit = 0) {
    if (g_ctx.database == nullptr || !g_ctx.database->isReady()) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para flush do buffer RX");
    }

    const size_t pendingBefore = EspNowConfig::pendingRxDbLogCount();
    const size_t maxItems = (limit > 0) ? static_cast<size_t>(limit) : 0U;
    const size_t flushed = EspNowConfig::flushRxDbLogBuffer(maxItems);
    const size_t pendingAfter = EspNowConfig::pendingRxDbLogCount();
    const size_t capacity = EspNowConfig::rxDbLogCapacity();
    const size_t threshold = (capacity * RX_DB_AUTO_FLUSH_PERCENT + 99U) / 100U;

        char line[256] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[espnow] flush_db modo=%s antes=%lu gravados=%lu depois=%lu cap=%lu thr=%lu heap=%lu min=%lu",
#if HIGH_FREQUENCY_INCOMMING_ESPNOW
        "buffer_ram",
#else
        "normal",
#endif
        static_cast<unsigned long>(pendingBefore),
        static_cast<unsigned long>(flushed),
        static_cast<unsigned long>(pendingAfter),
        static_cast<unsigned long>(capacity),
        static_cast<unsigned long>(threshold),
        static_cast<unsigned long>(ESP.getFreeHeap()),
        static_cast<unsigned long>(ESP.getMinFreeHeap())
    );
    printLine(line);
    return RESULT_OK;
}

uint8_t wrapper_espnow_flush_status() {
    const size_t pending = EspNowConfig::pendingRxDbLogCount();
    const size_t capacity = EspNowConfig::rxDbLogCapacity();
    const size_t threshold = (capacity * RX_DB_AUTO_FLUSH_PERCENT + 99U) / 100U;
    const unsigned long percent = (capacity > 0U)
        ? static_cast<unsigned long>((pending * 100U) / capacity)
        : 0UL;

    char line[288] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[espnow] flush_status modo=%s pending=%lu cap=%lu thr=%lu (%u%%) occ=%lu%% heap=%lu min=%lu",
#if HIGH_FREQUENCY_INCOMMING_ESPNOW
        "buffer_ram",
#else
        "normal",
#endif
        static_cast<unsigned long>(pending),
        static_cast<unsigned long>(capacity),
        static_cast<unsigned long>(threshold),
        static_cast<unsigned>(RX_DB_AUTO_FLUSH_PERCENT),
        percent,
        static_cast<unsigned long>(ESP.getFreeHeap()),
        static_cast<unsigned long>(ESP.getMinFreeHeap())
    );
    printLine(line);
    return RESULT_OK;
}

uint8_t wrapper_database_init() {
    if (g_ctx.database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando init");
    }

    if (g_ctx.espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para bootstrap do database");
    }

    if (g_ctx.database->begin(*g_ctx.espNow, g_ctx.io)) {
        const bool syncOk = g_ctx.database->syncPeersFromManager(*g_ctx.espNow);
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
    if (g_ctx.database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando status");
    }

    String status;
    const bool ok = g_ctx.database->getStatus(status);
    printLine(status.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_QUERY_FAILED, "falha ao consultar status do database");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_tables() {
    if (g_ctx.database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando tables");
    }

    String output;
    const bool ok = g_ctx.database->listTables(output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_QUERY_FAILED, "falha ao listar tabelas");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_read(string tableName, int32_t limit = 20) {
    if (g_ctx.database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando read");
    }

    String output;
    const size_t boundedLimit = (limit > 0) ? static_cast<size_t>(limit) : 20U;
    const bool ok = g_ctx.database->readTable(stripOuterQuotes(tableName).c_str(), boundedLimit, output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_QUERY_FAILED, "falha ao ler tabela");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_drop(string tableName) {
    if (g_ctx.database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando drop");
    }

    const String safeName = String(stripOuterQuotes(tableName).c_str());
    const bool ok = g_ctx.database->dropTable(safeName);
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_DROP_FAILED, "falha ao remover tabela");
    }

    printLine("[database] tabela removida");
    return RESULT_OK;
}

uint8_t wrapper_database_logs(int32_t limit = 20) {
    if (g_ctx.database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando logs");
    }

    const size_t boundedLimit = (limit > 0) ? static_cast<size_t>(limit) : 20U;
    String output;
    const bool ok = g_ctx.database->readCommandLogsWithOutput(boundedLimit, output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_QUERY_FAILED, "falha ao ler logs de comandos");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_espnow_history(int32_t limit = 30) {
    if (g_ctx.database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando espnow_history");
    }

    const size_t boundedLimit = (limit > 0) ? static_cast<size_t>(limit) : 30U;
    String output;
    const bool ok = g_ctx.database->readEspNowHistory(boundedLimit, output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_QUERY_FAILED, "falha ao ler historico ESP-NOW");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_rebuild() {
    if (g_ctx.database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando rebuild");
    }

    if (g_ctx.espNow == nullptr) {
        return failWithCode(AppError::Code::ESPNOW_NOT_READY, "espnow indisponivel para comando rebuild");
    }

    if (g_ctx.database->rebuild(*g_ctx.espNow)) {
        if (!g_ctx.database->syncPeersFromManager(*g_ctx.espNow)) {
            warnWithCode(AppError::Code::DATABASE_PEER_SYNC_FAILED, "banco recriado, mas falhou ao sincronizar peers");
        }
        printLine("[database] banco recriado com bootstrap.sql");
        return RESULT_OK;
    }

    return failWithCode(AppError::Code::DATABASE_REBUILD_FAILED, "falha ao recriar banco");
}

uint8_t wrapper_database_backup() {
    if (g_ctx.database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando backup");
    }

    String output;
    const bool ok = g_ctx.database->backup(output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_QUERY_FAILED, "falha ao gerar backup do banco");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_exec(string sql) {
    if (g_ctx.database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando exec");
    }

    String output;
    const bool ok = g_ctx.database->executeSql(stripOuterQuotes(sql).c_str(), output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_EXEC_FAILED, "falha ao executar SQL");
    }

    return RESULT_OK;
}

uint8_t wrapper_database_exec_nolog(string sql) {
    if (g_ctx.database == nullptr) {
        return failWithCode(AppError::Code::DATABASE_NOT_READY, "database indisponivel para comando exec_nolog");
    }

    String output;
    const bool ok = g_ctx.database->executeSql(stripOuterQuotes(sql).c_str(), output);
    printLine(output.c_str());
    if (!ok) {
        return failWithCode(AppError::Code::DATABASE_EXEC_FAILED, "falha ao executar SQL");
    }

    return RESULT_OK;
}

} // namespace

namespace ShellConfig {

bool bind(const Context& context) {
    if (context.shell == nullptr ||
        context.espNow == nullptr ||
        context.peripherals == nullptr ||
        context.lcdTerminal == nullptr ||
        context.database == nullptr ||
        context.io == nullptr) {
        return false;
    }

    g_ctx = context;

    g_ctx.lcdTerminal->begin(*g_ctx.peripherals);
    return true;
}

uint8_t registerDefaultModules() {
    if (g_ctx.shell == nullptr) {
        return failWithCode(AppError::Code::SHELL_NOT_READY, "shell nao configurada para registrar modulos");
    }

    g_ctx.shell->create_module("help", "ajuda e informacoes");
    g_ctx.shell->create_module("dongle", "comandos executados localmente nesta ESP");
    g_ctx.shell->create_module("espnow", "gerenciamento de peers e envio de mensagens esp-now");
    g_ctx.shell->create_module("database", "sqlite no SD: status, leitura e manutencao");

    g_ctx.shell->add(wrapper_help_h, "h", "lista os modulos", "help");
    g_ctx.shell->add(wrapper_help_l, "l", "lista as funcoes de um modulo", "help");
    g_ctx.shell->add(wrapper_help_e, "e", "explica o uso dos comandos", "help");

    g_ctx.shell->add(wrapper_dongle_ping, "ping", "teste rapido local", "dongle");
    g_ctx.shell->add(wrapper_dongle_clock, "clock", "mostra horario atual do RTC", "dongle");
    g_ctx.shell->add(wrapper_dongle_set_clock, "set_clock", "ajusta RTC: <\"YYYY-MM-DD HH:MM:SS\">", "dongle");
    g_ctx.shell->add(wrapper_dongle_run, "run", "executa comando local (placeholder)", "dongle");
    g_ctx.shell->add(wrapper_dongle_led, "led", "define LED RGB: <r>, <g>, <b>", "dongle");
    g_ctx.shell->add(wrapper_dongle_led_off, "led_off", "desliga o LED", "dongle");
    g_ctx.shell->add(wrapper_dongle_lcd, "lcd", "escreve texto no terminal LCD: <texto>", "dongle");
    g_ctx.shell->add(wrapper_dongle_lcd_clear, "lcd_clear", "limpa o terminal LCD", "dongle");
    g_ctx.shell->add(wrapper_dongle_lcd_rot, "lcd_rot", "rotacao LCD: <0|1|2|3>", "dongle");
    g_ctx.shell->add(wrapper_dongle_lcd_rot_get, "lcd_rot_get", "mostra rotacao atual do LCD", "dongle");
    g_ctx.shell->add(wrapper_dongle_lcd_bl, "lcd_bl", "backlight LCD: <0=ON|1=OFF>", "dongle");
    g_ctx.shell->add(wrapper_dongle_lcd_bl_inv, "lcd_bl_inv", "polaridade backlight: <1=HIGH_ON|0=LOW_ON>", "dongle");
    g_ctx.shell->add(wrapper_dongle_lcd_reinit, "lcd_reinit", "reinicializa o LCD", "dongle");
    g_ctx.shell->add(wrapper_dongle_sd_init, "sd_init", "inicia o SD", "dongle");
    g_ctx.shell->add(wrapper_dongle_sd_status, "sd_status", "mostra status do SD", "dongle");
    g_ctx.shell->add(wrapper_dongle_sd_wipe, "sd_wipe", "apaga todo o conteudo do SD", "dongle");

    g_ctx.shell->add(wrapper_espnow_list, "list", "lista dispositivos cadastrados", "espnow");
    g_ctx.shell->add(wrapper_espnow_add, "add", "adiciona peer: <mac>, <nome>, <descricao>", "espnow");
    g_ctx.shell->add(wrapper_espnow_remove, "remove", "remove peer por indice: <numero>", "espnow");
    g_ctx.shell->add(wrapper_espnow_remove_mac, "remove_mac", "remove peer por mac: <mac>", "espnow");
    g_ctx.shell->add(wrapper_espnow_update, "update", "atualiza peer: <numero>, <nome>, <descricao>", "espnow");
    g_ctx.shell->add(wrapper_espnow_send_to, "send_to", "envia para indice: <numero|000>, <comando> (000=todos)", "espnow");
    g_ctx.shell->add(wrapper_espnow_send_all, "send_all", "envia para todos: <comando>", "espnow");
    g_ctx.shell->add(wrapper_espnow_flush_status, "flush_status", "mostra status do buffer RX->DB", "espnow");
    g_ctx.shell->add(wrapper_espnow_flush_db, "flush_db", "persiste buffer RX->DB: <limite opcional>", "espnow");

    g_ctx.shell->add(wrapper_database_init, "init", "abre o banco e aplica bootstrap.sql", "database");
    g_ctx.shell->add(wrapper_database_status, "status", "status geral do sqlite", "database");
    g_ctx.shell->add(wrapper_database_tables, "tables", "lista tabelas no banco", "database");
    g_ctx.shell->add(wrapper_database_read, "read", "le tabela: <nome>, <limite>", "database");
    g_ctx.shell->add(wrapper_database_logs, "logs", "historico de comandos com saidas: <limite>", "database");
    g_ctx.shell->add(wrapper_database_espnow_history, "espnow_history", "historico ESP-NOW RX/TX com status: <limite>", "database");
    g_ctx.shell->add(wrapper_database_drop, "drop", "remove tabela: <nome>", "database");
    g_ctx.shell->add(wrapper_database_rebuild, "rebuild", "recria banco a partir do bootstrap", "database");
    g_ctx.shell->add(wrapper_database_backup, "backup", "salva snapshot do banco em /database/backups", "database");
    g_ctx.shell->add(wrapper_database_exec, "exec", "executa SQL livre: <sql>", "database");
    g_ctx.shell->add(wrapper_database_exec_nolog, "exec_nolog", "executa SQL sem salvar no command_log: <sql>", "database");

    return RESULT_OK;
}

std::string runLine(const std::string& command) {
    if (g_ctx.shell == nullptr) {
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
    g_commandOutputBuffer.clear();

    std::string output;
    const std::string databaseExecPrefix = "database -exec";
    const std::string databaseExecNoLogPrefix = "database -exec_nolog";
    bool skipCommandPersistence = false;

    if (normalized == databaseExecNoLogPrefix || normalized.rfind(databaseExecNoLogPrefix + " ", 0) == 0) {
        const std::string sql = trimCopy(normalized.substr(databaseExecNoLogPrefix.length()));
        if (sql.empty()) {
            failWithCode(AppError::Code::INVALID_ARGUMENT, "uso: database -exec_nolog \"<sql>\"");
        } else {
            wrapper_database_exec_nolog(sql);
        }
        skipCommandPersistence = true;

    // Bypass TinyShell tokenizer here so SQL can contain commas/quotes safely.
    } else if (normalized == databaseExecPrefix || normalized.rfind(databaseExecPrefix + " ", 0) == 0) {
        const std::string sql = trimCopy(normalized.substr(databaseExecPrefix.length()));
        if (sql.empty()) {
            failWithCode(AppError::Code::INVALID_ARGUMENT, "uso: database -exec \"<sql>\"");
        } else {
            wrapper_database_exec(sql);
        }
        skipCommandPersistence = true;
    } else {
        output = g_ctx.shell->run_line_command(normalized);
    }

    if (!skipCommandPersistence && g_ctx.database != nullptr && g_ctx.database->isReady()) {
        std::string persistedOutput = g_commandOutputBuffer;
        if (!output.empty()) {
            if (!persistedOutput.empty()) {
                persistedOutput += "\n";
            }
            persistedOutput += output;
        }

        if (persistedOutput.empty()) {
            persistedOutput = "(sem saida textual)";
        }

        if (!g_ctx.database->logCommandWithOutput(normalized.c_str(), persistedOutput.c_str(), "serial")) {
            warnWithCode(AppError::Code::DATABASE_COMMAND_LOG_FAILED, "falha ao persistir log de comando");
        }
    }

    g_commandOutputBuffer.clear();

    return output;
}

} // namespace ShellConfig
