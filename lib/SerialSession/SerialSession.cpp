#include "SerialSession.h"

#include <BtpTransport.h>
#include <btp/messages.hpp>

#include <cstring>

namespace SerialSession {
namespace {

// The STATUS counter block (commands.md section 5.2) is btp::StatusV1; this
// maps the module's own counter struct onto it. status_version is written by
// the btp::encode_status_* call, not carried here.
btp::StatusV1 toStatusBlock(const StatusCounters& counters) noexcept {
    btp::StatusV1 base{};
    base.flags = counters.degraded ? btp::kStatusDegraded : static_cast<std::uint16_t>(0U);
    base.uptime_us = counters.uptimeUs;
    base.frames_rx = counters.framesRx;
    base.frames_tx = counters.framesTx;
    base.frames_dropped = counters.framesDropped;
    base.crc_errors = counters.crcErrors;
    base.decode_errors = counters.decodeErrors;
    base.reassembly_completed = counters.reassemblyCompleted;
    base.reassembly_timeouts = counters.reassemblyTimeouts;
    base.reassembly_rejected = counters.reassemblyRejected;
    base.command_duplicates = counters.commandDuplicates;
    base.telemetry_dropped = counters.telemetryDropped;
    return base;
}

bool isHexDigit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

char toLowerHex(char c) noexcept {
    return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

PriorityClass classify(btp::MessageType type, std::uint16_t objectId) noexcept {
    switch (type) {
        case btp::MessageType::Control:
            if (objectId == kHelloResultObjectId || objectId == kSessionCloseObjectId ||
                objectId == kSessionCloseResultObjectId) {
                return PriorityClass::kSession;
            }
            return PriorityClass::kLogStatus; // STATUS and any other reserved control object
        case btp::MessageType::Command:
            return PriorityClass::kSession; // COMMAND_RESULT
        case btp::MessageType::Terminal:
            return PriorityClass::kTerminal;
        case btp::MessageType::Log:
            return PriorityClass::kLogStatus;
        case btp::MessageType::Telemetry:
        case btp::MessageType::Invalid:
        default:
            return PriorityClass::kTelemetry;
    }
}

HelloParseError parseHello(btp::ByteView payload, HelloView* out) noexcept {
    if (out == nullptr) {
        return HelloParseError::PayloadTooShort;
    }

    // The HELLO byte layout (session-and-terminal.md section 1) is now
    // btp::decode_hello -- offsets, the reserved-flags/zero-capability/
    // ascending-versions rules and the section 6 limits all live in the
    // library. This wrapper only adapts the result to HelloView and folds
    // btp::MessageError onto the module's own error enum (nothing downstream
    // of onFrame() branches on a specific variant -- only Ok vs not-Ok).
    btp::Hello hello{};
    const btp::MessageError err = btp::decode_hello(payload.data, payload.size, &hello);
    if (err != btp::MessageError::Ok) {
        switch (err) {
            case btp::MessageError::PayloadTooShort:
                return HelloParseError::PayloadTooShort;
            case btp::MessageError::TrailingBytes:
                return HelloParseError::SizeMismatch;
            case btp::MessageError::ReservedNotZero:
                return HelloParseError::NonZeroReservedFlags;
            case btp::MessageError::NotAscending:
            case btp::MessageError::CountTooLarge:  // version_count past kMaxAnnouncedVersions
                return HelloParseError::VersionsNotAscending;
            default:  // ZeroField (no versions / zero capability / zero uuid), InvalidValue (bad role)
                return HelloParseError::ZeroCapability;
        }
    }

    HelloView view{};
    view.role = hello.role;
    view.versionCount = hello.version_count;
    view.flags = 0U;  // decode_hello already rejected a non-zero flags word
    view.maxLogicalPayload = hello.max_logical_payload;
    view.maxInflightReassemblies = hello.max_inflight_reassemblies;
    view.maxSubscriptions = hello.max_subscriptions;
    view.maxDedupEntries = hello.max_dedup_entries;
    view.sessionTimeoutMs = hello.session_timeout_ms;
    std::memcpy(view.peerUuid, hello.peer_uuid, 16U);
    view.configRevision = hello.config_revision;

    view.supportsVersion1 = false;
    for (std::size_t i = 0U; i < hello.version_count; ++i) {
        if (hello.versions[i] == 1U) {
            view.supportsVersion1 = true;
            break;
        }
    }

    *out = view;
    return HelloParseError::Ok;
}

EffectiveLimits negotiate(const HelloView& hello, const LocalLimits& local) noexcept {
    EffectiveLimits out{};
    out.maxLogicalPayload = (hello.maxLogicalPayload < local.maxLogicalPayload)
        ? hello.maxLogicalPayload : local.maxLogicalPayload;
    out.maxInflightReassemblies = (hello.maxInflightReassemblies < local.maxInflightReassemblies)
        ? hello.maxInflightReassemblies : local.maxInflightReassemblies;
    out.maxSubscriptions = (hello.maxSubscriptions < local.maxSubscriptions)
        ? hello.maxSubscriptions : local.maxSubscriptions;
    out.maxDedupEntries = (hello.maxDedupEntries < local.maxDedupEntries)
        ? hello.maxDedupEntries : local.maxDedupEntries;
    out.sessionTimeoutMs = (hello.sessionTimeoutMs < local.sessionTimeoutMs)
        ? hello.sessionTimeoutMs : local.sessionTimeoutMs;
    return out;
}

std::size_t buildHelloResultSuccess(std::uint32_t requestSourceId, std::uint32_t requestBootId,
                                    std::uint32_t replyToSequence, const EffectiveLimits& limits,
                                    const std::uint8_t localUuid[16], std::uint32_t configRevision,
                                    std::uint8_t* output, std::size_t outputCapacity) noexcept {
    if (localUuid == nullptr) {
        return 0U;
    }
    btp::HelloResult result{};
    result.request = {requestSourceId, requestBootId, replyToSequence};
    result.status = static_cast<std::uint8_t>(btp::ResultStatus::Success);
    result.selected_version = 1U;
    result.error_code = static_cast<std::uint16_t>(btp::ResultError::None);
    result.max_logical_payload = limits.maxLogicalPayload;
    result.max_inflight_reassemblies = limits.maxInflightReassemblies;
    result.max_subscriptions = limits.maxSubscriptions;
    result.max_dedup_entries = limits.maxDedupEntries;
    result.session_timeout_ms = limits.sessionTimeoutMs;
    std::memcpy(result.peer_uuid, localUuid, 16U);
    result.config_revision = configRevision;

    std::size_t written = 0U;
    if (btp::encode_hello_result(result, output, outputCapacity, &written) != btp::MessageError::Ok) {
        return 0U;
    }
    return written;
}

std::size_t buildHelloResultFailure(std::uint32_t requestSourceId, std::uint32_t requestBootId,
                                    std::uint32_t replyToSequence, std::uint8_t* output,
                                    std::size_t outputCapacity) noexcept {
    btp::HelloResult result{};
    result.request = {requestSourceId, requestBootId, replyToSequence};
    result.status = static_cast<std::uint8_t>(btp::ResultStatus::Unsupported);
    result.selected_version = 0U;  // no version selected on failure
    result.error_code = static_cast<std::uint16_t>(btp::ResultError::UnsupportedVersion);
    // limits, peer_uuid and config_revision stay zero.

    std::size_t written = 0U;
    if (btp::encode_hello_result(result, output, outputCapacity, &written) != btp::MessageError::Ok) {
        return 0U;
    }
    return written;
}

bool parseSessionClose(btp::ByteView payload, SessionCloseView* out) noexcept {
    if (out == nullptr) {
        return false;
    }
    // btp::decode_session_close enforces the 8-octet size, the zero reserved
    // octets and the reason enum (0..3) -- the last is stricter than the old
    // hand-rolled parse, which stored any reason byte; onFrame() only cares
    // whether the parse succeeded (SUCCESS vs REJECTED in the result).
    btp::SessionClose close{};
    if (btp::decode_session_close(payload.data, payload.size, &close) != btp::MessageError::Ok) {
        return false;
    }
    out->reason = close.reason;
    out->drainTimeoutMs = close.drain_timeout_ms;
    return true;
}

std::size_t buildSessionCloseResult(std::uint32_t requestSourceId, std::uint32_t requestBootId,
                                    std::uint32_t replyToSequence, std::uint8_t status,
                                    std::uint16_t errorCode, std::uint8_t* output,
                                    std::size_t outputCapacity) noexcept {
    btp::ControlResult result{};
    result.request = {requestSourceId, requestBootId, replyToSequence};
    result.status = status;
    result.error_code = errorCode;

    std::size_t written = 0U;
    if (btp::encode_session_close_result(result, output, outputCapacity, &written) != btp::MessageError::Ok) {
        return 0U;
    }
    return written;
}

// The module's wire constants must stay equal to the library's -- SerialMux
// and the tests size buffers off the SerialSession names, btp::messages does
// the actual serialization. This is the "24 vs 28" guard, now cross-checked
// against the single source of truth.
static_assert(kStatusPayloadSize == btp::kStatusV1Size,
              "kStatusPayloadSize must match btp::kStatusV1Size");
static_assert(kTopicStatusRecordSize == btp::kTopicStatusRecordSize,
              "kTopicStatusRecordSize must match btp::kTopicStatusRecordSize");

std::size_t buildStatus(const StatusCounters& counters, std::uint8_t* output,
                        std::size_t outputCapacity) noexcept {
    const btp::StatusV1 base = toStatusBlock(counters);
    std::size_t written = 0U;
    if (btp::encode_status_v1(base, output, outputCapacity, &written) != btp::MessageError::Ok) {
        return 0U;
    }
    return written;
}

std::size_t buildStatusV2(const StatusCounters& counters, const TopicStatusRecord* topics, std::size_t topicCount,
                          std::uint8_t* output, std::size_t outputCapacity) noexcept {
    const btp::StatusV1 base = toStatusBlock(counters);

    // SerialSession::TopicStatusRecord and btp::TopicStatusRecord are the same
    // wire shape; copy field-by-field so a future divergence is a compile error
    // here, not a silent misencode.
    btp::TopicStatusRecord scratch[kMaxStatusTopics];
    if (topicCount > kMaxStatusTopics || (topics == nullptr && topicCount != 0U)) {
        return 0U;
    }
    for (std::size_t i = 0U; i < topicCount; ++i) {
        scratch[i].source_id = topics[i].sourceId;
        scratch[i].topic_id = topics[i].topicId;
        scratch[i].subscriber_count = topics[i].subscriberCount;
        scratch[i].effective_rate_millihz = topics[i].effectiveRateMillihz;
        scratch[i].bytes_total = topics[i].bytesTotal;
        scratch[i].samples_dropped_total = topics[i].samplesDroppedTotal;
    }

    std::size_t written = 0U;
    if (btp::encode_status_v2(base, scratch, topicCount, output, outputCapacity, &written) !=
        btp::MessageError::Ok) {
        return 0U;
    }
    return written;
}

bool tryParseEnterLine(const char* line, char outReadyLine[kReadyLineCapacity]) noexcept {
    if (line == nullptr) {
        return false;
    }

    static const char kPrefix[] = "BTP/1 ENTER ";
    constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1U;

    std::size_t len = 0U;
    while (line[len] != '\0') {
        ++len;
    }
    if (len != kPrefixLen + kNonceHexLength) {
        return false;
    }

    for (std::size_t i = 0U; i < kPrefixLen; ++i) {
        if (line[i] != kPrefix[i]) {
            return false;
        }
    }

    char nonce[kNonceHexLength];
    for (std::size_t i = 0U; i < kNonceHexLength; ++i) {
        const char c = line[kPrefixLen + i];
        if (!isHexDigit(c)) {
            return false;
        }
        nonce[i] = toLowerHex(c);
    }

    std::size_t pos = 0U;
    static const char kReadyPrefix[] = "BTP/1 READY ";
    for (std::size_t i = 0U; kReadyPrefix[i] != '\0'; ++i) {
        outReadyLine[pos++] = kReadyPrefix[i];
    }
    for (std::size_t i = 0U; i < kNonceHexLength; ++i) {
        outReadyLine[pos++] = nonce[i];
    }
    outReadyLine[pos++] = '\r';
    outReadyLine[pos++] = '\n';
    outReadyLine[pos] = '\0';
    return true;
}

void buildReadyLineFromNonce(std::uint64_t nonce, char outReadyLine[kReadyLineCapacity]) noexcept {
    static const char kHexDigits[] = "0123456789abcdef";
    std::size_t pos = 0U;
    static const char kReadyPrefix[] = "BTP/1 READY ";
    for (std::size_t i = 0U; kReadyPrefix[i] != '\0'; ++i) {
        outReadyLine[pos++] = kReadyPrefix[i];
    }
    for (int shift = 60; shift >= 0; shift -= 4) {
        outReadyLine[pos++] = kHexDigits[(nonce >> shift) & 0xFU];
    }
    outReadyLine[pos++] = '\r';
    outReadyLine[pos++] = '\n';
    outReadyLine[pos] = '\0';
}

void buildConsoleLine(char outConsoleLine[kConsoleLineCapacity]) noexcept {
    static const char kLine[] = "BTP/1 CONSOLE\r\n";
    std::size_t i = 0U;
    for (; kLine[i] != '\0'; ++i) {
        outConsoleLine[i] = kLine[i];
    }
    outConsoleLine[i] = '\0';
}

Session::Session(const LocalLimits& localLimits) noexcept
    : local_(localLimits),
      state_(State::Console),
      deadlineMs_(0U),
      localUuid_{0},
      peerSourceId_(0U),
      peerBootId_(0U) {}

void Session::setLocalUuid(const std::uint8_t uuid[16]) noexcept {
    if (uuid == nullptr) {
        return;
    }
    std::memcpy(localUuid_, uuid, 16U);
}

void Session::beginNegotiation(std::uint64_t nowMs) noexcept {
    state_ = State::AwaitingHello;
    deadlineMs_ = nowMs + kHelloDeadlineMs;
    peerSourceId_ = 0U;
    peerBootId_ = 0U;
}

Session::FrameResult Session::onFrame(const btp::DecodedFrame& frame, std::uint64_t nowMs,
                                      std::uint8_t* outPayload, std::size_t outPayloadCapacity) noexcept {
    FrameResult result{};

    if (state_ == State::AwaitingHello) {
        if (frame.header.type == btp::MessageType::Control && frame.header.object_id == kHelloObjectId) {
            HelloView hello{};
            const HelloParseError parseError = parseHello(frame.payload, &hello);
            if (parseError == HelloParseError::Ok && hello.supportsVersion1) {
                effective_ = negotiate(hello, local_);
                peerSourceId_ = frame.header.source_id;
                peerBootId_ = frame.header.boot_id;
                const std::size_t written = buildHelloResultSuccess(
                    frame.header.source_id, frame.header.boot_id, frame.header.sequence,
                    effective_, localUuid_, localConfigRevision_, outPayload, outPayloadCapacity);
                if (written > 0U) {
                    state_ = State::Protocolled;
                    deadlineMs_ = nowMs + effective_.sessionTimeoutMs;
                    result.outcome = FrameOutcome::HelloAccepted;
                    result.outPayloadSize = written;
                    return result;
                }
            }

            // Malformed HELLO or no common version: fail closed, back to console
            // (session-and-terminal.md section 2: "closes the session after
            // transmitting the response").
            const std::size_t written = buildHelloResultFailure(
                frame.header.source_id, frame.header.boot_id, frame.header.sequence,
                outPayload, outPayloadCapacity);
            result.outcome = FrameOutcome::HelloRejected;
            result.outPayloadSize = written;
            result.consoleTransition = true;
            buildConsoleLine(result.consoleLine);
            state_ = State::Console;
            return result;
        }

        // "O primeiro frame do cliente MUST ser HELLO... nenhuma outra
        // mensagem antes dele": anything else is ignored and does NOT renew
        // the 2s HELLO deadline.
        return result;
    }

    if (state_ != State::Protocolled) {
        return result; // Console state never decodes frames; nothing to do here.
    }

    // Protocolled: any validly decoded BTP frame renews the session watchdog,
    // regardless of whether its object is understood below.
    deadlineMs_ = nowMs + effective_.sessionTimeoutMs;

    if (frame.header.type == btp::MessageType::Control && frame.header.object_id == kSessionCloseObjectId) {
        SessionCloseView close{};
        const bool parsed = parseSessionClose(frame.payload, &close);
        const std::size_t written = buildSessionCloseResult(
            frame.header.source_id, frame.header.boot_id, frame.header.sequence,
            parsed ? 0x00U : 0x01U /*SUCCESS/REJECTED*/, parsed ? 0x0000U : 0x0001U /*NONE/MALFORMED_PAYLOAD*/,
            outPayload, outPayloadCapacity);
        result.outcome = FrameOutcome::SessionClosed;
        result.outPayloadSize = written;
        result.consoleTransition = true;
        buildConsoleLine(result.consoleLine);
        state_ = State::Console;
        return result;
    }

    if (frame.header.type == btp::MessageType::Command &&
        frame.header.object_id == BtpTransport::btp_command::kCommandRequestObjectId) {
        result.outcome = FrameOutcome::CommandRequest;
        return result;
    }

    if (frame.header.type == btp::MessageType::Terminal && frame.header.object_id == kTerminalInObjectId) {
        result.outcome = FrameOutcome::TerminalIn;
        return result;
    }

    if (frame.header.type == btp::MessageType::Control && frame.header.object_id == kManifestRequestObjectId) {
        result.outcome = FrameOutcome::ManifestRequest;
        return result;
    }

    if (frame.header.type == btp::MessageType::Control && frame.header.object_id == kSubscribeObjectId) {
        result.outcome = FrameOutcome::SubscribeRequest;
        return result;
    }

    if (frame.header.type == btp::MessageType::Control && frame.header.object_id == kUnsubscribeObjectId) {
        result.outcome = FrameOutcome::UnsubscribeRequest;
        return result;
    }

    // Reserved/unsupported object for this topic (a stray HELLO mid-session,
    // etc.): ignored, watchdog already renewed above.
    return result;
}

bool Session::pollTimeout(std::uint64_t nowMs, char outConsoleLine[kConsoleLineCapacity]) noexcept {
    if (state_ == State::Console) {
        return false;
    }
    if (nowMs < deadlineMs_) {
        return false;
    }
    state_ = State::Console;
    buildConsoleLine(outConsoleLine);
    return true;
}

bool Session::onTransportLost() noexcept {
    if (state_ == State::Console) {
        return false;
    }
    state_ = State::Console;
    deadlineMs_ = 0U;
    return true;
}

} // namespace SerialSession
