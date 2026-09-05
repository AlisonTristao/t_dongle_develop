#include <unity.h>

#include <HubRegistry.h>
#include <HubRelay.h>
#include <bally_channels.h>
#include <btp/codec.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Topico 28 on the dongle side: traffic crosses the hub in both directions,
// verbatim. Everything asserted here is pure C++ (HubRelay + HubRegistry);
// the Arduino/FreeRTOS glue that calls it (EspNowConfig's radio ingress,
// SerialMux's relayUp/relayDown and its queues) is covered by
// pio run -e tdongle-s3 plus hardware verification, not here.
//
// The first test in this file is the one that had to exist before a single
// line of relay code did. See its own comment.

namespace {

using std::size_t;
using std::uint8_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;

std::vector<uint8_t> read_vector(const char* relative_path) {
    const std::string candidates[] = {
        std::string("../BTP/test-vectors/v2/") + relative_path,
        std::string("../../BTP/test-vectors/v2/") + relative_path,
    };
    for (const auto& path : candidates) {
        std::ifstream input(path, std::ios::binary);
        if (input) {
            return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        }
    }
    return {};
}

// aead_fragmented_gcm_0/_1: the two ESP-NOW fragments of ONE 220-octet
// TELEMETRY message sealed whole (AES-128-GCM), from BTP's own vector set.
// The identity triple below is the vector's, and it is the AEAD nonce
// (docs/encryption.md section 4: source_id || boot_id || sequence).
constexpr uint32_t kVectorSourceId = 0x0C0D0E0FU;
constexpr uint32_t kVectorBootId = 0x10203040U;
constexpr uint32_t kVectorSequence = 0x00000008U;
const uint8_t kVectorNonce[12] = {0x0F, 0x0E, 0x0D, 0x0C, 0x40, 0x30,
                                  0x20, 0x10, 0x08, 0x00, 0x00, 0x00};

// This dongle's identity in these tests. Deliberately different from the
// vector's, so a relay that stamped its own identity would be caught.
constexpr uint32_t kSelfSourceId = 0x11223344U;
constexpr uint32_t kSelfBootId = 0x55667788U;

// Object ids, from the repo headers they live in (redeclared so this suite
// does not pull ManifestCache/SubscriptionRegistry in just for a number).
constexpr uint16_t kCommandRequestObjectId = 0x0001U;
constexpr uint16_t kCommandResultObjectId = 0x0002U;
constexpr uint16_t kManifestDataObjectId = 0x0004U;
constexpr uint16_t kSubscribeResultObjectId = 0x0006U;
constexpr uint16_t kStatusObjectId = 0x0009U;
constexpr uint16_t kTerminalOutObjectId = 0x0002U;

void write_u32(uint8_t* output, uint32_t value) {
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> 8U);
    output[2] = static_cast<uint8_t>(value >> 16U);
    output[3] = static_cast<uint8_t>(value >> 24U);
}

btp::Header make_header(btp::MessageType type, uint16_t objectId) {
    btp::Header header{};
    header.type = type;
    header.flags = 0U;
    header.source_id = kVectorSourceId;
    header.boot_id = kVectorBootId;
    header.sequence = kVectorSequence;
    header.timestamp_us = 0x2710U;
    header.object_id = objectId;
    header.fragment_index = 0U;
    header.fragment_count = 1U;
    return header;
}

// Encodes one ESP-NOW datagram, the way a robot would put it on the air.
std::vector<uint8_t> encode_espnow(const btp::Header& header, const uint8_t* payload, size_t payloadSize) {
    std::vector<uint8_t> out(btp::kEspNowMaxFrameSize);
    size_t written = 0U;
    const btp::Frame frame{header, {payload, payloadSize}};
    const btp::Error error = btp::encode(frame, btp::kEspNowTransport, out.data(), out.size(), &written);
    if (error != btp::Error::Ok) {
        return {};
    }
    out.resize(written);
    return out;
}

// One COMMAND_RESULT reference prefix (BTP/docs/commands.md section 1): the
// 26-octet prefix build_result() writes, whose first field is the
// request_source_id this dongle's ingress rule keys on.
std::vector<uint8_t> make_command_result_payload(uint32_t requestSourceId) {
    std::vector<uint8_t> payload(26U, 0U);
    write_u32(payload.data(), requestSourceId);   // request_source_id
    write_u32(payload.data() + 4U, kSelfBootId);  // request_boot_id
    write_u32(payload.data() + 8U, 7U);           // reply_to_sequence
    // action_id/action_version/status/reserved/error_code and the two length
    // fields all stay zero: nothing below reads them.
    return payload;
}

