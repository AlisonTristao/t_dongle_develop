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

// This dongle's own advertisement, fed to btp::Session so it can run
// btp::negotiate against the desktop's HELLO and stamp HELLO_RESULT's
// peer_uuid / config_revision. It is never transmitted -- the dongle is the
// responder and never sends a HELLO -- so the role is nominal; Gateway fits a
// device that bridges ESP-NOW <-> serial. version_count/versions must contain
// the one envelope version this firmware speaks so negotiation finds a match.
btp::Hello makeLocalHello(const LocalLimits& limits, const std::uint8_t uuid[16],
                          std::uint32_t configRevision) noexcept {
    btp::Hello hello{};
    hello.role = static_cast<std::uint8_t>(btp::Role::Gateway);
    hello.version_count = 1U;
    hello.versions[0] = 1U;
    hello.max_logical_payload = limits.maxLogicalPayload;
    hello.max_inflight_reassemblies = limits.maxInflightReassemblies;
    hello.max_subscriptions = limits.maxSubscriptions;
    hello.max_dedup_entries = limits.maxDedupEntries;
    hello.session_timeout_ms = limits.sessionTimeoutMs;
    std::memcpy(hello.peer_uuid, uuid, 16U);
    hello.config_revision = configRevision;
    return hello;
}

EffectiveLimits translateLimits(const btp::EffectiveLimits& in) noexcept {
    EffectiveLimits out{};
    out.maxLogicalPayload = in.max_logical_payload;
    out.maxInflightReassemblies = in.max_inflight_reassemblies;
    out.maxSubscriptions = in.max_subscriptions;
    out.maxDedupEntries = in.max_dedup_entries;
    out.sessionTimeoutMs = in.session_timeout_ms;
    return out;
}

const std::uint8_t kZeroUuid[16] = {0};

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
      localHello_(makeLocalHello(localLimits, kZeroUuid, 0U)),
      localUuid_{0},
      transportInit_(*this),
      node_(*this, slots_, storage_, 1U, btp::kNodeDefaultReassemblyTimeoutMs, rxBuffer_, kSlotBytes,
            /*seal_scratch=*/nullptr, 0U, /*open_buffer=*/nullptr, 0U, /*scratch_buffer=*/nullptr, 0U) {
    // Channel B (dongle<->desktop) is never encrypted, so has_seal()/
    // has_open() stay at NodeConfig's own false default; no override needed.
    // Copied in once; setLocalUuid()/setLocalConfigRevision() below push
    // further updates straight into the node's own btp::Session via
    // refreshLocalHello()'s set_local() call, exactly as this always worked.
    node_.enable_session(localHello_, kHelloDeadlineMs);
}

bool Session::send(const std::uint8_t* frame, std::size_t frame_size) {
    if (frame == nullptr || frame_size > sizeof(pendingFrame_)) {
        return false;
    }
    std::memcpy(pendingFrame_, frame, frame_size);
    pendingFrameSize_ = frame_size;
    return true;
}

const std::uint8_t* Session::pendingFrame(std::size_t* outSize) const noexcept {
    if (outSize != nullptr) {
        *outSize = pendingFrameSize_;
    }
    return pendingFrame_;
}

State Session::state() const noexcept {
    // A TimedOut latched inside onFrame() has not been reported to SerialMux
    // yet: keep looking Protocolled so tick()'s early "isConsole()" returns do
    // not skip the pollTimeout() that runs the session-ended cleanup.
    if (timeoutLatched_) {
        return State::Protocolled;
    }
    switch (node_.session()->state()) {
        case btp::SessionState::Idle: return State::Console;
        case btp::SessionState::AwaitingHello: return State::AwaitingHello;
        case btp::SessionState::Active: return State::Protocolled;
    }
    return State::Console;
}

void Session::refreshLocalHello() noexcept {
    localHello_ = makeLocalHello(local_, localUuid_, localConfigRevision_);
    node_.session()->set_local(localHello_);
}

void Session::setLocalUuid(const std::uint8_t uuid[16]) noexcept {
    if (uuid == nullptr) {
        return;
    }
    std::memcpy(localUuid_, uuid, 16U);
    refreshLocalHello();
}

void Session::setLocalConfigRevision(std::uint32_t configRevision) noexcept {
    localConfigRevision_ = configRevision;
    refreshLocalHello();
}

void Session::configureIdentity(std::uint32_t sourceId, std::uint32_t bootId) noexcept {
    this->source_id = sourceId;
    this->boot_id = bootId;
    (void)node_.begin(/*arm_and_announce=*/false);
}

