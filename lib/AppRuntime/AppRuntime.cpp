#include "AppRuntime.h"

#include <WiFi.h>
#include <Esp.h>
#include <esp_random.h>
#include <esp_system.h>
// This is arduino-esp32 on IDF 4.4: the app descriptor lives in
// esp_app_format.h (esp_app_desc_t) + esp_ota_ops.h (the accessor is
// esp_ota_get_app_description(), not IDF 5.x's esp_app_get_description()).
#include <esp_app_format.h>
#include <esp_chip_info.h>
#include <esp_ota_ops.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

// processAsyncWarnings: min gap between two console lines of the SAME warning
// category, so a sustained loss (a robot stuck on the wrong key spraying
// STATUS at 1Hz) is one line every few seconds, not a flood.
constexpr uint32_t kWarnThrottleMs = 5000U;

// hub.peers "online" (plano 36 fase 5.3): a peer conta como online somente se
// o dongle autenticou um frame de administracao canal C/AEAD-L nos ultimos
// kPeerOnlineWindowMs. Trafego endpoint B continua sendo relayado opaco, mas
// nao cria identidade nem renova presenca. Um robo vivo manda STATUS C a cada
// ~500ms (bally_OS kStatusPeriodUs); 4s tolera varias perdas seguidas de
// STATUS (a RF perto dos motores derruba frames) sem deixar a bolinha piscar,
// e ainda respeita o criterio "offline <=4s" da fase 5. A transicao para
// offline e publicada sozinha: peersFingerprint() inclui "online", e
// DonglePublisher publica hub.peers on-change (fase 2.C, sem codigo extra).
constexpr uint32_t kPeerOnlineWindowMs = 4000U;

// Same POSIX timezone used by bally_OS's default RobotSettings. The system
// clock itself is Unix epoch/UTC; this only defines how manual set_clock
// calendar input and every localtime_r() presentation (LCD, database, shell)
// map that epoch to Bally's local wall clock.
constexpr char kDongleTimezone[] = "BRT3";

// This dongle's own MANIFEST_DATA source_info block (BTP/docs/commands.md
// 3.12), serialized once at boot and handed to ManifestCache::configure().
// Static so its bytes outlive begin(); ManifestCache only borrows the
// pointer. buildDongleSourceInfo() fills it from esp_app_get_description(),
// esp_chip_info() and the running OTA partition -- all boot-stable, so a
// fixed snapshot is enough (unlike a robot's name/description, this dongle
// has no runtime-editable identity yet).
std::uint8_t g_dongleSourceInfo[256];
std::size_t g_dongleSourceInfoSize = 0U;

void buildDongleSourceInfo() {
    const esp_app_desc_t* app = esp_ota_get_app_description();
    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    const esp_partition_t* running = esp_ota_get_running_partition();

    static char scratch[48];
    char* cursor = scratch;
    char* const end = scratch + sizeof(scratch);
    const char* built = "";
    const char* chipRev = "";
    if (app != nullptr) {
        const int n = std::snprintf(cursor, static_cast<size_t>(end - cursor),
                                    "%s %s", app->date, app->time);
        if (n > 0 && cursor + n + 1 <= end) { built = cursor; cursor += n + 1; }
    }
    {
        const int n = std::snprintf(cursor, static_cast<size_t>(end - cursor),
                                    "%u.%u", chip.revision / 100U, chip.revision % 100U);
        if (n > 0 && cursor + n + 1 <= end) { chipRev = cursor; cursor += n + 1; }
    }

    // Arduino-ESP32 leaves esp_app_desc.version a placeholder ("1"); a real
    // git rev is injected as -DDONGLE_GIT_REV by scripts/inject_git_rev.py.
#ifdef DONGLE_GIT_REV
    const char* fwVersion = DONGLE_GIT_REV;
#else
    const char* fwVersion = (app != nullptr) ? app->version : "";
#endif

    const ManifestCache::SourceInfoEntry entries[] = {
        {"description", "Description", "Bally hub dongle"},
        {"model", "Model", "T-Dongle-S3"},
        {"fw_version", "Firmware", fwVersion},
        {"fw_build", "Built", built},
        {"idf_version", "ESP-IDF", (app != nullptr) ? app->idf_ver : ""},
        {"chip", "Chip", "ESP32-S3"},
        {"chip_revision", "Chip revision", chipRev},
        {"ota_partition", "Running partition", (running != nullptr) ? running->label : ""},
    };
    g_dongleSourceInfoSize = ManifestCache::serializeSourceInfo(
        entries, sizeof(entries) / sizeof(entries[0]), g_dongleSourceInfo, sizeof(g_dongleSourceInfo));
}