// One MANIFEST_DATA reference prefix (BTP/docs/commands.md section 3.2): the
// 12-octet prefix ManifestResponder::build_manifest_data / ManifestCache::
// writeManifestData both write first, whose first field is the original
// request's own source_id -- this dongle's own identity when it is the one
// answered (primeManifestIfNeeded), same shape as COMMAND_RESULT above.
std::vector<uint8_t> make_manifest_data_payload(uint32_t requestSourceId) {
    std::vector<uint8_t> payload(12U, 0U);
    write_u32(payload.data(), requestSourceId);  // request reference: source_id
    write_u32(payload.data() + 4U, kSelfBootId);  // request reference: boot_id
    write_u32(payload.data() + 8U, 3U);           // request reference: sequence
    return payload;
}

}  // namespace

void setUp() {}
void tearDown() { HubRegistry::clear(); }

// ---------------------------------------------------------------------------
// The test that came before any relay code
// ---------------------------------------------------------------------------
// It fails if the downstream relay rewrites source_id, boot_id or sequence of
// a frame merely passing through. Those three fields ARE the AEAD nonce
// (docs/encryption.md section 4), and every other send path in this firmware
// re-originates through BtpTransport::sendLogical -- which reserves a fresh
// sequence and stamps this dongle's own source_id/boot_id. Following "the
// house style" in the relay therefore breaks the seal, in a place where the
// symptom (a tag that will not verify) surfaces two repositories away from
// the cause.
//
// Note what this test deliberately does NOT forbid: re-fragmenting. The AAD
// is the canonicalized logical header (FRAGMENTED cleared, fragment_index 0,
// fragment_count 1, payload_size of the whole message), so the fields
// fragmentation touches are outside the tag on purpose -- BTP's own vector
// says so in its `description`. The dongle does not re-fragment for
// throughput and retransmission reasons (D4/D5), not for the seal's sake.
void test_relay_down_never_rewrites_the_nonce_triple() {
    const std::vector<uint8_t> onCable = read_vector("valid/aead_fragmented_gcm_0.bin");
    TEST_ASSERT_TRUE_MESSAGE(!onCable.empty(), "aead_fragmented_gcm_0.bin not found");

    // Arrival over the cable: same frame octets, Serial profile ceilings.
    btp::DecodedFrame decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(btp::Error::Ok),
                      static_cast<int>(btp::decode(onCable.data(), onCable.size(),
                                                   btp::kSerialTransport, &decoded)));

    uint8_t onAir[btp::kEspNowMaxFrameSize];
    size_t onAirSize = 0U;
    TEST_ASSERT_TRUE(HubRelay::reencodeVerbatim(decoded, btp::kEspNowTransport, onAir,
                                                sizeof(onAir), &onAirSize));

    // The producer's identity triple survived the crossing.
    btp::DecodedFrame relayed{};
    TEST_ASSERT_EQUAL(static_cast<int>(btp::Error::Ok),
                      static_cast<int>(btp::decode(onAir, onAirSize, btp::kEspNowTransport, &relayed)));
    TEST_ASSERT_EQUAL_HEX32(kVectorSourceId, relayed.header.source_id);
    TEST_ASSERT_EQUAL_HEX32(kVectorBootId, relayed.header.boot_id);
    TEST_ASSERT_EQUAL_HEX32(kVectorSequence, relayed.header.sequence);

    // Stated as the nonce itself, which is what actually breaks: the 12
    // octets BTP derives from those three fields must still be the vector's.
    uint8_t nonce[12] = {0};
    btp::aead_nonce(relayed.header, nonce);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kVectorNonce, nonce, 12);

    // And, stronger: the whole frame is octet-for-octet what arrived.
    TEST_ASSERT_EQUAL_size_t(onCable.size(), onAirSize);
    TEST_ASSERT_EQUAL_UINT8(0, std::memcmp(onCable.data(), onAir, onAirSize));
}

