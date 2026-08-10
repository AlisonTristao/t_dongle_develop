#include "EspNowConfig.h"
#include "LcdDashboard.h"
#include "SerialMux.h"
#include "ShellConfig.h"
#include "ShellOutput.h"
#include "BtpTransport.h"

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

volatile uint32_t g_droppedRxCount = 0;      // raw datagram queue full
volatile uint32_t g_droppedDecodeCount = 0;  // ProtocolRouter: bad envelope
volatile uint32_t g_droppedCrcCount = 0;     // ProtocolRouter: CRC mismatch
volatile uint32_t g_droppedReassemblyCount = 0; // ProtocolRouter: reassembly conflict/timeout/etc
volatile uint32_t g_droppedQueueFullCount = 0;  // routed message, but its type queue was full
bool g_asyncRxEnabled = false;

// Heartbeat target: last peer we received real data from.
uint8_t g_heartbeatTargetMac[6] = {0};
bool g_hasHeartbeatTarget = false;

// Adapter binding BtpTransport's transport-agnostic send callback to this
// dongle's actual EspNowManager instance.
bool sendViaManager(void* context, const uint8_t mac[6], const uint8_t* data, size_t size) {
    return static_cast<EspNowManager*>(context)->sendToMac(mac, data, size);
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

    ++g_droppedQueueFullCount;
    return false;
}

// Sends one COMMAND_RESULT back to the requester, correlated by the original
// request's (source_id, boot_id, sequence) per bally_protocol/docs/
// COMMANDS_AND_ACTIONS.md section 2.
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
    BtpTransport::sendLogical(sendViaManager, g_manager, mac, btp::MessageType::Command,
                              BtpTransport::btp_command::kCommandResultObjectId,
                              resultPayload, resultSize, timestampUs);
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
    // Reserved COMMAND object_id (bally_protocol/docs/COMMANDS_AND_ACTIONS.md
    // section 3.1): a v1 receiver MUST reject it without reinterpreting it.
}

void dispatchRouted(const uint8_t mac[6], const ProtocolRouter::RoutedMessage& routed) {
    if (routed.header.type == btp::MessageType::Command) {
        // Latency sensitive (remote shell execution): handled synchronously,
        // not queued -- see EspNowConfig.h for the rationale.
        handleRoutedCommand(mac, routed);
        return;
    }

    QueueHandle_t queue = queueForType(routed.header.type);
    enqueueRouted(queue, routed);
}

void processRxDatagramInternal(const uint8_t mac[6], const uint8_t* data, size_t len) {
    if (g_lcdDashboard != nullptr) {
        g_lcdDashboard->notifyRx();
    }

    ProtocolRouter::RoutedMessage routed{};
    const ProtocolRouter::Outcome outcome = g_router.submit(mac, data, len, millis(), &routed);

    switch (outcome) {
        case ProtocolRouter::Outcome::DroppedDecode:
            ++g_droppedDecodeCount;
            return;
        case ProtocolRouter::Outcome::DroppedCrc:
            ++g_droppedCrcCount;
            return;
        case ProtocolRouter::Outcome::DroppedReassembly:
            ++g_droppedReassemblyCount;
            return;
        case ProtocolRouter::Outcome::DroppedInvalidArgument:
        case ProtocolRouter::Outcome::FragmentAccepted:
        case ProtocolRouter::Outcome::DuplicateFragment:
            return;
        case ProtocolRouter::Outcome::Routed:
            break;
    }

    if (mac != nullptr) {
        std::memcpy(g_heartbeatTargetMac, mac, sizeof(g_heartbeatTargetMac));
        g_hasHeartbeatTarget = true;
        BtpTransport::rememberPeer(mac, routed.header.source_id, routed.header.boot_id);
    }

    if (mac != nullptr && g_manager != nullptr && g_manager->deviceIndexByMac(mac) < 0) {
        char autoName[24] = {0};
        std::snprintf(autoName, sizeof(autoName), "peer-%02X%02X", mac[4], mac[5]);
        g_manager->addDevice(mac, autoName, "adicionado automaticamente por RX ESP-NOW");
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

        ++g_droppedRxCount;
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
    // port SerialMux owns exclusively (TRANSPORT_SERIAL.md section 7).
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

// TERMINAL_IN/OUT protocol handling belongs to topico 19; drop for now.
void handleTerminalItem(const ProtocolRouter::RoutedMessage&) {}

// HELLO/MANIFEST/STATUS payload handling belongs to topico 16; drop for now.
void handleControlItem(const ProtocolRouter::RoutedMessage&) {}

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
    g_droppedRxCount = 0;
    g_droppedDecodeCount = 0;
    g_droppedCrcCount = 0;
    g_droppedReassemblyCount = 0;
    g_droppedQueueFullCount = 0;
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

uint32_t takeDroppedRxCount() {
    const uint32_t dropped = g_droppedRxCount;
    g_droppedRxCount = 0;
    return dropped;
}

uint32_t takeDroppedDecodeCount() {
    const uint32_t dropped = g_droppedDecodeCount;
    g_droppedDecodeCount = 0;
    return dropped;
}

uint32_t takeDroppedCrcCount() {
    const uint32_t dropped = g_droppedCrcCount;
    g_droppedCrcCount = 0;
    return dropped;
}

uint32_t takeDroppedReassemblyCount() {
    const uint32_t dropped = g_droppedReassemblyCount;
    g_droppedReassemblyCount = 0;
    return dropped;
}

uint32_t takeDroppedQueueFullCount() {
    const uint32_t dropped = g_droppedQueueFullCount;
    g_droppedQueueFullCount = 0;
    return dropped;
}

void heartbeatTick() {
    if (g_manager == nullptr || !g_hasHeartbeatTarget) {
        return;
    }

    uint32_t sequence = 0;
    if (!BtpTransport::reserveSequence(&sequence)) {
        return;
    }

    uint8_t frame[btp::kV1MinimumFrameSize];
    size_t frameSize = 0;
    const uint64_t timestampUs = static_cast<uint64_t>(millis()) * 1000ULL;
    // STATUS (bally_protocol/docs/COMMANDS_AND_ACTIONS.md 3.2, object_id
    // 0x0009): "publicação espontânea e não possui resposta" -- an empty
    // payload is a legitimate liveness probe until topico 17 defines a real
    // STATUS payload schema.
    if (!BtpTransport::encodeSingleFrame(btp::MessageType::Control, 0x0009U, sequence, timestampUs,
                                         nullptr, 0, frame, sizeof(frame), &frameSize)) {
        return;
    }

    bool delivered = false;
    const bool gotStatus = g_manager->sendToMacWithStatus(g_heartbeatTargetMac, frame, frameSize, delivered,
                                                           HEARTBEAT_SEND_TIMEOUT_MS);

    if (g_lcdDashboard != nullptr) {
        g_lcdDashboard->notifyHeartbeat(gotStatus && delivered);
    }
}

} // namespace EspNowConfig
