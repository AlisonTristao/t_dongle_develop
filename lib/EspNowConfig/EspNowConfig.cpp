#include "EspNowConfig.h"
#include "LcdDashboard.h"
#include "ManifestCache.h"
#include "HubRelay.h"
#include "SerialMux.h"
#include "ShellConfig.h"
#include "ShellOutput.h"
#include "BtpTransport.h"
#include "RadioSeal.h"
#include "bally_channels.h"

#include <btp/telemetry.hpp>

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

// COMMAND and MANIFEST_DATA can be either C-link traffic (key L) or endpoint
// traffic (key E). Their wire header is enough to make them candidates, but
// their reference prefix is ciphertext and a long message is fragmented. Keep
// the raw candidate fragments until the reassembled payload either opens with
// L (consume locally) or does not (relay the original E ciphertext verbatim).
// The bounds intentionally mirror ProtocolRouter so retaining a candidate
// cannot consume more concurrent-message capacity than the reassembler it
// accompanies.
constexpr std::size_t kPendingRelaySlots = ProtocolRouter::kSlotCount;
constexpr std::size_t kPendingRelayMaxFragments =
    (ProtocolRouter::kMaxPayloadSize + btp::kEspNowMaxPayloadSize - 1U) /
    btp::kEspNowMaxPayloadSize;

struct PendingRelay {
    bool active;
    std::uint32_t sourceId;
    std::uint32_t bootId;
    std::uint32_t sequence;
    std::uint8_t fragmentCount;
    std::uint32_t lastArrivalMs;
    bool present[kPendingRelayMaxFragments];
    std::uint16_t sizes[kPendingRelayMaxFragments];
    std::uint8_t frames[kPendingRelayMaxFragments][btp::kEspNowMaxFrameSize];
};

PendingRelay g_pendingRelays[kPendingRelaySlots] = {};

bool samePendingIdentity(const PendingRelay& pending, const btp::Header& header) {
    return pending.active && pending.sourceId == header.source_id && pending.bootId == header.boot_id &&
           pending.sequence == header.sequence;
}

void clearPendingRelay(const btp::Header& header) {
    for (PendingRelay& pending : g_pendingRelays) {
        if (samePendingIdentity(pending, header)) {
            pending = {};
            return;
        }
    }
}

void expirePendingRelays(std::uint32_t nowMs) {
    for (PendingRelay& pending : g_pendingRelays) {
        if (pending.active &&
            static_cast<std::uint32_t>(nowMs - pending.lastArrivalMs) >= ProtocolRouter::kReassemblyTimeoutMs) {
            pending = {};
        }
    }
}

// Stores the exact radio datagram, not a decoded/re-encoded representation.
// Returning false makes the caller drop the candidate rather than risk
// forwarding a real C-link message just because the side buffer was full.
bool retainPendingRelay(const btp::Header& header, const std::uint8_t* data, std::size_t len,
                        std::uint32_t nowMs) {
    if ((header.flags & btp::kFlagFragmented) == 0U) {
        return true;
    }
    if (data == nullptr || len > btp::kEspNowMaxFrameSize || header.fragment_count == 0U ||
        header.fragment_count > kPendingRelayMaxFragments || header.fragment_index >= header.fragment_count) {
        return false;
    }

    expirePendingRelays(nowMs);
    PendingRelay* selected = nullptr;
    for (PendingRelay& pending : g_pendingRelays) {
        if (samePendingIdentity(pending, header)) {
            selected = &pending;
            break;
        }
        if (!pending.active && selected == nullptr) {
            selected = &pending;
        }
    }
    if (selected == nullptr) {
        return false;
    }

    if (!selected->active) {
        selected->active = true;
        selected->sourceId = header.source_id;
        selected->bootId = header.boot_id;
        selected->sequence = header.sequence;
        selected->fragmentCount = header.fragment_count;
    } else if (selected->fragmentCount != header.fragment_count) {
        *selected = {};
        return false;
    }

    const std::size_t index = header.fragment_index;
    std::memcpy(selected->frames[index], data, len);
    selected->sizes[index] = static_cast<std::uint16_t>(len);
    selected->present[index] = true;
    selected->lastArrivalMs = nowMs;
    return true;
}

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
// An unequivocal C-link STATUS message that did not open under key L. COMMAND
// and MANIFEST_DATA cannot use this counter: the same public headers are also
// legal endpoint (E-key) traffic and a failed L open must be relayed, not
// reported as an authentication failure. Unlike the five above it is not part
// of the wire schema (RxCounters/hub.link).
volatile uint32_t g_droppedAuthTotal = 0;

