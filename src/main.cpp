#include <WiFi.h>
#include <Esp.h>
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
TaskHandle_t g_espNowRxDbTaskHandle = nullptr;

constexpr uint8_t kRxDbAutoFlushPercent = RX_DB_AUTO_FLUSH_PERCENT;

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

void espNowRxDbWorkerTask(void*) {
    EspNowConfig::RxDbLogEvent event = {};

    while (true) {
        if (EspNowConfig::dequeueRxDbLog(event, 250)) {
            EspNowConfig::processRxDbLog(event);
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

    const bool asyncRxEnabled = EspNowConfig::enableAsyncRx(RX_ASYNC_QUEUE_DEPTH);
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
        const BaseType_t rxTaskOk = xTaskCreatePinnedToCore(
            espNowRxWorkerTask,
            "espnow_rx",
            6144,
            nullptr,
            1,
            &g_espNowRxTaskHandle,
            workerCore
        );

        if (rxTaskOk != pdPASS) {
            EspNowConfig::disableAsyncRx();
            ShellOutput::printTagged(Serial, "espnow", "rx task create failed (fallback callback)");
        } else {
            char rxLine[48] = {0};
            std::snprintf(rxLine, sizeof(rxLine), "rx task core=%d", static_cast<int>(workerCore));
            ShellOutput::printTagged(Serial, "espnow", rxLine);

#if HIGH_FREQUENCY_INCOMMING_ESPNOW
            EspNowConfig::setAsyncDbLogEnabled(true);
            ShellOutput::printTagged(Serial, "espnow", "rx db mode=buffer_ram (autoflush 80% + flush manual)");
            char queueLine[96] = {0};
            std::snprintf(
                queueLine,
                sizeof(queueLine),
                "rx queues: main=%u db=%u",
                static_cast<unsigned>(RX_ASYNC_QUEUE_DEPTH),
                static_cast<unsigned>(RX_DB_LOG_QUEUE_DEPTH)
            );
            ShellOutput::printTagged(Serial, "espnow", queueLine);

            const size_t dbCapacity = EspNowConfig::rxDbLogCapacity();
            const size_t dbThreshold = (dbCapacity * kRxDbAutoFlushPercent + 99U) / 100U;
            char flushCfgLine[120] = {0};
            std::snprintf(
                flushCfgLine,
                sizeof(flushCfgLine),
                "rx autoflush cfg: cap=%lu thr=%lu (%u%%)",
                static_cast<unsigned long>(dbCapacity),
                static_cast<unsigned long>(dbThreshold),
                static_cast<unsigned>(kRxDbAutoFlushPercent)
            );
            ShellOutput::printTagged(Serial, "espnow", flushCfgLine);

            if (dbCapacity == 0U) {
                ShellOutput::printTagged(Serial, "espnow", "warn: rx db queue indisponivel; sem buffer/autoflush");
            }
#else
            const BaseType_t rxDbTaskOk = xTaskCreatePinnedToCore(
                espNowRxDbWorkerTask,
                "espnow_rx_db",
                6144,
                nullptr,
                1,
                &g_espNowRxDbTaskHandle,
                workerCore
            );

            if (rxDbTaskOk != pdPASS) {
                EspNowConfig::setAsyncDbLogEnabled(false);
                ShellOutput::printTagged(Serial, "espnow", "rx db task create failed (fallback log sync)");
            } else {
                EspNowConfig::setAsyncDbLogEnabled(true);
                char dbLine[52] = {0};
                std::snprintf(dbLine, sizeof(dbLine), "rx db task core=%d", static_cast<int>(workerCore));
                ShellOutput::printTagged(Serial, "espnow", dbLine);
            }
#endif
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

#if HIGH_FREQUENCY_INCOMMING_ESPNOW
    const size_t dbCapacity = EspNowConfig::rxDbLogCapacity();
    if (dbCapacity > 0) {
        const size_t dbPending = EspNowConfig::pendingRxDbLogCount();
        const size_t dbThreshold = (dbCapacity * kRxDbAutoFlushPercent + 99U) / 100U;
        if (dbPending >= dbThreshold) {
            const size_t flushed = EspNowConfig::flushRxDbLogBuffer(dbPending);
            const size_t dbAfter = EspNowConfig::pendingRxDbLogCount();

            char flushLine[200] = {0};
            std::snprintf(
                flushLine,
                sizeof(flushLine),
                "autoflush before=%lu flushed=%lu after=%lu cap=%lu thr=%lu heap=%lu min=%lu",
                static_cast<unsigned long>(dbPending),
                static_cast<unsigned long>(flushed),
                static_cast<unsigned long>(dbAfter),
                static_cast<unsigned long>(dbCapacity),
                static_cast<unsigned long>(dbThreshold),
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMinFreeHeap())
            );
            ShellOutput::printTagged(Serial, "espnow", flushLine);
            needPromptRefresh = true;
        }
    }
#endif

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

    const uint32_t droppedDbBefore = EspNowConfig::takeDroppedRxDbLogCount();
    if (droppedDbBefore > 0) {
        char line[64] = {0};
        std::snprintf(line, sizeof(line), "rx_db_dropped=%lu", static_cast<unsigned long>(droppedDbBefore));
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

    const uint32_t droppedDbAfter = EspNowConfig::takeDroppedRxDbLogCount();
    if (droppedDbAfter > 0) {
        char line[64] = {0};
        std::snprintf(line, sizeof(line), "rx_db_dropped=%lu", static_cast<unsigned long>(droppedDbAfter));
        ShellOutput::printTagged(Serial, "espnow", line);
        needPromptRefresh = true;
    }

    if (needPromptRefresh) {
        serialShell.refreshLine();
    }

    // Small cooperative delay for non-blocking loop behavior.
    delay(1);
}
