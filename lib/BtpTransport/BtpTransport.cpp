#include "BtpTransport.h"

#include <btp/fragmentation.hpp>

#include <atomic>
#include <cstring>
#include <limits>

namespace BtpTransport {
namespace {

std::atomic<std::uint32_t> g_sourceId{0U};
std::atomic<std::uint32_t> g_bootId{0U};
std::atomic<std::uint32_t> g_nextSequence{0U};

struct PeerIdentity {
    bool used = false;
    std::uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    std::uint32_t sourceId = 0U;
    std::uint32_t bootId = 0U;
};

PeerIdentity g_peers[kPeerIdentityCapacity];

bool sameMac(const std::uint8_t* lhs, const std::uint8_t* rhs) noexcept {
    return std::memcmp(lhs, rhs, 6U) == 0;
}

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

}  // namespace

void configureIdentity(std::uint32_t sourceId, std::uint32_t bootId) noexcept {
    g_sourceId.store(sourceId, std::memory_order_release);
    g_bootId.store(bootId, std::memory_order_release);
    g_nextSequence.store(1U, std::memory_order_release);
}

std::uint32_t sourceId() noexcept {
    return g_sourceId.load(std::memory_order_acquire);
}

std::uint32_t bootId() noexcept {
    return g_bootId.load(std::memory_order_acquire);
}

bool reserveSequence(std::uint32_t* sequenceOut) noexcept {
    if (sequenceOut == nullptr) return false;

    std::uint32_t current = g_nextSequence.load(std::memory_order_acquire);
    while (current != 0U) {
        const std::uint32_t next = current == std::numeric_limits<std::uint32_t>::max()
                                        ? 0U
                                        : current + 1U;
        if (g_nextSequence.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            *sequenceOut = current;
            return true;
        }
    }
    return false;
}

bool encodeSingleFrame(btp::MessageType type,
                       std::uint16_t objectId,
                       std::uint32_t sequence,
                       std::uint64_t timestampUs,
                       const std::uint8_t* payload,
                       std::size_t payloadSize,
                       std::uint8_t* output,
                       std::size_t outputCapacity,
                       std::size_t* bytesWritten) noexcept {
    if (sequence == 0U || payloadSize > btp::kEspNowMaxPayloadSize) return false;

    const btp::Header header{
        .type = type,
        .flags = 0U,
        .source_id = sourceId(),
        .boot_id = bootId(),
        .sequence = sequence,
        .timestamp_us = timestampUs,
        .object_id = objectId,
        .fragment_index = 0U,
        .fragment_count = 1U,
    };
    const btp::Frame frame{header, {payload, payloadSize}};
    return btp::encode(frame, btp::TransportProfile::EspNow, output, outputCapacity,
                       bytesWritten) == btp::Error::Ok;
}

bool sendLogical(SendFn send,
                 void* context,
                 const std::uint8_t mac[6],
                 btp::MessageType type,
                 std::uint16_t objectId,
                 const std::uint8_t* payload,
                 std::size_t payloadSize,
                 std::uint64_t timestampUs) noexcept {
    if (send == nullptr || mac == nullptr || (payload == nullptr && payloadSize != 0U)) return false;

    std::uint8_t count = 0U;
    if (btp::fragment_count(payloadSize, btp::TransportProfile::EspNow, &count) !=
        btp::Error::Ok) {
        return false;
    }

    std::uint32_t sequence = 0U;
    if (!reserveSequence(&sequence)) return false;

    const btp::Header logicalHeader{
        .type = type,
        .flags = static_cast<std::uint16_t>(count > 1U ? btp::kFlagFragmented : 0U),
        .source_id = sourceId(),
        .boot_id = bootId(),
        .sequence = sequence,
        .timestamp_us = timestampUs,
        .object_id = objectId,
        .fragment_index = 0U,
        .fragment_count = count,
    };

    for (std::uint8_t index = 0U; index < count; ++index) {
        btp::Frame fragment{};
        if (btp::make_fragment(logicalHeader, {payload, payloadSize}, btp::TransportProfile::EspNow,
                               index, &fragment) != btp::Error::Ok) {
            return false;
        }

        std::uint8_t frame[btp::kEspNowMaxFrameSize];
        std::size_t frameSize = 0U;
        if (btp::encode(fragment, btp::TransportProfile::EspNow, frame, sizeof(frame),
                        &frameSize) != btp::Error::Ok) {
            return false;
        }
        if (!send(context, mac, frame, frameSize)) return false;
    }
    return true;
}

bool sendLogicalWithStatus(SendWithStatusFn sendWithStatus,
                           void* context,
                           const std::uint8_t mac[6],
                           btp::MessageType type,
                           std::uint16_t objectId,
                           const std::uint8_t* payload,
                           std::size_t payloadSize,
                           std::uint64_t timestampUs,
                           bool& outDelivered,
                           std::uint32_t timeoutMs) noexcept {
    outDelivered = false;
    if (sendWithStatus == nullptr || mac == nullptr || (payload == nullptr && payloadSize != 0U)) return false;

    std::uint8_t count = 0U;
    if (btp::fragment_count(payloadSize, btp::TransportProfile::EspNow, &count) != btp::Error::Ok) {
        return false;
    }

    std::uint32_t sequence = 0U;
    if (!reserveSequence(&sequence)) return false;

    const btp::Header logicalHeader{
        .type = type,
        .flags = static_cast<std::uint16_t>(count > 1U ? btp::kFlagFragmented : 0U),
        .source_id = sourceId(),
        .boot_id = bootId(),
        .sequence = sequence,
        .timestamp_us = timestampUs,
        .object_id = objectId,
        .fragment_index = 0U,
        .fragment_count = count,
    };

    bool allDelivered = true;
    for (std::uint8_t index = 0U; index < count; ++index) {
        btp::Frame fragment{};
        if (btp::make_fragment(logicalHeader, {payload, payloadSize}, btp::TransportProfile::EspNow,
                               index, &fragment) != btp::Error::Ok) {
            return false;
        }

        std::uint8_t frame[btp::kEspNowMaxFrameSize];
        std::size_t frameSize = 0U;
        if (btp::encode(fragment, btp::TransportProfile::EspNow, frame, sizeof(frame),
                        &frameSize) != btp::Error::Ok) {
            return false;
        }

        bool fragmentDelivered = false;
        if (!sendWithStatus(context, mac, frame, frameSize, &fragmentDelivered, timeoutMs)) {
            return false;
        }
        allDelivered = allDelivered && fragmentDelivered;
    }

    outDelivered = allDelivered;
    return true;
}

void rememberPeer(const std::uint8_t mac[6], std::uint32_t sourceId, std::uint32_t bootId) noexcept {
    if (mac == nullptr || sourceId == 0U || bootId == 0U) return;

    for (std::size_t index = 0U; index < kPeerIdentityCapacity; ++index) {
        if (g_peers[index].used && sameMac(g_peers[index].mac, mac)) {
            g_peers[index].sourceId = sourceId;
            g_peers[index].bootId = bootId;
            return;
        }
    }

    for (std::size_t index = 0U; index < kPeerIdentityCapacity; ++index) {
        if (!g_peers[index].used) {
            g_peers[index].used = true;
            std::memcpy(g_peers[index].mac, mac, 6U);
            g_peers[index].sourceId = sourceId;
            g_peers[index].bootId = bootId;
            return;
        }
    }

    // Table full: overwrite the first slot rather than silently refusing to
    // ever learn a new peer's identity again (small bounded table, no drop counter
    // needed here -- this is a best-effort cache, not a delivery-critical queue).
    g_peers[0].used = true;
    std::memcpy(g_peers[0].mac, mac, 6U);
    g_peers[0].sourceId = sourceId;
    g_peers[0].bootId = bootId;
}

bool lookupPeer(const std::uint8_t mac[6], std::uint32_t* outSourceId, std::uint32_t* outBootId) noexcept {
    if (mac == nullptr) return false;

    for (std::size_t index = 0U; index < kPeerIdentityCapacity; ++index) {
        if (g_peers[index].used && sameMac(g_peers[index].mac, mac)) {
            if (outSourceId != nullptr) *outSourceId = g_peers[index].sourceId;
            if (outBootId != nullptr) *outBootId = g_peers[index].bootId;
            return true;
        }
    }
    return false;
}

bool lookupPeerMacBySourceId(std::uint32_t sourceId, std::uint8_t outMac[6], std::uint32_t* outBootId) noexcept {
    if (sourceId == 0U || outMac == nullptr) return false;

    for (std::size_t index = 0U; index < kPeerIdentityCapacity; ++index) {
        if (g_peers[index].used && g_peers[index].sourceId == sourceId) {
            std::memcpy(outMac, g_peers[index].mac, 6U);
            if (outBootId != nullptr) *outBootId = g_peers[index].bootId;
            return true;
        }
    }
    return false;
}

namespace btp_command {

ParseError parse_request(const btp::Header& header,
                         btp::ByteView payload,
                         std::uint32_t local_source_id,
                         std::uint32_t local_boot_id,
                         RequestView* request_out) noexcept {
    if (request_out == nullptr || (payload.data == nullptr && payload.size != 0U)) {
        return ParseError::InvalidEnvelope;
    }
    if (header.type != btp::MessageType::Command) return ParseError::WrongType;
    if (header.object_id != kCommandRequestObjectId) return ParseError::WrongObject;
    if (payload.size < kRequestPrefixSize) return ParseError::PayloadTooShort;

    RequestView request{
        .target_source_id = readU32(payload.data),
        .target_boot_id = readU32(payload.data + 4U),
        .action_id = readU16(payload.data + 8U),
        .action_version = readU16(payload.data + 10U),
        .parameters = {payload.data + kRequestPrefixSize, 0U},
    };
    const std::uint16_t flags = readU16(payload.data + 12U);
    const std::uint16_t reserved = readU16(payload.data + 14U);
    const std::uint32_t parameterSize = readU32(payload.data + 16U);

    if (request.target_source_id == 0U || request.target_boot_id == 0U) {
        return ParseError::InvalidEnvelope;
    }
    if (request.target_source_id != local_source_id || request.target_boot_id != local_boot_id) {
        return ParseError::WrongTarget;
    }
    if (request.action_id == 0U || request.action_version == 0U) {
        return ParseError::InvalidAction;
    }
    if (flags != 0U || reserved != 0U) return ParseError::InvalidFlags;
    if (parameterSize != payload.size - kRequestPrefixSize) return ParseError::SizeMismatch;
    if (parameterSize > kMaxShellCommandSize) return ParseError::ParametersTooLarge;

    request.parameters.size = parameterSize;
    *request_out = request;
    return ParseError::Ok;
}

ParseError copy_shell_command(const RequestView& request, char* output, std::size_t output_capacity) noexcept {
    if (request.action_id != kShellActionId || request.action_version != kShellActionVersion) {
        return ParseError::UnsupportedAction;
    }
    if (request.parameters.data == nullptr || request.parameters.size == 0U ||
        request.parameters.size > kMaxShellCommandSize) {
        return ParseError::InvalidShellText;
    }
    if (output == nullptr || output_capacity <= request.parameters.size) {
        return ParseError::OutputTooSmall;
    }

    for (std::size_t index = 0U; index < request.parameters.size; ++index) {
        const std::uint8_t byte = request.parameters.data[index];
        // One TinyShell line, never a stream or batch: reject bytes that
        // would let a "command" smuggle extra lines/terminators.
        if (byte == 0U || byte == '\r' || byte == '\n' || byte == 0x7FU ||
            (byte < 0x20U && byte != '\t')) {
            return ParseError::InvalidShellText;
        }
    }

    std::memcpy(output, request.parameters.data, request.parameters.size);
    output[request.parameters.size] = '\0';
    return ParseError::Ok;
}

ParseError parse_result(const btp::Header& header, btp::ByteView payload, ResultView* result_out) noexcept {
    if (result_out == nullptr || (payload.data == nullptr && payload.size != 0U)) {
        return ParseError::InvalidEnvelope;
    }
    if (header.type != btp::MessageType::Command) return ParseError::WrongType;
    if (header.object_id != kCommandResultObjectId) return ParseError::WrongObject;
    if (payload.size < kResultPrefixSize) return ParseError::PayloadTooShort;

    ResultView result{};
    result.request_source_id = readU32(payload.data);
    result.request_boot_id = readU32(payload.data + 4U);
    result.reply_to_sequence = readU32(payload.data + 8U);
    result.action_id = readU16(payload.data + 12U);
    result.action_version = readU16(payload.data + 14U);
    result.status = static_cast<Status>(payload.data[16]);
    result.error_code = static_cast<ErrorCode>(readU16(payload.data + 18U));

    const std::uint16_t messageSize = readU16(payload.data + 20U);
    const std::size_t messageOffset = 22U;
    if (payload.size < messageOffset + messageSize + 4U) return ParseError::SizeMismatch;
    result.message = {payload.data + messageOffset, messageSize};

    const std::size_t resultLenOffset = messageOffset + messageSize;
    const std::uint32_t resultSize = readU32(payload.data + resultLenOffset);
    const std::size_t resultOffset = resultLenOffset + 4U;
    if (payload.size != resultOffset + resultSize) return ParseError::SizeMismatch;
    result.result = {payload.data + resultOffset, resultSize};

    *result_out = result;
    return ParseError::Ok;
}

std::size_t build_result(std::uint32_t request_source_id,
                         std::uint32_t request_boot_id,
                         std::uint32_t reply_to_sequence,
                         std::uint16_t action_id,
                         std::uint16_t action_version,
                         Status status,
                         ErrorCode error_code,
                         const char* message,
                         const std::uint8_t* result,
                         std::size_t result_size,
                         std::uint8_t* output,
                         std::size_t output_capacity) noexcept {
    const std::size_t messageLen = (message == nullptr) ? 0U : std::strlen(message);
    if (messageLen > kMaxResultMessageSize) return 0U;

    const std::size_t total = kResultPrefixSize + messageLen + result_size;
    if (output == nullptr || output_capacity < total) return 0U;
    if (result_size > 0U && result == nullptr) return 0U;

    writeU32(output, request_source_id);
    writeU32(output + 4U, request_boot_id);
    writeU32(output + 8U, reply_to_sequence);
    writeU16(output + 12U, action_id);
    writeU16(output + 14U, action_version);
    output[16] = static_cast<std::uint8_t>(status);
    output[17] = 0U;
    writeU16(output + 18U, static_cast<std::uint16_t>(error_code));
    writeU16(output + 20U, static_cast<std::uint16_t>(messageLen));
    if (messageLen > 0U) std::memcpy(output + 22U, message, messageLen);
    writeU32(output + 22U + messageLen, static_cast<std::uint32_t>(result_size));
    if (result_size > 0U) std::memcpy(output + 26U + messageLen, result, result_size);

    return total;
}

std::uint32_t source_id_from_mac(const std::uint8_t mac[6]) noexcept {
    if (mac == nullptr) return 0U;
    std::uint32_t sourceId = (static_cast<std::uint32_t>(mac[2]) << 24U) |
                             (static_cast<std::uint32_t>(mac[3]) << 16U) |
                             (static_cast<std::uint32_t>(mac[4]) << 8U) |
                             static_cast<std::uint32_t>(mac[5]);
    // A factory MAC cannot normally produce zero here, but BTP reserves it.
    if (sourceId == 0U) sourceId = 1U;
    return sourceId;
}

const char* parse_error_string(ParseError error) noexcept {
    switch (error) {
        case ParseError::Ok: return "ok";
        case ParseError::WrongType: return "not a COMMAND message";
        case ParseError::WrongObject: return "unexpected command object_id";
        case ParseError::InvalidEnvelope: return "invalid logical envelope";
        case ParseError::PayloadTooShort: return "command payload too short";
        case ParseError::WrongTarget: return "command target does not match this dongle";
        case ParseError::InvalidAction: return "invalid action identity";
        case ParseError::InvalidFlags: return "command reserved field is nonzero";
        case ParseError::SizeMismatch: return "command parameter size mismatch";
        case ParseError::ParametersTooLarge: return "command parameters too large";
        case ParseError::UnsupportedAction: return "unsupported action";
        case ParseError::InvalidShellText: return "invalid shell command bytes";
        case ParseError::OutputTooSmall: return "shell output buffer too small";
    }
    return "unknown command parse error";
}

const char* status_string(Status status) noexcept {
    switch (status) {
        case Status::Success: return "success";
        case Status::Rejected: return "rejected";
        case Status::Failed: return "failed";
        case Status::Timeout: return "timeout";
        case Status::Cancelled: return "cancelled";
        case Status::Unsupported: return "unsupported";
        case Status::Busy: return "busy";
    }
    return "unknown";
}

}  // namespace btp_command

}  // namespace BtpTransport
