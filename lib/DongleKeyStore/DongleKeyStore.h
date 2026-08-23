#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief This dongle's channel C key (dongle<->robot, key L -- see
 * include/bally_channels.h), derived from an operator-typed password the
 * same way bally_OS's scripts/provision_key.py derives it -- byte for byte,
 * same salt, same iteration count, same output length -- so a password
 * typed here and the one typed into that script produce identical keys
 * (topico 29's acceptance criteria: "provisionador, dongle e TraceView, com
 * as mesmas senhas, produzem chaves identicas").
 *
 * This dongle NEVER derives key E (TraceView<->robot, channel B): it has no
 * function for it and never will, on purpose -- bally_channels.h's whole
 * point is "whoever holds the dongle administers the link but reads
 * nothing", and a KeyStore that could derive both keys would be one bug
 * away from breaking that.
 *
 * Pure C++, no Arduino/NVS/aead dependency: the derivation itself is what
 * env:native tests against a golden vector produced by
 * scripts/provision_key.py (test/test_dongle_key_store). Keeping this
 * library free of btp/aead.hpp is deliberate -- see RadioSeal.h for why a
 * call into btp::aead from anything env:native has to link would break that
 * whole test suite on a host with no mbedtls.
 */
namespace DongleKeyStore {

// Mirrors bally_OS/scripts/provision_key.py's derivation contract exactly.
// Changing any of these breaks compatibility with every already-typed
// password on every already-provisioned card -- see that script's own
// warning about this being the canonical definition of the contract.
constexpr std::size_t kKeyLength = 16U;  // AES-128-GCM, BTP CIPHER_ID 0
constexpr std::size_t kVerifyLength = 8U;
constexpr std::uint32_t kIterations = 200000U;
constexpr char kSalt[] = "bally-kdf-salt-1";  // 16 ASCII octets, no NUL fed into the KDF
constexpr std::size_t kSaltLength = 16U;
constexpr char kVerifyLabelL[] = "bally-canal-c";  // 13 ASCII octets, no NUL fed into the HMAC
constexpr std::size_t kVerifyLabelLLength = 13U;

/**
 * @brief PBKDF2-HMAC-SHA256(password, kSalt, kIterations, kKeyLength).
 *
 * Pure function -- no allocation, no global state touched -- which is the
 * whole reason env:native can check it against a golden vector without a
 * card, an NVS partition or a device.
 */
void deriveKeyL(const char* password, std::size_t passwordLength,
               std::uint8_t outKey[kKeyLength]) noexcept;

/**
 * @brief HMAC-SHA256(key, "bally-canal-c")[:8].
 *
 * Public by construction (a one-way function of the key), so it is safe to
 * print. This is what lets "hub -key_status" tell the operator which key it
 * derived without ever printing the key itself -- same reasoning as
 * KeyStore::verify_l() on the robot.
 */
void verifyTagL(const std::uint8_t key[kKeyLength], std::uint8_t outTag[kVerifyLength]) noexcept;

/**
 * @brief Loads a derived key into the in-RAM holder RadioSeal reads from.
 *
 * Overwrites whatever was held before; never logs or copies the key
 * anywhere else. Pass the output of deriveKeyL() (or of loadFromNvs()).
 */
void setKeyL(const std::uint8_t key[kKeyLength]) noexcept;

/** True once setKeyL() (directly, or indirectly via loadFromNvs()) has run
 * and clearKeyL() has not run since. */
bool hasKeyL() noexcept;

/**
 * @brief Pointer to the current key, kKeyLength octets, valid only while
 * hasKeyL() is true and no clearKeyL()/setKeyL() call has happened since.
 * Never print, log or copy what this points to outside RadioSeal.
 */
const std::uint8_t* keyL() noexcept;

/** Forgets the key, overwriting the RAM that held it. */
void clearKeyL() noexcept;

/** Test-only: identical to clearKeyL(), named for symmetry with the other
 * *_registry/*_publisher resetForTests() helpers so env:native cases can
 * isolate each other. Production code never calls it. */
void resetForTests() noexcept;

#if defined(ARDUINO)
/**
 * @brief Persists the current key to NVS (Preferences, namespace
 * "ballykey") so it survives a reboot without the operator retyping the
 * password every time (topico 29 passo 3: "grava a chave em NVS").
 *
 * False, and NVS left untouched, when hasKeyL() is false -- there is
 * nothing to persist.
 */
bool saveToNvs() noexcept;

/**
 * @brief Reads a previously saveToNvs()'d key back into the RAM holder.
 *
 * False (and hasKeyL() stays whatever it already was) when nothing was ever
 * saved, or the stored blob is not exactly kKeyLength octets. Call once
 * from AppRuntime::begin(), mirroring KeyStore::load_from_card on the robot
 * -- except this dongle never runs off a value it cannot itself have
 * derived moments earlier, so there is no magic/CRC framing to validate
 * here, only a length check.
 */
bool loadFromNvs() noexcept;
#endif

}  // namespace DongleKeyStore