// Deferred SQLite open (topico 35 C.3): first try this many ms into uptime
// (let begin()'s allocations settle), then retry at this cadence, at most
// this many times before giving up and running without persistence.
// While a BTP session is negotiating/active the open is held off -- its
// bootstrap I/O would stall the main loop mid-handshake -- but only until
// kDatabaseForceMs, so a permanently-attached desktop still gets persistence.
constexpr uint32_t kDatabaseFirstAttemptMs = 400U;
constexpr uint32_t kDatabaseRetryMs = 3000U;
constexpr uint32_t kDatabaseForceMs = 10000U;
constexpr uint8_t kDatabaseMaxAttempts = 5U;

// Bisecting the ~200KB drop seen between SD mount and espNowManager_.begin()
// in the field (topico 33): each call point marks one candidate allocator
// (SQLite open/bootstrap, SerialMux, UsbHidMux, the async RX queue) so the
// boot log pinpoints which one is responsible instead of just bracketing
// the whole block. Compiled out unless -DDIAG_BOOT (platformio.ini) -- see
// topico 35 D.3; a normal boot must not print a dozen heap lines.
void logFreeHeap(const char* label) {
#ifdef DIAG_BOOT
    char line[64] = {0};
    std::snprintf(line, sizeof(line), "%s free_heap=%lu min_free=%lu", label,
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(ESP.getMinFreeHeap()));
    ShellOutput::printTagged(Serial, "heap", line);
#else
    (void) label;
#endif
}

