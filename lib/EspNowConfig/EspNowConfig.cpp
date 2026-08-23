#include "EspNowConfig.h"
#include "LcdDashboard.h"
#include "ManifestCache.h"
#include "HubRelay.h"
#include "SerialMux.h"
#include "ShellConfig.h"
#include "ShellOutput.h"
#include "BtpTransport.h"
#include "RadioSeal.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace {

// Shared output stream and runtime services used by ESP-NOW callbacks.
Stream* g_io = nullptr;
EspNowManager* g_manager = nullptr;
DatabaseStore* g_database = nullptr;
LcdDashboard* g_lcdDashboard = nullptr;

ProtocolRouter::Router g_router;

QueueHandle_t g_rxQueue = nullptr;
QueueHandle_t g_logQueue = nullptr;
QueueHandle_t g_telemetryQueue = nullptr;
QueueHandle_t g_terminalQueue = nullptr;
QueueHandle_t g_controlQueue = nullptr;

// Cumulative counters, never cleared by a read. The takeDropped*Count()
// accessors below keep their original "delta since my last call" contract by
// remembering their own watermark instead of zeroing the counter -- the LCD
// dashboard consumes those deltas every tick (AppRuntime::
// flushPendingEspNowOutput), so a counter it clears is useless for measuring
// a rate. peekRxCounters() reads the totals and clears nothing, which is what
// makes "difference two snapshots, divide by the interval" possible.
//
// The totals count from the last enableAsyncRx() (see the reset there), which
// is a well-defined epoch: the dongle's own boot, in practice.
volatile uint32_t g_droppedRxTotal = 0;         // raw datagram queue full
volatile uint32_t g_droppedDecodeTotal = 0;     // ProtocolRouter: bad envelope
volatile uint32_t g_droppedCrcTotal = 0;        // ProtocolRouter: CRC mismatch
volatile uint32_t g_droppedReassemblyTotal = 0; // ProtocolRouter: reassembly conflict/timeout/etc
volatile uint32_t g_droppedQueueFullTotal = 0;  // routed message, but its type queue was full
// Topico 30: a consumed (channel C) message that did not open under key L --
// no key configured, missing ENCRYPTED, wrong cipher, or a TagMismatch. This
// is the counter "recusado e contado" (a forged frame is refused AND
// counted) points at; unlike the five above it is not part of the wire
// schema (RxCounters/hub.link) -- see the comment at
// processRxDatagramInternal's open() call for why that scope stays out of
// this topic.
volatile uint32_t g_droppedAuthTotal = 0;

// Watermarks for the takeDropped*Count() deltas. Unsigned subtraction against
// the total stays correct across a 32-bit wrap.
volatile uint32_t g_droppedRxTaken = 0;
volatile uint32_t g_droppedDecodeTaken = 0;
volatile uint32_t g_droppedCrcTaken = 0;
volatile uint32_t g_droppedReassemblyTaken = 0;
volatile uint32_t g_droppedQueueFullTaken = 0;
volatile uint32_t g_droppedAuthTaken = 0;

// Ingress counters: the g_dropped*Total set above only ever answered "what was
// lost", which is not enough to compute a rate -- nothing counted what came
// through. These do, at the two points that matter: every datagram the radio
// handed us, and every complete logical message the router published, by type.
volatile uint32_t g_rxDatagramTotal = 0;        // datagrams handed to ProtocolRouter
volatile uint32_t g_fragmentAcceptedTotal = 0;  // fragment stored, message not complete yet
volatile uint32_t g_routedTelemetryTotal = 0;
volatile uint32_t g_routedLogTotal = 0;
volatile uint32_t g_routedCommandTotal = 0;
volatile uint32_t g_routedControlTotal = 0;
volatile uint32_t g_routedTerminalTotal = 0;

bool g_asyncRxEnabled = false;

// Heartbeat target: last peer we received real data from.
uint8_t g_heartbeatTargetMac[6] = {0};
bool g_hasHeartbeatTarget = false;

// Adapter binding BtpTransport's transport-agnostic send callback to this
// dongle's actual EspNowManager instance.
bool sendViaManager(void* context, const uint8_t mac[6], const uint8_t* data, size_t size) {
    return static_cast<EspNowManager*>(context)->sendToMac(mac, data, size);
}

