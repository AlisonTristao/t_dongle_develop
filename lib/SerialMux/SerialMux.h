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
 * negotiating or protocolled (bally_protocol/docs/TRANSPORT_SERIAL.md
 * section 7). Wraps SerialSession (pure session/handshake logic) with the
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

// Topico 17: SerialMux decides *whether* an upstream (dongle -> robot)
// SUBSCRIBE/UNSUBSCRIBE is newly needed (SubscriptionRegistry's refcounting),
// but sending it is EspNowConfig's job (it owns EspNowManager/BtpTransport
// peer lookup) -- same callback-injection pattern as RunShellLineFn above,
// used for the same reason: SerialMux must not depend on EspNowConfig
// (would create the cycle CONTRIBUTING.md section 3 already warns about for
// ShellConfig). rateMillihz/leaseMs are the union across every remaining
// desktop subscriber of that topic (already computed by SubscriptionRegistry).
using RequestUpstreamSubscribeFn = void (*)(std::uint32_t sourceId, std::uint32_t topicId,
                                            std::uint32_t rateMillihz, std::uint32_t leaseMs);
using RequestUpstreamUnsubscribeFn = void (*)(std::uint32_t sourceId, std::uint32_t topicId,
                                              std::uint32_t upstreamSubscriptionId);

// selfUuid is a 16-byte, non-zero, session-stable identifier reported in
// HELLO_RESULT (BTP has no separate concept of "dongle UUID" yet outside
// this field -- topico 16 may formalize one). Caller derives it (e.g. from
// the MAC) and owns its storage only for the duration of this call.
// terminalPrompt is copied (not aliased) into the private BTP terminal
// ShellSerial's prompt text -- callers may pass a temporary's c_str() (e.g.
// ShellOutput::commandPrompt().c_str()); the pointer only needs to remain
// valid for the duration of this call.
// requestUpstreamSubscribe/requestUpstreamUnsubscribe may be nullptr in a
// context that never expects any topic to actually be reachable upstream
// (e.g. a future test harness); production callers (AppRuntime) always pass
// EspNowConfig::requestUpstreamSubscribe/requestUpstreamUnsubscribe.
void begin(Stream& io, RunShellLineFn runShellLine, const std::uint8_t selfUuid[16],
          const char* terminalPrompt, RequestUpstreamSubscribeFn requestUpstreamSubscribe = nullptr,
          RequestUpstreamUnsubscribeFn requestUpstreamUnsubscribe = nullptr) noexcept;

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

} // namespace SerialMux
