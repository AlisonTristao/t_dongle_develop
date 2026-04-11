#include <WiFi.h>
#include <sys/time.h>

#include <cstdio>
#include <ctime>

#include "config.h"
#include "shell_config.h"
#include "espnow_config.h"
#include "shell_output.h"

#include <ShellSerial.h>
#include <EspNowManager.h>
#include <DonglePeripherals.h>
#include <LcdTerminal.h>
#include <DatabaseStore.h>
#include <TinyShell.h>

static constexpr uint8_t PEER_MAC[] = {0x80, 0xB5, 0x4E, 0xC6, 0xD8, 0xC8};

ShellSerial serialShell;
EspNowManager espNowManager;
DonglePeripherals donglePeripherals;
LcdTerminal lcdTerminal;
DatabaseStore databaseStore;
TinyShell tinyShell;

namespace {

bool parseDateTime(const String& text, time_t& outEpoch) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
        return false;
    }

    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }

    struct tm tmValue = {};
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

void promptAndSetDateTime(Stream& io, uint32_t timeoutMs = 30000) {
    io.println("[clock] informe data/hora: YYYY-MM-DD HH:MM:SS");
    io.println("[clock] ENTER vazio para pular (timeout 30s)");
    io.print("[clock] > ");

    const uint32_t start = millis();
    String line;

    while ((millis() - start) < timeoutMs) {
        if (Serial.available()) {
            line = Serial.readStringUntil('\n');
            break;
        }
        delay(10);
    }

    line.trim();
    if (line.isEmpty()) {
        io.println("[clock] horario mantido");
        return;
    }

    time_t epoch = 0;
    if (!parseDateTime(line, epoch)) {
        io.println("[clock] formato invalido, mantendo horario atual");
        return;
    }

    timeval tv = {};
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    char out[48] = {0};
    std::tm* now = std::localtime(&epoch);
    if (now != nullptr) {
        std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", now);
        io.print("[clock] horario ajustado para ");
        io.println(out);
    } else {
        io.println("[clock] horario ajustado");
    }
}

} // namespace

void setup() {
    // Hardware baseline (pins and default levels) before any peripheral init.
    BoardConfig::initBoardPins(false);

    // Bring up onboard peripherals used by shell commands.
    donglePeripherals.begin();
    donglePeripherals.beginSd(false);

    // Interactive serial shell setup.
    serialShell.begin(Serial, 921600);
    serialShell.setPrompt("$ ");
    while (!Serial) {
    }

    Serial.print("mac: ");
    Serial.println(WiFi.macAddress());

    promptAndSetDateTime(Serial);

    EspNowConfig::attachCallbacks(espNowManager, Serial, &databaseStore);

    if (!espNowManager.begin(0, false)) {
        Serial.println("esp-now init failed");
        return;
    }

    if (!databaseStore.begin(espNowManager, &Serial)) {
        Serial.println("database init failed (continuando sem persistencia)");
    } else {
        databaseStore.logBootEvent("power_on");
    }

    if (espNowManager.addDevice(PEER_MAC, "peer-main", "peer principal para mensagens esp-now")) {
        if (databaseStore.isReady()) {
            databaseStore.upsertPeer(PEER_MAC, "peer-main", "peer principal para mensagens esp-now");
        }
    }

    const bool shellBound = ShellConfig::bind({
        &tinyShell,
        &espNowManager,
        &donglePeripherals,
        &lcdTerminal,
        &databaseStore,
        &Serial
    });
    if (!shellBound) {
        Serial.println("shell bind failed");
        return;
    }

    if (ShellConfig::registerDefaultModules() != RESULT_OK) {
        Serial.println("shell module registration failed");
        return;
    }

    Serial.println("shell ready: <module> -<command> [args]");
    Serial.println("use: help -e");
}

void loop() {
    String command;
    if (serialShell.readInputLine(command)) {
        // Keep command/output/result visually separated in the serial terminal.
        Serial.println();

        const std::string response = ShellConfig::runLine(std::string(command.c_str()));
        if (!response.empty()) {
            Serial.println();
            ShellOutput::printResponse(Serial, response);
        }

        Serial.println();
    }

    // Small cooperative delay for non-blocking loop behavior.
    delay(1);
}