// The negative control for the test above: what re-originating the same
// payload through this dongle's own identity would have produced. If this
// ever stops differing, the test above has stopped proving anything.
void test_reoriginating_the_same_payload_would_change_the_nonce() {
    const std::vector<uint8_t> onCable = read_vector("valid/aead_fragmented_gcm_0.bin");
    TEST_ASSERT_TRUE(!onCable.empty());

    btp::DecodedFrame decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(btp::Error::Ok),
                      static_cast<int>(btp::decode(onCable.data(), onCable.size(),
                                                   btp::kSerialTransport, &decoded)));

    btp::Header reoriginated = decoded.header;
    reoriginated.source_id = kSelfSourceId;  // what sendLogical() stamps
    reoriginated.boot_id = kSelfBootId;
    reoriginated.sequence = 1U;              // what reserveSequence() hands out

    uint8_t original[12] = {0};
    uint8_t rewritten[12] = {0};
    btp::aead_nonce(decoded.header, original);
    btp::aead_nonce(reoriginated, rewritten);
    TEST_ASSERT_NOT_EQUAL(0, std::memcmp(original, rewritten, 12));
}

// ---------------------------------------------------------------------------
// Blind relay, both directions, on a sealed message the dongle cannot read
// ---------------------------------------------------------------------------
void test_relay_up_passes_each_fragment_verbatim_and_reassembles_nothing() {
    const char* names[] = {"valid/aead_fragmented_gcm_0.bin", "valid/aead_fragmented_gcm_1.bin"};
    const size_t expectedPayloadSize[] = {210U, 26U};

    for (size_t i = 0U; i < 2U; ++i) {
        const std::vector<uint8_t> datagram = read_vector(names[i]);
        TEST_ASSERT_TRUE_MESSAGE(!datagram.empty(), names[i]);

        const HubRelay::RadioIngress ingress = HubRelay::classifyRadio(datagram.data(), datagram.size());
        TEST_ASSERT_EQUAL(static_cast<int>(btp::Error::Ok), static_cast<int>(ingress.error));

        // A sealed TELEMETRY fragment is not a C-link candidate, so it goes
        // up. The dongle holds no key for it and never needed one.
        TEST_ASSERT_FALSE(ingress.mayConsume);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(btp::MessageType::Telemetry),
                                static_cast<uint8_t>(ingress.header.type));
        TEST_ASSERT_TRUE((ingress.header.flags & btp::kFlagEncrypted) != 0U);

        // Nothing was reassembled: the fragment still says it is fragment i
        // of 2 and still carries only its own slice, never the 236-octet
        // whole. Reassembly belongs to the ends (D5).
        TEST_ASSERT_TRUE((ingress.header.flags & btp::kFlagFragmented) != 0U);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(i), ingress.header.fragment_index);
        TEST_ASSERT_EQUAL_UINT8(2U, ingress.header.fragment_count);
        TEST_ASSERT_EQUAL_size_t(expectedPayloadSize[i] + btp::kV1MinimumFrameSize, datagram.size());

        // Both fragments carry the producer's identity, unchanged and equal.
        TEST_ASSERT_EQUAL_HEX32(kVectorSourceId, ingress.header.source_id);
        TEST_ASSERT_EQUAL_HEX32(kVectorBootId, ingress.header.boot_id);
        TEST_ASSERT_EQUAL_HEX32(kVectorSequence, ingress.header.sequence);

        // What SerialMux::relayUp enqueues is the datagram itself -- the
        // frame is not re-encoded on the way up at all, only COBS-framed at
        // drain time -- so the same octets must still decode under the cable
        // profile, still carrying one fragment's worth of payload.
        btp::DecodedFrame onCable{};
        TEST_ASSERT_EQUAL(static_cast<int>(btp::Error::Ok),
                          static_cast<int>(btp::decode(datagram.data(), datagram.size(),
                                                       btp::kSerialTransport, &onCable)));
        TEST_ASSERT_EQUAL_size_t(expectedPayloadSize[i], onCable.payload.size);
    }
}

void test_relay_down_passes_each_fragment_verbatim() {
    const char* names[] = {"valid/aead_fragmented_gcm_0.bin", "valid/aead_fragmented_gcm_1.bin"};

    for (size_t i = 0U; i < 2U; ++i) {
        const std::vector<uint8_t> onCable = read_vector(names[i]);
        TEST_ASSERT_TRUE_MESSAGE(!onCable.empty(), names[i]);

        btp::DecodedFrame decoded{};
        TEST_ASSERT_EQUAL(static_cast<int>(btp::Error::Ok),
                          static_cast<int>(btp::decode(onCable.data(), onCable.size(),
                                                       btp::kSerialTransport, &decoded)));

        uint8_t onAir[btp::kEspNowMaxFrameSize];
        size_t onAirSize = 0U;
        TEST_ASSERT_TRUE(HubRelay::reencodeVerbatim(decoded, btp::kEspNowTransport, onAir,
                                                    sizeof(onAir), &onAirSize));
        TEST_ASSERT_EQUAL_size_t(onCable.size(), onAirSize);
        TEST_ASSERT_EQUAL_UINT8(0, std::memcmp(onCable.data(), onAir, onAirSize));
    }
}

