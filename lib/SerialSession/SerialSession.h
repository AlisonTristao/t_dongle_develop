#pragma once

#include <btp/codec.hpp>

#include <cstddef>
#include <cstdint>

/**
 * @brief Pure C++ (no Arduino/FreeRTOS) state machine for one BTP v1 serial
 * session: Console -> AwaitingHello -> Protocolled, per
 * BTP/docs/session-and-terminal.md sections 1-5 and 7 and
 * BTP/docs/commands.md section 5. Also
 * builds/parses the small set of CONTROL/logical payloads a session needs
 * (HELLO, HELLO_RESULT, SESSION_CLOSE, SESSION_CLOSE_RESULT, STATUS) and
 * recognizes MANIFEST_REQUEST (topico 16) and SUBSCRIBE/UNSUBSCRIBE
 * (topico 17) -- the payloads themselves are built by ManifestCache /
 * SubscriptionRegistry respectively, kept out of this native-testable
 * session-state module to avoid pulling either dependency in here (see
 * bally_protocol/topicos/13_dongle_serial_mux_sessao.txt RESULTADO for the
 * original topico 13 scoping, which this topico closes).
 *
 * Kept free of Serial/FreeRTOS so it compiles and is unit-tested under
 * env:native, exactly like ProtocolRouter/BtpTransport (topico 12). The
 * Arduino-facing glue (byte I/O, COBS framing on the wire, priority queues,
 * envelope encode via BtpTransport) lives one layer up in SerialMux.
 */
