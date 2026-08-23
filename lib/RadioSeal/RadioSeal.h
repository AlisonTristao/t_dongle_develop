#pragma once

#include "DongleKeyStore.h"

#include <btp/aead.hpp>
#include <btp/codec.hpp>

#include <cstddef>
#include <cstdint>

/**
 * @brief The only place in this firmware that calls into btp::aead. Seals
 * and opens channel C traffic (dongle<->robot, key L -- bally_channels.h)
 * with AES-128-GCM, using the key DongleKeyStore holds in RAM.
 *
 * DELIBERATELY NOT INCLUDED BY BtpTransport.h/.cpp, and therefore never
 * compiled into anything env:native has to link. BTP/library.json's srcDir
 * pulls src/aead.cpp into every consumer unconditionally, and on a host with
 * neither mbedtls backend that translation unit is intentionally empty
 * (BTP/src/aead.cpp's own comment: "a stub returning an error would let a
 * build that believes it is encrypting ship without doing so" -- so it
 * fails at LINK time instead). This Windows dev host has no mbedtls at all
 * (`__has_include(<mbedtls/gcm.h>)` is false here), so if BtpTransport ever
 * called btp::aead_seal_aes_gcm directly, `platformio test -e native` would
 * fail to link every one of its five test binaries the moment any of them
 * touched BtpTransport::sendLogical/encodeSingleFrame -- which
 * test/test_protocol_router and test/test_serial_session already do.
 *
 * The fix is the same shape CONTRIBUTING.md section 3 already prescribes
 * for EspNowManager: BtpTransport takes a function-pointer callback
 * (BtpTransport::SealFn) instead of a direct dependency, so the actual
 * crypto call lives here, in a library only Arduino-only glue (EspNowConfig,
 * EspNowCommands, AppRuntime) ever includes -- files env:native cannot
 * compile at all (they include <Arduino.h>/FreeRTOS headers), so its build
 * graph never reaches this file, and never needs a real mbedtls to link.
 * On the real target (env:tdongle-s3) the Arduino ESP32 core ships mbedtls
 * 2.x/3.x with public <mbedtls/gcm.h>, so BTP's classic AEAD backend links
 * there without any extra configuration.
 */
namespace RadioSeal {

// BTP/docs/encryption.md section 2: the payload grows by exactly this many
// octets once sealed, regardless of cipher. Redeclared here (btp::aead
// itself has no public constant for it, only a comment) rather than
// hardcoded a second time at every call site.
constexpr std::size_t kTagSize = 16U;

/**
 * @brief Matches BtpTransport::SealFn exactly, so this can be passed as the
 * callback at every one of channel C's four origination points with no
 * wrapper needed. `context` is unused (the key lives in DongleKeyStore, a
 * process-wide holder like BtpTransport's own identity), kept only so the
 * signature matches SendFn's existing shape.
 *
 * Fails (false, nothing written to `out`) when DongleKeyStore::hasKeyL() is
 * false -- the fail-closed rule topico 30 asks for: no key, no frame, never
 * a silent cleartext fallback.
 */
bool seal(void* context, const btp::Header& header, std::uint16_t payloadSize,
         const std::uint8_t* plaintext, std::uint8_t* out) noexcept;

/**
 * @brief Opens one already-reassembled channel C message.
 *
 * `header` MUST be the canonical logical header ProtocolRouter hands back
 * on Outcome::Routed (FRAGMENTED cleared, fragment_index 0, fragment_count
 * 1 -- see ProtocolRouter.cpp/fragmentation.cpp, which restore exactly that
 * shape on completion and already guarantee it for an unfragmented frame
 * via decode()'s own validation). `ciphertextSize` is `routed.payloadSize`
 * unchanged (it already includes the trailing tag); `out` needs room for
 * `ciphertextSize - kTagSize` octets.
 *
 * Refuses (false) on any of: no key configured, the ENCRYPTED flag not
 * set, a cipher other than AES-128-GCM, or a tag that does not verify. The
 * caller MUST drop the message on false -- there is no fallback to reading
 * the still-sealed bytes as if they were plaintext. This is what closes the
 * hole topico 28 opened: a forged CONTROL/COMMAND frame simply does not
 * open, where before it was accepted by MAC alone.
 */
bool open(const btp::Header& header, std::uint16_t ciphertextSize,
         const std::uint8_t* ciphertextAndTag, std::uint8_t* outPlaintext) noexcept;

}  // namespace RadioSeal