// Proactively asks a peer for its manifest the moment this dongle has no
// usable cache entry for its current (source_id, boot_id) -- topico 16
// PASSO 3's "cachear/agregar manifests por source", primed automatically
// rather than waiting for a desktop client to ask first (decision 13:
// "o dongle e dono do catalogo apresentado ao computador"). Rate-limited by
// ManifestCache::shouldRequestManifest itself so a steady TELEMETRY stream
// from a not-yet-cached robot cannot flood it with duplicate requests.
void primeManifestIfNeeded(const uint8_t mac[6], const btp::Header& header) {
    if (g_manager == nullptr) {
        return;
    }
    const uint32_t nowMs = millis();
    if (!ManifestCache::shouldRequestManifest(header.source_id, header.boot_id, nowMs)) {
        return;
    }

    uint8_t requestPayload[12];
    const size_t requestSize =
        ManifestCache::buildRequest(header.source_id, header.boot_id, 0U, requestPayload, sizeof(requestPayload));
    if (requestSize == 0) {
        return;
    }

    const uint64_t timestampUs = static_cast<uint64_t>(nowMs) * 1000ULL;
    // Topico 30: this priming request is dongle<->robot, channel C, key L --
    // sealed like every other message this file originates over the radio.
    BtpTransport::sendLogical(sendViaManager, g_manager, mac, btp::MessageType::Control,
                              ManifestCache::kManifestRequestObjectId, requestPayload, requestSize,
                              timestampUs, RadioSeal::seal, nullptr);
}

// "state changed: SETUP -> WAIT" style lines emitted by the robot's Logger.
// Checked only on LOG payloads (never TELEMETRY, which stays opaque bytes).
bool tryExtractRobotState(const char* text, String& outState) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    const String line(text);
    String lower = line;
    lower.toLowerCase();
    if (lower.indexOf("state changed") < 0) {
        return false;
    }

    const int32_t arrowPos = line.lastIndexOf("->");
    if (arrowPos < 0) {
        return false;
    }

    String state = line.substring(arrowPos + 2);
    state.trim();
    if (state.length() == 0) {
        return false;
    }

    outState = state;
    return true;
}