// ---------------------------------------------------------------------------
// The ingress rule
// ---------------------------------------------------------------------------
void test_heartbeat_status_is_a_link_candidate() {
    const btp::Header header = make_header(btp::MessageType::Control, kStatusObjectId);
    const std::vector<uint8_t> datagram = encode_espnow(header, nullptr, 0U);
    TEST_ASSERT_TRUE(!datagram.empty());

    const HubRelay::RadioIngress ingress = HubRelay::classifyRadio(datagram.data(), datagram.size());
    TEST_ASSERT_EQUAL(static_cast<int>(btp::Error::Ok), static_cast<int>(ingress.error));
    TEST_ASSERT_TRUE(ingress.mayConsume);
}

// A command/manifest reference is payload data and therefore may be E-key
// ciphertext. The envelope-only classifier must produce the same result for
// both byte patterns; final ownership is decided after reassembly and L-key
// authentication in EspNowConfig.
void test_command_result_is_candidate_regardless_of_prefix_bytes() {
    const std::vector<uint8_t> own = make_command_result_payload(kSelfSourceId);
    const std::vector<uint8_t> other = make_command_result_payload(0x99887766U);
    const btp::Header header = make_header(btp::MessageType::Command, kCommandResultObjectId);

    for (const std::vector<uint8_t>* payload : {&own, &other}) {
        const std::vector<uint8_t> datagram = encode_espnow(header, payload->data(), payload->size());
        TEST_ASSERT_TRUE(!datagram.empty());
        const HubRelay::RadioIngress ingress = HubRelay::classifyRadio(datagram.data(), datagram.size());
        TEST_ASSERT_TRUE(ingress.mayConsume);
    }
}

void test_manifest_data_is_candidate_regardless_of_prefix_bytes() {
    const std::vector<uint8_t> own = make_manifest_data_payload(kSelfSourceId);
    const std::vector<uint8_t> other = make_manifest_data_payload(0x99887766U);
    const btp::Header header = make_header(btp::MessageType::Control, kManifestDataObjectId);

    for (const std::vector<uint8_t>* payload : {&own, &other}) {
        const std::vector<uint8_t> datagram = encode_espnow(header, payload->data(), payload->size());
        TEST_ASSERT_TRUE(!datagram.empty());
        const HubRelay::RadioIngress ingress = HubRelay::classifyRadio(datagram.data(), datagram.size());
        TEST_ASSERT_TRUE(ingress.mayConsume);
    }
}

void test_everything_else_goes_up() {
    struct Case {
        btp::MessageType type;
        uint16_t objectId;
        const char* label;
    };
    const Case cases[] = {
        {btp::MessageType::Telemetry, 0x0001U, "TELEMETRY"},
        {btp::MessageType::Log, 0x0000U, "LOG"},
        {btp::MessageType::Terminal, kTerminalOutObjectId, "TERMINAL_OUT"},
        {btp::MessageType::Control, kSubscribeResultObjectId, "SUBSCRIBE_RESULT"},
    };

    const uint8_t payload[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    for (const Case& testCase : cases) {
        const btp::Header header = make_header(testCase.type, testCase.objectId);
        const std::vector<uint8_t> datagram = encode_espnow(header, payload, sizeof(payload));
        TEST_ASSERT_TRUE_MESSAGE(!datagram.empty(), testCase.label);

        const HubRelay::RadioIngress ingress = HubRelay::classifyRadio(datagram.data(), datagram.size());
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(btp::Error::Ok), static_cast<int>(ingress.error),
                                  testCase.label);
        TEST_ASSERT_FALSE_MESSAGE(ingress.mayConsume, testCase.label);
    }
}

void test_a_malformed_datagram_is_a_drop_not_a_relay() {
    const uint8_t garbage[48] = {0};
    const HubRelay::RadioIngress ingress = HubRelay::classifyRadio(garbage, sizeof(garbage));
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(btp::Error::Ok), static_cast<int>(ingress.error));
    TEST_ASSERT_FALSE(ingress.mayConsume);
}

