#include <unity.h>

#include <DongleKeyStore.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

// Topico 29 passo 3 on the dongle side: this dongle's key L, derived from an
// operator-typed password, must be byte-for-byte the same key
// bally_OS/scripts/provision_key.py derives from the same password -- that
// script is the canonical definition of the salt and iteration count
// (KDF_SALT = "bally-kdf-salt-1", KDF_ITERATIONS = 200000), and this is
// PBKDF2-HMAC-SHA256/HMAC-SHA256 vendored independently (DongleKeyStore.cpp
// has no mbedtls dependency, same reason lib/KeyStore on the robot vendors
// its own SHA-256/HMAC).
//
// The golden vectors below were produced by running provision_key.py itself
// (`python provision_key.py --password-e <anything> --password-l <pw>
// out.key`, reading key_l/verify_l back out of the 96-octet file at offsets
// 48/72) -- this is exactly the acceptance criteria topico 29 states:
// "provisionador, dongle e TraceView, com as mesmas senhas, produzem chaves
// identicas -- comparadas por teste automatizado, nao a olho." A bug that
// makes this dongle QUASE derive the same key (one iteration off, a wrong
// salt octet, a truncated password) fails loudly here instead of showing up
// months later as "channel C just doesn't work".
//
// Pure C++, no Arduino/NVS/aead dependency (see DongleKeyStore.h for why),
// so this suite runs under env:native exactly like test_protocol_router.

namespace {

using std::size_t;
using std::uint8_t;

void hexDecode(const char* hex, uint8_t* out, size_t outSize) {
    for (size_t i = 0; i < outSize; ++i) {
        auto nibble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            return static_cast<uint8_t>(c - 'a' + 10);
        };
        out[i] = static_cast<uint8_t>((nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
    }
}

void assertDerivation(const char* password, const char* expectedKeyHex,
                      const char* expectedVerifyHex) {
    uint8_t expectedKey[DongleKeyStore::kKeyLength] = {0};
    hexDecode(expectedKeyHex, expectedKey, sizeof(expectedKey));
    uint8_t expectedVerify[DongleKeyStore::kVerifyLength] = {0};
    hexDecode(expectedVerifyHex, expectedVerify, sizeof(expectedVerify));

    uint8_t key[DongleKeyStore::kKeyLength] = {0};
    DongleKeyStore::deriveKeyL(password, std::strlen(password), key);
    TEST_ASSERT_EQUAL_MEMORY(expectedKey, key, sizeof(expectedKey));

    uint8_t verify[DongleKeyStore::kVerifyLength] = {0};
    DongleKeyStore::verifyTagL(key, verify);
    TEST_ASSERT_EQUAL_MEMORY(expectedVerify, verify, sizeof(expectedVerify));
}

// Golden vector: provision_key.py --password-e x --password-l
// "senha-l-de-teste", key_l/verify_l read back from the written file.
void test_matches_provision_key_py_golden_vector_one() {
    assertDerivation("senha-l-de-teste", "e28b8edfd919d39f5b573c92b77224d7", "2a5e4ddc866f8a08");
}

// A second password, so a bug that happens to be right for one specific
// input (e.g. an off-by-one that cancels out) does not slip through.
void test_matches_provision_key_py_golden_vector_two() {
    assertDerivation("outra-senha", "7d3da0d60a35f35fb48d1fc236c37fb5", "840072f1a7fcc95b");
}

// A one-character password: exercises the PBKDF2 salt||INT(1) input at its
// shortest realistic length and confirms nothing about the implementation
// silently assumes a minimum size.
void test_matches_provision_key_py_golden_vector_short_password() {
    assertDerivation("a", "20ff092e88cc894a1e307665556f5167", "fd1ecb08eca1802f");
}

// Two different passwords must not, even by accident, derive the same key
// -- the exact bug class topico 29's acceptance criteria calls out ("pontas
// que QUASE derivam a mesma chave" is the wrong direction; two distinct
// inputs producing the same key would be catastrophic in the other one).
void test_different_passwords_derive_different_keys() {
    uint8_t keyA[DongleKeyStore::kKeyLength] = {0};
    uint8_t keyB[DongleKeyStore::kKeyLength] = {0};
    DongleKeyStore::deriveKeyL("senha-l-de-teste", 16, keyA);
    DongleKeyStore::deriveKeyL("outra-senha", 11, keyB);
    TEST_ASSERT_TRUE(std::memcmp(keyA, keyB, sizeof(keyA)) != 0);
}

// The RAM holder RadioSeal reads from: unset until setKeyL(), forgotten by
// clearKeyL(), and never returns a stale pointer once cleared.
void test_ram_holder_lifecycle() {
    TEST_ASSERT_FALSE(DongleKeyStore::hasKeyL());
    TEST_ASSERT_NULL(DongleKeyStore::keyL());

    uint8_t key[DongleKeyStore::kKeyLength] = {0};
    DongleKeyStore::deriveKeyL("qualquer-senha", 14, key);
    DongleKeyStore::setKeyL(key);

    TEST_ASSERT_TRUE(DongleKeyStore::hasKeyL());
    TEST_ASSERT_NOT_NULL(DongleKeyStore::keyL());
    TEST_ASSERT_EQUAL_MEMORY(key, DongleKeyStore::keyL(), sizeof(key));

    DongleKeyStore::clearKeyL();
    TEST_ASSERT_FALSE(DongleKeyStore::hasKeyL());
    TEST_ASSERT_NULL(DongleKeyStore::keyL());
}

}  // namespace

void setUp() {
    DongleKeyStore::resetForTests();
}

void tearDown() {
    DongleKeyStore::resetForTests();
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_matches_provision_key_py_golden_vector_one);
    RUN_TEST(test_matches_provision_key_py_golden_vector_two);
    RUN_TEST(test_matches_provision_key_py_golden_vector_short_password);
    RUN_TEST(test_different_passwords_derive_different_keys);
    RUN_TEST(test_ram_holder_lifecycle);
    return UNITY_END();
}
