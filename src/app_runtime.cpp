#include "app_runtime.h"

#include <WiFi.h>

#include "config.h"
#include "shell_config.h"

#include <ShellSerial.h>
#include <EspNowManager.h>
#include <DonglePeripherals.h>
#include <LcdTerminal.h>
#include <TinyShell.h>

namespace {

static constexpr uint8_t PEER_MAC[] = {0x80, 0xB5, 0x4E, 0xC6, 0xD8, 0xC8};

EspNowManager::message g_myData = {};
ShellSerial g_serialShell;
EspNowManager g_espNowManager;
DonglePeripherals g_donglePeripherals;
LcdTerminal g_lcdTerminal;
TinyShell g_tinyShell;

const char* shellOutputPrefix(const String& text) {
    String lower = text;
    lower.toLowerCase();

    if (
        lower.indexOf("falhou") >= 0 ||
        lower.indexOf("erro") >= 0 ||
        lower.indexOf("error") >= 0 ||
        lower.indexOf("invalid") >= 0
    ) {
        return "! ";
    }

    return "> ";
}

void printShellResponse(const std::string& response) {
    String text = String(response.c_str());
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');
    text.trim();
    if (text.length() == 0) {
        return;
    }

    int32_t start = 0;
    while (start <= static_cast<int32_t>(text.length())) {
        const int32_t newline = text.indexOf('\n', start);
        String line;

        if (newline < 0) {
            line = text.substring(start);
        } else {
            line = text.substring(start, newline);
        }

        line.trim();
        if (line.length() > 0) {
            Serial.print(shellOutputPrefix(line));
            Serial.println(line);
        }

        if (newline < 0) {
            break;
        }
        start = newline + 1;
    }
}

void onDataRecv(const uint8_t* mac, const EspNowManager::message& incomingData) {
    (void)mac;
    g_myData = incomingData;
    Serial.printf("%s\n", g_myData.msg);
}

void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    (void)mac_addr;
    (void)status;
}

} // namespace

namespace AppRuntime {

void setup() {
    BoardConfig::initBoardPins(false);

    g_donglePeripherals.begin();
    g_donglePeripherals.beginSd(false);

    g_serialShell.begin(Serial, 921600);
    g_serialShell.setPrompt("$ ");
    while (!Serial) {
    }

    Serial.print("mac: ");
    Serial.println(WiFi.macAddress());

    g_espNowManager.setReceiveCallback(onDataRecv);
    g_espNowManager.setSendCallback(onDataSent);

    if (!g_espNowManager.begin(0, false)) {
        Serial.println("esp-now init failed");
        return;
    }

    g_espNowManager.addDevice(PEER_MAC, "peer-main", "peer principal para mensagens esp-now");

    const bool shellBound = ShellConfig::bind({
        &g_tinyShell,
        &g_espNowManager,
        &g_donglePeripherals,
        &g_lcdTerminal,
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
    if (g_serialShell.readInputLine(command)) {
        Serial.println();

        const std::string response = ShellConfig::runLine(std::string(command.c_str()));
        if (!response.empty()) {
            Serial.println();
            printShellResponse(response);
        }

        Serial.println();
    }

    delay(5);
}

} // namespace AppRuntime
