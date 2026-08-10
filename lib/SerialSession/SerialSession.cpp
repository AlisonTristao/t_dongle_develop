#include "SerialSession.h"

#include <BtpTransport.h>

#include <cstring>

namespace SerialSession {
namespace {

void writeU16(std::uint8_t* output, std::uint16_t value) noexcept {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
}

void writeU32(std::uint8_t* output, std::uint32_t value) noexcept {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
    output[2] = static_cast<std::uint8_t>(value >> 16U);
    output[3] = static_cast<std::uint8_t>(value >> 24U);
}

void writeU64(std::uint8_t* output, std::uint64_t value) noexcept {
    for (std::size_t i = 0U; i < 8U; ++i) {
        output[i] = static_cast<std::uint8_t>(value >> (8U * i));
    }
}

std::uint16_t readU16(const std::uint8_t* input) noexcept {
    return static_cast<std::uint16_t>(input[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[1]) << 8U);
}

std::uint32_t readU32(const std::uint8_t* input) noexcept {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
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
    if (out == nullptr || (payload.data == nullptr && payload.size != 0U)) {
        return HelloParseError::PayloadTooShort;
    }
    if (payload.size < 40U) {
        return HelloParseError::PayloadTooShort;
    }

    const std::uint8_t versionCount = payload.data[1];
    if (payload.size != 40U + static_cast<std::size_t>(versionCount)) {
        return HelloParseError::SizeMismatch;
    }

    HelloView hello{};
    hello.role = payload.data[0];
    hello.versionCount = versionCount;
    hello.flags = readU16(payload.data + 2U);
    hello.maxLogicalPayload = readU32(payload.data + 4U);
    hello.maxInflightReassemblies = readU16(payload.data + 8U);
    hello.maxSubscriptions = readU16(payload.data + 10U);
    hello.maxDedupEntries = readU32(payload.data + 12U);
    hello.sessionTimeoutMs = readU32(payload.data + 16U);
    std::memcpy(hello.peerUuid, payload.data + 20U, 16U);
    hello.configRevision = readU32(payload.data + 36U);

    if (hello.flags != 0U) {
        return HelloParseError::NonZeroReservedFlags;
    }
    if (hello.maxLogicalPayload == 0U || hello.maxInflightReassemblies == 0U ||
        hello.maxSubscriptions == 0U || hello.maxDedupEntries == 0U || hello.sessionTimeoutMs == 0U) {
        return HelloParseError::ZeroCapability;
    }

    bool uuidAllZero = true;
    for (std::size_t i = 0U; i < 16U; ++i) {
        if (hello.peerUuid[i] != 0U) {
            uuidAllZero = false;
            break;
        }
    }
    if (uuidAllZero) {
        return HelloParseError::ZeroCapability;
    }

    if (versionCount == 0U) {
        return HelloParseError::NoVersions;
    }

    hello.supportsVersion1 = false;
    std::uint8_t previous = 0U;
    for (std::size_t i = 0U; i < versionCount; ++i) {
        const std::uint8_t version = payload.data[40U + i];
        if (version == 0U) {
            return HelloParseError::VersionsNotAscending;
        }
        if (i > 0U && version <= previous) {
            return HelloParseError::VersionsNotAscending;
        }
        previous = version;
        if (version == 1U) {
            hello.supportsVersion1 = true;
        }
    }

    *out = hello;
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
    constexpr std::size_t kSize = 52U;
    if (output == nullptr || outputCapacity < kSize || localUuid == nullptr) {
        return 0U;
    }

    writeU32(output, requestSourceId);
    writeU32(output + 4U, requestBootId);
    writeU32(output + 8U, replyToSequence);
    output[12] = 0x00U; // SUCCESS
    output[13] = 0x01U; // selected_version
    writeU16(output + 14U, 0x0000U); // error_code NONE
    writeU32(output + 16U, limits.maxLogicalPayload);
    writeU16(output + 20U, limits.maxInflightReassemblies);
    writeU16(output + 22U, limits.maxSubscriptions);
    writeU32(output + 24U, limits.maxDedupEntries);
    writeU32(output + 28U, limits.sessionTimeoutMs);
    std::memcpy(output + 32U, localUuid, 16U);
    writeU32(output + 48U, configRevision);
    return kSize;
}

std::size_t buildHelloResultFailure(std::uint32_t requestSourceId, std::uint32_t requestBootId,
                                    std::uint32_t replyToSequence, std::uint8_t* output,
                                    std::size_t outputCapacity) noexcept {
    constexpr std::size_t kSize = 52U;
    if (output == nullptr || outputCapacity < kSize) {
        return 0U;
    }
    std::memset(output, 0, kSize);
    writeU32(output, requestSourceId);
    writeU32(output + 4U, requestBootId);
    writeU32(output + 8U, replyToSequence);
    output[12] = 0x05U; // UNSUPPORTED
    output[13] = 0x00U; // selected_version = 0 on failure
    writeU16(output + 14U, 0x0008U); // UNSUPPORTED_VERSION
    return kSize;
}

bool parseSessionClose(btp::ByteView payload, SessionCloseView* out) noexcept {
    if (out == nullptr || payload.data == nullptr || payload.size != 8U) {
        return false;
    }
    if (payload.data[1] != 0U || payload.data[2] != 0U || payload.data[3] != 0U) {
        return false; // reserved octets must be zero
    }
    out->reason = payload.data[0];
    out->drainTimeoutMs = readU32(payload.data + 4U);
    return true;
}

std::size_t buildSessionCloseResult(std::uint32_t requestSourceId, std::uint32_t requestBootId,
                                    std::uint32_t replyToSequence, std::uint8_t status,
                                    std::uint16_t errorCode, std::uint8_t* output,
                                    std::size_t outputCapacity) noexcept {
    constexpr std::size_t kSize = 16U;
    if (output == nullptr || outputCapacity < kSize) {
        return 0U;
    }
    writeU32(output, requestSourceId);
    writeU32(output + 4U, requestBootId);
    writeU32(output + 8U, replyToSequence);
    output[12] = status;
    output[13] = 0U;
    writeU16(output + 14U, errorCode);
    return kSize;
}

std::size_t buildStatus(const StatusCounters& counters, std::uint8_t* output,
                        std::size_t outputCapacity) noexcept {
    if (output == nullptr || outputCapacity < kStatusPayloadSize) {
        return 0U;
    }
    writeU16(output, 1U); // status_version
    writeU16(output + 2U, counters.degraded ? 0x0001U : 0x0000U);
    writeU64(output + 4U, counters.uptimeUs);
    writeU64(output + 12U, counters.framesRx);
    writeU64(output + 20U, counters.framesTx);
    writeU64(output + 28U, counters.framesDropped);
    writeU64(output + 36U, counters.crcErrors);
    writeU64(output + 44U, counters.decodeErrors);
    writeU64(output + 52U, counters.reassemblyCompleted);
    writeU64(output + 60U, counters.reassemblyTimeouts);
    writeU64(output + 68U, counters.reassemblyRejected);
    writeU64(output + 76U, counters.commandDuplicates);
    writeU64(output + 84U, counters.telemetryDropped);
    return kStatusPayloadSize;
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
                    effective_, localUuid_, /*configRevision=*/0U, outPayload, outPayloadCapacity);
                if (written > 0U) {
                    state_ = State::Protocolled;
                    deadlineMs_ = nowMs + effective_.sessionTimeoutMs;
                    result.outcome = FrameOutcome::HelloAccepted;
                    result.outPayloadSize = written;
                    return result;
                }
            }

            // Malformed HELLO or no common version: fail closed, back to console
            // (COMMANDS_AND_ACTIONS.md section 5: "fecha a sessao depois de
            // transmitir a resposta").
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

    // Reserved/unsupported object for this topic (e.g. MANIFEST_*, SUBSCRIBE*,
    // a stray HELLO mid-session): ignored, watchdog already renewed above.
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

} // namespace SerialSession