void macToText(const uint8_t mac[6], char out[18]) {
    std::snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Copies a routed payload into a NUL-terminated stack buffer for display
// purposes only (routing itself already preserved the full byte range).
size_t copyPayloadAsText(const ProtocolRouter::RoutedMessage& routed, char* out, size_t outCapacity) {
    if (out == nullptr || outCapacity == 0) {
        return 0;
    }

    const size_t copyLen = (routed.payloadSize < outCapacity - 1) ? routed.payloadSize : (outCapacity - 1);
    if (copyLen > 0) {
        std::memcpy(out, routed.payload, copyLen);
    }
    out[copyLen] = '\0';
    return copyLen;
}

QueueHandle_t queueForType(btp::MessageType type) {
    switch (type) {
        case btp::MessageType::Log: return g_logQueue;
        case btp::MessageType::Telemetry: return g_telemetryQueue;
        case btp::MessageType::Terminal: return g_terminalQueue;
        case btp::MessageType::Control: return g_controlQueue;
        case btp::MessageType::Command:
        case btp::MessageType::Invalid:
        default: return nullptr;
    }
}

bool enqueueRouted(QueueHandle_t queue, const ProtocolRouter::RoutedMessage& routed) {
    if (queue == nullptr) {
        return false;
    }

    if (xQueueSend(queue, &routed, 0) == pdTRUE) {
        return true;
    }

    ++g_droppedQueueFullTotal;
    return false;
}

// Sends one COMMAND_RESULT back to the requester, correlated by the original
// request's (source_id, boot_id, sequence) per BTP/docs/commands.md
// section 1's request reference.
void replyCommandResult(
    const uint8_t mac[6],
    const btp::Header& requestHeader,
    uint16_t actionId,
    uint16_t actionVersion,
    BtpTransport::btp_command::Status status,
    BtpTransport::btp_command::ErrorCode errorCode,
    const std::string& message
) {
    if (g_manager == nullptr) {
        return;
    }

    const std::string truncated = message.substr(
        0, BtpTransport::btp_command::kMaxResultMessageSize);

    uint8_t resultPayload[BtpTransport::btp_command::kResultPrefixSize +
                          BtpTransport::btp_command::kMaxResultMessageSize];
    const size_t resultSize = BtpTransport::btp_command::build_result(
        requestHeader.source_id, requestHeader.boot_id, requestHeader.sequence,
        actionId, actionVersion, status, errorCode, truncated.c_str(),
        nullptr, 0, resultPayload, sizeof(resultPayload));

    if (resultSize == 0) {
        return;
    }

    const uint64_t timestampUs = static_cast<uint64_t>(millis()) * 1000ULL;
    // Topico 30: a COMMAND_RESULT this dongle originates (answering a
    // COMMAND_REQUEST a peer sent it) is channel C, same as the request.
    BtpTransport::sendLogical(sendViaManager, g_manager, mac, btp::MessageType::Command,
                              BtpTransport::btp_command::kCommandResultObjectId,
                              resultPayload, resultSize, timestampUs, RadioSeal::seal, nullptr);
}

// A peer we sent a COMMAND_REQUEST to (espnow -send_to/-send_all) replying
// back. Replaces the old "reply arrives tagged INFO, shown like any RX line".
void handleRoutedCommandResult(const uint8_t mac[6], const btp::Header& header, btp::ByteView payload) {
    BtpTransport::btp_command::ResultView result{};
    if (BtpTransport::btp_command::parse_result(header, payload, &result) !=
        BtpTransport::btp_command::ParseError::Ok) {
        return;
    }

    // Only meaningful as human-readable console text; the serial BTP session
    // (when protocolled) owns its own COMMAND_RESULT semantics for whatever
    // it requested itself -- an ESP-NOW peer's reply to an unrelated
    // espnow -send_to is not relayed onto that session (see SerialMux.h /
    // ShellCommandSupport::printLine for the "single writer" rule this
    // avoids breaking).
    if (!SerialMux::isConsoleOwned()) {
        return;
    }

    char macText[18] = {0};
    macToText(mac, macText);

    char line[64] = {0};
    std::snprintf(line, sizeof(line), "%s status=%s", macText,
                  BtpTransport::btp_command::status_string(result.status));
    if (g_io != nullptr) {
        ShellOutput::printTagged(*g_io, "cmd_result", line);
    }

    if (result.message.size > 0 && result.message.data != nullptr && g_io != nullptr) {
        char messageText[BtpTransport::btp_command::kMaxResultMessageSize + 1] = {0};
        const size_t messageLen = (result.message.size < sizeof(messageText) - 1)
            ? result.message.size
            : (sizeof(messageText) - 1);
        std::memcpy(messageText, result.message.data, messageLen);
        messageText[messageLen] = '\0';
        ShellOutput::printTagged(*g_io, "cmd_result", messageText);
    }
}

// Runs a shell command received from a trusted peer (COMMAND_REQUEST) and
// replies with a COMMAND_RESULT. Only peers already in our registry are
// trusted to trigger execution -- content of the command is never inspected
// for authorization (see CONTRIBUTING.md section 5).
void handleRoutedCommandRequest(const uint8_t mac[6], const ProtocolRouter::RoutedMessage& routed) {
    if (g_manager == nullptr || g_manager->deviceIndexByMac(mac) < 0) {
        return;
    }

    using namespace BtpTransport::btp_command;

    const btp::ByteView payloadView{routed.payload, routed.payloadSize};
    RequestView request{};
    const ParseError parseError = parse_request(routed.header, payloadView, BtpTransport::sourceId(),
                                                BtpTransport::bootId(), &request);
    if (parseError == ParseError::WrongTarget) {
        return; // not addressed to this dongle's current boot
    }
    if (parseError != ParseError::Ok) {
        replyCommandResult(mac, routed.header, 0, 0, Status::Rejected, ErrorCode::MalformedPayload,
                           parse_error_string(parseError));
        return;
    }

    char commandText[kMaxShellCommandSize + 1] = {0};
    const ParseError copyError = copy_shell_command(request, commandText, sizeof(commandText));
    if (copyError != ParseError::Ok) {
        const ErrorCode errorCode = (copyError == ParseError::UnsupportedAction)
            ? ErrorCode::UnsupportedVersion
            : ErrorCode::InvalidArgument;
        replyCommandResult(mac, routed.header, request.action_id, request.action_version,
                           Status::Rejected, errorCode, parse_error_string(copyError));
        return;
    }

    char macText[18] = {0};
    macToText(mac, macText);
    const std::string userId = std::string("espnow:") + macText;

    std::string fullOutput;
    ShellConfig::runLine(std::string(commandText), "espnow", &fullOutput, userId);

    replyCommandResult(mac, routed.header, request.action_id, request.action_version,
                       Status::Success, ErrorCode::None, fullOutput);
}

void handleRoutedCommand(const uint8_t mac[6], const ProtocolRouter::RoutedMessage& routed) {
    if (routed.header.object_id == BtpTransport::btp_command::kCommandResultObjectId) {
        handleRoutedCommandResult(mac, routed.header, {routed.payload, routed.payloadSize});
        return;
    }
    if (routed.header.object_id == BtpTransport::btp_command::kCommandRequestObjectId) {
        handleRoutedCommandRequest(mac, routed);
        return;
    }
    // Reserved COMMAND object_id (BTP/docs/commands.md section 1's
    // `object_id` namespaces): a v1 receiver MUST reject it without
    // reinterpreting it.
}

// Counts one complete logical message per type. Sits here rather than inside
// queueForType() because COMMAND never reaches a queue (handled synchronously)
// and would otherwise be the one type with no ingress number.
void countRouted(btp::MessageType type) {
    switch (type) {
        case btp::MessageType::Telemetry: ++g_routedTelemetryTotal; break;
        case btp::MessageType::Log:       ++g_routedLogTotal; break;
        case btp::MessageType::Command:   ++g_routedCommandTotal; break;
        case btp::MessageType::Control:   ++g_routedControlTotal; break;
        case btp::MessageType::Terminal:  ++g_routedTerminalTotal; break;
        case btp::MessageType::Invalid:
        default: break;
    }
}

void dispatchRouted(const uint8_t mac[6], const ProtocolRouter::RoutedMessage& routed) {
    countRouted(routed.header.type);

    if (routed.header.type == btp::MessageType::Command) {
        // Latency sensitive (remote shell execution): handled synchronously,
        // not queued -- see EspNowConfig.h for the rationale.
        handleRoutedCommand(mac, routed);
        return;
    }

    QueueHandle_t queue = queueForType(routed.header.type);
    enqueueRouted(queue, routed);
}

// Topico 28, upstream half: the radio's ingress rule, inverted.
//
// Before this, every datagram was decoded, reassembled and dispatched by
// type, and only a couple of types found a consumer at the end of that -- so
// a handler left empty swallowed a whole MessageType in silence
// (handleTerminalItem was exactly that: the robot's terminal output never
// arrived anywhere, and nobody noticed).
//
// The rule now is the hub's: EVERYTHING goes up to the console except the
// short, explicit list in bally::dongle_consumes() (bally_channels.h, the one
// place that list is written). Forgetting to add something to it makes the
// message arrive at the console -- visible and harmless -- instead of
// disappearing.
//
// Only the 36-octet envelope is read to decide, which is what lets this work
// on traffic the dongle holds no key for: BTP encrypts the payload and never
// the header (docs/encryption.md section 5 -- the header is the AAD).
// ProtocolRouter is off the relay path entirely and stays only for what the
// dongle actually consumes (D5).
void processRxDatagramInternal(const uint8_t mac[6], const uint8_t* data, size_t len) {
    if (g_lcdDashboard != nullptr) {
        g_lcdDashboard->notifyRx();
    }

    ++g_rxDatagramTotal;

    const HubRelay::RadioIngress ingress = HubRelay::classifyRadio(data, len, BtpTransport::sourceId());
    if (ingress.error != btp::Error::Ok) {
        if (ingress.error == btp::Error::CrcMismatch) {
            ++g_droppedCrcTotal;
        } else {
            ++g_droppedDecodeTotal;
        }
        return;
    }

    // Learned from every datagram, consumed or not: lookupPeerMacBySourceId
    // is how the DOWNSTREAM relay finds a robot's MAC (SerialMux::relayDown),
    // so a robot the dongle only ever relays for still has to be addressable.
    if (mac != nullptr) {
        std::memcpy(g_heartbeatTargetMac, mac, sizeof(g_heartbeatTargetMac));
        g_hasHeartbeatTarget = true;
        BtpTransport::rememberPeer(mac, ingress.header.source_id, ingress.header.boot_id, millis());

        if (g_manager != nullptr && g_manager->deviceIndexByMac(mac) < 0) {
            char autoName[24] = {0};
            std::snprintf(autoName, sizeof(autoName), "peer-%02X%02X", mac[4], mac[5]);
            g_manager->addDevice(mac, autoName, "adicionado automaticamente por RX ESP-NOW");
        }
    }

    // Counted by type here rather than after routing, because a relayed
    // datagram never reaches countRouted(): this is one fragment, not one
    // logical message, so `espnow -stats` now reads as radio ingress per
    // type instead of completed messages per type.
    if (!ingress.consume && SerialMux::isProtocolled()) {
        countRouted(ingress.header.type);
        SerialMux::relayUp(ingress.header, data, len);
        return;
    }

    // Consumed, or nobody upstream to relay to. With the port still
    // console-owned the relay has no destination at all, so the pre-hub local
    // path stays in charge -- that is what keeps LOG lines on the console,
    // the LCD's robot-state tile alive and the manifest cache warm while a
    // human is at the bench. It is what "relay" degrades to with no consumer,
    // not a second consume list.
    ProtocolRouter::RoutedMessage routed{};
    const ProtocolRouter::Outcome outcome = g_router.submit(mac, data, len, millis(), &routed);

    switch (outcome) {
        case ProtocolRouter::Outcome::DroppedDecode:
            ++g_droppedDecodeTotal;
            return;
        case ProtocolRouter::Outcome::DroppedCrc:
            ++g_droppedCrcTotal;
            return;
        case ProtocolRouter::Outcome::DroppedReassembly:
            ++g_droppedReassemblyTotal;
            return;
        case ProtocolRouter::Outcome::FragmentAccepted:
            // Not a loss and not yet a message: counted on its own so the
            // ratio datagrams/routed can be read as "how much of the traffic
            // is fragmentation overhead" instead of looking like a leak.
            ++g_fragmentAcceptedTotal;
            return;
        case ProtocolRouter::Outcome::DroppedInvalidArgument:
        case ProtocolRouter::Outcome::DuplicateFragment:
            return;
        case ProtocolRouter::Outcome::Routed:
            break;
    }

    if (mac != nullptr) {
        primeManifestIfNeeded(mac, routed.header);
    }

    // Topico 30: everything that reached here via `ingress.consume` is
    // channel C (dongle<->robot, key L) -- heartbeat/presence STATUS, or a
    // COMMAND addressed to this dongle. It must open under key L before any
    // handler sees it; the "nobody to relay to" fallback below (a robot's
    // LOG/TELEMETRY/etc routed locally only because no client is attached,
    // topico 28's console-owned path) is channel B content this dongle
    // never holds the key for, and stays untouched -- opening it here would
    // both fail (wrong key) and be architecturally wrong (bally_channels.h:
    // "the hub holds key L, never any robot's key E").
    if (ingress.consume) {
        uint8_t plaintext[ProtocolRouter::kMaxPayloadSize];
        const bool opened = routed.payloadSize >= RadioSeal::kTagSize &&
            RadioSeal::open(routed.header, static_cast<uint16_t>(routed.payloadSize), routed.payload,
                            plaintext);

        // Real, bidirectional evidence of who holds key L -- stronger than
        // the heartbeat's own radio-layer ACK (EspNowConfig.h's heartbeatTick
        // doc comment). A peer that cannot produce an openable frame is
        // marked not-online even if its heartbeat replies keep ACKing.
        if (mac != nullptr) {
            BtpTransport::notePeerLinkResult(mac, opened);
        }

        if (!opened) {
            ++g_droppedAuthTotal;
            return;
        }

        routed.payloadSize -= RadioSeal::kTagSize;
        std::memcpy(routed.payload, plaintext, routed.payloadSize);
    }

    dispatchRouted(mac, routed);
}

void onDataRecv(const uint8_t* mac, const uint8_t* data, size_t len) {
    if (mac == nullptr || data == nullptr || len == 0) {
        return;
    }

    if (g_asyncRxEnabled && g_rxQueue != nullptr) {
        EspNowConfig::RxDatagramEvent event{};
        std::memcpy(event.mac, mac, sizeof(event.mac));
        const size_t copyLen = (len < sizeof(event.data)) ? len : sizeof(event.data);
        std::memcpy(event.data, data, copyLen);
        event.len = copyLen;

        if (xQueueSend(g_rxQueue, &event, 0) == pdTRUE) {
            return;
        }

        ++g_droppedRxTotal;
        return;
    }

    processRxDatagramInternal(mac, data, len);
}

void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    (void)mac_addr;
    (void)status;
}