namespace SerialSession {

enum class State : std::uint8_t {
    Console,
    AwaitingHello,
    Protocolled,
};

// object_id values this topic understands, within their respective
// MessageType namespaces (BTP/docs/commands.md section 1).
constexpr std::uint16_t kHelloObjectId = 0x0001U;
constexpr std::uint16_t kHelloResultObjectId = 0x0002U;
constexpr std::uint16_t kManifestRequestObjectId = 0x0003U;
constexpr std::uint16_t kManifestDataObjectId = 0x0004U;
constexpr std::uint16_t kSubscribeObjectId = 0x0005U;
constexpr std::uint16_t kSubscribeResultObjectId = 0x0006U;
constexpr std::uint16_t kUnsubscribeObjectId = 0x0007U;
constexpr std::uint16_t kUnsubscribeResultObjectId = 0x0008U;
constexpr std::uint16_t kStatusObjectId = 0x0009U;
constexpr std::uint16_t kSessionCloseObjectId = 0x000AU;
constexpr std::uint16_t kSessionCloseResultObjectId = 0x000BU;
constexpr std::uint16_t kTerminalInObjectId = 0x0001U;
constexpr std::uint16_t kTerminalOutObjectId = 0x0002U;

// This dongle's own advertised capabilities/ceilings for a serial session.
// Effective limits sent in HELLO_RESULT are min(requested, these).
struct LocalLimits {
    // Must stay equal to SerialMux's kOutboundPayloadCap: this is the number
    // HELLO_RESULT promises the desktop, and a MANIFEST_DATA/TELEMETRY frame
    // larger than what was negotiated is a protocol violation even if the
    // queues happen to hold it. Raised with that cap by topico 27 so the
    // dongle's own manifest (1440 octets) fits inside the negotiated limit.
    std::uint32_t maxLogicalPayload = 1600U;
    std::uint16_t maxInflightReassemblies = 1U;  // topico 13 does not reassemble serial fragments (see deviation note)
    std::uint16_t maxSubscriptions = 8U;         // matches SubscriptionRegistry::kMaxTopics (topico 17)
    std::uint32_t maxDedupEntries = 32U;         // declared; command dedup not implemented on this transport yet
    std::uint32_t sessionTimeoutMs = 15000U;     // watchdog: no valid BTP frame in this window -> back to console
};

constexpr std::uint32_t kHelloDeadlineMs = 2000U; // session-and-terminal.md section 3

// Priority classes a caller (SerialMux) uses to pick a send queue, derived
// from model.md section 6. Classes 2/3 there collapse into
// kSession/kLogStatus here because this dongle never originates
// COMMAND_REQUEST or SUBSCRIBE traffic *toward* the desktop in this topic.
enum class PriorityClass : std::uint8_t {
    kSession = 0,   // HELLO_RESULT, SESSION_CLOSE_RESULT, COMMAND_RESULT
    kTerminal = 1,  // TERMINAL_IN / TERMINAL_OUT
    kLogStatus = 2, // LOG, STATUS, MANIFEST_DATA
    kTelemetry = 3, // TELEMETRY
    kCount = 4,
};

PriorityClass classify(btp::MessageType type, std::uint16_t objectId) noexcept;

struct HelloView {
    std::uint8_t role;
    std::uint8_t versionCount;
    std::uint16_t flags;
    std::uint32_t maxLogicalPayload;
    std::uint16_t maxInflightReassemblies;
    std::uint16_t maxSubscriptions;
    std::uint32_t maxDedupEntries;
    std::uint32_t sessionTimeoutMs;
    std::uint8_t peerUuid[16];
    std::uint32_t configRevision;
    bool supportsVersion1;
};

enum class HelloParseError : std::uint8_t {
    Ok,
    PayloadTooShort,
    SizeMismatch,
    NonZeroReservedFlags,
    ZeroCapability,
    NoVersions,
    VersionsNotAscending,
};

HelloParseError parseHello(btp::ByteView payload, HelloView* out) noexcept;

struct EffectiveLimits {
    std::uint32_t maxLogicalPayload = 0U;
    std::uint16_t maxInflightReassemblies = 0U;
    std::uint16_t maxSubscriptions = 0U;
    std::uint32_t maxDedupEntries = 0U;
    std::uint32_t sessionTimeoutMs = 0U;
};

EffectiveLimits negotiate(const HelloView& hello, const LocalLimits& local) noexcept;

std::size_t buildHelloResultSuccess(std::uint32_t requestSourceId, std::uint32_t requestBootId,
                                    std::uint32_t replyToSequence, const EffectiveLimits& limits,
                                    const std::uint8_t localUuid[16], std::uint32_t configRevision,
                                    std::uint8_t* output, std::size_t outputCapacity) noexcept;

std::size_t buildHelloResultFailure(std::uint32_t requestSourceId, std::uint32_t requestBootId,
                                    std::uint32_t replyToSequence, std::uint8_t* output,
                                    std::size_t outputCapacity) noexcept;

struct SessionCloseView {
    std::uint8_t reason;
    std::uint32_t drainTimeoutMs;
};

bool parseSessionClose(btp::ByteView payload, SessionCloseView* out) noexcept;

std::size_t buildSessionCloseResult(std::uint32_t requestSourceId, std::uint32_t requestBootId,
                                    std::uint32_t replyToSequence, std::uint8_t status,
                                    std::uint16_t errorCode, std::uint8_t* output,
                                    std::size_t outputCapacity) noexcept;

struct StatusCounters {
    std::uint64_t uptimeUs = 0;
    std::uint64_t framesRx = 0;
    std::uint64_t framesTx = 0;
    std::uint64_t framesDropped = 0;
    std::uint64_t crcErrors = 0;
    std::uint64_t decodeErrors = 0;
    std::uint64_t reassemblyCompleted = 0;
    std::uint64_t reassemblyTimeouts = 0;
    std::uint64_t reassemblyRejected = 0;
    std::uint64_t commandDuplicates = 0;
    std::uint64_t telemetryDropped = 0;
    bool degraded = false;
};

constexpr std::size_t kStatusPayloadSize = 92U;

std::size_t buildStatus(const StatusCounters& counters, std::uint8_t* output,
                        std::size_t outputCapacity) noexcept;

// commands.md section 5.1 (topico 17, status_version=2): one
// fixed-size record per (source_id, topic_id) this dongle currently
// tracks a subscription state for. Kept as a plain wire-shaped struct here
// (not SubscriptionRegistry::TopicStatusEntry) so this module stays free of
// a SubscriptionRegistry dependency, matching the ManifestCache precedent
// for MANIFEST_REQUEST/DATA above -- the caller (SerialMux) converts.
struct TopicStatusRecord {
    std::uint32_t sourceId = 0U;
    std::uint16_t topicId = 0U;
    std::uint16_t subscriberCount = 0U;
    std::uint32_t effectiveRateMillihz = 0U;
    std::uint64_t bytesTotal = 0U;
    std::uint64_t samplesDroppedTotal = 0U;
};

// commands.md section 5.1 declares "28 x T" and a 28-octet record:
// source_id(uint32=4) + topic_id(uint16=2) + subscriber_count(uint16=2) +
// effective_rate_millihz(uint32=4) + bytes_total(uint64=8) +
// samples_dropped_total(uint64=8) = 28 octets, which is what this firmware
// has always serialized. (A superseded revision of that section said
// "24 x T" and "24 octetos" by an arithmetic slip in the prose; the per-field
// types were unambiguous even then, so every implementer that wrote all six
// fields at the declared widths landed on 28.) This is the single place the
// stride is defined, so realigning the whole repo to a changed layout is a
// one-line change here.
constexpr std::size_t kTopicStatusRecordSize = 28U;

// Builds a status_version=2 payload: the same 92-byte v1 prefix (with
// status_version patched to 2), a uint16_le topic_status_count, then
// topicCount kTopicStatusRecordSize-byte TopicStatusRecord entries. Returns 0
// on capacity failure (caller falls back to buildStatus()'s v1-only payload
// rather than sending nothing).
std::size_t buildStatusV2(const StatusCounters& counters, const TopicStatusRecord* topics, std::size_t topicCount,
                          std::uint8_t* output, std::size_t outputCapacity) noexcept;

// ---- ENTER/READY/CONSOLE handshake text (session-and-terminal.md 3-4) -----

constexpr std::size_t kNonceHexLength = 16U;
constexpr std::size_t kReadyLineCapacity = 32U;   // "BTP/1 READY " + 16 hex + "\r\n" + NUL
constexpr std::size_t kConsoleLineCapacity = 20U; // "BTP/1 CONSOLE\r\n" + NUL

// Recognizes a fully-formed, already-trimmed "BTP/1 ENTER <16 hex>" console
// line (case insensitive hex digits, no surrounding CR/LF -- the caller's
// line reader already stripped those). On match, writes
// "BTP/1 READY <lowercase hex>\r\n" (NUL-terminated) to outReadyLine and
// returns true; leaves outReadyLine untouched otherwise.
bool tryParseEnterLine(const char* line, char outReadyLine[kReadyLineCapacity]) noexcept;

// Builds a READY line from a locally-generated 64-bit nonce (the
// "dongle -btp_v1" shell command path, PASSO 2), same wire format as
// tryParseEnterLine's reply.
void buildReadyLineFromNonce(std::uint64_t nonce, char outReadyLine[kReadyLineCapacity]) noexcept;

void buildConsoleLine(char outConsoleLine[kConsoleLineCapacity]) noexcept;

/**
 * @brief One session's negotiation state. Pure logic: the caller performs
 * all actual byte I/O (COBS framing, envelope encode/decode via
 * btp::encode/btp::decode + BtpTransport for outgoing sequence/identity) and
 * decides how/where to send an output payload built here.
 */
class Session {
public:
    explicit Session(const LocalLimits& localLimits) noexcept;

