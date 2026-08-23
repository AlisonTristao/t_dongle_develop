#include "RadioSeal.h"

namespace RadioSeal {

bool seal(void* /*context*/, const btp::Header& header, std::uint16_t payloadSize,
         const std::uint8_t* plaintext, std::uint8_t* out) noexcept {
    if (!DongleKeyStore::hasKeyL()) {
        return false;
    }

    const btp::AeadKey key{DongleKeyStore::keyL(), DongleKeyStore::kKeyLength};
    return btp::aead_seal_aes_gcm(key, header, payloadSize, plaintext, out) == btp::AeadError::Ok;
}

bool open(const btp::Header& header, std::uint16_t ciphertextSize,
         const std::uint8_t* ciphertextAndTag, std::uint8_t* outPlaintext) noexcept {
    if (!DongleKeyStore::hasKeyL()) {
        return false;
    }

    // Never trust a "consumed" frame that arrived unsealed or under a
    // cipher this channel does not use: fail closed rather than fall back
    // to reading it as plaintext. Channel C is AES-128-GCM only (same
    // CIPHER_ID the bally.key file records for key L), so anything else --
    // including a well-formed ChaCha20-Poly1305 frame -- is refused here,
    // not silently accepted under the wrong assumption.
    if ((header.flags & btp::kFlagEncrypted) == 0U) {
        return false;
    }
    if (btp::cipher_id(header.flags) != btp::CipherId::AesGcm) {
        return false;
    }

    const btp::AeadKey key{DongleKeyStore::keyL(), DongleKeyStore::kKeyLength};
    return btp::aead_open_aes_gcm(key, header, ciphertextSize, ciphertextAndTag, outPlaintext) ==
           btp::AeadError::Ok;
}

}  // namespace RadioSeal