size_t drainOneQueue(QueueHandle_t queue, size_t maxItems, void (*handler)(const ProtocolRouter::RoutedMessage&)) {
    if (queue == nullptr || maxItems == 0) {
        return 0;
    }

    ProtocolRouter::RoutedMessage item{};
    size_t drained = 0;
    while (drained < maxItems && xQueueReceive(queue, &item, 0) == pdTRUE) {
        handler(item);
        ++drained;
    }
    return drained;
}

void handleLogItem(const ProtocolRouter::RoutedMessage& routed) {
    char text[ProtocolRouter::kMaxPayloadSize + 1] = {0};
    copyPayloadAsText(routed, text, sizeof(text));

    if (g_lcdDashboard != nullptr) {
        String robotState;
        if (tryExtractRobotState(text, robotState)) {
            g_lcdDashboard->notifyRobotState(robotState);
        }
    }

    // Plain human console: unchanged behavior, print as before. Protocolled
    // session (topico 13): relay the original LOG frame verbatim to the
    // desktop instead -- printing here would leak raw console text onto a
    // port SerialMux owns exclusively (session-and-terminal.md sections 3-4).
    if (SerialMux::isConsoleOwned()) {
        if (g_io != nullptr) {
            char tag[16] = {0};
            std::snprintf(tag, sizeof(tag), "log %08lX", static_cast<unsigned long>(routed.header.source_id));
            ShellOutput::printTagged(*g_io, tag, text);
        }
        return;
    }

    SerialMux::forwardRelay(routed.header, routed.payload, routed.payloadSize);
}

