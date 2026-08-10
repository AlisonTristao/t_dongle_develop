#include "AppRuntime.h"

#include <WiFi.h>
#include <Esp.h>
#include <esp_random.h>

#include <cstdio>
#include <vector>

#include "config.h"
#include "StartupConfig.h"
#include "ShellConfig.h"
#include "EspNowConfig.h"
#include "ShellOutput.h"
#include "BtpTransport.h"

namespace {

constexpr size_t kRxDisplayFlushBurst = 48;

} // namespace

void AppRuntime::espNowRxWorkerTask(void*) {
    EspNowConfig::RxDatagramEvent event = {};

    while (true) {
        if (EspNowConfig::dequeueRxDatagram(event, 250)) {
            EspNowConfig::processRxDatagram(event);
        }
    }
}

void AppRuntime::espNowHeartbeatWorkerTask(void*) {
    // Blocking send-with-status lives here, off the main loop, so a slow/absent
    // peer never stalls the shell. See HEARTBEAT_INTERVAL_MS in EspNowConfig.h.
    const TickType_t periodTicks = pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS);

    while (true) {
        EspNowConfig::heartbeatTick();
        vTaskDelay(periodTicks);
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
        2,
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
}

void AppRuntime::startHeartbeatWorker() {
    const BaseType_t workerCore = selectEspNowWorkerCore();
    const BaseType_t heartbeatTaskOk = xTaskCreatePinnedToCore(
        espNowHeartbeatWorkerTask,
        "espnow_heartbeat",
        4096,
        nullptr,
        1,
        &espNowHeartbeatTaskHandle_,
        workerCore
    );

    if (heartbeatTaskOk != pdPASS) {
        ShellOutput::printTagged(Serial, "espnow", "heartbeat task create failed");
    }
}

void AppRuntime::processAsyncWarnings(bool& needPromptRefresh) {
    (void) needPromptRefresh;
}

void AppRuntime::flushPendingEspNowOutput(bool& needPromptRefresh) {
    const size_t flushed = EspNowConfig::drainRoutedQueues(kRxDisplayFlushBurst);
    if (flushed > 0) {
        needPromptRefresh = true;
    }

    uint32_t dropped = EspNowConfig::takeDroppedRxCount();
    dropped += EspNowConfig::takeDroppedDecodeCount();
    dropped += EspNowConfig::takeDroppedCrcCount();
    dropped += EspNowConfig::takeDroppedReassemblyCount();
    dropped += EspNowConfig::takeDroppedQueueFullCount();
    if (dropped > 0) {
        char line[64] = {0};
        std::snprintf(line, sizeof(line), "rx_dropped=%lu", static_cast<unsigned long>(dropped));
        ShellOutput::printTagged(Serial, "espnow", line);
        lcdDashboard_.notifyDropped(dropped);
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

    #ifdef BAUDRATE
        serialShell_.begin(Serial, BAUDRATE);
    #else
        serialShell_.begin(Serial);
    #endif

    serialShell_.setPrompt(ShellOutput::commandPrompt());

    StartupConfig::waitForSerialAndAnimateLed(donglePeripherals_);
    donglePeripherals_.beginSd(false);

    ShellOutput::printTagged(Serial, "startup", String("mac=") + WiFi.macAddress());
    StartupConfig::promptAndSetDateTime(Serial);

    // BTP identity: source_id derived from this dongle's own MAC (same
    // formula every firmware in the ecosystem uses, so it needs no
    // handshake); boot_id is a random nonzero value for this boot only --
    // there is no HELLO/MANIFEST yet (topico 16) to persist/announce it, and
    // nothing here requires it to survive a reboot.
    uint8_t selfMac[6] = {0};
    WiFi.macAddress(selfMac);
    uint32_t bootId = esp_random();
    if (bootId == 0) {
        bootId = 1;
    }
    BtpTransport::configureIdentity(BtpTransport::btp_command::source_id_from_mac(selfMac), bootId);

    EspNowConfig::attachCallbacks(espNowManager_, Serial, &databaseStore_, &lcdDashboard_);

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
    startHeartbeatWorker();

    const bool shellBound = ShellConfig::bind({
        &tinyShell_,
        &espNowManager_,
        &donglePeripherals_,
        &lcdDashboard_,
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

    serialShell_.setCompletionProvider([this](const String& input, String* outSuggestions, size_t maxSuggestions) -> size_t {
        if (outSuggestions == nullptr || maxSuggestions == 0) {
            return 0;
        }

        const std::vector<std::string> matches = tinyShell_.complete_line(std::string(input.c_str()), maxSuggestions);
        size_t written = 0;
        for (const std::string& candidate : matches) {
            if (written >= maxSuggestions) {
                break;
            }

            outSuggestions[written++] = String(candidate.c_str());
        }

        return written;
    });

    restoreShellHistoryFromDatabase();

    ShellOutput::printTagged(Serial, "shell", "ready: <module> -<command> [args]");
    ShellOutput::printTagged(Serial, "shell", "use: help -e");
    serialShell_.refreshLine();
}

void AppRuntime::tick() {
    static uint32_t lastAsyncOutputTime = 0;
    static bool pendingPromptRefresh = false;
    
    bool asyncOutputOccurred = false;

    processAsyncWarnings(asyncOutputOccurred);
    flushPendingEspNowOutput(asyncOutputOccurred);
    lcdDashboard_.tick();

    handleShellInput();
    flushPendingEspNowOutput(asyncOutputOccurred);

    if (asyncOutputOccurred) {
        lastAsyncOutputTime = millis();
        pendingPromptRefresh = true;
    }

    // Aguarda 150ms de "silencio" antes de redesenhar o prompt.
    // Isso permite que pacotes ESP-NOW fragmentados (ex: [1/5], [2/5]) 
    // sejam impressos de forma contigua sem que o prompt quebre a linha no meio.
    if (pendingPromptRefresh && (millis() - lastAsyncOutputTime > 150)) {
        Serial.println(); // Garante que o prompt inicie em uma linha limpa
        serialShell_.refreshLine();
        pendingPromptRefresh = false;
    }

    delay(1);
}
