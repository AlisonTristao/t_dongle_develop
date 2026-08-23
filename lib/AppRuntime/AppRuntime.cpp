#include "AppRuntime.h"

#include <WiFi.h>
#include <Esp.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "config.h"
#include "StartupConfig.h"
#include "ShellConfig.h"
#include "EspNowConfig.h"
#include "EspNowCommands.h"
#include "DonglePublisher.h"
#include "ManifestCache.h"
#include "SerialMux.h"
#include "ShellOutput.h"
#include "BtpTransport.h"
#include "DongleKeyStore.h"
#include "UsbHidMux.h"
#include <USB.h>

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
            // Topico 19: the BTP terminal channel has its own ShellSerial
            // instance (SerialMux.cpp), so persisted history has to be
            // replayed into it explicitly too.
            SerialMux::addTerminalHistory(line.c_str());
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
    if (flushed > 0 && SerialMux::isConsoleOwned()) {
        needPromptRefresh = true;
    }

    uint32_t dropped = EspNowConfig::takeDroppedRxCount();
    dropped += EspNowConfig::takeDroppedDecodeCount();
    dropped += EspNowConfig::takeDroppedCrcCount();
    dropped += EspNowConfig::takeDroppedReassemblyCount();
    dropped += EspNowConfig::takeDroppedQueueFullCount();
    dropped += EspNowConfig::takeDroppedAuthCount();
    if (dropped > 0) {
        lcdDashboard_.notifyDropped(dropped);

        // A BTP-protocolled session owns the port exclusively (PASSO 11):
        // this diagnostic line is console-only and simply not emitted while
        // protocolled, same choice as ShellCommandSupport::printLine.
        if (SerialMux::isConsoleOwned()) {
            char line[64] = {0};
            std::snprintf(line, sizeof(line), "rx_dropped=%lu", static_cast<unsigned long>(dropped));
            ShellOutput::printTagged(Serial, "espnow", line);
            needPromptRefresh = true;
        }
    }
}

void AppRuntime::handleShellInput() {
    // SerialMux (topico 13) owns the port instead of ShellSerial once a BTP
    // v1 session is negotiating/protocolled -- its own tick() call below
    // does the reading/dispatch in that case.
    if (!SerialMux::isConsoleOwned()) {
        return;
    }

    String command;
    if (!serialShell_.readInputLine(command)) {
        return;
    }

    const std::string commandText(command.c_str());

    // "BTP/1 ENTER <16 hex>" is recognized as a reserved control line, not a
    // TinyShell command: on match, SerialMux already wrote "BTP/1 READY
    // ...\r\n" and took over the port.
    if (SerialMux::tryEnterFromConsoleLine(commandText.c_str(), millis())) {
        return;
    }

    const std::string response = ShellConfig::runLine(commandText);
    if (!response.empty() && SerialMux::isConsoleOwned()) {
        ShellOutput::printResponse(Serial, response);
    }

    // A command itself may have entered protocol mode (e.g. "dongle
    // -btp_v1"): do not print a stray prompt into the middle of a handshake.
    if (SerialMux::isConsoleOwned()) {
        serialShell_.refreshLine();
    }
}