// TELEMETRY finally has a consumer as of topico 13: relayed byte-for-byte
// (never turned into String/printf text, per PLANO_GERAL.txt decisions 5/6)
// to a protocolled desktop session. With no session attached, forwardRelay()
// is a no-op (counted, not queued) -- same net effect as topico 12's
// drain-and-discard placeholder.
void handleTelemetryItem(const ProtocolRouter::RoutedMessage& routed) {
    SerialMux::forwardRelay(routed.header, routed.payload, routed.payloadSize);
}

// This empty handler is the bug D6 was written against: a whole MessageType
// swallowed in silence, so the robot's terminal output never arrived
// anywhere and nobody noticed. Since topico 28 TERMINAL is relayed straight
// off the radio and never reaches this queue while a client is attached; what
// is left here only runs with the port console-owned, where a robot's
// terminal stream has no reader by definition.
void handleTerminalItem(const ProtocolRouter::RoutedMessage&) {}

// A robot answering a MANIFEST_REQUEST this dongle sent it (topico 16). Only
// reachable on the console-owned fallback path since topico 28 -- with a
// desktop attached, CONTROL that is not on bally::dongle_consumes' list is
// relayed and never routed. Any other CONTROL object_id is ignored.
void handleControlItem(const ProtocolRouter::RoutedMessage& routed) {
    if (routed.header.object_id == ManifestCache::kManifestDataObjectId) {
        ManifestCache::ingestManifestData({routed.payload, routed.payloadSize}, millis());
        return;
    }
    // SUBSCRIBE_RESULT/UNSUBSCRIBE_RESULT used to be folded into
    // SubscriptionRegistry here, correlating the upstream request this dongle
    // had merged on its clients' behalf. Topico 28 removed that: a robot's
    // subscriptions are channel B now -- TraceView subscribes at the robot
    // and the robot arbitrates per session -- so the dongle neither asks nor
    // has anything to correlate, and the answer belongs to the client, which
    // gets it verbatim through the relay.
}

} // namespace

