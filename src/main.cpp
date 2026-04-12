#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

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

void restoreShellHistoryFromDatabase() {
    if (!databaseStore.isReady()) {
        return;
    }

    String historyText;
    if (!databaseStore.readRecentCommands(ShellSerial::DEFAULT_LOG_CAPACITY, historyText)) {
        return;
    }

    if (historyText.isEmpty()) {
        return;
    }

    int32_t start = 0;
    while (start <= static_cast<int32_t>(historyText.length())) {
        const int32_t newline = historyText.indexOf('\n', start);
        String line;

        if (newline < 0) {
            line = historyText.substring(start);
        } else {
            line = historyText.substring(start, newline);
        }

        line.trim();
        if (!line.isEmpty()) {
            serialShell.addLog(line);
        }

        if (newline < 0) {
            break;
        }

        start = newline + 1;
    }
}

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

    ShellOutput::printTagged(Serial, "startup", String("mac=") + WiFi.macAddress());

    StartupConfig::promptAndSetDateTime(Serial);

    EspNowConfig::attachCallbacks(espNowManager, Serial, &databaseStore, &lcdTerminal);

    const bool asyncRxEnabled = EspNowConfig::enableAsyncRx(24);
    if (!asyncRxEnabled) {
        ShellOutput::printTagged(Serial, "espnow", "async queue init failed (fallback no callback)");
    }

    if (!espNowManager.begin(0, false)) {
        ShellOutput::printTagged(Serial, "espnow", "init failed");
        return;
    }

    if (!databaseStore.begin(espNowManager, &Serial)) {
        ShellOutput::printTagged(Serial, "database", "init failed (continuando sem persistencia)");
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
            ShellOutput::printTagged(Serial, "espnow", "rx task create failed (fallback callback)");
        } else {
            char line[48] = {0};
            std::snprintf(line, sizeof(line), "rx task core=%d", static_cast<int>(workerCore));
            ShellOutput::printTagged(Serial, "espnow", line);
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
        ShellOutput::printTagged(Serial, "shell", "bind failed");
        return;
    }

    if (ShellConfig::registerDefaultModules() != RESULT_OK) {
        ShellOutput::printTagged(Serial, "shell", "module registration failed");
        return;
    }

    restoreShellHistoryFromDatabase();

    ShellOutput::printTagged(Serial, "shell", "ready: <module> -<command> [args]");
    ShellOutput::printTagged(Serial, "shell", "use: help -e");
    serialShell.refreshLine();
}

void loop() {
    bool needPromptRefresh = false;

    const size_t flushedBefore = EspNowConfig::flushRxDisplayLines(12);
    if (flushedBefore > 0) {
        needPromptRefresh = true;
    }

    const uint32_t droppedBefore = EspNowConfig::takeDroppedRxCount();
    if (droppedBefore > 0) {
        char line[64] = {0};
        std::snprintf(line, sizeof(line), "rx_dropped=%lu", static_cast<unsigned long>(droppedBefore));
        ShellOutput::printTagged(Serial, "espnow", line);
        needPromptRefresh = true;
    }

    if (needPromptRefresh) {
        serialShell.refreshLine();
        needPromptRefresh = false;
    }

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
        serialShell.refreshLine();
    }

    const size_t flushedAfter = EspNowConfig::flushRxDisplayLines(12);
    if (flushedAfter > 0) {
        needPromptRefresh = true;
    }

    const uint32_t droppedAfter = EspNowConfig::takeDroppedRxCount();
    if (droppedAfter > 0) {
        char line[64] = {0};
        std::snprintf(line, sizeof(line), "rx_dropped=%lu", static_cast<unsigned long>(droppedAfter));
        ShellOutput::printTagged(Serial, "espnow", line);
        needPromptRefresh = true;
    }

    if (needPromptRefresh) {
        serialShell.refreshLine();
    }

    // Small cooperative delay for non-blocking loop behavior.
    delay(1);
}
