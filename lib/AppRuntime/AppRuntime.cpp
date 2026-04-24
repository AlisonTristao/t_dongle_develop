#include "AppRuntime.h"

#include <WiFi.h>
#include <Esp.h>

#include <cstdio>

#include "config.h"
#include "StartupConfig.h"
#include "ShellConfig.h"
#include "EspNowConfig.h"
#include "ShellOutput.h"

#ifndef BOUDERATE
#define BAUDRATE 9600
#endif

namespace {

constexpr uint8_t kRxDbWarningPercent = RX_DB_WARNING_PERCENT;
constexpr size_t kRxDisplayFlushBurst = 24;

} // namespace

void AppRuntime::espNowRxWorkerTask(void*) {
    EspNowConfig::RxMessageEvent event = {};

    while (true) {
        if (EspNowConfig::dequeueRxMessage(event, 250)) {
            EspNowConfig::processRxMessage(event);
        }
    }
}

void AppRuntime::espNowRxDbWorkerTask(void*) {
    EspNowConfig::RxDbLogEvent event = {};

    while (true) {
        if (EspNowConfig::dequeueRxDbLog(event, 250)) {
            EspNowConfig::processRxDbLog(event);
        }
    }
}

void AppRuntime::restoreShellHistoryFromDatabase() {
    if (!databaseStore_.isReady()) {
        return;
    }

    String historyText;
    if (!databaseStore_.readRecentCommands(ShellSerial::DEFAULT_LOG_CAPACITY, historyText)) {
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
            serialShell_.addLog(line);
        }

        if (newline < 0) {
            break;
        }

        start = newline + 1;
    }
}

BaseType_t AppRuntime::selectEspNowWorkerCore() const {
#if defined(CONFIG_FREERTOS_UNICORE) && (CONFIG_FREERTOS_UNICORE == 1)
    return ARDUINO_RUNNING_CORE;
#else
    return (ARDUINO_RUNNING_CORE == 0) ? 1 : 0;
#endif
}

void AppRuntime::startEspNowWorkers(bool asyncRxEnabled) {
    if (!asyncRxEnabled) {
        return;
    }

    const BaseType_t workerCore = selectEspNowWorkerCore();
    const BaseType_t rxTaskOk = xTaskCreatePinnedToCore(
        espNowRxWorkerTask,
        "espnow_rx",
        6144,
        nullptr,
        1,
        &espNowRxTaskHandle_,
        workerCore
    );

    if (rxTaskOk != pdPASS) {
        EspNowConfig::disableAsyncRx();
        ShellOutput::printTagged(Serial, "espnow", "rx task create failed (fallback callback)");
        return;
    }

    char rxLine[48] = {0};
    std::snprintf(rxLine, sizeof(rxLine), "rx task core=%d", static_cast<int>(workerCore));
    ShellOutput::printTagged(Serial, "espnow", rxLine);

#if HIGH_FREQUENCY_INCOMMING_ESPNOW
    EspNowConfig::setAsyncDbLogEnabled(true);
    ShellOutput::printTagged(Serial, "espnow", "rx db mode=buffer_ram (warning 80% + flush manual)");
    const size_t dbCapacity = EspNowConfig::rxDbLogCapacity();

    char queueLine[96] = {0};
    std::snprintf(
        queueLine,
        sizeof(queueLine),
        "rx queues: main=%u db_min=%u db_cap=%lu",
        static_cast<unsigned>(RX_ASYNC_QUEUE_DEPTH),
        static_cast<unsigned>(RX_DB_LOG_QUEUE_DEPTH),
        static_cast<unsigned long>(dbCapacity)
    );
    ShellOutput::printTagged(Serial, "espnow", queueLine);

    const size_t dbThreshold = (dbCapacity * kRxDbWarningPercent + 99U) / 100U;
    char flushCfgLine[120] = {0};
    std::snprintf(
        flushCfgLine,
        sizeof(flushCfgLine),
        "rx warning cfg: thr=%lu (%u%%)",
        static_cast<unsigned long>(dbThreshold),
        static_cast<unsigned>(kRxDbWarningPercent)
    );
    ShellOutput::printTagged(Serial, "espnow", flushCfgLine);

    if (dbCapacity == 0U) {
        ShellOutput::printTagged(Serial, "espnow", "warn: rx db queue indisponivel; sem buffer em RAM");
    }
#else
    const BaseType_t rxDbTaskOk = xTaskCreatePinnedToCore(
        espNowRxDbWorkerTask,
        "espnow_rx_db",
        6144,
        nullptr,
        1,
        &espNowRxDbTaskHandle_,
        workerCore
    );

    if (rxDbTaskOk != pdPASS) {
        EspNowConfig::setAsyncDbLogEnabled(false);
        ShellOutput::printTagged(Serial, "espnow", "rx db task create failed (fallback log sync)");
        return;
    }

    EspNowConfig::setAsyncDbLogEnabled(true);
    char dbLine[52] = {0};
    std::snprintf(dbLine, sizeof(dbLine), "rx db task core=%d", static_cast<int>(workerCore));
    ShellOutput::printTagged(Serial, "espnow", dbLine);
#endif
}