// ---------------------------------------------------------------------------
// HubRegistry
// ---------------------------------------------------------------------------
void test_bind_then_lookup() {
    TEST_ASSERT_TRUE(HubRegistry::bind(0xC1U, 0xB1U));
    uint32_t peer = 0U;
    TEST_ASSERT_TRUE(HubRegistry::lookup(0xC1U, &peer));
    TEST_ASSERT_EQUAL_HEX32(0xB1U, peer);
    TEST_ASSERT_EQUAL_size_t(1U, HubRegistry::count());
}

void test_lookup_of_an_unbound_child_fails() {
    uint32_t peer = 0xDEADU;
    TEST_ASSERT_FALSE(HubRegistry::lookup(0xC9U, &peer));
    TEST_ASSERT_FALSE(HubRegistry::lookup(0U, &peer));
}

void test_unbind() {
    TEST_ASSERT_TRUE(HubRegistry::bind(0xC1U, 0xB1U));
    TEST_ASSERT_TRUE(HubRegistry::unbind(0xC1U));
    uint32_t peer = 0U;
    TEST_ASSERT_FALSE(HubRegistry::lookup(0xC1U, &peer));
    TEST_ASSERT_FALSE(HubRegistry::unbind(0xC1U));  // already gone
    TEST_ASSERT_EQUAL_size_t(0U, HubRegistry::count());
}

// Re-binding must replace in place, never consume a second slot: pointing a
// channel at a different robot is the normal way this table changes and must
// not be able to exhaust it.
void test_rebinding_the_same_child_replaces_in_place() {
    TEST_ASSERT_TRUE(HubRegistry::bind(0xC1U, 0xB1U));
    TEST_ASSERT_TRUE(HubRegistry::bind(0xC1U, 0xB2U));
    uint32_t peer = 0U;
    TEST_ASSERT_TRUE(HubRegistry::lookup(0xC1U, &peer));
    TEST_ASSERT_EQUAL_HEX32(0xB2U, peer);
    TEST_ASSERT_EQUAL_size_t(1U, HubRegistry::count());
}

void test_table_full() {
    for (uint32_t i = 0U; i < HubRegistry::kMaxBindings; ++i) {
        TEST_ASSERT_TRUE(HubRegistry::bind(0x100U + i, 0x200U + i));
    }
    TEST_ASSERT_EQUAL_size_t(HubRegistry::kMaxBindings, HubRegistry::count());

    TEST_ASSERT_FALSE(HubRegistry::bind(0x999U, 0x888U));
    // A full table still re-binds someone already in it.
    TEST_ASSERT_TRUE(HubRegistry::bind(0x100U, 0x777U));

    // Freeing one slot makes room again.
    TEST_ASSERT_TRUE(HubRegistry::unbind(0x101U));
    TEST_ASSERT_TRUE(HubRegistry::bind(0x999U, 0x888U));

    HubRegistry::Binding bindings[HubRegistry::kMaxBindings];
    TEST_ASSERT_EQUAL_size_t(HubRegistry::kMaxBindings,
                             HubRegistry::enumerate(bindings, HubRegistry::kMaxBindings));
}

void test_zero_ids_are_refused() {
    TEST_ASSERT_FALSE(HubRegistry::bind(0U, 0xB1U));
    TEST_ASSERT_FALSE(HubRegistry::bind(0xC1U, 0U));
    TEST_ASSERT_EQUAL_size_t(0U, HubRegistry::count());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_relay_down_never_rewrites_the_nonce_triple);
    RUN_TEST(test_reoriginating_the_same_payload_would_change_the_nonce);
    RUN_TEST(test_relay_up_passes_each_fragment_verbatim_and_reassembles_nothing);
    RUN_TEST(test_relay_down_passes_each_fragment_verbatim);
    RUN_TEST(test_heartbeat_status_is_a_link_candidate);
    RUN_TEST(test_command_result_is_candidate_regardless_of_prefix_bytes);
    RUN_TEST(test_manifest_data_is_candidate_regardless_of_prefix_bytes);
    RUN_TEST(test_everything_else_goes_up);
    RUN_TEST(test_a_malformed_datagram_is_a_drop_not_a_relay);
    RUN_TEST(test_bind_then_lookup);
    RUN_TEST(test_lookup_of_an_unbound_child_fails);
    RUN_TEST(test_unbind);
    RUN_TEST(test_rebinding_the_same_child_replaces_in_place);
    RUN_TEST(test_table_full);
    RUN_TEST(test_zero_ids_are_refused);
    return UNITY_END();
}
