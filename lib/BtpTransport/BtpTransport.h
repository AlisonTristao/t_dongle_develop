#pragma once

#include <btp/codec.hpp>

#include <cstddef>
#include <cstdint>

/**
 * @brief Dongle-side BTP identity, sequencing and send helpers, plus
 * COMMAND_REQUEST/COMMAND_RESULT envelope parsing (namespace btp_command).
 *
 * Mirrors the BtpEndpoint/btp_command split already used in bally_software's
 * lib/BtpTransport (same wire layout, same source_id-from-MAC formula), so
 * both firmwares derive identical identity from a MAC with no handshake.
 * boot_id has no equivalent shortcut: a peer's boot_id can only be learned by
 * observing a BTP frame it actually sent (see rememberPeer/lookupPeer
 * below), never invented locally. This is a deliberate stand-in for the
 * HELLO/MANIFEST handshake (topico 16, not implemented yet): commands can
 * only target a peer this dongle has already heard from at least once.
 *
 * A plain namespace (no owning instance) on purpose: BtpTransport holds only
 * process-wide identity/state, the same shape as SudoManager, so any layer
 * (EspNowConfig, EspNowCommands) can reach it directly without threading a
 * pointer through ShellCommandSupport::Context.
 *
 * Deliberately has no dependency on EspNowManager or Arduino: sendLogical/
 * sendLogicalWithStatus take a plain send callback instead, so this whole
 * library compiles and is unit-testable under env:native (see
 * test/test_protocol_router) exactly like ProtocolRouter.
 */
