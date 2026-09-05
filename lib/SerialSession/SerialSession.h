#pragma once

#include <btp/codec.hpp>
#include <btp/endpoint.hpp>
#include <btp/node.hpp>

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
    std::uint32_t maxLogicalPayload = 2048U;
    std::uint16_t maxInflightReassemblies = 1U;  // topico 13 does not reassemble serial fragments (see deviation note)
    std::uint16_t maxSubscriptions = 8U;         // matches SubscriptionRegistry::kMaxTopics (topico 17)
    std::uint32_t maxDedupEntries = 32U;         // declared; command dedup not implemented on this transport yet
    // Watchdog: no valid BTP frame in this window -> back to console. 30s, not
    // the original 15s: the desktop keepalive (TraceView BtpBackend, topico 35
    // B.1) sends every ~5s, so this is only the backstop for a desktop that
    // died without closing -- and a slightly longer backstop costs nothing
    // while making a brief stall (GC pause, a busy main loop) far less likely
    // to drop a live session. Negotiated value is min(this, client's).
    std::uint32_t sessionTimeoutMs = 30000U;
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

// Kept for test_parse_hello_matches_canonical_vector; the live HELLO decode
// path now runs inside btp::Session (which owns btp::decode_hello +
// btp::negotiate + the HELLO_RESULT / SESSION_CLOSE_RESULT encoders).
HelloParseError parseHello(btp::ByteView payload, HelloView* out) noexcept;

// The negotiated communication limits, surfaced by Session::effectiveLimits().
// Filled from btp::EffectiveLimits after a HELLO is accepted.
struct EffectiveLimits {
    std::uint32_t maxLogicalPayload = 0U;
    std::uint16_t maxInflightReassemblies = 0U;
    std::uint16_t maxSubscriptions = 0U;
    std::uint32_t maxDedupEntries = 0U;
    std::uint32_t sessionTimeoutMs = 0U;
};

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

// Upper bound on topicCount for buildStatusV2 in a single call: the dongle
// tracks a subscription for at most this many topics. Kept independent of
// SubscriptionRegistry::kMaxTopics so this module stays a leaf (SerialMux
// static_asserts the two agree), matching LocalLimits::maxSubscriptions.
constexpr std::size_t kMaxStatusTopics = 8U;