// Why the LAST reset happened. On a native-USB (OTG CDC) board the panic
// handler's own output goes to USB-Serial/JTAG, not to this CDC port, so a
// crash looks like a silent reboot here -- this line is the one piece of the
// panic that survives, printed on the NEXT boot. ESP_RST_BROWNOUT = power,
// ESP_RST_PANIC = code crash (look at USB-Serial/JTAG for the backtrace),
// ESP_RST_INT_WDT/TASK_WDT = a task hogged the CPU or blocked.
const char* resetReasonText(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:  return "POWERON";
        case ESP_RST_SW:       return "SW";
        case ESP_RST_PANIC:    return "PANIC (crash - see USB-Serial/JTAG)";
        case ESP_RST_INT_WDT:  return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT:      return "OTHER_WDT";
        case ESP_RST_BROWNOUT: return "BROWNOUT (power)";
        case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
        case ESP_RST_SDIO:     return "SDIO";
        default:               return "UNKNOWN";
    }
}

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
    if (!databaseStore_.readRecentCommands(ShellLineEditor::DEFAULT_HISTORY_CAPACITY, historyText)) {
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
            serialShell_.addHistory(std::string(line.c_str()));
            // Topico 19: the BTP terminal channel has its own ShellLineEditor
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

void AppRuntime::maybeInitDatabase() {
    if (databaseReady_) {
        return; // the one-time post-open load below already ran
    }

    if (!databaseStore_.isReady()) {
        if (databaseInitAttempts_ >= kDatabaseMaxAttempts) {
            return; // gave up; DB methods keep failing closed, same as an SD-less boot
        }

        const uint32_t now = millis();
        if (databaseInitAttempts_ == 0) {
            if (now < kDatabaseFirstAttemptMs) {
                return; // let begin()'s SerialMux/ESP-NOW/WiFi allocations settle first
            }
        } else if (now - lastDatabaseInitMs_ < kDatabaseRetryMs) {
            return;
        }

        // Never run the bootstrap I/O in the middle of a BTP handshake/session
        // -- it blocks tick() for tens of ms. Wait for a console-owned moment,
        // but give up waiting at kDatabaseForceMs so an always-connected
        // desktop is not a reason persistence never comes up.
        if (!SerialMux::isConsoleOwned() && now < kDatabaseForceMs) {
            return;
        }

        ++databaseInitAttempts_;
        lastDatabaseInitMs_ = now;

        if (!databaseStore_.begin(&Serial)) {
            // DatabaseStore already logged the specific failure.
            if (databaseInitAttempts_ >= kDatabaseMaxAttempts) {
                ShellOutput::printTagged(Serial, "database",
                    "init adiado desistiu -- seguindo sem persistencia");
            }
            return;
        }
    }

    // The DB is open -- this call opened it, or a shell command (dongle
    // -sd_wipe) did while we were still retrying. Run the one-time boot load.
    databaseReady_ = true;
    databaseStore_.logBootEvent("power_on");
    if (!databaseStore_.loadPeers(espNowManager_)) {
        ShellOutput::printTagged(Serial, "database",
            "banco aberto, mas falhou carga inicial de peers");
    }
    restoreShellHistoryFromDatabase();
    ShellOutput::printTagged(Serial, "database", "persistencia pronta");
    logFreeHeap("after_database_lazy"); // steady-state heap, DB now resident
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
    // 10240, not the original 6144: processRxDatagram -> RadioSeal::open ->
    // btp::aead_open_aes_gcm puts an mbedtls_gcm_context plus a
    // RoutedMessage (payload[616]) and a plaintext[616] on this task's
    // stack. 6144 predates the channel-C AEAD work (topicos 28-31) and is
    // below Espressif's ~8 KB guidance for a task that touches mbedtls --
    // one of the suspects for the boot-time reset loop.
    const BaseType_t rxTaskOk = xTaskCreatePinnedToCore(
        espNowRxWorkerTask,
        "espnow_rx",
        10240,
        nullptr,
        2,
        &espNowRxTaskHandle_,
        workerCore
    );

    if (rxTaskOk != pdPASS) {
        EspNowConfig::disableAsyncRx();
        // No worker means no one drains the RX queue, so onDataRecv now drops
        // (it must not process inline on the WiFi stack). Radio is deaf.
        ShellOutput::printTagged(Serial, "espnow",
            "ALERTA rx worker nao criou (heap) -- datagramas de radio serao DESCARTADOS");
        return;
    }

    char rxLine[48] = {0};
    std::snprintf(rxLine, sizeof(rxLine), "rx task core=%d", static_cast<int>(workerCore));
    ShellOutput::printTagged(Serial, "espnow", rxLine);
}

void AppRuntime::startHeartbeatWorker() {
    const BaseType_t workerCore = selectEspNowWorkerCore();
    // 8192, not the original 4096: heartbeatTick() now seals every beat with
    // key L (topico 30) -> mbedtls AES-GCM on this stack, up to 5x/s
    // (HEARTBEAT_INTERVAL_MS). 4096 was sized before the seal existed.
    const BaseType_t heartbeatTaskOk = xTaskCreatePinnedToCore(
        espNowHeartbeatWorkerTask,
        "espnow_heartbeat",
        8192,
        nullptr,
        1,
        &espNowHeartbeatTaskHandle_,
        workerCore
    );

    if (heartbeatTaskOk != pdPASS) {
        ShellOutput::printTagged(Serial, "espnow", "heartbeat task create failed");
    }
}

// Turns the drop counters the RX worker accumulates into per-category console
// lines, so "the robot is connected but nothing works" has a visible cause
// (plano 36 fase 4.4). SOLE consumer of the takeDropped*Count() family: each
// call advances a watermark, so a second reader elsewhere would steal half
// the deltas -- flushPendingEspNowOutput used to report a lump `rx_dropped=`
// sum here and no longer does.
//
// Console-owned only, same constraint as every other diagnostic line this
// firmware prints: a BTP-protocolled session owns the port exclusively
// (PASSO 11), and the counters that matter most while protocolled
// (droppedDecode/Reassembly/Crc/QueueFull) are already in the hub.link topic
// a desktop can plot. droppedAuth and the manifest-reject counters are not on
// the wire yet -- `hub -manifest` and `espnow -stats` are where a protocolled
// session reads those.
void AppRuntime::processAsyncWarnings(bool& needPromptRefresh) {
    // Delta since last tick for the five wire-schema drop counters + the
    // auth-fail counter. Drained here unconditionally (watermark must keep
    // moving even while protocolled) and fed to the LCD's ERR tile.
    const uint32_t rxq    = EspNowConfig::takeDroppedRxCount();
    const uint32_t decode = EspNowConfig::takeDroppedDecodeCount();
    const uint32_t crc    = EspNowConfig::takeDroppedCrcCount();
    const uint32_t reasm  = EspNowConfig::takeDroppedReassemblyCount();
    const uint32_t queue  = EspNowConfig::takeDroppedQueueFullCount();
    const uint32_t auth   = EspNowConfig::takeDroppedAuthCount();

    const uint32_t lcdTotal = rxq + decode + crc + reasm + queue + auth;
    if (lcdTotal > 0) {
        lcdDashboard_.notifyDropped(lcdTotal);
    }

    // Manifest-path rejections and the heap-starved-RX drop are cumulative
    // totals with no delta accessor -- keep our own watermarks.
    const uint32_t consumeRejTotal = ManifestCache::consumeRejectedTotal();
    const uint32_t ingestFailTotal = ManifestCache::ingestFailedTotal();
    const uint32_t syncFb = EspNowConfig::peekSyncFallbackDropCount();
    static uint32_t lastConsumeRejected = 0;
    static uint32_t lastIngestFailed = 0;
    static uint32_t lastSyncFallback = 0;
    const uint32_t consumeRej = consumeRejTotal - lastConsumeRejected;
    const uint32_t ingestFail = ingestFailTotal - lastIngestFailed;
    const uint32_t syncFbDelta = syncFb - lastSyncFallback;
    lastConsumeRejected = consumeRejTotal;
    lastIngestFailed = ingestFailTotal;
    lastSyncFallback = syncFb;

    if (!SerialMux::isConsoleOwned()) {
        return;
    }

    EspNowConfig::RxCounters rx{};
    EspNowConfig::peekRxCounters(rx);
    const uint32_t nowMs = millis();

    struct WarnCategory {
        const char* label;
        uint32_t delta;
        uint32_t total;
        uint32_t* lastEmitMs;
    };
    static uint32_t emitAuth = 0, emitReasm = 0, emitDecode = 0, emitQueue = 0,
                    emitConsume = 0, emitIngest = 0, emitSync = 0;
    // rxq is deliberately absent: a full raw-datagram queue is a burst event
    // the LCD tile and "queue_full" (routed) already cover, and itemizing it
    // adds noise without a distinct fix.
    const WarnCategory cats[] = {
        {"auth_drop chave-L", auth, EspNowConfig::peekDroppedAuthCount(), &emitAuth},
        {"reassembly_drop", reasm, rx.droppedReassembly, &emitReasm},
        {"decode_drop", decode, rx.droppedDecode, &emitDecode},
        {"queue_full", queue, rx.droppedQueueFull, &emitQueue},
        {"manifest_reject ref", consumeRej, consumeRejTotal, &emitConsume},
        {"manifest_ingest_fail", ingestFail, ingestFailTotal, &emitIngest},
        {"rx_async_down heap", syncFbDelta, syncFb, &emitSync},
    };

    for (const WarnCategory& c : cats) {
        if (c.delta == 0) {
            continue;
        }
        if (*c.lastEmitMs != 0 && (nowMs - *c.lastEmitMs) < kWarnThrottleMs) {
            continue;
        }
        *c.lastEmitMs = (nowMs == 0) ? 1U : nowMs;
        char line[96] = {0};
        std::snprintf(line, sizeof(line), "%s +%lu total=%lu", c.label,
                      static_cast<unsigned long>(c.delta), static_cast<unsigned long>(c.total));
        ShellOutput::printTagged(Serial, "hub warn", line);
        needPromptRefresh = true;
    }
}

void AppRuntime::flushPendingEspNowOutput(bool& needPromptRefresh) {
    const size_t flushed = EspNowConfig::drainRoutedQueues(kRxDisplayFlushBurst);
    if (flushed > 0 && SerialMux::isConsoleOwned()) {
        needPromptRefresh = true;
    }
    // The drop counters (and the LCD ERR tile + per-category console lines
    // they feed) moved to processAsyncWarnings() -- it is the single reader of
    // the takeDropped*Count() watermarks now.
}

void AppRuntime::flushEditorOutput() {
    if (editorOut_.empty()) {
        return;
    }
    Serial.write(reinterpret_cast<const uint8_t*>(editorOut_.data()), editorOut_.size());
    editorOut_.clear();
}

void AppRuntime::handleShellInput() {
    // SerialMux (topico 13) owns the port instead of the line editor once a
    // BTP v1 session is negotiating/protocolled -- its own tick() call below
    // does the reading/dispatch in that case.
    if (!SerialMux::isConsoleOwned()) {
        return;
    }

    // Hand every byte the host typed to the editor. It buffers them and
    // echoes into editorOut_ (drained by flushEditorOutput); poll() then
    // yields whole command lines as they complete.
    while (Serial.available() > 0) {
        const int value = Serial.read();
        if (value < 0) {
            break;
        }
        const uint8_t byte = static_cast<uint8_t>(value);
        serialShell_.feed(&byte, 1);
    }

    std::string command;
    while (serialShell_.poll(editorOut_, &command)) {
        flushEditorOutput();

        // "BTP/1 ENTER <16 hex>" is a reserved control line, not a TinyShell
        // command: on match, SerialMux already wrote "BTP/1 READY ...\r\n" and
        // took over the port.
        if (SerialMux::tryEnterFromConsoleLine(command.c_str(), millis())) {
            return;
        }

        const std::string response = ShellConfig::runLine(command);

        // A command itself may have entered protocol mode (e.g. "dongle
        // -btp_v1"): do not print a stray response/prompt into the middle of
        // a handshake, and stop draining -- SerialMux owns the port now.
        if (!SerialMux::isConsoleOwned()) {
            return;
        }

        if (!response.empty()) {
            ShellOutput::printResponse(Serial, response);
        }
        serialShell_.refreshLine(editorOut_);
        flushEditorOutput();
    }

    // Echo of characters that did not complete a line this tick.
    flushEditorOutput();
}

void AppRuntime::begin() {
    setenv("TZ", kDongleTimezone, 1);
    tzset();

    // Must run before the USB stack comes up (ARDUINO_USB_CDC_ON_BOOT=1
    // starts it as soon as Serial is touched below) -- tinyusb reads these
    // strings when building the descriptor for host enumeration.
    USB.productName("Bally Dongle");
    USB.manufacturerName("Bally");

    BoardConfig::initBoardPins(false);

    // The host can pile up several "BTP/1 ENTER" retry lines (~30 B each)
    // while begin() runs, before the shell loop starts reading -- and a HELLO
    // frame on top of that. The CDC's default 256-byte RX queue is tight for
    // that; 1 KB gives margin. Must precede Serial.begin() (topico 35 C.5).
    Serial.setRxBufferSize(1024);

    #ifdef BAUDRATE
        Serial.begin(BAUDRATE);
    #else
        Serial.begin();
    #endif

    // Seed the prompt text without painting -- the boot log below and the
    // refreshLine() at the end of begin() do the first repaint.
    serialShell_.setPrompt(std::string(ShellOutput::commandPrompt().c_str()));

    // Opt-in hardening for the field: with USB auto-reset off, no DTR/RTS
    // dance a host driver happens to perform -- deliberately or not -- can
    // bounce the running firmware into the bootloader mid-session (the
    // esptool reset sequence and the 1200-baud touch both go through
    // USBCDC::reboot_enable). The cost is that `pio run -t upload` can no
    // longer auto-enter the bootloader, so a build with this set is flashed
    // with the BOARD's BOOT button held. Left off in the dev env on purpose;
    // enable with -DDONGLE_USB_NO_AUTORESET for a field/demo build.
    #ifdef DONGLE_USB_NO_AUTORESET
        Serial.enableReboot(false);
    #endif

    // Under -DDIAG_BOOT: hold long enough for the freshly reset OTG CDC to
    // re-enumerate on the host so a monitor reattaches and catches this boot's
    // first lines. That hold is pure blackout on the BTP handshake budget, so
    // a normal build skips it (topico 35 C.3/D.3).
#ifdef DIAG_BOOT
    delay(2000);
#endif
    // Always on, and cheap: the one line that survives a panic (whose own
    // output goes to USB-Serial/JTAG, not this CDC port). A "PANIC" here on
    // successive boots is the boot-loop regression this whole topico guards.
    {
        char line[80] = {0};
        std::snprintf(line, sizeof(line), "last_reset=%s", resetReasonText(esp_reset_reason()));
        ShellOutput::printTagged(Serial, "boot", line);
    }

    StartupConfig::announceBoot(donglePeripherals_);
    donglePeripherals_.beginSd(false);

    ShellOutput::printTagged(Serial, "startup", String("mac=") + WiFi.macAddress());

    logFreeHeap("after_sd");

    // SQLite is NOT opened here any more (topico 35 C.3). Its bootstrap SQL
    // has a 30-40KB transient page-cache/parser footprint, and running that
    // inside begin() -- next to SerialMux's 40KB of queues, ESP-NOW's ~47KB
    // and the WiFi driver's ~52KB -- is what squeezed min_free to ~18KB on
    // the bench and put an OOM on the boot critical path. It now opens from
    // tick() a beat after boot (maybeInitDatabase), with ~40KB of slack, and
    // a failure there no longer stops the dongle reaching its prompt. Nothing
    // in the boot path needs the DB: ManifestCache/hub.peers are RAM-only and
    // onDataRecv re-adds a peer on its first radio frame anyway.

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
    buildDongleSourceInfo();
    ManifestCache::configure(selfUuid, g_dongleSourceInfo, g_dongleSourceInfoSize);
    SerialMux::begin(Serial, [](const char* cmd, const char* source, const char* userId, std::string* out) {
        ShellConfig::runLine(std::string(cmd), source, out, userId);
    }, selfUuid, ShellOutput::commandPrompt().c_str(), EspNowConfig::sendRawToMac);

    logFreeHeap("after_serialmux_begin");

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
        out.syncFallbackDrops = EspNowConfig::peekSyncFallbackDropCount();

        EspNowManager::TxSchedulerCounters radioTx{};
        EspNowConfig::peekTxSchedulerCounters(radioTx);
        for (size_t i = 0U; i < 3U; ++i) {
            out.txEnqueued[i] = radioTx.enqueued[i];
            out.txDroppedQueueFull[i] = radioTx.droppedQueueFull[i];
        }
        out.txDriverRejected = radioTx.driverRejected;
        out.txCallbackTimeouts = radioTx.callbackTimeouts;
        out.txCallbacksReceived = radioTx.callbacksReceived;

        SerialMux::TxCounters tx{};
        SerialMux::peekTxCounters(tx);
        out.framesRx = tx.framesRx;
        out.framesTx = tx.framesTx;
        out.framesTxStalled = tx.framesTxStalled;
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
        out.usb.relayDownOk = tx.relayDownOk;
        out.usb.relayDownUnbound = tx.relayDownUnbound;
        out.usb.relayDownNoPeer = tx.relayDownNoPeer;
        out.usb.relayDownOversized = tx.relayDownOversized;
        out.usb.relayDownSendFailed = tx.relayDownSendFailed;

        BtpTransport::PeerSnapshot peers[BtpTransport::kPeerIdentityCapacity];
        const size_t peerCount = BtpTransport::enumeratePeers(peers, BtpTransport::kPeerIdentityCapacity);
        const uint32_t nowMs = millis();
        for (size_t i = 0; i < peerCount; ++i) {
            out.peers[i].sourceId = peers[i].sourceId;
            out.peers[i].bootId = peers[i].bootId;
            std::memcpy(out.peers[i].mac, peers[i].mac, sizeof(out.peers[i].mac));
            // Unsigned wrap makes this the correct elapsed time even across
            // the ~49-day millis() rollover.
            const uint32_t ageMs = nowMs - peers[i].lastAuthenticatedMs;
            out.peers[i].lastSeenAgeMs = ageMs;
            // plano 36 fase 2.A: "ouvi algo dele ha pouco", nao o linkOk do
            // heartbeat (que so vale para o ultimo peer). Ver kPeerOnlineWindowMs.
            out.peers[i].online = (ageMs < kPeerOnlineWindowMs);
        }
        out.peerCount = peerCount;
    });

    // UsbHidMux (topico 20, hardware bring-up spike): the dongle's second
    // USB interface, active alongside the CDC/SerialMux link above on the
    // same composite device. No BTP framing yet -- see UsbHidMux.h.
    UsbHidMux::begin();

    logFreeHeap("after_usbhidmux_begin");

    EspNowConfig::attachCallbacks(espNowManager_, Serial, &databaseStore_, &lcdDashboard_);

    const bool asyncRxEnabled = EspNowConfig::enableAsyncRx(RX_ASYNC_QUEUE_DEPTH);
    if (!asyncRxEnabled) {
        // Heap starvation (topico 34/35 F2). The fallback is NOT inline
        // processing any more -- that would stack-overflow the WiFi task --
        // it is DROP: this dongle boots deaf to the radio. Loud on purpose.
        ShellOutput::printTagged(Serial, "espnow",
            "ALERTA async RX nao subiu (heap) -- datagramas de radio serao DESCARTADOS. ver 'espnow -stats'");
    }

    logFreeHeap("after_asyncrx_queue");

    if (!espNowManager_.begin(0, false)) {
        ShellOutput::printTagged(Serial, "espnow", "init failed");
        return;
    }

    logFreeHeap("after_espnow_begin");

    // Persisted peers load with the DB, from maybeInitDatabase() in tick().
    // Until then the manager runs with just the broadcast peer -- a known
    // robot re-registers itself on its first inbound frame (onDataRecv).

