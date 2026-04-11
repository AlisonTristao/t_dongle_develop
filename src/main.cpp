#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

ShellSerial serialShell;
EspNowManager espNowManager;
DonglePeripherals donglePeripherals;
LcdTerminal lcdTerminal;
DatabaseStore databaseStore;
TinyShell tinyShell;

namespace {

TaskHandle_t g_espNowRxTaskHandle = nullptr;

BaseType_t selectEspNowWorkerCore() {
#if defined(CONFIG_FREERTOS_UNICORE) && (CONFIG_FREERTOS_UNICORE == 1)
    return ARDUINO_RUNNING_CORE;
#else
    return (ARDUINO_RUNNING_CORE == 0) ? 1 : 0;
#endif
}

void espNowRxWorkerTask(void*) {
    EspNowConfig::RxMessageEvent event = {};

    while (true) {
        if (EspNowConfig::dequeueRxMessage(event, 250)) {
            EspNowConfig::processRxMessage(event);
        }

        const uint32_t dropped = EspNowConfig::takeDroppedRxCount();
        if (dropped > 0) {
            Serial.print("espnow rx dropped=");
            Serial.println(static_cast<unsigned long>(dropped));
        }
    }
}

} // namespace

void setup() {
    // Hardware baseline (pins and default levels) before any peripheral init.
    BoardConfig::initBoardPins(false);

    // Interactive serial shell setup.
    serialShell.begin(Serial, 921600);
    serialShell.setPrompt("$ ");

    // Hold startup until serial terminal is attached, with LED rainbow animation.
    StartupConfig::waitForSerialAndAnimateLed(donglePeripherals);

    // Startup visuals already initialize LED/LCD; continue with remaining peripherals.
    donglePeripherals.beginSd(false);

    Serial.print("mac: ");
    Serial.println(WiFi.macAddress());

    StartupConfig::promptAndSetDateTime(Serial);

    EspNowConfig::attachCallbacks(espNowManager, Serial, &databaseStore);

    const bool asyncRxEnabled = EspNowConfig::enableAsyncRx(24);
    if (!asyncRxEnabled) {
        Serial.println("espnow async queue init failed (fallback no callback)");
    }

    if (!espNowManager.begin(0, false)) {
        Serial.println("esp-now init failed");
        return;
    }

    if (!databaseStore.begin(espNowManager, &Serial)) {
        Serial.println("database init failed (continuando sem persistencia)");
    } else {
        databaseStore.logBootEvent("power_on");
    }

    if (asyncRxEnabled) {
        const BaseType_t workerCore = selectEspNowWorkerCore();
        const BaseType_t taskOk = xTaskCreatePinnedToCore(
            espNowRxWorkerTask,
            "espnow_rx",
            6144,
            nullptr,
            1,
            &g_espNowRxTaskHandle,
            workerCore
        );

        if (taskOk != pdPASS) {
            EspNowConfig::disableAsyncRx();
            Serial.println("espnow rx task create failed (fallback callback)");
        } else {
            Serial.print("espnow rx task core=");
            Serial.println(static_cast<int>(workerCore));
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