// Builds a status_version=2 payload: the same 92-byte v1 prefix (with
// status_version patched to 2), a uint16_le topic_status_count, then
// topicCount kTopicStatusRecordSize-byte TopicStatusRecord entries. Returns 0
// on capacity failure or topicCount > kMaxStatusTopics (caller falls back to
// buildStatus()'s v1-only payload rather than sending nothing).
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
 * @brief One serial session's state, on top of btp::Node (BTP >= 2.42.0).
 *
 * btp::Node owns the responder state machine -- the Console/AwaitingHello/
 * Protocolled lifetime, the 2s HELLO deadline, the negotiated inactivity
 * watchdog, the HELLO / SESSION_CLOSE handling, AND (unlike the bare
 * btp::Session this replaced) the reassembly/CRC/decode pass and the actual
 * transmission of the HELLO_RESULT / SESSION_CLOSE_RESULT reply -- Node's
 * NodeConfig::send() hook is where that reply is handed off, which is why
 * Session privately implements NodeConfig instead of just holding a session
 * object. This wrapper keeps the dongle-specific pieces the library layer has
 * no notion of: the object_id -> FrameOutcome routing this topic understands,
 * the console<->protocol ASCII text, and the "leave the Console flip to
 * pollTimeout()" ordering SerialMux::tick() was written against.
 *
 * Still pure logic: the caller performs the COBS framing and decides how/
 * where to send the already-encoded reply frame this object stages via
 * pendingFrame() (or the STATUS/MANIFEST_DATA/COMMAND_RESULT/etc. bytes it
 * builds by hand elsewhere, via BtpTransport -- unaffected by this).
 * Outgoing sequence/identity for BOTH transports comes from THIS object's own
 * btp::Node (see endpoint()): btp::Node's session responder always sends its
 * own auto-replies through its own internal Endpoint, so that Endpoint has to
 * be the one every other send on either transport reserves a sequence from
 * too, per BTP/docs/model.md section 4.3 ("the sequence space is shared by
 * all message types generated by the same producer"). SerialMux::begin()
 * binds it into BtpTransport once this object's identity is configured.
 */
class Session : private btp::NodeConfig {
public:
    explicit Session(const LocalLimits& localLimits) noexcept;

    State state() const noexcept;
    bool isConsole() const noexcept { return state() == State::Console; }
    bool isProtocolled() const noexcept { return state() == State::Protocolled; }

    // Sets this boot's source_id/boot_id and goes live (btp::Node::begin()).
    // Called once, after the identity is known (SerialMux::begin()) --
    // mirrors BtpTransport::configureIdentity, kept separate from the
    // constructor because this object is a static-duration global
    // (SerialMux's g_session) built before that identity exists.
    void configureIdentity(std::uint32_t sourceId, std::uint32_t bootId) noexcept;

    // The canonical Endpoint for this dongle's whole outgoing sequence space
    // (both transports -- see the class comment). SerialMux::begin() passes
    // this to BtpTransport::bindEndpoint() once configureIdentity() above has
    // run.
    btp::Endpoint& endpoint() noexcept { return node_.endpoint(); }

    void setLocalUuid(const std::uint8_t uuid[16]) noexcept;

    // This dongle's own manifest-catalog revision (ManifestCache::
    // catalogRevision(), topico 16 PASSO 5), reported in HELLO_RESULT's
    // config_revision field. Kept as a plain setter rather than a
    // ManifestCache dependency here -- SerialSession stays a leaf, native-
    // testable module; the caller (SerialMux) refreshes this before each
    // HELLO exchange. Pushed into btp::Session (via the node's escape hatch)
    // by set_local().
    void setLocalConfigRevision(std::uint32_t configRevision) noexcept;

    // Called once a "BTP/1 ENTER ..." line was recognized (or synthesized by
    // a shell command). Arms the HELLO deadline and moves to AwaitingHello.
    void beginNegotiation(std::uint64_t nowMs) noexcept;

    enum class FrameOutcome : std::uint8_t {
        Ignored,        // valid BTP frame, but not meaningful in this state/type; watchdog still renewed once Protocolled
        HelloAccepted,  // pendingFrame() carries an already-encoded HELLO_RESULT SUCCESS; state is now Protocolled
        HelloRejected,  // pendingFrame() carries an already-encoded HELLO_RESULT UNSUPPORTED; state is back to Console
        SessionClosed,  // pendingFrame() carries an already-encoded SESSION_CLOSE_RESULT; state is back to Console
        CommandRequest, // frame is a COMMAND_REQUEST; caller parses/executes/replies via BtpTransport::btp_command
        TerminalIn,     // frame is a TERMINAL_IN block; caller relays frame.payload bytes to the local shell
        ManifestRequest, // frame is a MANIFEST_REQUEST; caller parses/answers via ManifestCache (topico 16)
        SubscribeRequest,   // frame is a SUBSCRIBE; caller parses/answers via SubscriptionRegistry (topico 17)
        UnsubscribeRequest, // frame is an UNSUBSCRIBE; caller parses/answers via SubscriptionRegistry (topico 17)
    };

    struct FrameResult {
        FrameOutcome outcome = FrameOutcome::Ignored;
        bool consoleTransition = false;
        char consoleLine[kConsoleLineCapacity] = {0};
    };

    // frame must already be a validly decoded BTP envelope (btp::decode()
    // returned Ok) -- COBS/CRC/envelope errors never reach this call and must
    // not renew the watchdog (matches session-and-terminal.md section 5).
    FrameResult onFrame(const btp::DecodedFrame& frame, std::uint64_t nowMs) noexcept;

    // Returns true exactly once when the session times out (AwaitingHello
    // deadline, or no valid frame within the negotiated session_timeout_ms)
    // and fills outConsoleLine with "BTP/1 CONSOLE\r\n".
    bool pollTimeout(std::uint64_t nowMs, char outConsoleLine[kConsoleLineCapacity]) noexcept;

    // The USB transport disappeared (normally DTR was deasserted). Unlike a
    // protocol SESSION_CLOSE there is nobody left to receive a result or the
    // CONSOLE banner, so this only makes the state transition. The caller
    // performs the same queue/subscription cleanup as the normal close path.
    // Returns true exactly when an active/negotiating session was abandoned.
    bool onTransportLost() noexcept;

    EffectiveLimits effectiveLimits() const noexcept;
    std::uint32_t peerSourceId() const noexcept;
    std::uint32_t peerBootId() const noexcept;

    // Valid only right after onFrame() returned HelloAccepted, HelloRejected
    // or SessionClosed: the already-encoded reply frame btp::Node's session
    // responder built and "sent" through this object's NodeConfig::send()
    // override below, staged here instead of onto a real wire so the caller
    // can COBS-frame and queue/write it exactly where every other outgoing
    // frame already goes. Overwritten by the next onFrame() call.
    const std::uint8_t* pendingFrame(std::size_t* outSize) const noexcept;

private:
    // NodeConfig override: btp::Node hands the fully-encoded reply frame
    // here (never more than once per onFrame() call -- HELLO_RESULT/
    // SESSION_CLOSE_RESULT never fragment). Always "accepted": the real
    // transmission is the caller's job once onFrame() returns, same as it
    // always was for these two message types.
    bool send(const std::uint8_t* frame, std::size_t frame_size) override;

    void refreshLocalHello() noexcept;
    FrameOutcome classifyProtocolObject(const btp::Header& header) const noexcept;

    LocalLimits local_;
    btp::Hello localHello_;
    std::uint8_t localUuid_[16];
    std::uint32_t localConfigRevision_ = 0U;

    // btp::Node's underlying btp::Session self-expires a stale deadline
    // inside receive(); the old onFrame() left that flip to pollTimeout(),
    // which SerialMux::tick() relies on to run its session-ended cleanup.
    // Latch a TimedOut seen inside onFrame() and report it on the next
    // pollTimeout().
    bool timeoutLatched_ = false;

    // This topic does not reassemble serial fragments (see this file's own
    // header comment), so one slot is enough; kSlotBytes matches
    // LocalLimits::maxLogicalPayload's default and SerialMux's
    // kOutboundPayloadCap (static_assert'd against the latter in the .cpp).
    // btp::Node's constructor hands `cfg.transport` to btp::Receiver, which
    // copies it into ITS OWN member right then -- not a live reference read
    // again later (unlike identity, which Node::begin() re-reads from cfg_
    // every time). Base-before-member construction order means the
    // NodeConfig base above is already live by the time node_ (a later data
    // member) constructs, so this ordering-only member's constructor -- run
    // for its side effect, between the two -- is what gets kSerialTransport
    // in before node_ captures the wrong (default, zero) value permanently.
    struct TransportInit {
        explicit TransportInit(btp::NodeConfig& cfg) noexcept { cfg.transport = btp::kSerialTransport; }
    };
    TransportInit transportInit_;

    static constexpr std::size_t kSlotBytes = 2048U;
    btp::ReassemblySlot slots_[1];
    std::uint8_t slotStorage_[kSlotBytes];
    btp::ReassemblyStorage storage_[1] = {btp::ReassemblyStorage{slotStorage_, kSlotBytes}};
    std::uint8_t rxBuffer_[kSlotBytes];
    btp::Node node_;

    // send() staging area for pendingFrame() above. Sized generously over a
    // HELLO_RESULT/SESSION_CLOSE_RESULT's real worst case (well under 100
    // octets of payload plus the fixed v1 header/CRC) -- these never
    // fragment, so this never needs to be as large as kSlotBytes.
    static constexpr std::size_t kMaxSessionReplyFrameBytes = 256U;
    std::uint8_t pendingFrame_[kMaxSessionReplyFrameBytes];
    std::size_t pendingFrameSize_ = 0U;
};

} // namespace SerialSession