namespace BtpTransport {

/** Reassembled/command payload ceiling shared with ProtocolRouter. Covers a
 * full btp_command::kMaxShellCommandSize request with its envelope prefix. */
constexpr std::size_t kMaxLogicalPayloadSize = 600U;

/**
 * @brief Sets this boot's source_id/boot_id. source_id SHOULD be derived via
 * btp_command::source_id_from_mac(); boot_id MUST be nonzero and SHOULD
 * change every boot (no cross-boot persistence is required by this topic).
 */
void configureIdentity(std::uint32_t sourceId, std::uint32_t bootId) noexcept;

std::uint32_t sourceId() noexcept;
std::uint32_t bootId() noexcept;

/** Reserves the next outgoing logical-message sequence. Zero is never
 * returned and becomes a permanent exhausted sentinel (matches bally_software). */
bool reserveSequence(std::uint32_t* sequenceOut) noexcept;

/** Sends raw bytes to one MAC, fire-and-forget ("queued or not"). Bound to
 * EspNowManager::sendToMac() by the caller (context is typically an
 * EspNowManager*). */
using SendFn = bool (*)(void* context, const std::uint8_t mac[6], const std::uint8_t* data, std::size_t size);

/** Sends raw bytes to one MAC and blocks up to timeoutMs for the delivery
 * callback, same contract as EspNowManager::sendToMacWithStatus(). */
using SendWithStatusFn = bool (*)(void* context, const std::uint8_t mac[6], const std::uint8_t* data,
                                  std::size_t size, bool* outDelivered, std::uint32_t timeoutMs);

/**
 * @brief Encodes and sends one logical message to a MAC, fragmenting via
 * btp::fragment_count/make_fragment as needed. Uses a freshly reserved
 * sequence. Best-effort/fire-and-forget: returns false if any fragment fails
 * to send (no partial-send rollback, matches the transport's existing
 * best-effort semantics elsewhere in this codebase).
 */
bool sendLogical(SendFn send,
                 void* context,
                 const std::uint8_t mac[6],
                 btp::MessageType type,
                 std::uint16_t objectId,
                 const std::uint8_t* payload,
                 std::size_t payloadSize,
                 std::uint64_t timestampUs) noexcept;

/**
 * @brief Same as sendLogical, but blocks (up to timeoutMs per fragment) on
 * the delivery callback for every fragment and reports whether all of them
 * were delivered. Used where the caller wants a real yes/no (espnow
 * -send_to/-send_all), unlike sendLogical's fire-and-forget "queued or not".
 */
bool sendLogicalWithStatus(SendWithStatusFn sendWithStatus,
                           void* context,
                           const std::uint8_t mac[6],
                           btp::MessageType type,
                           std::uint16_t objectId,
                           const std::uint8_t* payload,
                           std::size_t payloadSize,
                           std::uint64_t timestampUs,
                           bool& outDelivered,
                           std::uint32_t timeoutMs) noexcept;

/**
 * @brief Encodes one single-fragment frame (payload MUST fit in one ESP-NOW
 * packet) without sending it. Used by the heartbeat, which needs the raw
 * frame bytes to call EspNowManager::sendToMacWithStatus() itself.
 */
bool encodeSingleFrame(btp::MessageType type,
                       std::uint16_t objectId,
                       std::uint32_t sequence,
                       std::uint64_t timestampUs,
                       const std::uint8_t* payload,
                       std::size_t payloadSize,
                       std::uint8_t* output,
                       std::size_t outputCapacity,
                       std::size_t* bytesWritten) noexcept;

/** Bounded cache of the last observed (source_id, boot_id) per peer MAC. */
constexpr std::size_t kPeerIdentityCapacity = 16U;

/** Records the identity carried by a frame actually received from this MAC.
 * Called by the router's consumer for every successfully routed message. */
void rememberPeer(const std::uint8_t mac[6],
                  std::uint32_t sourceId,
                  std::uint32_t bootId) noexcept;

/** Returns the last identity observed for this MAC, when any. */
bool lookupPeer(const std::uint8_t mac[6],
                std::uint32_t* outSourceId,
                std::uint32_t* outBootId) noexcept;

namespace btp_command {

constexpr std::uint16_t kCommandRequestObjectId = 0x0001U;
constexpr std::uint16_t kCommandResultObjectId = 0x0002U;
constexpr std::uint16_t kShellActionId = 0x0001U;
constexpr std::uint16_t kShellActionVersion = 0x0001U;
constexpr std::size_t kRequestPrefixSize = 20U;
constexpr std::size_t kResultPrefixSize = 26U;
constexpr std::size_t kMaxShellCommandSize = 512U;
constexpr std::size_t kMaxResultMessageSize = 400U;

// Common result/error codes from bally_protocol/docs/COMMANDS_AND_ACTIONS.md
// section 2, shared verbatim by every executor in the ecosystem.
enum class Status : std::uint8_t {
    Success = 0x00U,
    Rejected = 0x01U,
    Failed = 0x02U,
    Timeout = 0x03U,
    Cancelled = 0x04U,
    Unsupported = 0x05U,
    Busy = 0x06U,
};

enum class ErrorCode : std::uint16_t {
    None = 0x0000U,
    MalformedPayload = 0x0001U,
    UnknownObject = 0x0002U,
    InvalidArgument = 0x0003U,
    NotAuthorized = 0x0004U,
    CapacityExhausted = 0x0005U,
    ExecutionTimeout = 0x0006U,
    InternalError = 0x0007U,
    UnsupportedVersion = 0x0008U,
    StaleTargetBoot = 0x0009U,
    RequestConflict = 0x000AU,
    NotFound = 0x000BU,
};

enum class ParseError : std::uint8_t {
    Ok,
    WrongType,
    WrongObject,
    InvalidEnvelope,
    PayloadTooShort,
    WrongTarget,
    InvalidAction,
    InvalidFlags,
    SizeMismatch,
    ParametersTooLarge,
    UnsupportedAction,
    InvalidShellText,
    OutputTooSmall,
};

struct RequestView {
    std::uint32_t target_source_id;
    std::uint32_t target_boot_id;
    std::uint16_t action_id;
    std::uint16_t action_version;
    btp::ByteView parameters;
};

struct ResultView {
    std::uint32_t request_source_id;
    std::uint32_t request_boot_id;
    std::uint32_t reply_to_sequence;
    std::uint16_t action_id;
    std::uint16_t action_version;
    Status status;
    ErrorCode error_code;
    btp::ByteView message;
    btp::ByteView result;
};

/** Validates envelope + payload shape of a COMMAND_REQUEST addressed to
 * (local_source_id, local_boot_id). WrongTarget means "not for us", every
 * other non-Ok value means "addressed to us but malformed". */
ParseError parse_request(const btp::Header& header,
                         btp::ByteView payload,
                         std::uint32_t local_source_id,
                         std::uint32_t local_boot_id,
                         RequestView* request_out) noexcept;

/** Extracts and validates the shell one-liner from a parsed request's
 * parameters (rejects embedded NUL/CR/LF/control bytes: one TinyShell line,
 * never a stream). output is NUL-terminated on success. */
ParseError copy_shell_command(const RequestView& request,
                              char* output,
                              std::size_t output_capacity) noexcept;

/** Parses a COMMAND_RESULT payload (any object other than kCommandResultObjectId is WrongObject). */
ParseError parse_result(const btp::Header& header,
                        btp::ByteView payload,
                        ResultView* result_out) noexcept;

/** Encodes one COMMAND_RESULT payload. Returns 0 on failure (capacity too
 * small or message/result too large), else bytes written. */
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
                         std::size_t output_capacity) noexcept;

std::uint32_t source_id_from_mac(const std::uint8_t mac[6]) noexcept;

const char* parse_error_string(ParseError error) noexcept;
const char* status_string(Status status) noexcept;

}  // namespace btp_command

}  // namespace BtpTransport