// Radio datagrams thrown away because async RX never came up (the queues or
// the worker task could not be allocated -- a heap-starved boot, topico 34/35
// F2). onDataRecv() must NOT process them inline: that runs on the WiFi
// task's small stack and processRxDatagramInternal now needs kilobytes for
// RadioSeal::open + the routed/plaintext buffers (topicos 28-31), so inline
// processing stack-overflows the moment a robot heartbeat lands. A dongle in
// this state has a heap problem to fix and can only be fixed if it stays up,
// so: drop, count, keep running. Bench-only (not a wire field).
volatile uint32_t g_syncFallbackDropTotal = 0;

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

EspNowManager::TxPriority txPriorityForFrame(const uint8_t* data, size_t size) {
    const HubRelay::RadioIngress ingress = HubRelay::classifyRadio(data, size);
    if (ingress.error != btp::Error::Ok) {
        return EspNowManager::TxPriority::Control;
    }
    switch (ingress.header.type) {
        case btp::MessageType::Control:
        case btp::MessageType::Command:
            return EspNowManager::TxPriority::Critical;
        case btp::MessageType::Log:
        case btp::MessageType::Terminal:
            return EspNowManager::TxPriority::Control;
        case btp::MessageType::Telemetry:
            return EspNowManager::TxPriority::Data;
    }
    return EspNowManager::TxPriority::Control;
}

// Adapter binding BtpTransport's transport-agnostic send callback to the
// manager's bounded, single-owner scheduler. The frame is copied before this
// returns, so callers may keep using stack buffers.
bool sendViaManager(void* context, const uint8_t mac[6], const uint8_t* data, size_t size) {
    return static_cast<EspNowManager*>(context)->sendToMac(
        mac, data, size, txPriorityForFrame(data, size));
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
    ManifestCache::notePrimeSent();  // plano 36 fase 0a
}

// robot.state (bally_OS TelemetryPublisher, topic_id=0x0002, schema v1):
// schema_version:u16_le + one PACKED_LE uint8 field holding the robot's
// StateMachine::stateName value. Replaced the old "state changed: OLD ->
// NEW" LOG-text scraping (topico 41): that line is logType::INFO, which
// TxScheduler::classify() puts in the lowest-priority Debug queue on the
// robot, behind Telemetry -- it was starving out under normal traffic. This
// topic has its own Telemetry-priority queue and is published ungated on
// every real transition, so it actually arrives.
//
// robotStateName mirrors bally_OS's StateMachine::stateToString -- no shared
// header between the two repos for this one enum, so keep the two switches
// in sync by hand if StateMachine::stateName grows a member.
constexpr uint16_t kRobotStateTopicId = 0x0002U;
constexpr uint16_t kRobotStateSchemaVersion = 1U;
constexpr btp::FieldSpec kRobotStateFields[] = {
    {1U, 0U, static_cast<uint8_t>(btp::WireType::Uint8), 0U, 1U, 1U, 1.0, 0.0},
};