void AppRuntime::processAsyncWarnings(bool& needPromptRefresh) {
#if HIGH_FREQUENCY_INCOMMING_ESPNOW
    static bool warnedAt80 = false;
    static bool warnedFull = false;

    const size_t dbCapacity = EspNowConfig::rxDbLogCapacity();
    if (dbCapacity > 0U) {
        const size_t dbPending = EspNowConfig::pendingRxDbLogCount();
        const size_t dbThreshold = (dbCapacity * kRxDbWarningPercent + 99U) / 100U;

        if (dbPending >= dbThreshold) {
            if (!warnedAt80) {
                char warnLine[220] = {0};
                std::snprintf(
                    warnLine,
                    sizeof(warnLine),
                    "warning: fila RX->DB em %lu/%lu (>= %u%%). use espnow -flush_db",
                    static_cast<unsigned long>(dbPending),
                    static_cast<unsigned long>(dbCapacity),
                    static_cast<unsigned>(kRxDbWarningPercent)
                );
                ShellOutput::printTagged(Serial, "espnow", warnLine);
                warnedAt80 = true;
                needPromptRefresh = true;
            }
        } else {
            warnedAt80 = false;
        }

        if (dbPending >= dbCapacity) {
            if (!warnedFull) {
                char fullLine[200] = {0};
                std::snprintf(
                    fullLine,
                    sizeof(fullLine),
                    "warning: fila RX->DB lotada (%lu/%lu), descartando mensagens recentes",
                    static_cast<unsigned long>(dbPending),
                    static_cast<unsigned long>(dbCapacity)
                );
                ShellOutput::printTagged(Serial, "espnow", fullLine);
                warnedFull = true;
                needPromptRefresh = true;
            }
        } else {
            warnedFull = false;
        }
    } else {
        warnedAt80 = false;
        warnedFull = false;
    }
#else
    (void) needPromptRefresh;
#endif
}

void AppRuntime::flushPendingEspNowOutput(bool& needPromptRefresh) {
    const size_t flushed = EspNowConfig::flushRxDisplayLines(kRxDisplayFlushBurst);
    if (flushed > 0) {
        needPromptRefresh = true;
    }

    const uint32_t dropped = EspNowConfig::takeDroppedRxCount();
    if (dropped > 0) {
        char line[64] = {0};
        std::snprintf(line, sizeof(line), "rx_dropped=%lu", static_cast<unsigned long>(dropped));
        ShellOutput::printTagged(Serial, "espnow", line);
        needPromptRefresh = true;
    }

    const uint32_t overwrittenDisplay = EspNowConfig::takeOverwrittenRxDisplayCount();
    if (overwrittenDisplay > 0) {
        char line[72] = {0};
        std::snprintf(line, sizeof(line), "rx_display_overwritten=%lu", static_cast<unsigned long>(overwrittenDisplay));
        ShellOutput::printTagged(Serial, "espnow", line);
        needPromptRefresh = true;
    }

    const uint32_t droppedDb = EspNowConfig::takeDroppedRxDbLogCount();
    if (droppedDb > 0) {
        char line[64] = {0};
        std::snprintf(line, sizeof(line), "rx_db_dropped=%lu", static_cast<unsigned long>(droppedDb));
        ShellOutput::printTagged(Serial, "espnow", line);
        needPromptRefresh = true;
    }
}

void AppRuntime::handleShellInput() {
    String command;
    if (!serialShell_.readInputLine(command)) {
        return;
    }

    const std::string response = ShellConfig::runLine(std::string(command.c_str()));
    if (!response.empty()) {
        ShellOutput::printResponse(Serial, response);
    }

    serialShell_.refreshLine();
}

void AppRuntime::begin() {
    BoardConfig::initBoardPins(false);

    serialShell_.begin(Serial, BAUDRATE);
    serialShell_.setPrompt(ShellOutput::commandPrompt());

    StartupConfig::waitForSerialAndAnimateLed(donglePeripherals_);
    donglePeripherals_.beginSd(false);

    ShellOutput::printTagged(Serial, "startup", String("mac=") + WiFi.macAddress());
    StartupConfig::promptAndSetDateTime(Serial);

    EspNowConfig::attachCallbacks(espNowManager_, Serial, &databaseStore_, &lcdTerminal_);

    const bool asyncRxEnabled = EspNowConfig::enableAsyncRx(RX_ASYNC_QUEUE_DEPTH);
    if (!asyncRxEnabled) {
        ShellOutput::printTagged(Serial, "espnow", "async queue init failed (fallback no callback)");
    }

    if (!espNowManager_.begin(0, false)) {
        ShellOutput::printTagged(Serial, "espnow", "init failed");
        return;
    }

    if (!databaseStore_.begin(espNowManager_, &Serial)) {
        ShellOutput::printTagged(Serial, "database", "init failed (continuando sem persistencia)");
    } else {
        databaseStore_.logBootEvent("power_on");
    }

    startEspNowWorkers(asyncRxEnabled);

    const bool shellBound = ShellConfig::bind({
        &tinyShell_,
        &espNowManager_,
        &donglePeripherals_,
        &lcdTerminal_,
        &databaseStore_,
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
    serialShell_.refreshLine();
}

void AppRuntime::tick() {
    bool needPromptRefresh = false;

    processAsyncWarnings(needPromptRefresh);
    flushPendingEspNowOutput(needPromptRefresh);

    if (needPromptRefresh) {
        serialShell_.refreshLine();
        needPromptRefresh = false;
    }

    handleShellInput();
    flushPendingEspNowOutput(needPromptRefresh);

    if (needPromptRefresh) {
        serialShell_.refreshLine();
    }

    delay(1);
}
