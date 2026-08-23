#include "DongleKeyStore.h"

#include <cstring>

#if defined(ARDUINO)
#include <Preferences.h>
#endif

namespace DongleKeyStore {
namespace {

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4), HMAC (RFC 2104) and PBKDF2 (RFC 8018), self-contained.
//
// Why not mbedtls, which the Arduino ESP32 core does ship: this library also
// has to build and be tested under env:native, on a host with no mbedtls at
// all (see RadioSeal.h for the reason it must stay that way -- this is not
// merely "no mbedtls installed", it is "must never require it"). bally_OS's
// lib/KeyStore hit the same wall from the other side (ESP-IDF 6.x's mbedtls
// 4.x moved the legacy digest API private) and vendored the same three
// primitives for the same reason; this is a second, independent
// implementation rather than a shared one because the two firmwares do not
// share a library boundary, but it is checked against the same kind of
// evidence: RFC 4231 test cases 1, 2 and 7 for HMAC, and a golden vector
// produced by scripts/provision_key.py for the full PBKDF2 chain
// (test/test_dongle_key_store).
//
// Only ever used on a handful-of-characters password, once per "hub
// -set_key_l" call -- 200000 PBKDF2 iterations at a few microseconds per
// SHA-256 compression is a sub-second operation, not a hot path.
// ---------------------------------------------------------------------------

constexpr std::size_t kShaDigestSize = 32U;
constexpr std::size_t kShaBlockSize = 64U;

constexpr std::uint32_t kShaRoundConstants[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
    0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
    0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
    0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
    0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
    0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
    0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
    0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
    0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
    0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
};

std::uint32_t rotate_right(std::uint32_t value, unsigned bits) noexcept {
    return (value >> bits) | (value << (32U - bits));
}

struct Sha256 {
    std::uint32_t state[8] = {0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U,
                              0xA54FF53AU, 0x510E527FU, 0x9B05688CU,
                              0x1F83D9ABU, 0x5BE0CD19U};
    std::uint8_t block[kShaBlockSize] = {};
    std::size_t pending = 0U;
    std::uint64_t total = 0U;  // message length in octets
};

void sha256_compress(Sha256& sha) noexcept {
    std::uint32_t w[64] = {};
    for (std::size_t i = 0U; i < 16U; ++i) {
        w[i] = (static_cast<std::uint32_t>(sha.block[i * 4U]) << 24U) |
               (static_cast<std::uint32_t>(sha.block[i * 4U + 1U]) << 16U) |
               (static_cast<std::uint32_t>(sha.block[i * 4U + 2U]) << 8U) |
               static_cast<std::uint32_t>(sha.block[i * 4U + 3U]);
    }
    for (std::size_t i = 16U; i < 64U; ++i) {
        const std::uint32_t s0 = rotate_right(w[i - 15U], 7U) ^
                                 rotate_right(w[i - 15U], 18U) ^
                                 (w[i - 15U] >> 3U);
        const std::uint32_t s1 = rotate_right(w[i - 2U], 17U) ^
                                 rotate_right(w[i - 2U], 19U) ^
                                 (w[i - 2U] >> 10U);
        w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }

    std::uint32_t a = sha.state[0], b = sha.state[1], c = sha.state[2];
    std::uint32_t d = sha.state[3], e = sha.state[4], f = sha.state[5];
    std::uint32_t g = sha.state[6], h = sha.state[7];

    for (std::size_t i = 0U; i < 64U; ++i) {
        const std::uint32_t sigma1 =
            rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t t1 =
            h + sigma1 + choose + kShaRoundConstants[i] + w[i];
        const std::uint32_t sigma0 =
            rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = sigma0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    sha.state[0] += a;
    sha.state[1] += b;
    sha.state[2] += c;
    sha.state[3] += d;
    sha.state[4] += e;
    sha.state[5] += f;
    sha.state[6] += g;
    sha.state[7] += h;
}

void sha256_update(Sha256& sha, const std::uint8_t* data, std::size_t size) noexcept {
    sha.total += size;
    for (std::size_t i = 0U; i < size; ++i) {
        sha.block[sha.pending++] = data[i];
        if (sha.pending == kShaBlockSize) {
            sha256_compress(sha);
            sha.pending = 0U;
        }
    }
}

void sha256_finish(Sha256& sha, std::uint8_t* digest) noexcept {
    const std::uint64_t bits = sha.total * 8U;

    const std::uint8_t padding = 0x80U;
    sha256_update(sha, &padding, 1U);
    const std::uint8_t zero = 0x00U;
    while (sha.pending != kShaBlockSize - 8U) sha256_update(sha, &zero, 1U);

    for (std::size_t i = 0U; i < 8U; ++i) {
        sha.block[kShaBlockSize - 8U + i] =
            static_cast<std::uint8_t>(bits >> (56U - (8U * i)));
    }
    sha256_compress(sha);

    for (std::size_t i = 0U; i < 8U; ++i) {
        digest[i * 4U] = static_cast<std::uint8_t>(sha.state[i] >> 24U);
        digest[i * 4U + 1U] = static_cast<std::uint8_t>(sha.state[i] >> 16U);
        digest[i * 4U + 2U] = static_cast<std::uint8_t>(sha.state[i] >> 8U);
        digest[i * 4U + 3U] = static_cast<std::uint8_t>(sha.state[i]);
    }
}

// HMAC-SHA256 of one contiguous message. Keys longer than the block size are
// hashed first per RFC 2104; the keys used here (a typed password, or a
// 16-octet derived key) are shorter than that in every real call, but the
// branch is kept correct rather than assumed away.
void hmac_sha256(const std::uint8_t* key, std::size_t key_size,
                 const std::uint8_t* message, std::size_t message_size,
                 std::uint8_t* digest) noexcept {
    std::uint8_t padded_key[kShaBlockSize] = {};
    if (key_size > kShaBlockSize) {
        Sha256 shrink;
        sha256_update(shrink, key, key_size);
        sha256_finish(shrink, padded_key);
    } else {
        std::memcpy(padded_key, key, key_size);
    }

    std::uint8_t pad[kShaBlockSize] = {};

    for (std::size_t i = 0U; i < kShaBlockSize; ++i) {
        pad[i] = static_cast<std::uint8_t>(padded_key[i] ^ 0x36U);
    }
    Sha256 inner;
    sha256_update(inner, pad, sizeof(pad));
    sha256_update(inner, message, message_size);
    std::uint8_t inner_digest[kShaDigestSize] = {};
    sha256_finish(inner, inner_digest);

    for (std::size_t i = 0U; i < kShaBlockSize; ++i) {
        pad[i] = static_cast<std::uint8_t>(padded_key[i] ^ 0x5CU);
    }
    Sha256 outer;
    sha256_update(outer, pad, sizeof(pad));
    sha256_update(outer, inner_digest, sizeof(inner_digest));
    sha256_finish(outer, digest);
}

// PBKDF2-HMAC-SHA256, RFC 8018 section 5.2. dklen is capped to one hash
// block's worth of output (32 octets, the "T_1 only" case) because
// kKeyLength is 16 -- the multi-block T_i concatenation RFC 8018 describes
// is real but genuinely unreachable from this file's only caller, and
// implementing it unexercised is worse than not implementing it.
void pbkdf2_hmac_sha256(const std::uint8_t* password, std::size_t password_size,
                        const std::uint8_t* salt, std::size_t salt_size,
                        std::uint32_t iterations, std::uint8_t* out_key,
                        std::size_t out_key_size) noexcept {
    // salt || INT(1), the block index big-endian per RFC 8018 -- this file
    // only ever derives block 1 (see the dklen note above).
    std::uint8_t block_input[64U + 4U] = {};
    std::memcpy(block_input, salt, salt_size);
    block_input[salt_size + 0U] = 0x00U;
    block_input[salt_size + 1U] = 0x00U;
    block_input[salt_size + 2U] = 0x00U;
    block_input[salt_size + 3U] = 0x01U;

    std::uint8_t u[kShaDigestSize] = {};
    hmac_sha256(password, password_size, block_input, salt_size + 4U, u);

    std::uint8_t accumulated[kShaDigestSize] = {};
    std::memcpy(accumulated, u, sizeof(u));

    for (std::uint32_t i = 1U; i < iterations; ++i) {
        std::uint8_t next[kShaDigestSize] = {};
        hmac_sha256(password, password_size, u, sizeof(u), next);
        std::memcpy(u, next, sizeof(u));
        for (std::size_t j = 0U; j < sizeof(accumulated); ++j) {
            accumulated[j] = static_cast<std::uint8_t>(accumulated[j] ^ u[j]);
        }
    }

    std::memcpy(out_key, accumulated, out_key_size);
}

// Overwrite through a volatile pointer so the compiler cannot drop the store
// as dead on a buffer that is about to go out of scope or be reused. Same
// pattern as bally_OS's KeyStore.cpp.
void wipe(std::uint8_t* data, std::size_t size) noexcept {
    volatile std::uint8_t* target = data;
    for (std::size_t i = 0U; i < size; ++i) target[i] = 0U;
}

std::uint8_t g_keyL[kKeyLength] = {};
bool g_hasKeyL = false;

}  // namespace

void deriveKeyL(const char* password, std::size_t passwordLength,
                std::uint8_t outKey[kKeyLength]) noexcept {
    pbkdf2_hmac_sha256(reinterpret_cast<const std::uint8_t*>(password), passwordLength,
                       reinterpret_cast<const std::uint8_t*>(kSalt), kSaltLength, kIterations,
                       outKey, kKeyLength);
}

void verifyTagL(const std::uint8_t key[kKeyLength], std::uint8_t outTag[kVerifyLength]) noexcept {
    std::uint8_t digest[kShaDigestSize] = {};
    hmac_sha256(key, kKeyLength, reinterpret_cast<const std::uint8_t*>(kVerifyLabelL),
               kVerifyLabelLLength, digest);
    std::memcpy(outTag, digest, kVerifyLength);
    wipe(digest, sizeof(digest));
}

void setKeyL(const std::uint8_t key[kKeyLength]) noexcept {
    std::memcpy(g_keyL, key, kKeyLength);
    g_hasKeyL = true;
}

bool hasKeyL() noexcept {
    return g_hasKeyL;
}

const std::uint8_t* keyL() noexcept {
    return g_hasKeyL ? g_keyL : nullptr;
}

void clearKeyL() noexcept {
    wipe(g_keyL, sizeof(g_keyL));
    g_hasKeyL = false;
}

void resetForTests() noexcept {
    clearKeyL();
}

#if defined(ARDUINO)

namespace {
constexpr const char* kNvsNamespace = "ballykey";
constexpr const char* kNvsKeyName = "key_l";
}  // namespace

bool saveToNvs() noexcept {
    if (!g_hasKeyL) {
        return false;
    }

    Preferences preferences;
    if (!preferences.begin(kNvsNamespace, false)) {
        return false;
    }
    const size_t written = preferences.putBytes(kNvsKeyName, g_keyL, kKeyLength);
    preferences.end();
    return written == kKeyLength;
}

bool loadFromNvs() noexcept {
    Preferences preferences;
    if (!preferences.begin(kNvsNamespace, true)) {
        return false;
    }

    const size_t storedSize = preferences.getBytesLength(kNvsKeyName);
    if (storedSize != kKeyLength) {
        preferences.end();
        return false;
    }

    std::uint8_t loaded[kKeyLength] = {};
    const size_t readSize = preferences.getBytes(kNvsKeyName, loaded, sizeof(loaded));
    preferences.end();
    if (readSize != kKeyLength) {
        wipe(loaded, sizeof(loaded));
        return false;
    }

    setKeyL(loaded);
    wipe(loaded, sizeof(loaded));
    return true;
}

#endif  // defined(ARDUINO)

}  // namespace DongleKeyStore