const char* robotStateName(uint8_t value) {
    switch (value) {
        case 0: return "NONE";
        case 1: return "SETUP";
        case 2: return "WAIT";
        case 3: return "CALIBRATE";
        case 4: return "DEBUG";
        case 5: return "RUN";
        case 6: return "FINISH";
        case 7: return "TELEMETRY";
        case 8: return "ERROR";
        default: return "UNKNOWN";
    }
}

// Decodes one robot.state sample and forwards its label to the LCD's STATE
// strip. Any structural mismatch (wrong schema version, short payload, a
// field that fails to decode) is silently ignored -- a bad sample here must
// never crash or block the raw relay that always runs alongside this.
void handleRobotStateTelemetry(const ProtocolRouter::RoutedMessage& routed) {
    if (g_lcdDashboard == nullptr) {
        return;
    }

    btp::SampleReader reader(routed.payload, routed.payloadSize, kRobotStateFields, 1U,
                              btp::kEncodingPackedLe);
    if (reader.schema_version() != kRobotStateSchemaVersion) {
        return;
    }

    uint8_t stateValue = 0;
    bool haveState = false;
    btp::SampleValue value{};
    while (reader.next(&value) == btp::SampleStep::Item) {
        if (!value.is_null) {
            stateValue = static_cast<uint8_t>(value.u64(0));
            haveState = true;
        }
    }
    if (reader.finish() != btp::MessageError::Ok || !haveState) {
        return;
    }

    g_lcdDashboard->notifyRobotState(robotStateName(stateValue));
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

    if (result.action_id == BtpTransport::btp_command::kPingActionId) {
        // A ping's only purpose is the RTT it completes -- never surfaced as
        // a cmd_result line, unlike every other COMMAND_RESULT below.
        BtpTransport::notePingReply(mac, result.reply_to_sequence, millis());
        return;
    }

    if (!SerialMux::isConsoleOwned()) {
        if (SerialMux::isProtocolled()) {
            std::string terminalText = "cmd_result status=";
            terminalText += BtpTransport::btp_command::status_string(result.status);
            if (result.message.size > 0 && result.message.data != nullptr) {
                terminalText += "\n";
                terminalText.append(reinterpret_cast<const char*>(result.message.data),
                                    result.message.size);
            }
            SerialMux::writeTerminalResponse(terminalText);
        }
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

bool relayPendingUp(const btp::Header& header) {
    for (PendingRelay& pending : g_pendingRelays) {
        if (!samePendingIdentity(pending, header)) {
            continue;
        }

        bool complete = true;
        for (std::size_t index = 0U; index < pending.fragmentCount; ++index) {
            complete = complete && pending.present[index];
        }
        if (complete) {
            for (std::size_t index = 0U; index < pending.fragmentCount; ++index) {
                countRouted(header.type);
                SerialMux::relayUp(header, pending.frames[index], pending.sizes[index]);
            }
        }
        pending = {};
        return complete;
    }
    return false;
}

std::uint32_t readU32Le(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::uint32_t referenceSourceId(const btp::Header& header, const std::uint8_t* plaintext,
                                std::size_t plaintextSize) {
    const bool hasReference =
        (header.type == btp::MessageType::Command &&
         (header.object_id == bally::kCommandRequestObjectId ||
          header.object_id == bally::kCommandResultObjectId)) ||
        (header.type == btp::MessageType::Control && header.object_id == bally::kManifestDataObjectId);
    return hasReference && plaintext != nullptr && plaintextSize >= 4U ? readU32Le(plaintext) : 0U;
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

// Radio ingress has two paths. Frames which cannot be C-link traffic relay
// immediately. COMMAND, MANIFEST_DATA and STATUS are held through logical
// reassembly, then tried under key L: successful opens are consumed only when
// their authenticated reference names this dongle; failed opens are channel B
// and their original raw fragments relay upstream. No destination decision
// ever reads a payload byte before that authentication.
void processRxDatagramInternal(const uint8_t mac[6], const uint8_t* data, size_t len,
                                int8_t rssi) {
    if (g_lcdDashboard != nullptr) {
        g_lcdDashboard->notifyRx();
    }

    ++g_rxDatagramTotal;

    const HubRelay::RadioIngress ingress = HubRelay::classifyRadio(data, len);
    if (ingress.error != btp::Error::Ok) {
        if (ingress.error == btp::Error::CrcMismatch) {
            ++g_droppedCrcTotal;
        } else {
            ++g_droppedDecodeTotal;
        }
        return;
    }

    // Non-candidates are necessarily endpoint traffic and remain on the fast
    // blind-relay path. Candidates wait below because their payload may be
    // ciphertext, including its destination/reference field.
    if (!ingress.mayConsume && SerialMux::isProtocolled()) {
        countRouted(ingress.header.type);
        SerialMux::relayUp(ingress.header, data, len);
        return;
    }

    const std::uint32_t nowMs = millis();
    if (ingress.mayConsume && !retainPendingRelay(ingress.header, data, len, nowMs)) {
        ++g_droppedReassemblyTotal;
        return;
    }

    // Candidates, and the console-owned fallback for ordinary endpoint
    // traffic, enter the bounded reassembler.
    ProtocolRouter::RoutedMessage routed{};
    const ProtocolRouter::Outcome outcome = g_router.submit(mac, data, len, nowMs, &routed);

    switch (outcome) {
        case ProtocolRouter::Outcome::DroppedDecode:
            ++g_droppedDecodeTotal;
            return;
        case ProtocolRouter::Outcome::DroppedCrc:
            ++g_droppedCrcTotal;
            return;
        case ProtocolRouter::Outcome::DroppedReassembly:
            if (ingress.mayConsume) {
                clearPendingRelay(ingress.header);
            }
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

    if (ingress.mayConsume) {
        uint8_t plaintext[ProtocolRouter::kMaxPayloadSize];
        const bool opened = routed.payloadSize >= RadioSeal::kTagSize &&
            RadioSeal::open(routed.header, static_cast<uint16_t>(routed.payloadSize), routed.payload,
                            plaintext);

        if (!opened) {
            // A failed L open is not automatically an authentication error:
            // valid channel B traffic uses E and has the same public header.
            // With a desktop session, pass the exact retained fragments on;
            // without one, there is no endpoint consumer and it is dropped.
            if (SerialMux::isProtocolled()) {
                if ((ingress.header.flags & btp::kFlagFragmented) != 0U) {
                    if (!relayPendingUp(routed.header)) {
                        ++g_droppedReassemblyTotal;
                    }
                } else {
                    countRouted(ingress.header.type);
                    SerialMux::relayUp(ingress.header, data, len);
                }
            }
            if (ingress.header.type == btp::MessageType::Control &&
                ingress.header.object_id == bally::kStatusObjectId) {
                ++g_droppedAuthTotal;
            }
            return;
        }

        // Only an AEAD-L success may create/refresh the official peer
        // identity. Public channel-B headers still relay byte-for-byte, but
        // cannot spoof online or overwrite the source_id -> MAC route.
        if (mac != nullptr) {
            BtpTransport::rememberAuthenticatedPeer(mac, routed.header.source_id,
                                                    routed.header.boot_id, nowMs, rssi);
            BtpTransport::notePeerLinkResult(mac, true);

            if (g_manager != nullptr && g_manager->deviceIndexByMac(mac) < 0) {
                char autoName[24] = {0};
                std::snprintf(autoName, sizeof(autoName), "peer-%02X%02X", mac[4], mac[5]);
                g_manager->addDevice(mac, autoName, "adicionado automaticamente por RX ESP-NOW");
            }
        }

        routed.payloadSize -= RadioSeal::kTagSize;
        std::memcpy(routed.payload, plaintext, routed.payloadSize);

        // A valid L frame that names another requester is neither channel B
        // nor local work for this dongle. Do not leak authenticated link
        // administration upstream.
        const std::uint32_t referenceId =
            referenceSourceId(routed.header, routed.payload, routed.payloadSize);
        if (!bally::dongle_consumes(routed.header.type, routed.header.object_id, referenceId,
                                    BtpTransport::sourceId())) {
            // plano 36 fase 0a: uma MANIFEST_DATA que abriu com a chave L mas
            // cujo reference_source_id nao e o deste dongle nao vira ingest --
            // conta, para "hub -manifest" distinguir "o robo nao respondeu" de
            // "respondeu mas a referencia nao bate".
            if (routed.header.type == btp::MessageType::Control &&
                routed.header.object_id == ManifestCache::kManifestDataObjectId) {
                ManifestCache::noteConsumeRejected();
            }
            clearPendingRelay(routed.header);
            return;
        }

        clearPendingRelay(routed.header);
    }

    if (mac != nullptr) {
        primeManifestIfNeeded(mac, routed.header);
    }

    dispatchRouted(mac, routed);
}

void onDataRecv(const uint8_t* mac, const uint8_t* data, size_t len, int8_t rssi) {
    if (mac == nullptr || data == nullptr || len == 0) {
        return;
    }

    if (g_asyncRxEnabled && g_rxQueue != nullptr) {
        EspNowConfig::RxDatagramEvent event{};
        std::memcpy(event.mac, mac, sizeof(event.mac));
        const size_t copyLen = (len < sizeof(event.data)) ? len : sizeof(event.data);
        std::memcpy(event.data, data, copyLen);
        event.len = copyLen;
        event.rssi = rssi;

        if (xQueueSend(g_rxQueue, &event, 0) == pdTRUE) {
            return;
        }

        ++g_droppedRxTotal;
        return;
    }

    // Async RX is down (queues / worker task could not be allocated). Do NOT
    // fall through to processRxDatagramInternal here: this callback runs on
    // the WiFi task's ~3 KB stack, and that function needs several KB since
    // the channel-C AEAD work -- it would overflow and panic on the first
    // datagram, which is the "reboots on boot when a robot is nearby" loop.
    // Drop and count so the dongle stays up long enough to diagnose the heap.
    ++g_syncFallbackDropTotal;
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
    if (routed.header.object_id == kRobotStateTopicId) {
        handleRobotStateTelemetry(routed);
    }

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
    processRxDatagramInternal(event.mac, event.data, event.len, event.rssi);
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

uint32_t peekSyncFallbackDropCount() {
    return g_syncFallbackDropTotal;
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

void peekTxSchedulerCounters(EspNowManager::TxSchedulerCounters& out) {
    if (g_manager == nullptr) {
        out = {};
        return;
    }
    g_manager->peekTxSchedulerCounters(out);
}

void heartbeatTick() {
    if (g_manager == nullptr) {
        return;
    }

    // Snapshot while BtpTransport holds its peer-table lock, then release it
    // before the blocking send. This removes the cross-task mutable target
    // and probes every authenticated peer in round-robin order.
    BtpTransport::PeerSnapshot peers[BtpTransport::kPeerIdentityCapacity]{};
    const std::size_t peerCount =
        BtpTransport::enumeratePeers(peers, BtpTransport::kPeerIdentityCapacity);
    if (peerCount == 0U) {
        return;
    }
    static std::size_t nextPeer = 0U;  // owned only by the heartbeat task
    if (nextPeer >= peerCount) {
        nextPeer = 0U;
    }
    std::uint8_t targetMac[6]{};
    std::memcpy(targetMac, peers[nextPeer].mac, sizeof(targetMac));
    nextPeer = (nextPeer + 1U) % peerCount;

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
    const bool gotStatus = g_manager->sendToMacWithStatus(targetMac, frame, frameSize, delivered,
                                                           HEARTBEAT_SEND_TIMEOUT_MS);

    const bool linkOk = gotStatus && delivered;

    // Topico 27/41: the per-peer verdict feeds hub.peers' `online` flag and
    // the LCD's PEERS tile/page (both via BtpTransport::enumeratePeers's
    // age-window check, not this single-peer probe result directly -- see
    // AppRuntime::processAsyncWarnings). The old LINK tile that showed just
    // this one bit is gone.
    BtpTransport::notePeerLinkResult(targetMac, linkOk);
}

void pingTick() {
    if (g_manager == nullptr) {
        return;
    }

    BtpTransport::PeerSnapshot peers[BtpTransport::kPeerIdentityCapacity]{};
    const std::size_t peerCount =
        BtpTransport::enumeratePeers(peers, BtpTransport::kPeerIdentityCapacity);
    if (peerCount == 0U) {
        return;
    }
    static std::size_t nextPeer = 0U;  // owned only by this task, like heartbeatTick's
    if (nextPeer >= peerCount) {
        nextPeer = 0U;
    }
    const BtpTransport::PeerSnapshot target = peers[nextPeer];
    nextPeer = (nextPeer + 1U) % peerCount;

    uint32_t sequence = 0;
    if (!BtpTransport::reserveSequence(&sequence)) {
        return;
    }

    uint8_t payload[BtpTransport::btp_command::kRequestPrefixSize] = {0};
    payload[0] = static_cast<uint8_t>(target.sourceId);
    payload[1] = static_cast<uint8_t>(target.sourceId >> 8);
    payload[2] = static_cast<uint8_t>(target.sourceId >> 16);
    payload[3] = static_cast<uint8_t>(target.sourceId >> 24);
    payload[4] = static_cast<uint8_t>(target.bootId);
    payload[5] = static_cast<uint8_t>(target.bootId >> 8);
    payload[6] = static_cast<uint8_t>(target.bootId >> 16);
    payload[7] = static_cast<uint8_t>(target.bootId >> 24);
    payload[8] = static_cast<uint8_t>(BtpTransport::btp_command::kPingActionId);
    payload[9] = static_cast<uint8_t>(BtpTransport::btp_command::kPingActionId >> 8);
    payload[10] = static_cast<uint8_t>(BtpTransport::btp_command::kPingActionVersion);
    payload[11] = static_cast<uint8_t>(BtpTransport::btp_command::kPingActionVersion >> 8);
    // bytes 12-19 (flags, reserved, parameters_size) are already zero: a ping
    // carries no parameters.

    const uint32_t nowMs = millis();
    const uint64_t timestampUs = static_cast<uint64_t>(nowMs) * 1000ULL;
    // encodeSingleFrame (not sendLogical) because the sequence embedded in
    // the frame has to be the exact one notePingSent records: sendLogical
    // reserves its own internally and never reports it back, which would
    // leave the pending record correlated against the wrong number and the
    // reply never matching. A fixed 20-octet ping payload always fits one
    // frame, same as heartbeatTick's STATUS probe.
    uint8_t frame[btp::kV1MinimumFrameSize + BtpTransport::kAeadTagSize];
    size_t frameSize = 0;
    if (!BtpTransport::encodeSingleFrame(btp::MessageType::Command,
                                         BtpTransport::btp_command::kCommandRequestObjectId,
                                         sequence, timestampUs, payload, sizeof(payload), frame,
                                         sizeof(frame), &frameSize, RadioSeal::seal, nullptr)) {
        return;
    }

    if (sendRawToMac(target.mac, frame, frameSize)) {
        BtpTransport::notePingSent(target.mac, sequence, nowMs);
    }
}

bool sendRawToMac(const uint8_t mac[6], const uint8_t* frame, size_t frameSize) {
    if (g_manager == nullptr || mac == nullptr || frame == nullptr || frameSize == 0) {
        return false;
    }
    return g_manager->sendToMac(mac, frame, frameSize,
                                txPriorityForFrame(frame, frameSize));
}

} // namespace EspNowConfig