namespace EspNowConfig {

void attachCallbacks(EspNowManager& manager, Stream& io, DatabaseStore* database, LcdDashboard* lcdDashboard) {
    g_io = &io;
    g_manager = &manager;
    g_database = database;
    g_lcdDashboard = lcdDashboard;
    manager.setReceiveCallback(onDataRecv);
    manager.setSendCallback(onDataSent);
}

bool enableAsyncRx(size_t queueDepth) {
    if (queueDepth == 0) {
        queueDepth = RX_ASYNC_QUEUE_DEPTH;
    }

    if (g_rxQueue == nullptr) {
        g_rxQueue = xQueueCreate(static_cast<UBaseType_t>(queueDepth), sizeof(RxDatagramEvent));
    }
    if (g_logQueue == nullptr) {
        g_logQueue = xQueueCreate(static_cast<UBaseType_t>(RX_LOG_QUEUE_DEPTH), sizeof(ProtocolRouter::RoutedMessage));
    }
    if (g_telemetryQueue == nullptr) {
        g_telemetryQueue = xQueueCreate(static_cast<UBaseType_t>(RX_TELEMETRY_QUEUE_DEPTH), sizeof(ProtocolRouter::RoutedMessage));
    }
    if (g_terminalQueue == nullptr) {
        g_terminalQueue = xQueueCreate(static_cast<UBaseType_t>(RX_TERMINAL_QUEUE_DEPTH), sizeof(ProtocolRouter::RoutedMessage));
    }
    if (g_controlQueue == nullptr) {
        g_controlQueue = xQueueCreate(static_cast<UBaseType_t>(RX_CONTROL_QUEUE_DEPTH), sizeof(ProtocolRouter::RoutedMessage));
    }

    if (g_rxQueue == nullptr || g_logQueue == nullptr || g_telemetryQueue == nullptr ||
        g_terminalQueue == nullptr || g_controlQueue == nullptr) {
        g_asyncRxEnabled = false;
        return false;
    }

    xQueueReset(g_rxQueue);
    xQueueReset(g_logQueue);
    xQueueReset(g_telemetryQueue);
    xQueueReset(g_terminalQueue);
    xQueueReset(g_controlQueue);
    // Totals and their watermarks are cleared together, so the deltas
    // takeDropped*Count() reports stay consistent across the reset.
    g_droppedRxTotal = 0;
    g_droppedDecodeTotal = 0;
    g_droppedCrcTotal = 0;
    g_droppedReassemblyTotal = 0;
    g_droppedQueueFullTotal = 0;
    g_droppedAuthTotal = 0;
    g_droppedRxTaken = 0;
    g_droppedDecodeTaken = 0;
    g_droppedCrcTaken = 0;
    g_droppedReassemblyTaken = 0;
    g_droppedQueueFullTaken = 0;
    g_droppedAuthTaken = 0;
    g_rxDatagramTotal = 0;
    g_fragmentAcceptedTotal = 0;
    g_routedTelemetryTotal = 0;
    g_routedLogTotal = 0;
    g_routedCommandTotal = 0;
    g_routedControlTotal = 0;
    g_routedTerminalTotal = 0;
    g_asyncRxEnabled = true;
    return true;
}

void disableAsyncRx() {
    g_asyncRxEnabled = false;
    if (g_rxQueue != nullptr) xQueueReset(g_rxQueue);
    if (g_logQueue != nullptr) xQueueReset(g_logQueue);
    if (g_telemetryQueue != nullptr) xQueueReset(g_telemetryQueue);
    if (g_terminalQueue != nullptr) xQueueReset(g_terminalQueue);
    if (g_controlQueue != nullptr) xQueueReset(g_controlQueue);
}

bool dequeueRxDatagram(RxDatagramEvent& outEvent, uint32_t timeoutMs) {
    if (g_rxQueue == nullptr) {
        return false;
    }

    const TickType_t waitTicks = (timeoutMs == 0) ? static_cast<TickType_t>(0) : pdMS_TO_TICKS(timeoutMs);
    return xQueueReceive(g_rxQueue, &outEvent, waitTicks) == pdTRUE;
}

void processRxDatagram(const RxDatagramEvent& event) {
    processRxDatagramInternal(event.mac, event.data, event.len);
}

size_t drainRoutedQueues(size_t maxItemsPerQueue) {
    size_t total = 0;
    total += drainOneQueue(g_controlQueue, maxItemsPerQueue, handleControlItem);
    total += drainOneQueue(g_logQueue, maxItemsPerQueue, handleLogItem);
    total += drainOneQueue(g_telemetryQueue, maxItemsPerQueue, handleTelemetryItem);
    total += drainOneQueue(g_terminalQueue, maxItemsPerQueue, handleTerminalItem);
    return total;
}

// Each of these returns what accumulated since its own last call, exactly as
// before -- the counter itself is no longer cleared (see the comment on the
// g_dropped*Total block), only this accessor's watermark advances.
uint32_t takeDroppedRxCount() {
    const uint32_t total = g_droppedRxTotal;
    const uint32_t delta = total - g_droppedRxTaken;
    g_droppedRxTaken = total;
    return delta;
}

uint32_t takeDroppedDecodeCount() {
    const uint32_t total = g_droppedDecodeTotal;
    const uint32_t delta = total - g_droppedDecodeTaken;
    g_droppedDecodeTaken = total;
    return delta;
}

uint32_t takeDroppedCrcCount() {
    const uint32_t total = g_droppedCrcTotal;
    const uint32_t delta = total - g_droppedCrcTaken;
    g_droppedCrcTaken = total;
    return delta;
}

uint32_t takeDroppedReassemblyCount() {
    const uint32_t total = g_droppedReassemblyTotal;
    const uint32_t delta = total - g_droppedReassemblyTaken;
    g_droppedReassemblyTaken = total;
    return delta;
}

uint32_t takeDroppedQueueFullCount() {
    const uint32_t total = g_droppedQueueFullTotal;
    const uint32_t delta = total - g_droppedQueueFullTaken;
    g_droppedQueueFullTaken = total;
    return delta;
}

uint32_t takeDroppedAuthCount() {
    const uint32_t total = g_droppedAuthTotal;
    const uint32_t delta = total - g_droppedAuthTaken;
    g_droppedAuthTaken = total;
    return delta;
}

uint32_t peekDroppedAuthCount() {
    return g_droppedAuthTotal;
}

void peekRxCounters(RxCounters& out) {
    out.datagrams = g_rxDatagramTotal;
    out.fragmentsAccepted = g_fragmentAcceptedTotal;
    out.routedTelemetry = g_routedTelemetryTotal;
    out.routedLog = g_routedLogTotal;
    out.routedCommand = g_routedCommandTotal;
    out.routedControl = g_routedControlTotal;
    out.routedTerminal = g_routedTerminalTotal;
    out.droppedRx = g_droppedRxTotal;
    out.droppedDecode = g_droppedDecodeTotal;
    out.droppedCrc = g_droppedCrcTotal;
    out.droppedReassembly = g_droppedReassemblyTotal;
    out.droppedQueueFull = g_droppedQueueFullTotal;
}

void heartbeatTick() {
    if (g_manager == nullptr || !g_hasHeartbeatTarget) {
        return;
    }

    uint32_t sequence = 0;
    if (!BtpTransport::reserveSequence(&sequence)) {
        return;
    }

    // Sealed (topico 30: heartbeat is channel C, key L), so the empty
    // plaintext still grows into a 16-octet tag on the wire -- the buffer
    // has to cover that on top of the unsealed minimum.
    uint8_t frame[btp::kV1MinimumFrameSize + BtpTransport::kAeadTagSize];
    size_t frameSize = 0;
    const uint64_t timestampUs = static_cast<uint64_t>(millis()) * 1000ULL;
    // STATUS (BTP/docs/commands.md section 1's `object_id` namespaces,
    // object_id 0x0009): "spontaneous and gets no response" (section 5) --
    // an empty payload is a legitimate liveness probe until topico 17
    // defines a real STATUS payload schema.
    if (!BtpTransport::encodeSingleFrame(btp::MessageType::Control, 0x0009U, sequence, timestampUs,
                                         nullptr, 0, frame, sizeof(frame), &frameSize,
                                         RadioSeal::seal, nullptr)) {
        return;
    }

    bool delivered = false;
    const bool gotStatus = g_manager->sendToMacWithStatus(g_heartbeatTargetMac, frame, frameSize, delivered,
                                                           HEARTBEAT_SEND_TIMEOUT_MS);

    const bool linkOk = gotStatus && delivered;

    // Topico 27: the same verdict, kept per peer as well. Until now it only
    // lit the LCD's LINK tile -- a single global bit on a screen nobody can
    // plot; hub.peers publishes it per peer as `online`.
    BtpTransport::notePeerLinkResult(g_heartbeatTargetMac, linkOk);

    if (g_lcdDashboard != nullptr) {
        g_lcdDashboard->notifyHeartbeat(linkOk);
    }
}

bool sendRawToMac(const uint8_t mac[6], const uint8_t* frame, size_t frameSize) {
    if (g_manager == nullptr || mac == nullptr || frame == nullptr || frameSize == 0) {
        return false;
    }
    return g_manager->sendToMac(mac, frame, frameSize);
}

} // namespace EspNowConfig
