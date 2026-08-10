#include "SerialMux.h"

#include <BtpTransport.h>
#include <btp/stream.hpp>

#include <esp_random.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstring>

namespace SerialMux {
namespace {

// This dongle's own outbound payload ceiling for the serial link. Every
// frame this firmware originates (HELLO_RESULT, STATUS, COMMAND_RESULT,
// TERMINAL_OUT) fits comfortably under it; longer command/terminal output is
// truncated the same way EspNowConfig already truncates COMMAND_RESULT
// messages (see BtpTransport::btp_command::kMaxResultMessageSize). Chosen far
// below the wire's real ceiling (btp::kSerialMaxPayloadSize = 4056) to keep
// the priority queues cheap in RAM; topico 13 does not need Serial-side
// logical fragmentation (see SerialSession.h) so this is also this session's
// hard per-message limit, not just a soft default.
constexpr std::size_t kOutboundPayloadCap = 700U;
constexpr std::size_t kMaxFrameBytes = btp::kV1HeaderSize + btp::kV1CrcSize + kOutboundPayloadCap;
constexpr std::size_t kMaxCobsBytes = kMaxFrameBytes + kMaxFrameBytes / 254U + 4U;

constexpr std::size_t kPriorityClassCount = static_cast<std::size_t>(SerialSession::PriorityClass::kCount);

constexpr UBaseType_t kQueueDepth[kPriorityClassCount] = {
    /*kSession=*/4U,
    /*kTerminal=*/4U,
    /*kLogStatus=*/8U,
    /*kTelemetry=*/16U,
};

constexpr std::size_t kMaxShellLineFromTerminal = BtpTransport::btp_command::kMaxShellCommandSize;
constexpr std::uint32_t kStatusIntervalMs = 2000U;

struct QueuedFrame {
    std::uint8_t bytes[kMaxFrameBytes];
    std::size_t size;
};

Stream* g_io = nullptr;
RunShellLineFn g_runShellLine = nullptr;

std::uint8_t g_encodedBuffer[btp::kSerialMaxCobsBlockSize];
std::uint8_t g_decodedBuffer[btp::kSerialMaxFrameSize];
btp::SerialDecoder g_decoder(g_encodedBuffer, sizeof(g_encodedBuffer), g_decodedBuffer, sizeof(g_decodedBuffer));

SerialSession::Session g_session(SerialSession::LocalLimits{});

QueueHandle_t g_queues[kPriorityClassCount] = {nullptr, nullptr, nullptr, nullptr};

std::uint32_t g_lastStatusMs = 0U;

// STATUS counters (bally_protocol/docs/COMMANDS_AND_ACTIONS.md section 8).
volatile std::uint64_t g_framesRx = 0U;
volatile std::uint64_t g_framesTx = 0U;
volatile std::uint64_t g_crcErrors = 0U;
volatile std::uint64_t g_decodeErrors = 0U;
volatile std::uint64_t g_reassemblyRejected = 0U; // fragmented frames received (unsupported, see SerialSession.h)
volatile std::uint64_t g_telemetryDropped = 0U;
volatile std::uint64_t g_droppedByClass[kPriorityClassCount] = {0U, 0U, 0U, 0U};

std::size_t classIndex(SerialSession::PriorityClass cls) noexcept {
    return static_cast<std::size_t>(cls);
}

void writeAll(const std::uint8_t* data, std::size_t len) noexcept {
    if (g_io == nullptr) {
        return;
    }
    std::size_t remaining = len;
    const std::uint8_t* cursor = data;
    std::uint32_t idleRetries = 0U;
    while (remaining > 0U) {
        const std::size_t sent = g_io->write(cursor, remaining);
        if (sent == 0U) {
            if (++idleRetries > 2000U) {
                return; // port vanished mid-write; give up rather than spin forever
            }
            continue;
        }
        cursor += sent;
        remaining -= sent;
        idleRetries = 0U;
    }
}

// The only place that ever writes a BTP frame's bytes to the wire. Called
// exclusively from tick()/finalizeToConsole(), both driven from the main
// loop -- never from a FreeRTOS task -- so this is the whole firmware's
// single writer while protocolled (PASSO 11).
void writeFrameCobs(const std::uint8_t* frame, std::size_t frameSize) noexcept {
    static std::uint8_t encoded[kMaxCobsBytes];
    std::size_t written = 0U;
    if (btp::cobs_encode(frame, frameSize, encoded, sizeof(encoded), &written) != btp::CobsError::Ok) {
        return;
    }
    const std::uint8_t zero = 0U;
    writeAll(&zero, 1U);
    writeAll(encoded, written);
    writeAll(&zero, 1U);
    ++g_framesTx;
}

void writeConsoleText(const char* text) noexcept {
    if (g_io == nullptr || text == nullptr) {
        return;
    }
    const std::size_t len = std::strlen(text);
    writeAll(reinterpret_cast<const std::uint8_t*>(text), len);
}

void resetQueues() noexcept {
    for (std::size_t i = 0U; i < kPriorityClassCount; ++i) {
        if (g_queues[i] != nullptr) {
            xQueueReset(g_queues[i]);
        }
    }
}

bool encodeOwnFrame(btp::MessageType type, std::uint16_t objectId, const std::uint8_t* payload,
                    std::size_t payloadSize, std::uint8_t* outFrame, std::size_t outFrameCapacity,
                    std::size_t* outFrameSize) noexcept {
    std::uint32_t sequence = 0U;
    if (!BtpTransport::reserveSequence(&sequence)) {
        return false;
    }

    const btp::Header header{
        type, 0U, BtpTransport::sourceId(), BtpTransport::bootId(), sequence,
        static_cast<std::uint64_t>(millis()) * 1000ULL, objectId, 0U, 1U
    };
    const btp::Frame frame{header, {payload, payloadSize}};
    return btp::encode(frame, btp::TransportProfile::Serial, outFrame, outFrameCapacity, outFrameSize) ==
           btp::Error::Ok;
}

bool enqueueFrameBytes(SerialSession::PriorityClass cls, const std::uint8_t* frameBytes,
                       std::size_t frameSize) noexcept {
    if (frameSize > kMaxFrameBytes) {
        return false;
    }

    const std::size_t index = classIndex(cls);
    QueueHandle_t queue = g_queues[index];
    if (queue == nullptr) {
        return false;
    }

    QueuedFrame item{};
    std::memcpy(item.bytes, frameBytes, frameSize);
    item.size = frameSize;

    if (xQueueSend(queue, &item, 0) == pdTRUE) {
        return true;
    }

    // Backpressure per COMMANDS_AND_ACTIONS.md section 12: telemetry/log are
    // dropped first, freshest sample wins. Session/terminal never get this
    // treatment -- their bursts are small and shallow by construction, so a
    // full queue there just counts as a drop instead of evicting something
    // a client is waiting on (e.g. a COMMAND_RESULT).
    if (cls == SerialSession::PriorityClass::kTelemetry || cls == SerialSession::PriorityClass::kLogStatus) {
        QueuedFrame discard{};
        xQueueReceive(queue, &discard, 0);
        if (xQueueSend(queue, &item, 0) == pdTRUE) {
            ++g_droppedByClass[index];
            return true;
        }
    }

    ++g_droppedByClass[index];
    return false;
}

bool enqueueOwn(btp::MessageType type, std::uint16_t objectId, const std::uint8_t* payload,
                std::size_t payloadSize) noexcept {
    std::uint8_t frameBytes[kMaxFrameBytes];
    std::size_t frameSize = 0U;
    if (!encodeOwnFrame(type, objectId, payload, payloadSize, frameBytes, sizeof(frameBytes), &frameSize)) {
        return false;
    }
    return enqueueFrameBytes(SerialSession::classify(type, objectId), frameBytes, frameSize);
}

// Sends the session's last frame synchronously (bypassing the queues -- it
// must go out before anything else and nothing lower-priority should delay
// it), then discards every not-yet-sent queued item and hands the port back
// to the console. Matches TRANSPORT_SERIAL.md section 6 exactly: "descartar
// bloco serial parcial, reassemblies incompletos e itens ainda nao iniciados
// nas filas de transmissao" before "mudar a propriedade da porta".
void finalizeToConsole(std::uint16_t objectId, const std::uint8_t* payload, std::size_t payloadSize,
                       const char* consoleLine) noexcept {
    std::uint8_t frameBytes[kMaxFrameBytes];
    std::size_t frameSize = 0U;
    if (encodeOwnFrame(btp::MessageType::Control, objectId, payload, payloadSize, frameBytes,
                       sizeof(frameBytes), &frameSize)) {
        writeFrameCobs(frameBytes, frameSize);
    }

    resetQueues();
    writeConsoleText(consoleLine);
    g_decoder.reset();
}

void replyCommandResult(const btp::Header& requestHeader, std::uint16_t actionId, std::uint16_t actionVersion,
                        BtpTransport::btp_command::Status status, BtpTransport::btp_command::ErrorCode errorCode,
                        const std::string& message) noexcept {
    using namespace BtpTransport::btp_command;

    const std::string truncated = message.substr(0, kMaxResultMessageSize);
    std::uint8_t payload[kResultPrefixSize + kMaxResultMessageSize];
    const std::size_t size = build_result(requestHeader.source_id, requestHeader.boot_id, requestHeader.sequence,
                                          actionId, actionVersion, status, errorCode, truncated.c_str(), nullptr, 0U,
                                          payload, sizeof(payload));
    if (size == 0U) {
        return;
    }
    enqueueOwn(btp::MessageType::Command, kCommandResultObjectId, payload, size);
}

// Desktop -> dongle remote shell execution over the protocolled session,
// same envelope/parsing BtpTransport::btp_command already defines for the
// ESP-NOW COMMAND path (EspNowConfig::handleRoutedCommandRequest) -- this is
// the same dongle boot answering either way, so re-using it here (instead of
// a second parser) keeps the two paths byte-for-byte consistent. Identity is
// "serial": a wire-attached desktop shares the same physical trust boundary
// as a human typing at this same port, so it is not treated as a separate,
// less-trusted identity the way "espnow:<MAC>" is.
void handleCommandRequest(const btp::DecodedFrame& decoded) noexcept {
    using namespace BtpTransport::btp_command;

    RequestView request{};
    const ParseError parseError =
        parse_request(decoded.header, decoded.payload, BtpTransport::sourceId(), BtpTransport::bootId(), &request);
    if (parseError == ParseError::WrongTarget) {
        return; // not addressed to this dongle's current boot; silently ignored per spec
    }
    if (parseError != ParseError::Ok) {
        replyCommandResult(decoded.header, 0U, 0U, Status::Rejected, ErrorCode::MalformedPayload,
                           parse_error_string(parseError));
        return;
    }

    char commandText[kMaxShellCommandSize + 1U] = {0};
    const ParseError copyError = copy_shell_command(request, commandText, sizeof(commandText));
    if (copyError != ParseError::Ok) {
        const ErrorCode errorCode =
            (copyError == ParseError::UnsupportedAction) ? ErrorCode::UnsupportedVersion : ErrorCode::InvalidArgument;
        replyCommandResult(decoded.header, request.action_id, request.action_version, Status::Rejected, errorCode,
                           parse_error_string(copyError));
        return;
    }

    if (g_runShellLine == nullptr) {
        replyCommandResult(decoded.header, request.action_id, request.action_version, Status::Failed,
                           ErrorCode::InternalError, "shell indisponivel");
        return;
    }

    std::string fullOutput;
    g_runShellLine(commandText, "serial", "serial", &fullOutput);

    replyCommandResult(decoded.header, request.action_id, request.action_version, Status::Success, ErrorCode::None,
                       fullOutput);
}

// TERMINAL_IN/OUT MVP for this topic: the real interactive terminal protocol
// is topico 19's job. What topico 13 needs is a channel that (a) is fully
// isolated from TELEMETRY (CRITERIO 1) and (b) actually round-trips
// something real for the stress test (PASSO 12), so one TERMINAL_IN message
// is treated as one shell line -- trailing CR/LF trimmed, output relayed
// back as one TERMINAL_OUT -- rather than a byte-stream pty. Embedded NUL/CR/
// LF inside the middle of the payload (not just a trailing terminator) are
// left for ShellConfig::runLine to reject the same way a plain console line
// would never contain them either.
void handleTerminalIn(const btp::DecodedFrame& decoded) noexcept {
    if (decoded.payload.size == 0U || decoded.payload.size > kMaxShellLineFromTerminal) {
        return;
    }

    char line[kMaxShellLineFromTerminal + 1U] = {0};
    std::size_t len = decoded.payload.size;
    std::memcpy(line, decoded.payload.data, len);
    while (len > 0U && (line[len - 1U] == '\r' || line[len - 1U] == '\n')) {
        line[--len] = '\0';
    }
    line[len] = '\0';

    if (g_runShellLine == nullptr) {
        return;
    }

    std::string fullOutput;
    g_runShellLine(line, "serial", "serial", &fullOutput);
    fullOutput += "\n";

    const std::size_t sendSize = (fullOutput.size() < kOutboundPayloadCap) ? fullOutput.size() : kOutboundPayloadCap;
    enqueueOwn(btp::MessageType::Terminal, SerialSession::kTerminalOutObjectId,
              reinterpret_cast<const std::uint8_t*>(fullOutput.data()), sendSize);
}

void dispatchFrame(const btp::DecodedFrame& decoded, std::uint32_t nowMs) noexcept {
    if ((decoded.header.flags & btp::kFlagFragmented) != 0U) {
        // Serial-side logical fragmentation/reassembly is out of scope for
        // topico 13 (see SerialSession.h); reject explicitly rather than
        // misreading one fragment as a complete message.
        ++g_reassemblyRejected;
        return;
    }

    std::uint8_t replyPayload[64];
    const SerialSession::Session::FrameResult result =
        g_session.onFrame(decoded, nowMs, replyPayload, sizeof(replyPayload));

    switch (result.outcome) {
        case SerialSession::Session::FrameOutcome::Ignored:
            break;
        case SerialSession::Session::FrameOutcome::HelloAccepted:
            enqueueOwn(btp::MessageType::Control, SerialSession::kHelloResultObjectId, replyPayload,
                      result.outPayloadSize);
            g_lastStatusMs = nowMs; // first heartbeat kStatusIntervalMs from now, not immediately
            break;
        case SerialSession::Session::FrameOutcome::HelloRejected:
            finalizeToConsole(SerialSession::kHelloResultObjectId, replyPayload, result.outPayloadSize,
                              result.consoleLine);
            break;
        case SerialSession::Session::FrameOutcome::SessionClosed:
            finalizeToConsole(SerialSession::kSessionCloseResultObjectId, replyPayload, result.outPayloadSize,
                              result.consoleLine);
            break;
        case SerialSession::Session::FrameOutcome::CommandRequest:
            handleCommandRequest(decoded);
            break;
        case SerialSession::Session::FrameOutcome::TerminalIn:
            handleTerminalIn(decoded);
            break;
    }
}

void pumpRx(std::uint32_t nowMs) noexcept {
    if (g_io == nullptr) {
        return;
    }

    // Bounded per call so one huge burst can't starve TX draining or the
    // rest of AppRuntime::tick() -- the next tick() picks up where this left
    // off, Serial's own RX buffer holds the remainder.
    constexpr std::size_t kMaxBytesPerTick = 512U;
    std::size_t processed = 0U;

    while (processed < kMaxBytesPerTick && g_io->available() > 0) {
        const int value = g_io->read();
        if (value < 0) {
            break;
        }
        ++processed;

        btp::DecodedFrame decoded{};
        const btp::SerialDecodeResult decodeResult = g_decoder.push(static_cast<std::uint8_t>(value), &decoded);

        switch (decodeResult.event) {
            case btp::SerialDecodeEvent::None:
                break;
            case btp::SerialDecodeEvent::Frame:
                ++g_framesRx;
                dispatchFrame(decoded, nowMs);
                break;
            case btp::SerialDecodeEvent::FrameError:
                if (decodeResult.frame_error == btp::Error::CrcMismatch) {
                    ++g_crcErrors;
                } else {
                    ++g_decodeErrors;
                }
                break;
            case btp::SerialDecodeEvent::CobsError:
            case btp::SerialDecodeEvent::Overflow:
                ++g_decodeErrors;
                break;
            case btp::SerialDecodeEvent::InvalidConfiguration:
                return;
        }

        if (g_session.isConsole()) {
            // A HelloRejected/SessionClosed transition happened mid-burst:
            // remaining bytes in this tick belong to the console/next
            // negotiation, not this decode loop.
            return;
        }
    }
}

void maybeSendStatusHeartbeat(std::uint32_t nowMs) noexcept {
    if (nowMs - g_lastStatusMs < kStatusIntervalMs) {
        return;
    }
    g_lastStatusMs = nowMs;

    std::uint64_t droppedTotal = g_telemetryDropped;
    for (std::size_t i = 0U; i < kPriorityClassCount; ++i) {
        droppedTotal += g_droppedByClass[i];
    }

    SerialSession::StatusCounters counters{};
    counters.uptimeUs = static_cast<std::uint64_t>(millis()) * 1000ULL;
    counters.framesRx = g_framesRx;
    counters.framesTx = g_framesTx;
    counters.framesDropped = droppedTotal;
    counters.crcErrors = g_crcErrors;
    counters.decodeErrors = g_decodeErrors;
    counters.reassemblyRejected = g_reassemblyRejected;
    counters.telemetryDropped = g_telemetryDropped;

    std::uint8_t payload[SerialSession::kStatusPayloadSize];
    const std::size_t size = SerialSession::buildStatus(counters, payload, sizeof(payload));
    if (size > 0U) {
        enqueueOwn(btp::MessageType::Control, SerialSession::kStatusObjectId, payload, size);
    }
}

void drainQueueClass(SerialSession::PriorityClass cls, std::size_t maxItems) noexcept {
    QueueHandle_t queue = g_queues[classIndex(cls)];
    if (queue == nullptr) {
        return;
    }

    QueuedFrame item{};
    std::size_t drained = 0U;
    while (drained < maxItems && xQueueReceive(queue, &item, 0) == pdTRUE) {
        writeFrameCobs(item.bytes, item.size);
        ++drained;
    }
}

// Strict priority order (COMMANDS_AND_ACTIONS.md section 12), with a small
// per-class burst cap per tick() so telemetry still makes progress instead
// of starving outright under a steady stream of session/terminal traffic.
void drainTx() noexcept {
    drainQueueClass(SerialSession::PriorityClass::kSession, 8U);
    drainQueueClass(SerialSession::PriorityClass::kTerminal, 4U);
    drainQueueClass(SerialSession::PriorityClass::kLogStatus, 4U);
    drainQueueClass(SerialSession::PriorityClass::kTelemetry, 4U);
}

} // namespace

void begin(Stream& io, RunShellLineFn runShellLine, const std::uint8_t selfUuid[16]) noexcept {
    g_io = &io;
    g_runShellLine = runShellLine;
    g_session.setLocalUuid(selfUuid);

    for (std::size_t i = 0U; i < kPriorityClassCount; ++i) {
        if (g_queues[i] == nullptr) {
            g_queues[i] = xQueueCreate(kQueueDepth[i], sizeof(QueuedFrame));
        }
    }

    g_decoder.reset();
    g_lastStatusMs = millis();
}

bool isConsoleOwned() noexcept {
    return g_session.isConsole();
}

bool isProtocolled() noexcept {
    return g_session.isProtocolled();
}

bool tryEnterFromConsoleLine(const char* line, std::uint32_t nowMs) noexcept {
    if (!g_session.isConsole()) {
        return false;
    }

    char readyLine[SerialSession::kReadyLineCapacity];
    if (!SerialSession::tryParseEnterLine(line, readyLine)) {
        return false;
    }

    writeConsoleText(readyLine);
    g_session.beginNegotiation(nowMs);
    g_decoder.reset();
    return true;
}

bool enterFromCommand(std::uint32_t nowMs) noexcept {
    if (!g_session.isConsole()) {
        return false;
    }

    const std::uint64_t nonce =
        (static_cast<std::uint64_t>(esp_random()) << 32U) | static_cast<std::uint64_t>(esp_random());

    char readyLine[SerialSession::kReadyLineCapacity];
    SerialSession::buildReadyLineFromNonce(nonce, readyLine);

    writeConsoleText(readyLine);
    g_session.beginNegotiation(nowMs);
    g_decoder.reset();
    return true;
}

void tick(std::uint32_t nowMs) noexcept {
    if (g_session.isConsole()) {
        return; // ShellSerial owns the port; nothing for the mux to do.
    }

    pumpRx(nowMs);
    if (g_session.isConsole()) {
        return; // pumpRx() may have just closed the session (SESSION_CLOSE/HelloRejected).
    }

    char consoleLine[SerialSession::kConsoleLineCapacity];
    if (g_session.pollTimeout(nowMs, consoleLine)) {
        resetQueues();
        writeConsoleText(consoleLine);
        g_decoder.reset();
        return;
    }

    if (g_session.isProtocolled()) {
        maybeSendStatusHeartbeat(nowMs);
        drainTx();
    }
}

bool forwardRelay(const btp::Header& header, const std::uint8_t* payload, std::size_t payloadSize) noexcept {
    if (!g_session.isProtocolled()) {
        if (header.type == btp::MessageType::Telemetry) {
            ++g_telemetryDropped;
        }
        return false;
    }
    if (payload == nullptr && payloadSize != 0U) {
        return false;
    }
    if (payloadSize > kOutboundPayloadCap) {
        return false; // ProtocolRouter already caps this well below; defensive only
    }

    std::uint8_t frameBytes[kMaxFrameBytes];
    std::size_t frameSize = 0U;
    const btp::Frame frame{header, {payload, payloadSize}};
    if (btp::encode(frame, btp::TransportProfile::Serial, frameBytes, sizeof(frameBytes), &frameSize) !=
        btp::Error::Ok) {
        return false;
    }

    return enqueueFrameBytes(SerialSession::classify(header.type, header.object_id), frameBytes, frameSize);
}

} // namespace SerialMux