    State state() const noexcept { return state_; }
    bool isConsole() const noexcept { return state_ == State::Console; }
    bool isProtocolled() const noexcept { return state_ == State::Protocolled; }

    void setLocalUuid(const std::uint8_t uuid[16]) noexcept;

    // This dongle's own manifest-catalog revision (ManifestCache::
    // catalogRevision(), topico 16 PASSO 5), reported in HELLO_RESULT's
    // config_revision field. Kept as a plain setter rather than a
    // ManifestCache dependency here -- SerialSession stays a leaf, native-
    // testable module; the caller (SerialMux) refreshes this before each
    // HELLO exchange.
    void setLocalConfigRevision(std::uint32_t configRevision) noexcept { localConfigRevision_ = configRevision; }

    // Called once a "BTP/1 ENTER ..." line was recognized (or synthesized by
    // a shell command). Arms the HELLO deadline and moves to AwaitingHello.
    void beginNegotiation(std::uint64_t nowMs) noexcept;

    enum class FrameOutcome : std::uint8_t {
        Ignored,        // valid BTP frame, but not meaningful in this state/type; watchdog still renewed once Protocolled
        HelloAccepted,  // outPayload carries HELLO_RESULT SUCCESS; state is now Protocolled
        HelloRejected,  // outPayload carries HELLO_RESULT UNSUPPORTED; state is back to Console
        SessionClosed,  // outPayload carries SESSION_CLOSE_RESULT; state is back to Console
        CommandRequest, // frame is a COMMAND_REQUEST; caller parses/executes/replies via BtpTransport::btp_command
        TerminalIn,     // frame is a TERMINAL_IN block; caller relays frame.payload bytes to the local shell
        ManifestRequest, // frame is a MANIFEST_REQUEST; caller parses/answers via ManifestCache (topico 16)
        SubscribeRequest,   // frame is a SUBSCRIBE; caller parses/answers via SubscriptionRegistry (topico 17)
        UnsubscribeRequest, // frame is an UNSUBSCRIBE; caller parses/answers via SubscriptionRegistry (topico 17)
    };

    struct FrameResult {
        FrameOutcome outcome = FrameOutcome::Ignored;
        std::size_t outPayloadSize = 0U;
        bool consoleTransition = false;
        char consoleLine[kConsoleLineCapacity] = {0};
    };

    // frame must already be a validly decoded BTP envelope (btp::decode()
    // returned Ok) -- COBS/CRC/envelope errors never reach this call and must
    // not renew the watchdog (matches session-and-terminal.md section 5).
    FrameResult onFrame(const btp::DecodedFrame& frame, std::uint64_t nowMs,
                        std::uint8_t* outPayload, std::size_t outPayloadCapacity) noexcept;

    // Returns true exactly once when the session times out (AwaitingHello
    // deadline, or no valid frame within the negotiated session_timeout_ms)
    // and fills outConsoleLine with "BTP/1 CONSOLE\r\n".
    bool pollTimeout(std::uint64_t nowMs, char outConsoleLine[kConsoleLineCapacity]) noexcept;

    const EffectiveLimits& effectiveLimits() const noexcept { return effective_; }
    std::uint32_t peerSourceId() const noexcept { return peerSourceId_; }
    std::uint32_t peerBootId() const noexcept { return peerBootId_; }

private:
    LocalLimits local_;
    EffectiveLimits effective_{};
    State state_;
    std::uint64_t deadlineMs_;
    std::uint8_t localUuid_[16];
    std::uint32_t peerSourceId_;
    std::uint32_t peerBootId_;
    std::uint32_t localConfigRevision_ = 0U;
};

} // namespace SerialSession
