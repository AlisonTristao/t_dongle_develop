#include <WiFi.h>

#include "config.h"
#include "shell_config.h"
#include "espnow_config.h"
#include "shell_output.h"

#include <ShellSerial.h>
#include <EspNowManager.h>
#include <DonglePeripherals.h>
#include <LcdTerminal.h>
#include <TinyShell.h>

static constexpr uint8_t PEER_MAC[] = {0x80, 0xB5, 0x4E, 0xC6, 0xD8, 0xC8};

ShellSerial serialShell;
EspNowManager espNowManager;
DonglePeripherals donglePeripherals;
LcdTerminal lcdTerminal;
TinyShell tinyShell;

void setup() {
    BoardConfig::initBoardPins(false);

    donglePeripherals.begin();
    donglePeripherals.beginSd(false);

    serialShell.begin(Serial, 921600);
    serialShell.setPrompt("$ ");
    while (!Serial) {
    }

    Serial.print("mac: ");
    Serial.println(WiFi.macAddress());

    EspNowConfig::attachCallbacks(espNowManager, Serial);

    if (!espNowManager.begin(0, false)) {
        Serial.println("esp-now init failed");
        return;
    }

    espNowManager.addDevice(PEER_MAC, "peer-main", "peer principal para mensagens esp-now");

    const bool shellBound = ShellConfig::bind({
        &tinyShell,
        &espNowManager,
        &donglePeripherals,
        &lcdTerminal,
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
        Serial.println();

        const std::string response = ShellConfig::runLine(std::string(command.c_str()));
        if (!response.empty()) {
            Serial.println();
            ShellOutput::printResponse(Serial, response);
        }

        Serial.println();
    }

    delay(5);
}