#ifndef DIAG_NO_ESPNOW_WORKERS
    startEspNowWorkers(asyncRxEnabled);
    startHeartbeatWorker();
#else
    (void) asyncRxEnabled;
    ShellOutput::printTagged(Serial, "boot", "DIAG: espnow rx/heartbeat workers DISABLED");
#endif

    logFreeHeap("after_workers");

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

    logFreeHeap("after_shell_bind");

    if (ShellConfig::registerDefaultModules() != RESULT_OK) {
        ShellOutput::printTagged(Serial, "shell", "module registration failed");
        return;
    }

    logFreeHeap("after_register_modules");

    const ShellLineEditor::CompletionProvider completionProvider =
        [this](const std::string& input, std::string* outSuggestions, size_t maxSuggestions) -> size_t {
        if (outSuggestions == nullptr || maxSuggestions == 0) {
            return 0;
        }

        const std::vector<std::string> matches = tinyShell_.complete_line(input, maxSuggestions);
        size_t written = 0;
        for (const std::string& candidate : matches) {
            if (written >= maxSuggestions) {
                break;
            }

            outSuggestions[written++] = candidate;
        }

        return written;
    };
    serialShell_.setCompletionProvider(completionProvider);
    // Same TinyShell::complete_line-backed Tab completion for the BTP
    // terminal session (topico 19) as the real console gets.
    SerialMux::setTerminalCompletionProvider(completionProvider);

    logFreeHeap("after_completion_providers");

    // Shell history also comes from the DB, so it too waits for
    // maybeInitDatabase() (tick()). The arrow-up buffer is simply empty for
    // the first fraction of a second of uptime.

    ShellOutput::printTagged(Serial, "shell", "ready: <module> -<command> [args]");
    ShellOutput::printTagged(Serial, "shell", "use: help -e");

    logFreeHeap("after_shell_ready");

    serialShell_.refreshLine(editorOut_);
    flushEditorOutput();
}

void AppRuntime::tick() {
    static uint32_t lastAsyncOutputTime = 0;
    static bool pendingPromptRefresh = false;
    
    bool asyncOutputOccurred = false;

    maybeInitDatabase(); // opens SQLite off the boot path; no-op once up
    processAsyncWarnings(asyncOutputOccurred);
    flushPendingEspNowOutput(asyncOutputOccurred);
    lcdDashboard_.tick();

    // Native USB CDC's bool conversion reflects the host DTR line. If the
    // desktop closes the COM port without completing SESSION_CLOSE (process
    // killed, cable pulled, OS error), release SerialMux immediately instead
    // of leaving it protocol-owned until the 30 s watchdog expires.
    if (!Serial) {
        SerialMux::onTransportLost(millis());
    }

    handleShellInput();
    flushPendingEspNowOutput(asyncOutputOccurred);

    // SerialMux (topico 13): pumps RX decode/dispatch, the watchdog and TX
    // queue draining while a BTP v1 session is negotiating/protocolled; a
    // fast no-op otherwise (the line editor owns the port instead, above).
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
        serialShell_.refreshLine(editorOut_);
        flushEditorOutput();
        pendingPromptRefresh = false;
    }

    delay(1);
}
