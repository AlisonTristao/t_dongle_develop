#include <WiFi.h>
#include "config.h"
#include <ShellSerial.h>
#include <EspNowManager.h>
#include <DonglePeripherals.h>
#include <LcdTerminal.h>
#include <TinyShell.h>
#include "shell_config.h"

// 80:B5:4E:C6:D8:C8
static constexpr uint8_t PEER_MAC[] = {0x80, 0xB5, 0x4E, 0xC6, 0xD8, 0xC8};

EspNowManager::message myData = {};
ShellSerial serialShell;
EspNowManager espNowManager;
DonglePeripherals donglePeripherals;
LcdTerminal lcdTerminal;
TinyShell tinyShell;

static const char* shellOutputPrefix(const String& text) {
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

static void printShellResponse(const std::string& response) {
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

// callback for incoming esp-now data
void OnDataRecv(const uint8_t *mac, const EspNowManager::message& incomingData) {
    myData = incomingData;
    Serial.printf("%s\n", myData.msg);
}

// callback for esp-now send status
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    // no print, could handle errors if needed
}

void setup() {
    BoardConfig::initBoardPins(false);

    donglePeripherals.begin();
    donglePeripherals.beginSd(false);

    serialShell.begin(Serial, 921600);
    serialShell.setPrompt("$ ");
    while (!Serial); // wait for serial to be ready

    // print mac address
    Serial.print("mac: ");
    Serial.println(WiFi.macAddress());

    espNowManager.setReceiveCallback(OnDataRecv);
    espNowManager.setSendCallback(OnDataSent);

    if (!espNowManager.begin(0, false)) {
        Serial.println("esp-now init failed");
        return;
    }

    espNowManager.addDevice(PEER_MAC, "peer-main", "peer principal para mensagens esp-now");

    const bool shellBound = ShellConfig::bind({&tinyShell, &espNowManager, &donglePeripherals, &lcdTerminal, &Serial});
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
        // Visual spacing between typed command and shell output.
        Serial.println();

        const std::string response = ShellConfig::runLine(std::string(command.c_str()));
        if (!response.empty()) {
            // Keep shell command output and final status/result separated.
            Serial.println();
            printShellResponse(response);
        }

        Serial.println();
    }

    delay(5); // small delay to prevent blocking
}
