#include "BtpTransport.h"

#include <btp/endpoint.hpp>
#include <btp/messages.hpp>

#include <cstring>

#if defined(ARDUINO)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#else
#include <mutex>
#endif

namespace BtpTransport {

// The module's tag-size constant must stay equal to the library's -- callers
// and tests size sealed buffers off the BtpTransport name, btp::Endpoint does
// the actual sealing/fragmenting.
static_assert(kAeadTagSize == btp::kEndpointAeadTagSize,
              "kAeadTagSize must match btp::kEndpointAeadTagSize");

namespace {

// Identity, the atomic outgoing sequence and the seal -> fragment -> encode
// pipeline are btp::Endpoint (BTP >= 2.7.0). g_endpoint is a pointer, not the
// instance itself, so bindEndpoint() can redirect every call in this file to
// an externally-owned Endpoint (SerialMux's btp::Node, in production) without
// this module needing to know that type exists. g_defaultEndpoint is what
// every native test (and this module, before SerialMux::begin() runs) uses
// unless something rebinds it.
btp::Endpoint g_defaultEndpoint;
btp::Endpoint* g_endpoint = &g_defaultEndpoint;

// btp::Endpoint's send callback takes (frame, size); this module's SendFn adds
// the destination MAC (ESP-NOW is addressed). These thunks carry the MAC and
// the real callback through btp::Endpoint as its send context.
struct MacSendAdapter {
    SendFn fn;
    void* ctx;
    const std::uint8_t* mac;
};

bool macSendThunk(void* context, const std::uint8_t* frame, std::size_t size) noexcept {
    auto* adapter = static_cast<MacSendAdapter*>(context);
    return adapter->fn(adapter->ctx, adapter->mac, frame, size);
}

struct MacSendStatusAdapter {
    SendWithStatusFn fn;
    void* ctx;
    const std::uint8_t* mac;
    std::uint32_t timeoutMs;
    bool allDelivered;  // AND of every fragment's delivery verdict
};

bool macSendStatusThunk(void* context, const std::uint8_t* frame, std::size_t size) noexcept {
    auto* adapter = static_cast<MacSendStatusAdapter*>(context);
    bool delivered = false;
    if (!adapter->fn(adapter->ctx, adapter->mac, frame, size, &delivered, adapter->timeoutMs)) {
        return false;  // aborts the remaining fragments; send_logical() returns false
    }
    adapter->allDelivered = adapter->allDelivered && delivered;
    return true;
}

struct PeerIdentity {
    bool used = false;
    std::uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    std::uint32_t sourceId = 0U;
    std::uint32_t bootId = 0U;
    std::uint32_t lastAuthenticatedMs = 0U;
    bool linkOk = false;
    std::int8_t rssi = -128;
    bool pingPending = false;
    std::uint32_t pingSequence = 0U;
    std::uint32_t pingSentMs = 0U;
    std::uint32_t rttMs = 0U;
};

PeerIdentity g_peers[kPeerIdentityCapacity];

// rememberAuthenticatedPeer() runs on espnow_rx, notePeerLinkResult() on the
// heartbeat task, and enumerate/lookup on the Arduino loop. A plain struct assignment
// is not an atomic snapshot on either core, so every access to g_peers must
// share one very short critical section.  The firmware path uses ESP-IDF's
// cross-core spinlock; the native test path uses the standard mutex.
#if defined(ARDUINO)
portMUX_TYPE g_peerTableMux = portMUX_INITIALIZER_UNLOCKED;

class PeerTableGuard {
public:
    PeerTableGuard() noexcept { portENTER_CRITICAL(&g_peerTableMux); }
    ~PeerTableGuard() { portEXIT_CRITICAL(&g_peerTableMux); }

    PeerTableGuard(const PeerTableGuard&) = delete;
    PeerTableGuard& operator=(const PeerTableGuard&) = delete;
};
#else
std::mutex g_peerTableMutex;

class PeerTableGuard {
public:
    PeerTableGuard() noexcept : lock_(g_peerTableMutex) {}

