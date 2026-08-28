#include "SerialMux.h"

#include "TerminalPtyStream.h"

#include <BtpTransport.h>
#include <DonglePublisher.h>
#include <HubRegistry.h>
#include <HubRelay.h>
#include <ManifestCache.h>
#include <ShellOutput.h>
#include <SubscriptionRegistry.h>
#include <btp/stream.hpp>

#include <esp_random.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstddef>
#include <cstring>

namespace SerialMux {
namespace {

// This dongle's own outbound payload ceiling for the serial link. Every
// frame this firmware originates (HELLO_RESULT, STATUS, COMMAND_RESULT,
// TERMINAL_OUT, MANIFEST_DATA, TELEMETRY) must fit under it; longer
// command/terminal output is truncated the same way EspNowConfig already
// truncates COMMAND_RESULT messages (see
// BtpTransport::btp_command::kMaxResultMessageSize). Still far below the
// wire's real ceiling (btp::kSerialMaxPayloadSize = 4056); topico 13 does not
// implement Serial-side logical fragmentation (see SerialSession.h) so this is
// also this session's hard per-message limit, not just a soft default. It must
// stay equal to SerialSession::LocalLimits::maxLogicalPayload, which is what
// HELLO_RESULT actually promises the desktop.
//
// Raised 700 -> 1600 by topico 27. The dongle's own MANIFEST_DATA is no longer
// an empty descriptor: DonglePublisher's three topic records total 1364
// octets, plus 58 for the fixed prefix and 18 for the source name = 1440. At
// 700 the response would have been silently truncated to whole records that
// fit (ManifestCache::appendRecordsTruncated), i.e. the client would simply
// never see hub.usb/hub.peers. There is no cheaper fix available here: a
// record's fixed cost is 38 octets (mostly the mandatory scale/offset
// float64 pair, commands.md section 3.3), so twelve individually named
// counters cannot be described in less.
//
// The cost is real and worth stating: QueuedFrame grows with this constant and
// there are 32 of them across the four queues, so the queues go from ~24 KB to
// ~52 KB of heap. That is in line with what this firmware already spends on
// the ESP-NOW RX/routed queues (~75 KB) and is the reason the two large
// per-call frame buffers below became file-scope statics instead of growing
// the main loop's stack by 3 KB. Raised again to 2048 by the hub.usb v2
// counters: the complete three-topic descriptor must fit in ONE descriptor;
// emitting hub.link/hub.usb while silently losing hub.peers leaves the UI
// unable to discover robots. This remains well below BTP serial's 4056-byte
// wire ceiling and costs about 14 KB more across the 32 queued frames.
constexpr std::size_t kOutboundPayloadCap = 2048U;
constexpr std::size_t kMaxFrameBytes = btp::kV1HeaderSize + btp::kV1CrcSize + kOutboundPayloadCap;
constexpr std::size_t kMaxCobsBytes = kMaxFrameBytes + kMaxFrameBytes / 254U + 4U;

constexpr std::size_t kPriorityClassCount = static_cast<std::size_t>(SerialSession::PriorityClass::kCount);

// hub.usb publishes one dropped counter per priority class as a fixed array,
// and DonglePublisher cannot include SerialSession.h without pointing a
// dependency edge back at this library (CONTRIBUTING.md section 3), so it
// hardcodes the width. This is the one place that sees both.
static_assert(DonglePublisher::kUsbDropClassCount == kPriorityClassCount,
              "hub.usb's dropped_by_class array must match PriorityClass::kCount");

constexpr UBaseType_t kQueueDepth[kPriorityClassCount] = {
    /*kSession=*/4U,
    /*kTerminal=*/3U,
    /*kLogStatus=*/6U,
    /*kTelemetry=*/16U,
};

constexpr std::uint32_t kStatusIntervalMs = 2000U;

struct QueuedFrame {
    std::size_t size;                     // first, so a short per-class queue
    std::uint8_t bytes[kMaxFrameBytes];   // item still copies the length field
};

// Per-class heap budget for the four TX queues. Only kTelemetry is
// bounded-small by construction: one ESP-NOW-profile sample (<=250 payload)
// or a reassembled robot sample (<= ProtocolRouter::kMaxPayloadSize, 616).
// The other three can each legitimately hold a kOutboundPayloadCap-sized
// frame -- a chunked COMMAND_RESULT, a chunked TERMINAL_OUT, a MANIFEST_DATA
// descriptor or a long LOG line -- so they keep the full slot.
//
// Sizing every queue to the shared worst case reserved ~67 KB of heap in
// begin(); the boot then panicked inside shell-module registration at ~14 KB
// free (topico 34 section 1B). Right-sizing kTelemetry gives ~22 KB back.
constexpr std::size_t kClassFrameCap[kPriorityClassCount] = {
    /*kSession=*/  kMaxFrameBytes,
    /*kTerminal=*/ kMaxFrameBytes,
    /*kLogStatus=*/kMaxFrameBytes,
    /*kTelemetry=*/768U,
};

constexpr std::size_t classQueueItemSize(std::size_t classIdx) noexcept {
    return offsetof(QueuedFrame, bytes) + kClassFrameCap[classIdx];
}

Stream* g_io = nullptr;
RunShellLineFn g_runShellLine = nullptr;
RelayToRadioFn g_relayToRadio = nullptr;

std::uint8_t g_encodedBuffer[btp::kSerialMaxCobsBlockSize];
std::uint8_t g_decodedBuffer[btp::kSerialMaxFrameSize];
btp::SerialDecoder g_decoder(g_encodedBuffer, sizeof(g_encodedBuffer), g_decodedBuffer, sizeof(g_decodedBuffer));

SerialSession::Session g_session(SerialSession::LocalLimits{});

// Topico 19: private ShellSerial instance for the BTP terminal channel,
// bound to a pty-style Stream fed by TERMINAL_IN and drained into
// TERMINAL_OUT (PASSO 1/2 -- line editing stays server-side, see this
// file's header comment and the topico's RESULTADO). Kept separate from
// AppRuntime's own serialShell_ (real console) rather than re-pointing one
// shared instance at different Streams: console and BTP-terminal modes are
// mutually exclusive but SerialMux must not reach back into AppRuntime to
// flip anything, matching the existing callback-injection boundary. The two
// instances keep independent input-line/cursor state; command history is
// synced explicitly (addTerminalHistory), Tab completion via
// setTerminalCompletionProvider.
TerminalPtyStream g_terminalPty;
ShellSerial g_terminalShell;

QueueHandle_t g_queues[kPriorityClassCount] = {nullptr, nullptr, nullptr, nullptr};

std::uint32_t g_lastStatusMs = 0U;

// STATUS counters (BTP/docs/commands.md section 5).
volatile std::uint64_t g_framesRx = 0U;
volatile std::uint64_t g_framesTx = 0U;
volatile std::uint64_t g_crcErrors = 0U;
volatile std::uint64_t g_decodeErrors = 0U;
volatile std::uint64_t g_reassemblyRejected = 0U; // fragmented frames received (unsupported, see SerialSession.h)
volatile std::uint64_t g_telemetryDropped = 0U;
// Frames whose bytes could not all be pushed to the port -- a USB-CDC that
// reports "not connected" (the host is not asserting DTR) makes every
// write() return 0, so this counts, from the dongle's own console, exactly
// the "cable is up but the desktop is deaf" failure. Diagnostic only, not
// part of the STATUS wire schema.
volatile std::uint64_t g_framesTxStalled = 0U;
volatile std::uint64_t g_droppedByClass[kPriorityClassCount] = {0U, 0U, 0U, 0U};
// Downstream relay outcomes by reason -- see SerialMux.h's TxCounters for why
// these are five counters and not one.
volatile std::uint64_t g_relayDownOk = 0U;
volatile std::uint64_t g_relayDownUnbound = 0U;
volatile std::uint64_t g_relayDownNoPeer = 0U;
volatile std::uint64_t g_relayDownOversized = 0U;
volatile std::uint64_t g_relayDownSendFailed = 0U;

std::size_t classIndex(SerialSession::PriorityClass cls) noexcept {
    return static_cast<std::size_t>(cls);
}

// Returns true only if every byte was accepted by the port. A USB-CDC with no
// host asserting DTR makes write() return 0 forever, so the caller needs to
// know the bytes went nowhere rather than assume a frame left the building.
bool writeAll(const std::uint8_t* data, std::size_t len) noexcept {
    if (g_io == nullptr) {
        return false;
    }
    std::size_t remaining = len;
    const std::uint8_t* cursor = data;
    std::uint32_t idleRetries = 0U;
    while (remaining > 0U) {
        const std::size_t sent = g_io->write(cursor, remaining);
        if (sent == 0U) {
            if (++idleRetries > 2000U) {
                return false; // port vanished / host not reading; give up rather than spin forever
            }
            continue;
        }
        cursor += sent;
        remaining -= sent;
        idleRetries = 0U;
    }
    return true;
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
    // Short-circuit on the first failed write: if the leading delimiter did
    // not go out the payload will not either (same dead port), and stopping
    // there avoids both the wasted 2000-spin retries and pushing a headless
    // partial frame onto the wire if the port flickers back mid-sequence.
    const std::uint8_t zero = 0U;
    if (writeAll(&zero, 1U) && writeAll(encoded, written) && writeAll(&zero, 1U)) {
        ++g_framesTx;
    } else {
        ++g_framesTxStalled;
    }
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
    const std::size_t index = classIndex(cls);
    if (frameSize > kClassFrameCap[index]) {
        return false;
    }

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

    // Backpressure per model.md section 6: telemetry/log are
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
    // static, not a local: kMaxFrameBytes is 1.6 KB since topico 27 and every
    // caller runs on the main loop task (see writeFrameCobs' comment -- one
    // writer, never a FreeRTOS task, never re-entrant), so this belongs in BSS
    // rather than on a stack shared with TinyShell and std::string.
    static std::uint8_t frameBytes[kMaxFrameBytes];
    std::size_t frameSize = 0U;
    if (!encodeOwnFrame(type, objectId, payload, payloadSize, frameBytes, sizeof(frameBytes), &frameSize)) {
        return false;
    }
    return enqueueFrameBytes(SerialSession::classify(type, objectId), frameBytes, frameSize);
}

// Forward declarations: defined below (topico 17), used by finalizeToConsole
// (PASSO 6) ahead of their own definitions further down this file.
void dispatchUpstreamAction(const SubscriptionRegistry::UpstreamAction& action) noexcept;
void dispatchUpstreamActions(const SubscriptionRegistry::UpstreamAction* actions, std::size_t count) noexcept;

// Topico 28, downstream half: one frame that arrived on the cable and is not
// addressed to this dongle goes back out on the radio, unchanged.
//
// THE DESTINATION CANNOT BE DERIVED FROM THE FRAME. A BTP header has no
// destination field, and TERMINAL_IN in particular has none in its payload
// either, so the child's own source_id is resolved through HubRegistry --
// the operator's `hub -bind` -- and only then through BtpTransport's peer
// table to a MAC. An unbound child is refused, never guessed at: guessing
// would send one robot's terminal keystrokes to another.
//
// WHAT MUST NOT HAPPEN HERE. Every other send path in this firmware
// originates through BtpTransport::sendLogical, which reserves a fresh
// sequence and stamps this dongle's own source_id/boot_id. Doing that to a
// frame passing through would rewrite `source_id || boot_id || sequence`,
// which IS the AEAD nonce (BTP/docs/encryption.md section 4), and the seal
// would fail to open two repositories away from the line that broke it.
// HubRelay::reencodeVerbatim copies the producer's header whole; the test
// suite pins that (test/test_hub_relay).
//
// Re-fragmenting, for the record, would NOT break the seal -- the AAD is the
// canonicalized logical header, so fragment_index/fragment_count/FRAGMENTED
// are outside the tag exactly so a gateway can re-cut a message it cannot
// read. The dongle still never re-fragments, and the reason is throughput
// and retransmission (D4/D5): a 4056-octet serial message re-cut to the
// radio's 210 would become 20 fragments with nothing behind them. The child
// encodes on the EspNow profile from the origin instead.
bool relayDown(const btp::DecodedFrame& decoded) noexcept {
    if (g_relayToRadio == nullptr) {
        ++g_relayDownSendFailed;
        return false;
    }

    std::uint32_t peerSourceId = 0U;
    if (!HubRegistry::lookup(decoded.header.source_id, &peerSourceId)) {
        ++g_relayDownUnbound;
        return false;
    }

    std::uint8_t mac[6] = {0};
    std::uint32_t peerBootId = 0U;
    if (!BtpTransport::lookupPeerMacBySourceId(peerSourceId, mac, &peerBootId)) {
        ++g_relayDownNoPeer;
        return false; // never heard a BTP frame from that robot; cannot address it
    }

    std::uint8_t frameBytes[btp::kEspNowMaxFrameSize];
    std::size_t frameSize = 0U;
    if (!HubRelay::reencodeVerbatim(decoded, btp::TransportProfile::EspNow, frameBytes,
                                    sizeof(frameBytes), &frameSize)) {
        ++g_relayDownOversized;
        return false; // oversized for the radio: the child encodes on the EspNow profile (D4)
    }

    if (!g_relayToRadio(mac, frameBytes, frameSize)) {
        ++g_relayDownSendFailed;
        return false;
    }
    ++g_relayDownOk;
    return true;
}

// Sends the session's last frame synchronously (bypassing the queues -- it
// must go out before anything else and nothing lower-priority should delay
// it), then discards every not-yet-sent queued item and hands the port back
// to the console. Matches session-and-terminal.md section 4 exactly: the
// port owner "stops accepting new work", "discards incomplete reassemblies",
// "and only then returns to the console".
void finalizeToConsole(std::uint16_t objectId, const std::uint8_t* payload, std::size_t payloadSize,
                       const char* consoleLine) noexcept {
    static std::uint8_t frameBytes[kMaxFrameBytes];  // main-loop only, see enqueueOwn
    std::size_t frameSize = 0U;
    if (encodeOwnFrame(btp::MessageType::Control, objectId, payload, payloadSize, frameBytes,
                       sizeof(frameBytes), &frameSize)) {
        writeFrameCobs(frameBytes, frameSize);
    }

    // PASSO 6 (topico 17): a session ending -- SESSION_CLOSE here, or a
    // rejected HELLO whose leftover peerSourceId() (see Session::
    // beginNegotiation's peerSourceId_=0 reset) still names the *previous*
    // protocolled client -- clears every subscription that client owned.
    // Harmless no-op when peerSourceId() is 0 (no protocolled session ever
    // reached SUBSCRIBE, e.g. straight to HelloRejected on a first attempt).
    {
        SubscriptionRegistry::UpstreamAction actions[SubscriptionRegistry::kMaxTopics];
        const std::size_t count = SubscriptionRegistry::onClientDisconnected(
            g_session.peerSourceId(), millis(), actions, SubscriptionRegistry::kMaxTopics);
        dispatchUpstreamActions(actions, count);
    }

    resetQueues();
    writeConsoleText(consoleLine);
    g_decoder.reset();
    // session-and-terminal.md section 4: discard partial/pending work before
    // handing the port back -- a half-typed BTP terminal line is exactly
    // that, same as a partial serial block or an incomplete reassembly.
    g_terminalPty.reset();
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
        // Topico 28: a COMMAND_REQUEST addressed to somebody else is not a
        // mistake any more, it is the whole point of the hub -- this is the
        // frame TraceView aimed at a robot. It used to be dropped here in
        // silence, which is why commanding a robot through the cable was
        // impossible before this topico.
        // Discarding the result is deliberate and is no longer the same as
        // ignoring the failure: relayDown() counts every refusal by reason
        // (TxCounters::relayDown*), which surfaces in hub.usb and in
        // "espnow -stats". There is nothing useful to answer here beyond
        // that -- a bound child's payload may be sealed with a key this
        // dongle does not hold, so it cannot build a protocol reply about
        // a message it cannot read.
        (void)relayDown(decoded);
        return;
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

// TERMINAL_IN carries opaque raw bytes -- typically one or a handful of
// keystrokes, occasionally a larger pasted/typed chunk -- straight from the
// desktop's terminal widget. PASSO 1/2 (topico 19 RESULTADO): line editing
// (echo, backspace, arrows, history, Tab, Ctrl+R) stays entirely
// server-side, so these bytes are fed byte-for-byte into g_terminalPty,
// exactly the role the real UART plays for ShellSerial in console mode.
// pumpTerminalShell() (tick()) is what actually reads/interprets them; this
// replaces topico 13's one-message-equals-one-shell-line MVP.
void handleTerminalIn(const btp::DecodedFrame& decoded) noexcept {
    if (decoded.payload.size == 0U) {
        return;
    }

    // Topico 28: TERMINAL carries no target field of any kind, so the binding
    // table is the only thing that can say whether these keystrokes are meant
    // for the robot or for the dongle's own shell. A bound child is talking
    // to its robot; an unbound one is talking to the hub, which is the
    // pre-topico behavior and stays the default.
    //
    // The binding decides on its own, before relayDown() gets a chance to
    // fail: a child bound to a robot the radio cannot currently reach must
    // lose its keystrokes, never have them typed into the dongle's shell.
    std::uint32_t boundPeerSourceId = 0U;
    if (HubRegistry::lookup(decoded.header.source_id, &boundPeerSourceId)) {
        // Discarding the result is deliberate and is no longer the same as
        // ignoring the failure: relayDown() counts every refusal by reason
        // (TxCounters::relayDown*), which surfaces in hub.usb and in
        // "espnow -stats". There is nothing useful to answer here beyond
        // that -- a bound child's payload may be sealed with a key this
        // dongle does not hold, so it cannot build a protocol reply about
        // a message it cannot read.
        (void)relayDown(decoded);
        return;
    }

    g_terminalPty.feedInput(decoded.payload.data, decoded.payload.size);
}

std::uint32_t readU32Le(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) | (static_cast<std::uint32_t>(data[3]) << 24U);
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

// Desktop -> dongle catalog discovery (topico 16 PASSO 6/7): answers from
// ManifestCache, which already holds this dongle's own self-description plus
// whatever robot manifests have been cached over ESP-NOW (EspNowConfig's
// primeManifestIfNeeded). Every response is a single, already-bounded
// MANIFEST_DATA payload (ManifestCache truncates to whole records that fit
// kOutboundPayloadCap, see this repo's topico 16 RESULTADO for why that
// stays within the existing single-frame outbound path rather than a new
// large-buffer/multi-fragment queue for a message this small in practice)
// and goes out through the normal kLogStatus-priority queue like STATUS/
// HELLO_RESULT -- no bypass, so it cannot jump ahead of already-queued
// session/terminal traffic.
void handleManifestRequest(const btp::DecodedFrame& decoded) noexcept {
    if (decoded.payload.size != 12U || decoded.payload.data == nullptr) {
        return;  // malformed MANIFEST_REQUEST; silently dropped like any other malformed CONTROL payload
    }

    const std::uint32_t targetSourceId = readU32Le(decoded.payload.data);
    const std::uint32_t targetBootId = readU32Le(decoded.payload.data + 4U);
    const std::uint32_t knownRevision = readU32Le(decoded.payload.data + 8U);

    static std::uint8_t payload[kOutboundPayloadCap];  // main-loop only, see enqueueOwn

    if (targetSourceId == 0U) {
        const std::size_t total = ManifestCache::enumerationCount();
        for (std::size_t index = 0U; index < total; ++index) {
            const std::size_t size =
                ManifestCache::buildEnumerationResponse(index, decoded.header, payload, sizeof(payload));
            if (size > 0U) {
                enqueueOwn(btp::MessageType::Control, ManifestCache::kManifestDataObjectId, payload, size);
            }
        }
        return;
    }

    const std::size_t size = ManifestCache::buildTargetedResponse(targetSourceId, targetBootId, knownRevision,
                                                                   decoded.header, payload, sizeof(payload));
    if (size > 0U) {
        enqueueOwn(btp::MessageType::Control, ManifestCache::kManifestDataObjectId, payload, size);
    }
}

// This session's own identity (from HELLO, see SerialSession::Session::
// peerSourceId()) doubles as SubscriptionRegistry's opaque clientId --
// unique per desktop process (topico 15's BtpHandshake generates a fresh
// random source_id per run) and already tracked, so no separate session
// counter is needed. Only meaningful while Protocolled; callers here are
// only reached in that state.
std::uint32_t currentClientId() noexcept { return g_session.peerSourceId(); }

// SubscriptionRegistry already decided *what* the producer of a topic must
// be told (topic gone entirely vs. still wanted but at a different/renewed
// rate). Since topico 28 there is exactly one producer left that this dongle
// can tell anything: itself.
void dispatchUpstreamAction(const SubscriptionRegistry::UpstreamAction& action) noexcept {
    // Topico 27: a hub.* topic has no radio hop -- the "upstream" producer of
    // hub.link is this very firmware. Routing the action to DonglePublisher
    // is what lets the dongle's own topics reuse the whole existing
    // subscription machinery (refcount across clients, union rate, lease
    // sweep, session-end cleanup) with no second code path.
    if (action.sourceId != 0U && action.sourceId == BtpTransport::sourceId()) {
        if (action.kind == SubscriptionRegistry::UpstreamKind::Subscribe) {
            DonglePublisher::onLocalSubscribe(static_cast<std::uint16_t>(action.topicId),
                                              action.rateMillihz);
        } else if (action.kind == SubscriptionRegistry::UpstreamKind::Unsubscribe) {
            DonglePublisher::onLocalUnsubscribe(static_cast<std::uint16_t>(action.topicId));
        }
        return;
    }

    // Topico 28: an action about a ROBOT's topic stops here. Subscriptions to
    // a robot moved to channel B -- TraceView subscribes at the robot itself
    // and the robot arbitrates per session -- so the dongle no longer merges
    // its clients into one SUBSCRIBE of its own toward the radio. The
    // registry's refcounting is still what it was; only its upstream half
    // lost a consumer. What did NOT move is the local relay gate
    // (forwardRelay/relayUp), which reads object_id -- in the clear even in a
    // sealed frame -- and is what still makes closing a chart reduce traffic
    // on the cable.
}

void dispatchUpstreamActions(const SubscriptionRegistry::UpstreamAction* actions, std::size_t count) noexcept {
    for (std::size_t i = 0U; i < count; ++i) {
        dispatchUpstreamAction(actions[i]);
    }
}

std::uint32_t readU16LeAsU32(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(readU32Le(data) & 0xFFFFU);
}

// Desktop -> dongle SUBSCRIBE (commands.md section 4, 20-byte
// payload). This dongle answers synchronously from its own local knowledge
// (ManifestCache's already-cached schema max_rate_millihz) rather than
// waiting on a round trip to the robot -- CRITERIO 2 ("pedido acima do
// maximo e limitado e informado ao cliente") is satisfied immediately this
// way, and PASSO 7 ("rate limiting sem bloquear tasks criticas") is
// structurally true because the serial reply never waits on ESP-NOW.
// SubscriptionRegistry::onDesktopSubscribe applies the same min/max clamp a
// second (redundant but cheap) time against the *union* of every desktop
// subscriber once the caller passes it the already-capped rate below, so a
// second, lower-rate subscriber can never raise what the first one capped.
void handleSubscribeRequest(const btp::DecodedFrame& decoded) noexcept {
    // Topico 28/31.2: a SUBSCRIBE from a bound hub child is for its robot,
    // not this dongle -- relay it verbatim, same as handleTerminalIn already
    // does, and BEFORE touching the payload below. Once the robot accepts
    // channel B for SUBSCRIBE (bally_OS topico 31.2), that payload is sealed
    // with a key this dongle never holds, so reading it as the plaintext
    // fields below would be reading ciphertext -- this check has to come
    // first, not just be "also correct" alongside the local-answer path.
    std::uint32_t boundPeerSourceId = 0U;
    if (HubRegistry::lookup(decoded.header.source_id, &boundPeerSourceId)) {
        // Discarding the result is deliberate and is no longer the same as
        // ignoring the failure: relayDown() counts every refusal by reason
        // (TxCounters::relayDown*), which surfaces in hub.usb and in
        // "espnow -stats". There is nothing useful to answer here beyond
        // that -- a bound child's payload may be sealed with a key this
        // dongle does not hold, so it cannot build a protocol reply about
        // a message it cannot read.
        (void)relayDown(decoded);
        return;
    }

    if (decoded.payload.size < 20U || decoded.payload.data == nullptr) {
        return;  // malformed; silently dropped like any other malformed CONTROL payload
    }

    const std::uint32_t targetSourceId = readU32Le(decoded.payload.data);
    const std::uint32_t targetBootId = readU32Le(decoded.payload.data + 4U);
    const std::uint32_t topicId = readU16LeAsU32(decoded.payload.data + 8U);
    const std::uint32_t requestedRateMillihz = readU32Le(decoded.payload.data + 12U);
    const std::uint32_t requestedLeaseMs = readU32Le(decoded.payload.data + 16U);

    constexpr std::uint8_t kStatusSuccess = 0x00U;
    constexpr std::uint8_t kStatusRejected = 0x01U;
    constexpr std::uint16_t kErrorNone = 0x0000U;
    constexpr std::uint16_t kErrorInvalidArgument = 0x0003U;
    constexpr std::uint16_t kErrorNotFound = 0x000BU;
    constexpr std::uint16_t kErrorCapacityExhausted = 0x0005U;

    std::uint8_t status = kStatusSuccess;
    std::uint16_t errorCode = kErrorNone;
    std::uint32_t subscriptionId = 0U;
    std::uint32_t effectiveRateMillihz = 0U;
    std::uint32_t grantedLeaseMs = 0U;

    if (targetSourceId == 0U || targetBootId == 0U || topicId == 0U || topicId > 0xFFFFU ||
        requestedRateMillihz == 0U || requestedLeaseMs == 0U) {
        status = kStatusRejected;
        errorCode = kErrorInvalidArgument;
    } else {
        std::uint32_t schemaMaxRateMillihz = 0U;
        if (!ManifestCache::lookupTopicMaxRateMillihz(targetSourceId, topicId, &schemaMaxRateMillihz)) {
            status = kStatusRejected;
            errorCode = kErrorNotFound;
        } else {
            // Same clamp rule the robot itself applies (commands.md
            // section 4): effective never exceeds requested nor the schema max.
            // A schema max of zero means "not periodic" (event-driven topic);
            // the request is still accepted, capped only by what was asked.
            std::uint32_t cappedRateMillihz = requestedRateMillihz;
            if (schemaMaxRateMillihz != 0U && cappedRateMillihz > schemaMaxRateMillihz) {
                cappedRateMillihz = schemaMaxRateMillihz;
            }

            const SubscriptionRegistry::DesktopSubscribeOutcome outcome = SubscriptionRegistry::onDesktopSubscribe(
                currentClientId(), targetSourceId, topicId, cappedRateMillihz, requestedLeaseMs, millis());
            if (!outcome.accepted) {
                status = kStatusRejected;
                errorCode = kErrorCapacityExhausted;
            } else {
                subscriptionId = outcome.subscriptionId;
                effectiveRateMillihz = cappedRateMillihz;
                grantedLeaseMs = (requestedLeaseMs < SubscriptionRegistry::kMinLeaseMs)
                    ? SubscriptionRegistry::kMinLeaseMs
                    : (requestedLeaseMs > SubscriptionRegistry::kMaxLeaseMs) ? SubscriptionRegistry::kMaxLeaseMs
                                                                              : requestedLeaseMs;
                dispatchUpstreamAction(outcome.upstream);
            }
        }
    }

    std::uint8_t responsePayload[28];
    writeU32(responsePayload, decoded.header.source_id);
    writeU32(responsePayload + 4U, decoded.header.boot_id);
    writeU32(responsePayload + 8U, decoded.header.sequence);
    responsePayload[12] = status;
    responsePayload[13] = 0U;  // reserved
    writeU16(responsePayload + 14U, errorCode);
    writeU32(responsePayload + 16U, subscriptionId);
    writeU32(responsePayload + 20U, effectiveRateMillihz);
    writeU32(responsePayload + 24U, grantedLeaseMs);
    enqueueOwn(btp::MessageType::Control, SerialSession::kSubscribeResultObjectId, responsePayload,
              sizeof(responsePayload));
}

// Desktop -> dongle UNSUBSCRIBE (commands.md section 4, 12-byte
// payload). Always answers SUCCESS/NONE once the envelope parses -- removing
// an already-absent subscription is defined as idempotent success, not an
// error (section 4: "makes retries idempotent").
void handleUnsubscribeRequest(const btp::DecodedFrame& decoded) noexcept {
    // Topico 28/31.2: same relay-first rule as handleSubscribeRequest above
    // -- a bound child's UNSUBSCRIBE is for its robot, and once the robot
    // accepts channel B for it the payload is sealed under a key this dongle
    // never holds.
    std::uint32_t boundPeerSourceId = 0U;
    if (HubRegistry::lookup(decoded.header.source_id, &boundPeerSourceId)) {
        // Discarding the result is deliberate and is no longer the same as
        // ignoring the failure: relayDown() counts every refusal by reason
        // (TxCounters::relayDown*), which surfaces in hub.usb and in
        // "espnow -stats". There is nothing useful to answer here beyond
        // that -- a bound child's payload may be sealed with a key this
        // dongle does not hold, so it cannot build a protocol reply about
        // a message it cannot read.
        (void)relayDown(decoded);
        return;
    }

    if (decoded.payload.size < 12U || decoded.payload.data == nullptr) {
        return;  // malformed; silently dropped like any other malformed CONTROL payload
    }

    const std::uint32_t subscriptionId = readU32Le(decoded.payload.data + 8U);
    const SubscriptionRegistry::DesktopUnsubscribeOutcome outcome =
        SubscriptionRegistry::onDesktopUnsubscribe(currentClientId(), subscriptionId, millis());
    dispatchUpstreamAction(outcome.upstream);

    std::uint8_t responsePayload[16];
    writeU32(responsePayload, decoded.header.source_id);
    writeU32(responsePayload + 4U, decoded.header.boot_id);
    writeU32(responsePayload + 8U, decoded.header.sequence);
    responsePayload[12] = 0x00U;  // status: SUCCESS
    responsePayload[13] = 0U;     // reserved
    writeU16(responsePayload + 14U, 0x0000U);  // error_code: NONE
    enqueueOwn(btp::MessageType::Control, SerialSession::kUnsubscribeResultObjectId, responsePayload,
              sizeof(responsePayload));
}

// Drains whatever g_terminalShell wrote back (echo/prompt/redraw) since the
// last call and chunks it into TERMINAL_OUT frame(s) of at most
// kOutboundPayloadCap bytes each -- unlike topico 13's MVP, a long command
// result is no longer truncated to a single frame's worth, just split
// across several kTerminal-priority frames.
void flushTerminalPtyOutput() noexcept {
    static std::uint8_t chunk[kOutboundPayloadCap];  // main-loop only, see enqueueOwn
    std::size_t chunkSize = 0U;
    while (g_terminalPty.hasOutput() &&
          (chunkSize = g_terminalPty.takeOutput(chunk, sizeof(chunk))) > 0U) {
        enqueueOwn(btp::MessageType::Terminal, SerialSession::kTerminalOutObjectId, chunk, chunkSize);
    }
}

// Drives g_terminalShell against g_terminalPty once per tick(), mirroring
// AppRuntime::handleShellInput()'s console pattern exactly (one
// readInputLine() call, response + refreshLine() only when a full command
// line came back) so the BTP terminal and the real console behave
// identically from a user's point of view (CRITERIO 3: no duplicate
// prompt/echo). readInputLine() itself drains every byte currently
// buffered in g_terminalPty, echoing as it goes, whether or not a full
// line completes -- that per-keystroke echo is what flushTerminalPtyOutput
// below picks up even when no command runs this tick.
void pumpTerminalShell() noexcept {
    String line;
    if (g_terminalShell.readInputLine(line)) {
        const std::string commandText(line.c_str());
        std::string fullOutput;
        if (g_runShellLine != nullptr) {
            g_runShellLine(commandText.c_str(), "serial", "serial", &fullOutput);
        }
        if (!fullOutput.empty()) {
            ShellOutput::printResponse(g_terminalPty, fullOutput);
        }
        g_terminalShell.refreshLine();
    }
    flushTerminalPtyOutput();
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
            // Fresh BTP terminal session (topico 19): drop any half-typed
            // line/pending bytes from a previous session and re-arm
            // ShellSerial's input/cursor/escape state. History and prompt
            // text survive (ShellSerial::begin() never touches either).
            g_terminalPty.reset();
            g_terminalShell.begin(g_terminalPty);
            g_terminalShell.refreshLine(); // queue the initial prompt now
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
        case SerialSession::Session::FrameOutcome::ManifestRequest:
            handleManifestRequest(decoded);
            break;
        case SerialSession::Session::FrameOutcome::SubscribeRequest:
            handleSubscribeRequest(decoded);
            break;
        case SerialSession::Session::FrameOutcome::UnsubscribeRequest:
            handleUnsubscribeRequest(decoded);
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

    // Topico 17 PASSO 9: status_version=2 with a per-(source,topic) record
    // whenever this dongle currently tracks at least one topic (subscribed
    // now or previously). Falls back to the plain v1 payload if the
    // snapshot is empty (nothing to add) or somehow does not fit --
    // commands.md section 5.1 makes topic_status_count=0 valid,
    // but there is no reason to spend the extra 2 bytes when v1 already says
    // everything there is to say.
    SubscriptionRegistry::TopicStatusEntry snapshotEntries[SubscriptionRegistry::kMaxTopics];
    const std::size_t topicCount =
        SubscriptionRegistry::topicStatusSnapshot(snapshotEntries, SubscriptionRegistry::kMaxTopics);

    std::uint8_t payload[SerialSession::kStatusPayloadSize +
                         2U + SubscriptionRegistry::kMaxTopics * SerialSession::kTopicStatusRecordSize];
    std::size_t size = 0U;
    if (topicCount > 0U) {
        SerialSession::TopicStatusRecord records[SubscriptionRegistry::kMaxTopics];
        for (std::size_t i = 0U; i < topicCount; ++i) {
            records[i].sourceId = snapshotEntries[i].sourceId;
            records[i].topicId = snapshotEntries[i].topicId;
            records[i].subscriberCount = snapshotEntries[i].subscriberCount;
            records[i].effectiveRateMillihz = snapshotEntries[i].effectiveRateMillihz;
            records[i].bytesTotal = snapshotEntries[i].bytesTotal;
            records[i].samplesDroppedTotal = snapshotEntries[i].samplesDroppedTotal;
        }
        size = SerialSession::buildStatusV2(counters, records, topicCount, payload, sizeof(payload));
    }
    if (size == 0U) {
        size = SerialSession::buildStatus(counters, payload, sizeof(payload));
    }
    if (size > 0U) {
        enqueueOwn(btp::MessageType::Control, SerialSession::kStatusObjectId, payload, size);
    }
}

// Topico 27: the wire end of DonglePublisher. A hub.* sample is an ordinary
// TELEMETRY frame originated by this dongle, so it goes through the same
// enqueueOwn()/classify() path as every other frame this firmware sends --
// landing in the kTelemetry queue, dropped first under backpressure, exactly
// like a relayed robot sample. Deliberately not a second TX path.
bool emitOwnTelemetry(std::uint16_t topicId, const std::uint8_t* payload, std::size_t size) noexcept {
    return enqueueOwn(btp::MessageType::Telemetry, topicId, payload, size);
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

// Strict priority order (model.md section 6), with a small
// per-class burst cap per tick() so telemetry still makes progress instead
// of starving outright under a steady stream of session/terminal traffic.
void drainTx() noexcept {
    drainQueueClass(SerialSession::PriorityClass::kSession, 8U);
    drainQueueClass(SerialSession::PriorityClass::kTerminal, 4U);
    drainQueueClass(SerialSession::PriorityClass::kLogStatus, 4U);
    drainQueueClass(SerialSession::PriorityClass::kTelemetry, 4U);
}

} // namespace

void begin(Stream& io, RunShellLineFn runShellLine, const std::uint8_t selfUuid[16],
          const char* terminalPrompt, RelayToRadioFn relayToRadio) noexcept {
    g_io = &io;
    g_runShellLine = runShellLine;
    g_relayToRadio = relayToRadio;
    g_session.setLocalUuid(selfUuid);

    for (std::size_t i = 0U; i < kPriorityClassCount; ++i) {
        if (g_queues[i] == nullptr) {
            g_queues[i] = xQueueCreate(kQueueDepth[i], classQueueItemSize(i));
        }
    }

    g_decoder.reset();
    g_lastStatusMs = millis();

    g_terminalPty.reset();
    g_terminalShell.begin(g_terminalPty);
    // ShellSerial::begin() never touches prompt_, so this only needs to run
    // once here, not on every session (see HelloAccepted in dispatchFrame).
    g_terminalShell.setPrompt(String(terminalPrompt != nullptr ? terminalPrompt : ""));
}

void setTerminalCompletionProvider(ShellSerial::CompletionProvider provider) noexcept {
    g_terminalShell.setCompletionProvider(provider);
}

void addTerminalHistory(const char* line) noexcept {
    if (line == nullptr) {
        return;
    }
    g_terminalShell.addLog(String(line));
}

bool isConsoleOwned() noexcept {
    return g_session.isConsole();
}

bool isProtocolled() noexcept {
    return g_session.isProtocolled();
}

void peekTxCounters(TxCounters& out) noexcept {
    out.framesRx = g_framesRx;
    out.framesTx = g_framesTx;
    out.framesTxStalled = g_framesTxStalled;
    out.crcErrors = g_crcErrors;
    out.decodeErrors = g_decodeErrors;
    out.reassemblyRejected = g_reassemblyRejected;
    out.telemetryDropped = g_telemetryDropped;
    for (std::size_t i = 0U; i < kPriorityClassCount; ++i) {
        out.droppedByClass[i] = g_droppedByClass[i];
    }
    out.relayDownOk = g_relayDownOk;
    out.relayDownUnbound = g_relayDownUnbound;
    out.relayDownNoPeer = g_relayDownNoPeer;
    out.relayDownOversized = g_relayDownOversized;
    out.relayDownSendFailed = g_relayDownSendFailed;
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

void onTransportLost(std::uint32_t nowMs) noexcept {
    if (!g_session.onTransportLost()) {
        return;
    }

    // No bytes can be returned here: CDC already reports that the host is no
    // longer listening. Preserve the semantic cleanup of SESSION_CLOSE so a
    // later TraceView session starts from a genuinely clean console state.
    SubscriptionRegistry::UpstreamAction actions[SubscriptionRegistry::kMaxTopics];
    const std::size_t count = SubscriptionRegistry::onClientDisconnected(
        g_session.peerSourceId(), nowMs, actions, SubscriptionRegistry::kMaxTopics);
    dispatchUpstreamActions(actions, count);
    resetQueues();
    g_decoder.reset();
    g_terminalPty.reset();
}

void tick(std::uint32_t nowMs) noexcept {
    if (g_session.isConsole()) {
        return; // ShellSerial owns the port; nothing for the mux to do.
    }

    // Refreshed every tick (cheap: a global read + one field write) so
    // whatever HELLO_RESULT this session answers next always reports this
    // dongle's current manifest-catalog revision (topico 16 PASSO 5).
    g_session.setLocalConfigRevision(ManifestCache::catalogRevision());

    pumpRx(nowMs);
    if (g_session.isConsole()) {
        return; // pumpRx() may have just closed the session (SESSION_CLOSE/HelloRejected).
    }

    // Captured before pollTimeout() flips the state: distinguishes an
    // established session lost to the 30s inactivity watchdog from a
    // negotiation that never got its HELLO within kHelloDeadlineMs.
    const bool wasProtocolled = g_session.isProtocolled();
    char consoleLine[SerialSession::kConsoleLineCapacity];
    if (g_session.pollTimeout(nowMs, consoleLine)) {
        // PASSO 6: same "session ended" cleanup as finalizeToConsole() above,
        // for the timeout path that does not go through it. peerSourceId()
        // still names the client that just timed out (pollTimeout only
        // flips state_, never touches identity).
        {
            SubscriptionRegistry::UpstreamAction actions[SubscriptionRegistry::kMaxTopics];
            const std::size_t count = SubscriptionRegistry::onClientDisconnected(
                g_session.peerSourceId(), nowMs, actions, SubscriptionRegistry::kMaxTopics);
            dispatchUpstreamActions(actions, count);
        }
        resetQueues();
        writeConsoleText(consoleLine);
        // B.4 (topico 35): bench visibility -- from the port alone a session
        // that fell to the watchdog is indistinguishable from a healthy
        // console. The "BTP/1 CONSOLE" line above is for the desktop; this
        // is for a human reading the monitor.
        if (g_io != nullptr) {
            ShellOutput::printTagged(*g_io, "btp",
                wasProtocolled ? "sessao expirou (watchdog de inatividade) -> console"
                               : "HELLO nao chegou a tempo -> console");
        }
        g_decoder.reset();
        g_terminalPty.reset(); // same discard-pending-work rule as finalizeToConsole()
        return;
    }

    if (g_session.isProtocolled()) {
        pumpTerminalShell();
        maybeSendStatusHeartbeat(nowMs);

        // Lease sweep (topico 17): a client that stopped renewing a
        // SUBSCRIBE (without ever sending UNSUBSCRIBE or disconnecting) has
        // its grant time out here, independent of the session's own
        // session_timeout_ms watchdog. The same sweep also re-sends the
        // upstream SUBSCRIBE of any topic whose robot-side lease is half
        // spent, so the robot never stops publishing a topic a desktop
        // client is still actively holding.
        {
            SubscriptionRegistry::UpstreamAction actions[SubscriptionRegistry::kMaxTopics];
            const std::size_t count =
                SubscriptionRegistry::sweep(nowMs, actions, SubscriptionRegistry::kMaxTopics);
            dispatchUpstreamActions(actions, count);
        }

        // Topico 27: this dongle's own hub.* topics. Placed after the sweep so
        // a lease that just expired stops publishing on the same tick, and
        // before drainTx() so a sample queued now leaves on this tick instead
        // of waiting for the next one. A fast no-op with no subscriber.
        DonglePublisher::tick(nowMs, &emitOwnTelemetry);

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

    // PASSO 3/5 (topico 17): only relay a TELEMETRY topic someone actually
    // subscribed to -- this is the local half of "fechar um grafico reduz
    // trafego"; the desktop-facing UNSUBSCRIBE already stopped the upstream
    // ask towards the robot (handleUnsubscribeRequest), this gate just makes
    // sure any sample still in flight from before that lands isn't relayed
    // either. LOG keeps flowing unconditionally: it is not part of the
    // subscribe/rate-control model (commands.md section 4 only
    // ever mentions telemetry topics), same as before this topico.
    if (header.type == btp::MessageType::Telemetry) {
        if (SubscriptionRegistry::isKnownUnwanted(header.source_id, header.object_id)) {
            ++g_telemetryDropped;
            SubscriptionRegistry::recordDropped(header.source_id, header.object_id);
            return false;
        }
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

    const bool queued = enqueueFrameBytes(SerialSession::classify(header.type, header.object_id), frameBytes, frameSize);
    if (header.type == btp::MessageType::Telemetry) {
        if (queued) {
            SubscriptionRegistry::recordForwarded(header.source_id, header.object_id, payloadSize);
        } else {
            SubscriptionRegistry::recordDropped(header.source_id, header.object_id);
        }
    }
    return queued;
}

bool relayUp(const btp::Header& header, const std::uint8_t* frame, std::size_t frameSize) noexcept {
    if (!g_session.isProtocolled()) {
        if (header.type == btp::MessageType::Telemetry) {
            ++g_telemetryDropped;
        }
        return false;
    }

    if (frame == nullptr || frameSize == 0U) {
        return false;
    }

    // The same telemetry gate forwardRelay applies, and it keeps working on a
    // frame this dongle cannot read: object_id sits at a fixed header offset
    // in the clear even when the payload is sealed (BTP/docs/encryption.md
    // section 5 -- the header is the AAD, authenticated but not encrypted).
    // The payload size fed to recordForwarded is the fragment's, not the
    // logical message's, because under D5 nothing here ever sees the whole
    // message; summed over the fragments it comes out to the same number.
    const std::size_t payloadSize = frameSize - btp::kV1MinimumFrameSize;
    if (header.type == btp::MessageType::Telemetry) {
        if (SubscriptionRegistry::isKnownUnwanted(header.source_id, header.object_id)) {
            ++g_telemetryDropped;
            SubscriptionRegistry::recordDropped(header.source_id, header.object_id);
            return false;
        }
    }

    // Verbatim: the datagram goes into the queue exactly as the radio
    // delivered it, and writeFrameCobs() only wraps it in COBS on the way
    // out. Nothing is decoded, reassembled or re-encoded on this path (D5),
    // so the producer's identity triple -- the AEAD nonce -- cannot be
    // touched here even by accident.
    const bool queued =
        enqueueFrameBytes(SerialSession::classify(header.type, header.object_id), frame, frameSize);
    if (header.type == btp::MessageType::Telemetry) {
        if (queued) {
            SubscriptionRegistry::recordForwarded(header.source_id, header.object_id, payloadSize);
        } else {
            SubscriptionRegistry::recordDropped(header.source_id, header.object_id);
        }
    }
    return queued;
}

} // namespace SerialMux
