#include "DongleCommands.h"

#include "ShellCommandSupport.h"
#include "error_codes.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <SD_MMC.h>

namespace {

using std::string;
using ShellCommandSupport::clampByte;
using ShellCommandSupport::context;
using ShellCommandSupport::failWithCode;
using ShellCommandSupport::parseDateTimeText;
using ShellCommandSupport::printLine;
using ShellCommandSupport::stripOuterQuotes;
using ShellCommandSupport::warnWithCode;

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
    context().shell->add(wrapper_dongle_run, "run", "execute local command (placeholder)", "dongle");
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
    context().shell->add(wrapper_dongle_sd_wipe, "sd_wipe", "wipe all SD content", "dongle");

    return RESULT_OK;
}

} // namespace DongleCommands