    PeerTableGuard(const PeerTableGuard&) = delete;
    PeerTableGuard& operator=(const PeerTableGuard&) = delete;

private:
    std::lock_guard<std::mutex> lock_;
};
#endif

bool sameMac(const std::uint8_t* lhs, const std::uint8_t* rhs) noexcept {
    return std::memcmp(lhs, rhs, 6U) == 0;
}

}  // namespace

void configureIdentity(std::uint32_t sourceId, std::uint32_t bootId) noexcept {
    // configure() also resets the sequence counter to 1 (as the old store did)
    // and refuses a zero in either field -- BTP reserves 0, and a caller that
    // passes one used to get frames stamped 0/0; now the endpoint stays
    // unconfigured and every send fails closed.
    (void)g_endpoint->configure(sourceId, bootId);
}

void bindEndpoint(btp::Endpoint& endpoint) noexcept { g_endpoint = &endpoint; }

std::uint32_t sourceId() noexcept { return g_endpoint->source_id(); }

std::uint32_t bootId() noexcept { return g_endpoint->boot_id(); }

bool reserveSequence(std::uint32_t* sequenceOut) noexcept {
    return g_endpoint->reserve_sequence(sequenceOut);
}

bool encodeSingleFrame(btp::MessageType type,
                       std::uint16_t objectId,
                       std::uint32_t sequence,
                       std::uint64_t timestampUs,
                       const std::uint8_t* payload,
                       std::size_t payloadSize,
                       std::uint8_t* output,
                       std::size_t outputCapacity,
                       std::size_t* bytesWritten,
                       SealFn seal,
                       void* sealContext) noexcept {
    if (sequence == 0U || seal == nullptr || (payload == nullptr && payloadSize != 0U)) {
        return false;
    }
    // encode_fragment() with a seal enforces the single-frame ceiling itself
    // (payload.size + kEndpointAeadTagSize <= kEspNowMaxPayloadSize) and builds
    // the canonical ENCRYPTED header; a false from `seal` fails the call closed.
    const btp::LogicalMessage message{type, objectId, timestampUs, {payload, payloadSize}};
    return g_endpoint->encode_fragment(message, btp::kEspNowTransport, sequence,
                                      /*fragment_index=*/0U, /*fragment_count=*/1U, output,
                                      outputCapacity, bytesWritten, seal, sealContext);
}

bool sendLogical(SendFn send,
                 void* context,
                 const std::uint8_t mac[6],
                 btp::MessageType type,
                 std::uint16_t objectId,
                 const std::uint8_t* payload,
                 std::size_t payloadSize,
                 std::uint64_t timestampUs,
                 SealFn seal,
                 void* sealContext) noexcept {
    if (send == nullptr || seal == nullptr || mac == nullptr ||
        (payload == nullptr && payloadSize != 0U)) {
        return false;
    }
    if (payloadSize > kMaxLogicalPayloadSize) return false;

    MacSendAdapter adapter{send, context, mac};
    std::uint8_t sealScratch[kMaxLogicalPayloadSize + kAeadTagSize];
    const btp::LogicalMessage message{type, objectId, timestampUs, {payload, payloadSize}};
    return g_endpoint->send_logical(message, btp::kEspNowTransport, &macSendThunk, &adapter,
                                   sealScratch, sizeof(sealScratch), seal, sealContext);
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
                           std::uint32_t timeoutMs,
                           SealFn seal,
                           void* sealContext) noexcept {
    outDelivered = false;
    if (sendWithStatus == nullptr || seal == nullptr || mac == nullptr ||
        (payload == nullptr && payloadSize != 0U)) {
        return false;
    }
    if (payloadSize > kMaxLogicalPayloadSize) return false;

    MacSendStatusAdapter adapter{sendWithStatus, context, mac, timeoutMs, /*allDelivered=*/true};
    std::uint8_t sealScratch[kMaxLogicalPayloadSize + kAeadTagSize];
    const btp::LogicalMessage message{type, objectId, timestampUs, {payload, payloadSize}};
    if (!g_endpoint->send_logical(message, btp::kEspNowTransport, &macSendStatusThunk,
                                 &adapter, sealScratch, sizeof(sealScratch), seal, sealContext)) {
        return false;
    }
    outDelivered = adapter.allDelivered;
    return true;
}

void rememberAuthenticatedPeer(const std::uint8_t mac[6], std::uint32_t sourceId,
                               std::uint32_t bootId, std::uint32_t nowMs,
                               std::int8_t rssi) noexcept {
    if (mac == nullptr || sourceId == 0U || bootId == 0U) return;

    const PeerTableGuard guard;

    for (std::size_t index = 0U; index < kPeerIdentityCapacity; ++index) {
        if (g_peers[index].used && sameMac(g_peers[index].mac, mac)) {
            g_peers[index].sourceId = sourceId;
            g_peers[index].bootId = bootId;
            g_peers[index].lastAuthenticatedMs = nowMs;
            g_peers[index].rssi = rssi;
            return;
        }
    }

    for (std::size_t index = 0U; index < kPeerIdentityCapacity; ++index) {
        if (!g_peers[index].used) {
            g_peers[index].used = true;
            std::memcpy(g_peers[index].mac, mac, 6U);
            g_peers[index].sourceId = sourceId;
            g_peers[index].bootId = bootId;
            g_peers[index].lastAuthenticatedMs = nowMs;
            g_peers[index].rssi = rssi;
            // A recycled slot must not inherit the previous occupant's
            // heartbeat verdict: nothing has been probed at this MAC yet.
            g_peers[index].linkOk = false;
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
    g_peers[0].lastAuthenticatedMs = nowMs;
    g_peers[0].rssi = rssi;
    g_peers[0].linkOk = false;
}

void notePeerLinkResult(const std::uint8_t mac[6], bool delivered) noexcept {
    if (mac == nullptr) return;

    const PeerTableGuard guard;

    for (std::size_t index = 0U; index < kPeerIdentityCapacity; ++index) {
        if (g_peers[index].used && sameMac(g_peers[index].mac, mac)) {
            g_peers[index].linkOk = delivered;
            return;
        }
    }
}

void notePingSent(const std::uint8_t mac[6], std::uint32_t sequence, std::uint32_t nowMs) noexcept {
    if (mac == nullptr) return;

    const PeerTableGuard guard;

    for (std::size_t index = 0U; index < kPeerIdentityCapacity; ++index) {
        if (g_peers[index].used && sameMac(g_peers[index].mac, mac)) {
            g_peers[index].pingPending = true;
            g_peers[index].pingSequence = sequence;
            g_peers[index].pingSentMs = nowMs;
            return;
        }
    }
}

void notePingReply(const std::uint8_t mac[6], std::uint32_t replyToSequence,
                   std::uint32_t nowMs) noexcept {
    if (mac == nullptr) return;

    const PeerTableGuard guard;

    for (std::size_t index = 0U; index < kPeerIdentityCapacity; ++index) {
        if (g_peers[index].used && sameMac(g_peers[index].mac, mac)) {
            if (g_peers[index].pingPending && g_peers[index].pingSequence == replyToSequence) {
                // Unsigned wrap makes this the correct elapsed time even
                // across the ~49-day millis() rollover (same reasoning as
                // AppRuntime's lastSeenAgeMs).
                g_peers[index].rttMs = nowMs - g_peers[index].pingSentMs;
                g_peers[index].pingPending = false;
            }
            return;
        }
    }
}

std::size_t enumeratePeers(PeerSnapshot* out, std::size_t maxOut) noexcept {
    if (out == nullptr || maxOut == 0U) return 0U;

    const PeerTableGuard guard;

    std::size_t written = 0U;
    for (std::size_t index = 0U; index < kPeerIdentityCapacity && written < maxOut; ++index) {
        if (!g_peers[index].used) continue;
        std::memcpy(out[written].mac, g_peers[index].mac, 6U);
        out[written].sourceId = g_peers[index].sourceId;
        out[written].bootId = g_peers[index].bootId;
        out[written].lastAuthenticatedMs = g_peers[index].lastAuthenticatedMs;
        out[written].linkOk = g_peers[index].linkOk;
        out[written].rssi = g_peers[index].rssi;
        out[written].rttMs = g_peers[index].rttMs;
        ++written;
    }
    return written;
}

bool lookupPeer(const std::uint8_t mac[6], std::uint32_t* outSourceId, std::uint32_t* outBootId) noexcept {
    if (mac == nullptr) return false;

    const PeerTableGuard guard;

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

    const PeerTableGuard guard;

    for (std::size_t index = 0U; index < kPeerIdentityCapacity; ++index) {
        if (g_peers[index].used && g_peers[index].sourceId == sourceId) {
            std::memcpy(outMac, g_peers[index].mac, 6U);
            if (outBootId != nullptr) *outBootId = g_peers[index].bootId;
            return true;
        }
    }
    return false;
}

void resetPeerTableForTests() noexcept {
    const PeerTableGuard guard;
    std::memset(g_peers, 0, sizeof(g_peers));
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

    // The COMMAND_REQUEST layout (commands.md section 2.1) is btp::decode_command_request:
    // the 20-octet prefix, the zero flags/reserved words, every id non-zero and
    // "parameter_size consumes the payload exactly" all move into the library.
    btp::CommandRequest cmd{};
    const btp::MessageError err = btp::decode_command_request(payload.data, payload.size, &cmd);
    if (err != btp::MessageError::Ok) {
        switch (err) {
            case btp::MessageError::PayloadTooShort:
                return ParseError::PayloadTooShort;
            case btp::MessageError::ReservedNotZero:
                return ParseError::InvalidFlags;
            case btp::MessageError::TrailingBytes:
            case btp::MessageError::LengthOverflow:
                return ParseError::SizeMismatch;
            case btp::MessageError::CountTooLarge:  // parameter_size past the wire ceiling
                return ParseError::ParametersTooLarge;
            default:  // ZeroField: a zero target or action id -- was InvalidEnvelope / InvalidAction
                return ParseError::InvalidEnvelope;
        }
    }

    if (cmd.target_source_id != local_source_id || cmd.target_boot_id != local_boot_id) {
        return ParseError::WrongTarget;
    }
    // Stricter local ceiling than the wire's 32768: one TinyShell line.
    if (cmd.parameters.size > kMaxShellCommandSize) return ParseError::ParametersTooLarge;

    request_out->target_source_id = cmd.target_source_id;
    request_out->target_boot_id = cmd.target_boot_id;
    request_out->action_id = cmd.action_id;
    request_out->action_version = cmd.action_version;
    request_out->parameters = cmd.parameters;
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

    // COMMAND_RESULT layout (commands.md section 2.4) is btp::decode_command_result:
    // the 26-octet prefix, the zero reserved byte, the utf8_u16 message and the
    // bytes_u32 result, and the "status is a defined ResultStatus" check the
    // old hand parse skipped.
    btp::CommandResult res{};
    const btp::MessageError err = btp::decode_command_result(payload.data, payload.size, &res);
    if (err != btp::MessageError::Ok) {
        switch (err) {
            case btp::MessageError::PayloadTooShort:
                return ParseError::PayloadTooShort;
            default:  // TrailingBytes / LengthOverflow / CountTooLarge / ReservedNotZero / InvalidValue
                return ParseError::SizeMismatch;
        }
    }

    result_out->request_source_id = res.request.request_source_id;
    result_out->request_boot_id = res.request.request_boot_id;
    result_out->reply_to_sequence = res.request.reply_to_sequence;
    result_out->action_id = res.action_id;
    result_out->action_version = res.action_version;
    result_out->status = static_cast<Status>(res.status);
    result_out->error_code = static_cast<ErrorCode>(res.error_code);
    result_out->message = res.message;
    result_out->result = res.result;
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
    if (messageLen > kMaxResultMessageSize) return 0U;  // local cap, tighter than the wire's
    if (result_size > 0U && result == nullptr) return 0U;

    btp::CommandResult out{};
    out.request = {request_source_id, request_boot_id, reply_to_sequence};
    out.action_id = action_id;
    out.action_version = action_version;
    out.status = static_cast<std::uint8_t>(status);
    out.error_code = static_cast<std::uint16_t>(error_code);
    out.message = {reinterpret_cast<const std::uint8_t*>(message), messageLen};
    out.result = {result, result_size};

    std::size_t written = 0U;
    if (btp::encode_command_result(out, output, output_capacity, &written) != btp::MessageError::Ok) {
        return 0U;
    }
    return written;
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