void AppRuntime::begin() {
    // Must run before the USB stack comes up (ARDUINO_USB_CDC_ON_BOOT=1
    // starts it as soon as Serial is touched below) -- tinyusb reads these
    // strings when building the descriptor for host enumeration.
    USB.productName("Bally Dongle");
    USB.manufacturerName("Bally");

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

    // Topico 29 passo 3: restores channel C's key L from NVS, if a previous
    // "hub -set_key_l" session ever persisted one, so the fleet key survives
    // a reboot without the operator retyping the password every time. Silent
    // when nothing was ever saved -- every radio send/open then fails closed
    // (RadioSeal.h) until "hub -set_key_l" runs.
    DongleKeyStore::loadFromNvs();

    // SerialMux (topico 13): the port's single writer once a BTP v1 session
    // negotiates on this same USB link. selfUuid still has no separate
    // persisted identity store (topico 16 did not add one either) -- MAC
    // bytes plus a fixed, non-zero suffix keep it stable for the boot and
    // satisfy HELLO_RESULT's "peer_uuid nao pode ser toda zero" without
    // inventing real UUID storage. The same bytes double as this dongle's
    // own MANIFEST_DATA source_uuid (topico 16 PASSO 3/5) -- one identity,
    // reused by both, not two independent UUIDs to keep in sync.
    uint8_t selfUuid[16] = {0};
    std::memcpy(selfUuid, selfMac, 6);
    for (size_t i = 6; i < sizeof(selfUuid); ++i) {
        selfUuid[i] = static_cast<uint8_t>(0xB0 + i);
    }
    ManifestCache::configure(selfUuid);
    SerialMux::begin(Serial, [](const char* cmd, const char* source, const char* userId, std::string* out) {
        ShellConfig::runLine(std::string(cmd), source, out, userId);
    }, selfUuid, ShellOutput::commandPrompt().c_str(), EspNowConfig::sendRawToMac);

    // "espnow -stats" reads counters owned by two libraries that must not
    // depend on the command module (EspNowConfig.h from EspNowCommands would
    // close EspNowCommands -> EspNowConfig -> ShellConfig -> EspNowCommands).
    // AppRuntime is the only layer already holding both, so it binds the
    // provider -- same boundary as the SerialMux::begin callbacks above.
    EspNowCommands::setStatsProvider([](EspNowCommands::StatsSnapshot& out) {
        EspNowConfig::RxCounters rx{};
        EspNowConfig::peekRxCounters(rx);
        out.datagrams = rx.datagrams;
        out.fragmentsAccepted = rx.fragmentsAccepted;
        out.routedTelemetry = rx.routedTelemetry;
        out.routedLog = rx.routedLog;
        out.routedCommand = rx.routedCommand;
        out.routedControl = rx.routedControl;
        out.routedTerminal = rx.routedTerminal;
        out.droppedRx = rx.droppedRx;
        out.droppedDecode = rx.droppedDecode;
        out.droppedCrc = rx.droppedCrc;
        out.droppedReassembly = rx.droppedReassembly;
        out.droppedQueueFull = rx.droppedQueueFull;
        out.droppedAuth = EspNowConfig::peekDroppedAuthCount();

        SerialMux::TxCounters tx{};
        SerialMux::peekTxCounters(tx);
        out.framesTx = tx.framesTx;
        out.telemetryDropped = tx.telemetryDropped;
        out.droppedSession = tx.droppedByClass[static_cast<size_t>(SerialSession::PriorityClass::kSession)];
        out.droppedTerminal = tx.droppedByClass[static_cast<size_t>(SerialSession::PriorityClass::kTerminal)];
        out.droppedLogStatus = tx.droppedByClass[static_cast<size_t>(SerialSession::PriorityClass::kLogStatus)];
        out.droppedTelemetryQueue = tx.droppedByClass[static_cast<size_t>(SerialSession::PriorityClass::kTelemetry)];
        out.protocolled = SerialMux::isProtocolled();
    });

    // Topico 27: same boundary, same reason. DonglePublisher is pure C++ and
    // must not include EspNowConfig.h/SerialMux.h (ManifestCache and SerialMux
    // both include *it*, so the edge would close a cycle -- CONTRIBUTING.md
    // section 3). AppRuntime is the only layer already holding all three, so
    // it binds the one provider that gathers everything a publish cycle needs.
    // Called at most once per SerialMux tick, and only while some hub.* topic
    // has a subscriber.
    DonglePublisher::setSnapshotProvider([](DonglePublisher::Snapshot& out) {
        EspNowConfig::RxCounters rx{};
        EspNowConfig::peekRxCounters(rx);
        out.link.datagrams = rx.datagrams;
        out.link.fragmentsAccepted = rx.fragmentsAccepted;
        out.link.routedTelemetry = rx.routedTelemetry;
        out.link.routedLog = rx.routedLog;
        out.link.routedCommand = rx.routedCommand;
        out.link.routedControl = rx.routedControl;
        out.link.routedTerminal = rx.routedTerminal;
        out.link.droppedRx = rx.droppedRx;
        out.link.droppedDecode = rx.droppedDecode;
        out.link.droppedCrc = rx.droppedCrc;
        out.link.droppedReassembly = rx.droppedReassembly;
        out.link.droppedQueueFull = rx.droppedQueueFull;

        SerialMux::TxCounters tx{};
        SerialMux::peekTxCounters(tx);
        out.usb.framesRx = tx.framesRx;
        out.usb.framesTx = tx.framesTx;
        out.usb.crcErrors = tx.crcErrors;
        out.usb.decodeErrors = tx.decodeErrors;
        out.usb.reassemblyRejected = tx.reassemblyRejected;
        out.usb.telemetryDropped = tx.telemetryDropped;
        for (size_t i = 0; i < DonglePublisher::kUsbDropClassCount; ++i) {
            out.usb.droppedByClass[i] = tx.droppedByClass[i];
        }

        BtpTransport::PeerSnapshot peers[BtpTransport::kPeerIdentityCapacity];
        const size_t peerCount = BtpTransport::enumeratePeers(peers, BtpTransport::kPeerIdentityCapacity);
        const uint32_t nowMs = millis();
        for (size_t i = 0; i < peerCount; ++i) {
            out.peers[i].sourceId = peers[i].sourceId;
            out.peers[i].bootId = peers[i].bootId;
            std::memcpy(out.peers[i].mac, peers[i].mac, sizeof(out.peers[i].mac));
            // Unsigned wrap makes this the correct elapsed time even across
            // the ~49-day millis() rollover.
            out.peers[i].lastSeenAgeMs = nowMs - peers[i].lastSeenMs;
            out.peers[i].online = peers[i].linkOk;
        }
        out.peerCount = peerCount;
    });

    // UsbHidMux (topico 20, hardware bring-up spike): the dongle's second
    // USB interface, active alongside the CDC/SerialMux link above on the
    // same composite device. No BTP framing yet -- see UsbHidMux.h.
    UsbHidMux::begin();

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

    const ShellSerial::CompletionProvider completionProvider =
        [this](const String& input, String* outSuggestions, size_t maxSuggestions) -> size_t {
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
    };
    serialShell_.setCompletionProvider(completionProvider);
    // Same TinyShell::complete_line-backed Tab completion for the BTP
    // terminal session (topico 19) as the real console gets.
    SerialMux::setTerminalCompletionProvider(completionProvider);

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

    // SerialMux (topico 13): pumps RX decode/dispatch, the watchdog and TX
    // queue draining while a BTP v1 session is negotiating/protocolled; a
    // fast no-op otherwise (ShellSerial owns the port instead, above).
    SerialMux::tick(millis());

    // UsbHidMux (topico 20, hardware bring-up spike): drains the HID vendor
    // interface's echo loop; a fast no-op when nothing was received.
    UsbHidMux::tick();

    if (asyncOutputOccurred) {
        lastAsyncOutputTime = millis();
        pendingPromptRefresh = true;
    }

    // Aguarda 150ms de "silencio" antes de redesenhar o prompt.
    // Isso permite que pacotes ESP-NOW fragmentados (ex: [1/5], [2/5])
    // sejam impressos de forma contigua sem que o prompt quebre a linha no meio.
    // Nunca roda fora do console: um SerialMux protocolado e o unico dono da
    // porta (PASSO 11).
    if (pendingPromptRefresh && SerialMux::isConsoleOwned() && (millis() - lastAsyncOutputTime > 150)) {
        Serial.println(); // Garante que o prompt inicie em uma linha limpa
        serialShell_.refreshLine();
        pendingPromptRefresh = false;
    }

    delay(1);
}
