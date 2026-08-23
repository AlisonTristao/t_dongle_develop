#pragma once

#include <Arduino.h>
#include <SerialSession.h>
#include <ShellSerial.h>
#include <btp/codec.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * @brief The single owner of USB-Serial writes while a BTP v1 session is
 * negotiating or protocolled (BTP/docs/session-and-terminal.md
 * sections 3-4). Wraps SerialSession (pure session/handshake logic) with the
 * Arduino/FreeRTOS-specific pieces: byte-by-byte COBS decode of the incoming
 * stream (btp::SerialDecoder), per-MessageType priority queues for the
 * outgoing stream, and the actual Serial.write()/Serial.read() calls.
 *
 * Deliberately a plain namespace over global state (like EspNowConfig/
 * BtpTransport), not a Context-injected object: it must be reachable both
 * from AppRuntime (owns the loop) and from command modules such as
 * DongleCommands ("dongle -btp_v1", PASSO 2) without creating a dependency
 * cycle through ShellConfig -- see CONTRIBUTING.md section 3. To keep that
 * true, SerialMux never includes ShellConfig.h/ShellCommandSupport.h itself;
 * it calls back into the shell through the RunShellLineFn injected at
 * begin(), exactly the same callback-injection pattern topico 12 used for
 * BtpTransport::SendFn vs. EspNowManager.
 *
 * Topico 19 adds a second, private ShellSerial instance here (the "BTP
 * terminal shell"), fed by TERMINAL_IN and drained into TERMINAL_OUT
 * (SerialMux.cpp's g_terminalShell/g_terminalPty) -- see PASSO 1/2 in this
 * file's RESULTADO in bally_protocol/topicos/19_terminal_protocolado.txt.
 * Including <ShellSerial.h> here does not reopen the cycle above: ShellSerial
 * is a leaf lib (Arduino.h + <functional> only, no ShellConfig/
 * ShellCommandSupport dependency of its own).
 */
namespace SerialMux {

// One already-negotiated shell line's worth of execution, used for both the
// COMMAND_REQUEST and TERMINAL_IN inbound paths. Mirrors ShellConfig::runLine
// but as a plain function pointer (no captures needed) so this header never
// has to include ShellConfig.h.
using RunShellLineFn = void (*)(const char* commandLine, const char* source,
                                const char* userId, std::string* outFullText);

// Topico 28: the downstream half of the relay. SerialMux resolves *which*
// robot a cable frame belongs to (HubRegistry + BtpTransport's peer table),
// but putting octets on the air is EspNowConfig's job (it owns
// EspNowManager) -- same callback-injection pattern as RunShellLineFn above,
// used for the same reason: SerialMux must not depend on EspNowConfig (would
// create the cycle CONTRIBUTING.md section 3 already warns about for
// ShellConfig). `frame` is a complete, already-encoded BTP frame and is
// written to the radio unchanged; this callback must never re-encode it.
//
// This replaces topico 17's RequestUpstreamSubscribe/Unsubscribe callbacks.
// Subscriptions moved to channel B in topico 28: TraceView subscribes at the
// robot itself and the robot arbitrates per session, so the dongle no longer
// originates a merged SUBSCRIBE of its own toward the radio. What stayed is
// the LOCAL relay gate (see forwardRelay/relayUp), which only ever reads
// object_id -- a field that is in the clear even in a sealed frame.
using RelayToRadioFn = bool (*)(const std::uint8_t mac[6], const std::uint8_t* frame,
                                std::size_t frameSize);

// selfUuid is a 16-byte, non-zero, session-stable identifier reported in
// HELLO_RESULT (BTP has no separate concept of "dongle UUID" yet outside
// this field -- topico 16 may formalize one). Caller derives it (e.g. from
// the MAC) and owns its storage only for the duration of this call.
// terminalPrompt is copied (not aliased) into the private BTP terminal
// ShellSerial's prompt text -- callers may pass a temporary's c_str() (e.g.
// ShellOutput::commandPrompt().c_str()); the pointer only needs to remain
// valid for the duration of this call.
// relayToRadio may be nullptr in a context with no radio at all (e.g. a
// future test harness), in which case every downstream relay is refused
// instead of being silently re-originated; production callers (AppRuntime)
// always pass EspNowConfig::sendRawToMac.
void begin(Stream& io, RunShellLineFn runShellLine, const std::uint8_t selfUuid[16],
          const char* terminalPrompt, RelayToRadioFn relayToRadio = nullptr) noexcept;

// Tab completion for the BTP terminal session (topico 19, PASSO 1/2): the
// caller (AppRuntime) passes the same provider it gives serialShell_
// (TinyShell::complete_line) so "dongle -bt<TAB>" behaves identically
// whether typed on the real console or through TraceView's terminal widget.
void setTerminalCompletionProvider(ShellSerial::CompletionProvider provider) noexcept;

// Seeds the BTP terminal shell's arrow-up/down history. AppRuntime calls
// this alongside serialShell_.addLog() when replaying persisted history
// (restoreShellHistoryFromDatabase) so both shells recall the same past
// commands -- the two ShellSerial instances otherwise keep independent
// history storage (see topico 19 RESULTADO).
void addTerminalHistory(const char* line) noexcept;

// True while a plain human console (ShellSerial) owns the port: PASSO 11
// callers use this to decide whether a direct Serial print is still allowed.
bool isConsoleOwned() noexcept;
bool isProtocolled() noexcept;

/**
 * @brief Cumulative counters for the dongle -> desktop hop, read without
 * clearing.
 *
 * These are the same counters the STATUS heartbeat already reports
 * (commands.md section 5), with one difference that is the whole
 * point of this accessor: STATUS collapses every drop into a single
 * `frames_dropped`, so it says *that* frames were lost but never *where*.
 * Telling "the telemetry queue overflowed" (the link is saturated) apart from
 * "the session queue overflowed" (the main loop stalled) needs the per-class
 * split, and those two diagnoses point at opposite fixes.
 *
 * Read for a rate by differencing two snapshots. A 64-bit read is not atomic
 * on this target, so a snapshot taken while a counter is being incremented can
 * tear; these are monotonic, so the effect is a transient glitch in one
 * sample, never a wrong total. The STATUS path already reads them the same way.
 */
struct TxCounters {
    std::uint64_t framesRx;
    std::uint64_t framesTx;
    std::uint64_t crcErrors;
    std::uint64_t decodeErrors;
    std::uint64_t reassemblyRejected;
    std::uint64_t telemetryDropped;
    std::uint64_t droppedByClass[static_cast<std::size_t>(SerialSession::PriorityClass::kCount)];
};

void peekTxCounters(TxCounters& out) noexcept;

// Recognizes "BTP/1 ENTER <16 hex>" on an already read+trimmed console line.
// On match, writes "BTP/1 READY ...\r\n" directly (still console-owned at
// that instant) and arms the HELLO deadline. Returns false, doing nothing,
// for any other line or when a session is already active.
bool tryEnterFromConsoleLine(const char* line, std::uint32_t nowMs) noexcept;

// Manual entry point for "dongle -btp_v1" (PASSO 2): synthesizes a nonce
// locally instead of requiring the raw wire line. Same READY/AwaitingHello
// transition as tryEnterFromConsoleLine. Returns false if a session is
// already negotiating/protocolled.
bool enterFromCommand(std::uint32_t nowMs) noexcept;

// Drives everything else: incremental RX decode + dispatch, watchdog
// timeout, periodic STATUS heartbeat and TX queue draining. No-op (and does
// not touch Serial) while console-owned -- safe to call unconditionally once
// per AppRuntime::tick().
void tick(std::uint32_t nowMs) noexcept;

// Relays an already-decoded, already-reassembled message (typically routed
// TELEMETRY/LOG coming from EspNowConfig) toward the desktop client, under
// its own original header (source_id/boot_id/sequence/timestamp_us are never
// rewritten -- PLANO_GERAL.txt decision 11). No-op (returns false) unless a
// session is Protocolled; that is the expected common case with no desktop
// attached, matching topico 12's existing "no consumer yet" drop semantics.
bool forwardRelay(const btp::Header& header, const std::uint8_t* payload, std::size_t payloadSize) noexcept;

/**
 * @brief Topico 28: relays one raw radio datagram toward the desktop client
 * without decoding, reassembling or re-encoding it -- the octets that reached
 * the antenna are the octets that reach the cable, only COBS-framed at drain
 * time (D5).
 *
 * `header` is the envelope EspNowConfig already decoded for the ingress
 * decision and is used only to pick the priority class and to apply the
 * telemetry subscription gate; `frame` is the datagram itself and is what
 * actually travels. Passing both is deliberate: it makes it impossible to
 * "helpfully" rebuild the frame from the header here, which would rewrite
 * source_id/boot_id/sequence -- the AEAD nonce (BTP/docs/encryption.md
 * section 4) -- of a message this dongle is only carrying.
 *
 * Same no-session semantics as forwardRelay: false (counted, not queued)
 * unless a session is Protocolled.
 */
bool relayUp(const btp::Header& header, const std::uint8_t* frame, std::size_t frameSize) noexcept;

} // namespace SerialMux
