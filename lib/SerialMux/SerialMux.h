#pragma once

#include <Arduino.h>
#include <SerialSession.h>
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
 */
namespace SerialMux {

// One already-negotiated shell line's worth of execution, used for both the
// COMMAND_REQUEST and TERMINAL_IN inbound paths. Mirrors ShellConfig::runLine
// but as a plain function pointer (no captures needed) so this header never
// has to include ShellConfig.h.
using RunShellLineFn = void (*)(const char* commandLine, const char* source,
                                const char* userId, std::string* outFullText);

// selfUuid is a 16-byte, non-zero, session-stable identifier reported in
// HELLO_RESULT (BTP has no separate concept of "dongle UUID" yet outside
// this field -- topico 16 may formalize one). Caller derives it (e.g. from
// the MAC) and owns its storage only for the duration of this call.
void begin(Stream& io, RunShellLineFn runShellLine, const std::uint8_t selfUuid[16]) noexcept;

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
