#include <WiFi.h>

#include "config.h"
#include "startup_config.h"
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

void setup() {
    // Hardware baseline (pins and default levels) before any peripheral init.
    BoardConfig::initBoardPins(false);

    // Interactive serial shell setup.
    serialShell.begin(Serial, 921600);
    serialShell.setPrompt("$ ");

    // Hold startup until serial terminal is attached, with LED rainbow animation.
    StartupConfig::waitForSerialAndAnimateLed(donglePeripherals);

    // Bring up onboard peripherals only after serial monitor is open.
    donglePeripherals.begin();
    donglePeripherals.beginSd(false);

    Serial.print("mac: ");
    Serial.println(WiFi.macAddress());

    StartupConfig::promptAndSetDateTime(Serial);

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