void Session::beginNegotiation(std::uint64_t nowMs) noexcept {
    timeoutLatched_ = false;
    // arm_session() is a no-op unless Idle; the old beginNegotiation() moved
    // to AwaitingHello from any state, so drop a live session first.
    node_.session()->reset();
    node_.arm_session(nowMs);
}

Session::FrameOutcome Session::classifyProtocolObject(const btp::Header& header) const noexcept {
    if (header.type == btp::MessageType::Command &&
        header.object_id == BtpTransport::btp_command::kCommandRequestObjectId) {
        return FrameOutcome::CommandRequest;
    }
    if (header.type == btp::MessageType::Terminal && header.object_id == kTerminalInObjectId) {
        return FrameOutcome::TerminalIn;
    }
    if (header.type == btp::MessageType::Control && header.object_id == kManifestRequestObjectId) {
        return FrameOutcome::ManifestRequest;
    }
    if (header.type == btp::MessageType::Control && header.object_id == kSubscribeObjectId) {
        return FrameOutcome::SubscribeRequest;
    }
    if (header.type == btp::MessageType::Control && header.object_id == kUnsubscribeObjectId) {
        return FrameOutcome::UnsubscribeRequest;
    }
    // Reserved/unsupported object for this topic (a stray HELLO mid-session,
    // etc.): ignored, watchdog already renewed by btp::Node's session.
    return FrameOutcome::Ignored;
}

Session::FrameResult Session::onFrame(const btp::DecodedFrame& frame, std::uint64_t nowMs) noexcept {
    FrameResult result{};
    pendingFrameSize_ = 0U;  // send() (if btp::Node calls it below) sets this

    btp::ReceivedMessage msg{};
    const btp::NodeRx rx = node_.receive(frame, nowMs, &msg);

    switch (rx) {
        case btp::NodeRx::Pending:
        case btp::NodeRx::NoDatagram:
            break;  // FrameOutcome::Ignored: nothing decoded to route

        case btp::NodeRx::DroppedFrame:
            break;  // FrameOutcome::Ignored: CRC/decode/reassembly rejected it

        case btp::NodeRx::SessionHandled:
            switch (node_.session_event()) {
                case btp::SessionEvent::None:
                    break;  // FrameOutcome::Ignored

                case btp::SessionEvent::TimedOut:
                    // btp::Session self-expired a stale deadline on this frame;
                    // the old onFrame() never did, leaving the flip to
                    // pollTimeout(). Latch it, same as always.
                    timeoutLatched_ = true;
                    break;  // FrameOutcome::Ignored for now

                case btp::SessionEvent::HelloAccepted:
                    result.outcome = FrameOutcome::HelloAccepted;
                    break;

                case btp::SessionEvent::HelloRejected:
                    result.outcome = FrameOutcome::HelloRejected;
                    result.consoleTransition = true;
                    buildConsoleLine(result.consoleLine);
                    break;

                case btp::SessionEvent::SessionClosed:
                    result.outcome = FrameOutcome::SessionClosed;
                    result.consoleTransition = true;
                    buildConsoleLine(result.consoleLine);
                    break;

                case btp::SessionEvent::FrameAccepted:
                case btp::SessionEvent::Abandoned:
                    break;  // not produced by receive()'s session path
            }
            break;

        case btp::NodeRx::Complete:
            result.outcome = classifyProtocolObject(msg.header);
            break;

        default:
            // InitiatorHandled/SubscriptionServed/.../RequestServed: this
            // session never enables those Node features (see this file's own
            // header comment), so none of these should occur; treat as
            // Ignored defensively rather than misroute.
            break;
    }

    return result;
}

bool Session::pollTimeout(std::uint64_t nowMs, char outConsoleLine[kConsoleLineCapacity]) noexcept {
    if (timeoutLatched_) {
        timeoutLatched_ = false;
        buildConsoleLine(outConsoleLine);
        return true;
    }
    pendingFrameSize_ = 0U;
    if (node_.tick(nowMs) == btp::SessionEvent::TimedOut) {
        buildConsoleLine(outConsoleLine);
        return true;
    }
    return false;
}

bool Session::onTransportLost() noexcept {
    timeoutLatched_ = false;
    return node_.session()->reset().event == btp::SessionEvent::Abandoned;
}

EffectiveLimits Session::effectiveLimits() const noexcept {
    return translateLimits(node_.session()->effective_limits());
}

std::uint32_t Session::peerSourceId() const noexcept { return node_.session()->peer_source_id(); }

std::uint32_t Session::peerBootId() const noexcept { return node_.session()->peer_boot_id(); }

} // namespace SerialSession
