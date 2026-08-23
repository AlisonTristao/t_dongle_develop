#include <unity.h>

#include <ProtocolRouter.h>
#include <BtpTransport.h>
#include <btp/codec.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Runs ProtocolRouter (decode + CRC + reassembly + routing decision) and
// BtpTransport (identity, command envelope) against BTP's
// canonical v1 test vectors, on the host, without any ESP32/Arduino
// dependency. This is what topico 12 step 14 calls "testes com vetores
// canonicos".

namespace {

std::vector<std::uint8_t> read_vector(const char* relative_path) {
    const std::string candidates[] = {
        std::string("../BTP/test-vectors/v1/") + relative_path,
        std::string("../../BTP/test-vectors/v1/") + relative_path,
    };
    for (const auto& path : candidates) {
        std::ifstream input(path, std::ios::binary);
        if (input) {
            return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        }
    }
    return {};
}

const std::uint8_t kMacA[6] = {0xAA, 0x00, 0x00, 0x00, 0x00, 0x01};
const std::uint8_t kMacB[6] = {0xBB, 0x00, 0x00, 0x00, 0x00, 0x02};

void write_u16(std::uint8_t* output, std::uint16_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
}

void write_u32(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
    output[2] = static_cast<std::uint8_t>(value >> 16U);
    output[3] = static_cast<std::uint8_t>(value >> 24U);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_router_routes_every_unfragmented_valid_vector() {
    const char* names[] = {"hello", "log_utf8", "telemetry_packed_le", "protocol_test", "command_request"};
    for (const char* name : names) {
        const std::string fileName = std::string("valid/") + name + ".bin";
        const std::vector<std::uint8_t> bytes = read_vector(fileName.c_str());
        TEST_ASSERT_TRUE_MESSAGE(!bytes.empty(), name);

        ProtocolRouter::Router router;
        ProtocolRouter::RoutedMessage routed{};
        const ProtocolRouter::Outcome outcome =
            router.submit(kMacA, bytes.data(), bytes.size(), 1000U, &routed);

        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(ProtocolRouter::Outcome::Routed),
                                  static_cast<int>(outcome), name);
        TEST_ASSERT_EQUAL_UINT8(0, std::memcmp(routed.mac, kMacA, 6));
        TEST_ASSERT_EQUAL_UINT32(1000U, routed.arrivalMs);
    }
}

void test_router_preserves_header_fields_and_payload_bytes() {
    const std::vector<std::uint8_t> bytes = read_vector("valid/log_utf8.bin");
    TEST_ASSERT_FALSE(bytes.empty());

    ProtocolRouter::Router router;
    ProtocolRouter::RoutedMessage routed{};
    const ProtocolRouter::Outcome outcome = router.submit(kMacA, bytes.data(), bytes.size(), 42U, &routed);
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolRouter::Outcome::Routed), static_cast<int>(outcome));

    // log_utf8.json: source_id=0x11223344, boot_id=0xA1B2C3D4, sequence=2,
    // timestamp_us=0xF4240, object_id=0x0002, type=LOG.
    TEST_ASSERT_EQUAL_UINT32(0x11223344U, routed.header.source_id);
    TEST_ASSERT_EQUAL_UINT32(0xA1B2C3D4U, routed.header.boot_id);
    TEST_ASSERT_EQUAL_UINT32(2U, routed.header.sequence);
    TEST_ASSERT_TRUE(routed.header.timestamp_us == 0xF4240ULL);
    TEST_ASSERT_EQUAL_UINT16(0x0002U, routed.header.object_id);
    TEST_ASSERT_TRUE(routed.header.type == btp::MessageType::Log);

    const std::uint8_t expected[] = {0x49, 0x6e, 0x69, 0x63, 0x69, 0x61, 0x6c, 0x69, 0x7a, 0x61,
                                     0xc3, 0xa7, 0xc3, 0xa3, 0x6f, 0x20, 0x4f, 0x4b, 0x3a, 0x20,
                                     0xc2, 0xb5};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), routed.payloadSize);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, routed.payload, sizeof(expected));

    // Local arrival metadata must never overwrite the origin timestamp.
    TEST_ASSERT_TRUE(routed.arrivalMs != routed.header.timestamp_us);
}

// Topico 30/31 ("confira que os tetos comportam o +16"): a channel C message
// at BtpTransport::kMaxLogicalPayloadSize (600 octets of plaintext -- no
// real caller reaches this today, but it is the documented ceiling) grows to
// 616 once sealed, and this ROUTER is what has to hold that whole message
// across reassembly, before RadioSeal::open() ever gets a chance to shrink
// it back down. Before ProtocolRouter::kMaxPayloadSize was bumped from 600
// to 616 (this topico), this exact case failed as DroppedReassembly /
// MessageTooLarge -- not a hypothetical, a real latent ceiling mismatch this
// test pins down. No real seal is needed to prove the SIZE plumbing: this
// builds the 616-octet "sealed" payload as identifiable filler bytes,
// fragments/encodes it exactly as BtpTransport::sendLogical would, and feeds
// the frames through a real Router.
void test_router_holds_a_full_size_sealed_channel_c_message() {
    constexpr std::size_t kSealedSize = BtpTransport::kMaxLogicalPayloadSize + BtpTransport::kAeadTagSize;  // 616
    std::uint8_t sealedPayload[kSealedSize];
    for (std::size_t i = 0U; i < kSealedSize; ++i) {
        sealedPayload[i] = static_cast<std::uint8_t>(i);
    }

    std::uint8_t count = 0U;
    TEST_ASSERT_EQUAL(static_cast<int>(btp::Error::Ok),
                      static_cast<int>(btp::fragment_count(kSealedSize, btp::TransportProfile::EspNow, &count)));
    TEST_ASSERT_TRUE(count > 1U);  // 616 does not fit one ESP-NOW fragment (210)

    const btp::Header logicalHeader{
        btp::MessageType::Command,
        static_cast<std::uint16_t>(btp::kFlagEncrypted | btp::kFlagFragmented),
        0x11223344U,
        0x55667788U,
        99U,
        0ULL,
        0x0001U,
        0U,
        count,
    };

    ProtocolRouter::Router router;
    ProtocolRouter::RoutedMessage routed{};
    ProtocolRouter::Outcome outcome = ProtocolRouter::Outcome::FragmentAccepted;

    for (std::uint8_t index = 0U; index < count; ++index) {
        btp::Frame fragment{};
        TEST_ASSERT_EQUAL(static_cast<int>(btp::Error::Ok),
                          static_cast<int>(btp::make_fragment(logicalHeader, {sealedPayload, kSealedSize},
                                                              btp::TransportProfile::EspNow, index, &fragment)));

        std::uint8_t frame[btp::kEspNowMaxFrameSize];
        std::size_t frameSize = 0U;
        TEST_ASSERT_EQUAL(static_cast<int>(btp::Error::Ok),
                          static_cast<int>(btp::encode(fragment, btp::TransportProfile::EspNow, frame,
                                                       sizeof(frame), &frameSize)));

        const std::uint8_t mac[6] = {0, 1, 2, 3, 4, 5};
        outcome = router.submit(mac, frame, frameSize, static_cast<std::uint64_t>(index), &routed);
    }

    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolRouter::Outcome::Routed), static_cast<int>(outcome));
    TEST_ASSERT_EQUAL_UINT32(kSealedSize, routed.payloadSize);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(sealedPayload, routed.payload, kSealedSize);
}

void test_router_rejects_every_invalid_vector() {
    struct Case {
        const char* file;
        ProtocolRouter::Outcome expected;
    };
    const Case cases[] = {
        {"crc.bin", ProtocolRouter::Outcome::DroppedCrc},
        {"fragment_count.bin", ProtocolRouter::Outcome::DroppedDecode},
        {"fragment_index.bin", ProtocolRouter::Outcome::DroppedDecode},
        {"header_size.bin", ProtocolRouter::Outcome::DroppedDecode},
        {"magic.bin", ProtocolRouter::Outcome::DroppedDecode},
        {"payload_size.bin", ProtocolRouter::Outcome::DroppedDecode},
        {"version.bin", ProtocolRouter::Outcome::DroppedDecode},
    };

    for (const Case& testCase : cases) {
        const std::vector<std::uint8_t> bytes = read_vector((std::string("invalid/") + testCase.file).c_str());
        TEST_ASSERT_TRUE_MESSAGE(!bytes.empty(), testCase.file);

        ProtocolRouter::Router router;
        ProtocolRouter::RoutedMessage routed{};
        const ProtocolRouter::Outcome outcome =
            router.submit(kMacA, bytes.data(), bytes.size(), 1U, &routed);

        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(testCase.expected), static_cast<int>(outcome), testCase.file);
    }
}

// Canonical scenario from test-vectors/v1/manifest.json:
// "two_sources_interleaved_out_of_order" -- two different sources (A, B)
// fragmenting concurrently, arriving out of order, must reassemble
// independently without one clobbering the other's slot.
void test_router_reassembles_two_concurrent_sources_out_of_order() {
    const std::vector<std::uint8_t> a0 = read_vector("valid/fragment_source_a_0.bin");
    const std::vector<std::uint8_t> a1 = read_vector("valid/fragment_source_a_1.bin");
    const std::vector<std::uint8_t> b0 = read_vector("valid/fragment_source_b_0.bin");
    const std::vector<std::uint8_t> b1 = read_vector("valid/fragment_source_b_1.bin");
    TEST_ASSERT_FALSE(a0.empty());
    TEST_ASSERT_FALSE(a1.empty());
    TEST_ASSERT_FALSE(b0.empty());
    TEST_ASSERT_FALSE(b1.empty());

    ProtocolRouter::Router router;
    ProtocolRouter::RoutedMessage routed{};

    // arrival_order: a_1, b_0, a_0, b_1
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolRouter::Outcome::FragmentAccepted),
                      static_cast<int>(router.submit(kMacA, a1.data(), a1.size(), 1U, &routed)));
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolRouter::Outcome::FragmentAccepted),
                      static_cast<int>(router.submit(kMacB, b0.data(), b0.size(), 2U, &routed)));

    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolRouter::Outcome::Routed),
                      static_cast<int>(router.submit(kMacA, a0.data(), a0.size(), 3U, &routed)));
    const std::uint8_t expectedA[] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expectedA), routed.payloadSize);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedA, routed.payload, sizeof(expectedA));
    TEST_ASSERT_EQUAL_UINT8(0, std::memcmp(routed.mac, kMacA, 6));

    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolRouter::Outcome::Routed),
                      static_cast<int>(router.submit(kMacB, b1.data(), b1.size(), 4U, &routed)));
    const std::uint8_t expectedB[] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expectedB), routed.payloadSize);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedB, routed.payload, sizeof(expectedB));
    TEST_ASSERT_EQUAL_UINT8(0, std::memcmp(routed.mac, kMacB, 6));

    const ProtocolRouter::Stats stats = router.stats();
    TEST_ASSERT_EQUAL_UINT32(2U, stats.routed);
    TEST_ASSERT_EQUAL_UINT32(2U, stats.fragmentsAccepted);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.droppedReassembly);
}

void test_command_request_vector_parses_with_matching_target() {
    const std::vector<std::uint8_t> bytes = read_vector("valid/command_request.bin");
    TEST_ASSERT_FALSE(bytes.empty());

    ProtocolRouter::Router router;
    ProtocolRouter::RoutedMessage routed{};
    TEST_ASSERT_EQUAL(static_cast<int>(ProtocolRouter::Outcome::Routed),
                      static_cast<int>(router.submit(kMacA, bytes.data(), bytes.size(), 1U, &routed)));

    using namespace BtpTransport::btp_command;
    RequestView request{};
    const btp::ByteView payload{routed.payload, routed.payloadSize};
    // command_request.json: target_source_id=0x11223344, target_boot_id=0x55667788.
    const ParseError ok = parse_request(routed.header, payload, 0x11223344U, 0x55667788U, &request);
    TEST_ASSERT_EQUAL(static_cast<int>(ParseError::Ok), static_cast<int>(ok));
    TEST_ASSERT_EQUAL_UINT16(0x0201U, request.action_id);
    TEST_ASSERT_EQUAL_UINT16(1U, request.action_version);
    TEST_ASSERT_EQUAL_UINT32(6U, request.parameters.size);

    const ParseError wrongTarget = parse_request(routed.header, payload, 0x99999999U, 0x55667788U, &request);
    TEST_ASSERT_EQUAL(static_cast<int>(ParseError::WrongTarget), static_cast<int>(wrongTarget));
}

void test_build_result_round_trips_through_parse_result() {
    using namespace BtpTransport::btp_command;

    std::uint8_t buffer[64];
    const std::size_t written = build_result(0x11223344U, 0x55667788U, 7U, kShellActionId, kShellActionVersion,
                                             Status::Success, ErrorCode::None, "ok", nullptr, 0, buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(written > 0);

    btp::Header header{};
    header.type = btp::MessageType::Command;
    header.object_id = kCommandResultObjectId;

    ResultView result{};
    const ParseError parseError = parse_result(header, {buffer, written}, &result);
    TEST_ASSERT_EQUAL(static_cast<int>(ParseError::Ok), static_cast<int>(parseError));
    TEST_ASSERT_EQUAL_UINT32(0x11223344U, result.request_source_id);
    TEST_ASSERT_EQUAL_UINT32(0x55667788U, result.request_boot_id);
    TEST_ASSERT_EQUAL_UINT32(7U, result.reply_to_sequence);
    TEST_ASSERT_TRUE(result.status == Status::Success);
    TEST_ASSERT_TRUE(result.error_code == ErrorCode::None);
    TEST_ASSERT_EQUAL_UINT32(2U, result.message.size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY("ok", result.message.data, 2U);
}

void test_copy_shell_command_rejects_control_bytes() {
    using namespace BtpTransport::btp_command;

    std::uint8_t payload[kRequestPrefixSize + 4];
    write_u32(payload, 1U);
    write_u32(payload + 4U, 1U);
    write_u16(payload + 8U, kShellActionId);
    write_u16(payload + 10U, kShellActionVersion);
    write_u16(payload + 12U, 0U);
    write_u16(payload + 14U, 0U);
    write_u32(payload + 16U, 4U);
    payload[kRequestPrefixSize + 0] = 'p';
    payload[kRequestPrefixSize + 1] = 'i';
    payload[kRequestPrefixSize + 2] = 'n';
    payload[kRequestPrefixSize + 3] = 'g';

    RequestView request{1U, 1U, kShellActionId, kShellActionVersion, {payload + kRequestPrefixSize, 4U}};
    char out[8] = {0};
    TEST_ASSERT_EQUAL(static_cast<int>(ParseError::Ok),
                      static_cast<int>(copy_shell_command(request, out, sizeof(out))));
    TEST_ASSERT_EQUAL_STRING("ping", out);

    const std::uint8_t withNewline[] = {'p', 'i', '\n', 'g'};
    RequestView badRequest{1U, 1U, kShellActionId, kShellActionVersion, {withNewline, sizeof(withNewline)}};
    TEST_ASSERT_EQUAL(static_cast<int>(ParseError::InvalidShellText),
                      static_cast<int>(copy_shell_command(badRequest, out, sizeof(out))));
}

void test_source_id_from_mac_matches_ecosystem_formula() {
    const std::uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    TEST_ASSERT_EQUAL_UINT32(0x33445566U, BtpTransport::btp_command::source_id_from_mac(mac));

    const std::uint8_t zeroTail[6] = {0x11, 0x22, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT32(1U, BtpTransport::btp_command::source_id_from_mac(zeroTail));
}

// ---------------------------------------------------------------------------
// Topico 30: sealing plumbing in BtpTransport::sendLogical/encodeSingleFrame.
//
// A real seal (btp::aead_seal_aes_gcm) cannot link on this host -- see
// RadioSeal.h for why that is deliberate, not a gap. What IS testable here,
// with a fake SealFn, is the plumbing BtpTransport itself is responsible
// for, independent of which cipher backend eventually runs it:
//   - sealing happens ONCE per logical message, before fragmenting;
//   - fragment_count is computed from the SEALED size, not the plaintext
//     size (the exact bug class topico 31 passo 5 calls out for the robot,
//     and the reason sealing lives inside sendLogical rather than at each
//     of channel C's four call sites);
//   - the header handed to the sealer already carries ENCRYPTED, in
//     canonical (unfragmented) form;
//   - a seal failure aborts the whole send -- fail closed, no cleartext
//     fallback, and no partial send either.
// ---------------------------------------------------------------------------

namespace seal_plumbing {

bool g_shouldSucceed = true;
int g_callCount = 0;
btp::Header g_lastHeader{};
std::uint16_t g_lastPayloadSize = 0;
std::uint8_t g_lastPlaintext[BtpTransport::kMaxLogicalPayloadSize] = {0};

void reset() {
    g_shouldSucceed = true;
    g_callCount = 0;
    g_lastHeader = btp::Header{};
    g_lastPayloadSize = 0;
    std::memset(g_lastPlaintext, 0, sizeof(g_lastPlaintext));
}

// Not real AEAD -- a fixed-size-growth stand-in (XOR "ciphertext", a
// constant 16-octet "tag") whose only job is to exercise the SIZE and
// ORDERING contract SealFn documents, which is what these tests check.
bool fakeSeal(void*, const btp::Header& header, std::uint16_t payloadSize,
             const std::uint8_t* plaintext, std::uint8_t* out) {
    ++g_callCount;
    g_lastHeader = header;
    g_lastPayloadSize = payloadSize;
    if (payloadSize > 0U) {
        std::memcpy(g_lastPlaintext, plaintext, payloadSize);
    }
    if (!g_shouldSucceed) {
        return false;
    }
    for (std::uint16_t i = 0U; i < payloadSize; ++i) {
        out[i] = static_cast<std::uint8_t>(plaintext[i] ^ 0xAAU);
    }
    for (std::size_t i = 0U; i < 16U; ++i) {
        out[payloadSize + i] = 0x55U;
    }
    return true;
}

bool captureSend(void* context, const std::uint8_t*, const std::uint8_t* data, std::size_t size) {
    auto* frames = static_cast<std::vector<std::vector<std::uint8_t>>*>(context);
    frames->emplace_back(data, data + size);
    return true;
}

}  // namespace seal_plumbing

void test_sendLogical_fails_closed_when_seal_fails() {
    using namespace seal_plumbing;
    reset();
    g_shouldSucceed = false;

    BtpTransport::configureIdentity(0x11223344U, 0x55667788U);
    const std::uint8_t mac[6] = {0, 1, 2, 3, 4, 5};
    const std::uint8_t payload[4] = {1, 2, 3, 4};
    std::vector<std::vector<std::uint8_t>> sentFrames;

    const bool ok = BtpTransport::sendLogical(captureSend, &sentFrames, mac, btp::MessageType::Control,
                                              0x0009U, payload, sizeof(payload), 0ULL, fakeSeal, nullptr);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(sentFrames.empty());
    TEST_ASSERT_EQUAL_INT(1, g_callCount);
}

void test_sendLogical_seals_once_and_refragments_on_the_sealed_size() {
    using namespace seal_plumbing;
    reset();

    BtpTransport::configureIdentity(0x11223344U, 0x55667788U);
    const std::uint8_t mac[6] = {0, 1, 2, 3, 4, 5};

    // btp::kEspNowMaxPayloadSize is 210: 200 octets fit one fragment
    // unsealed, but 200 + 16 = 216 needs two. Getting fragment_count wrong
    // here is exactly "a 195-210 octet sample silently costs double" from
    // topico 31 passo 5, just triggered from the send side instead of the
    // publisher.
    std::uint8_t payload[200];
    for (std::size_t i = 0U; i < sizeof(payload); ++i) {
        payload[i] = static_cast<std::uint8_t>(i);
    }
    std::vector<std::vector<std::uint8_t>> sentFrames;

    const bool ok = BtpTransport::sendLogical(captureSend, &sentFrames, mac, btp::MessageType::Telemetry,
                                              0x1234U, payload, sizeof(payload), 42ULL, fakeSeal, nullptr);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(1, g_callCount);  // sealed ONCE, not once per fragment
    TEST_ASSERT_EQUAL_UINT16(200U, g_lastPayloadSize);
    TEST_ASSERT_EQUAL_MEMORY(payload, g_lastPlaintext, sizeof(payload));

    // What the sealer saw: ENCRYPTED already set, canonical (unfragmented)
    // shape -- the AAD contract (BTP/docs/encryption.md section 5) demands
    // exactly this, before fragment_count is even known.
    TEST_ASSERT_TRUE((g_lastHeader.flags & btp::kFlagEncrypted) != 0U);
    TEST_ASSERT_TRUE((g_lastHeader.flags & btp::kFlagFragmented) == 0U);
    TEST_ASSERT_EQUAL_UINT8(0U, g_lastHeader.fragment_index);
    TEST_ASSERT_EQUAL_UINT8(1U, g_lastHeader.fragment_count);

    // 216 sealed octets over a 210 ceiling: two fragments on the wire.
    TEST_ASSERT_EQUAL_UINT32(2U, sentFrames.size());

    std::size_t reassembledSize = 0U;
    std::uint32_t sequence = 0U;
    for (std::size_t i = 0U; i < sentFrames.size(); ++i) {
        btp::DecodedFrame decoded{};
        TEST_ASSERT_EQUAL(static_cast<int>(btp::Error::Ok),
                          static_cast<int>(btp::decode(sentFrames[i].data(), sentFrames[i].size(),
                                                       btp::TransportProfile::EspNow, &decoded)));
        TEST_ASSERT_TRUE((decoded.header.flags & btp::kFlagEncrypted) != 0U);
        TEST_ASSERT_TRUE((decoded.header.flags & btp::kFlagFragmented) != 0U);
        TEST_ASSERT_EQUAL_UINT8(2U, decoded.header.fragment_count);
        TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(i), decoded.header.fragment_index);
        if (i == 0U) {
            sequence = decoded.header.sequence;
        } else {
            // One logical message, one sequence -- not one per fragment.
            TEST_ASSERT_EQUAL_UINT32(sequence, decoded.header.sequence);
        }
        reassembledSize += decoded.payload.size;
    }
    TEST_ASSERT_EQUAL_UINT32(216U, reassembledSize);  // 200 plaintext + 16-octet tag
}

void test_encodeSingleFrame_seals_and_respects_the_espnow_ceiling() {
    using namespace seal_plumbing;
    reset();

    BtpTransport::configureIdentity(0x11223344U, 0x55667788U);

    // 194 + 16 == btp::kEspNowMaxPayloadSize exactly: must still fit.
    std::uint8_t payload194[194] = {0};
    std::uint8_t output[btp::kEspNowMaxFrameSize] = {0};
    std::size_t bytesWritten = 0U;
    TEST_ASSERT_TRUE(BtpTransport::encodeSingleFrame(btp::MessageType::Control, 0x0009U, 7U, 0ULL,
                                                     payload194, sizeof(payload194), output, sizeof(output),
                                                     &bytesWritten, fakeSeal, nullptr));

    btp::DecodedFrame decoded{};
    TEST_ASSERT_EQUAL(static_cast<int>(btp::Error::Ok),
                      static_cast<int>(btp::decode(output, bytesWritten, btp::TransportProfile::EspNow, &decoded)));
    TEST_ASSERT_EQUAL_UINT32(210U, decoded.payload.size);
    TEST_ASSERT_TRUE((decoded.header.flags & btp::kFlagEncrypted) != 0U);

    // One octet over that ceiling once sealed: must fail closed rather than
    // silently truncate or overrun the ESP-NOW frame.
    std::uint8_t payload195[195] = {0};
    TEST_ASSERT_FALSE(BtpTransport::encodeSingleFrame(btp::MessageType::Control, 0x0009U, 8U, 0ULL, payload195,
                                                      sizeof(payload195), output, sizeof(output), &bytesWritten,
                                                      fakeSeal, nullptr));

    // The heartbeat's own case: an empty plaintext still seals into a
    // 16-octet tag, never sent as if it were cleartext-empty.
    TEST_ASSERT_TRUE(BtpTransport::encodeSingleFrame(btp::MessageType::Control, 0x0009U, 9U, 0ULL, nullptr, 0U,
                                                     output, sizeof(output), &bytesWritten, fakeSeal, nullptr));
    btp::DecodedFrame decodedEmpty{};
    TEST_ASSERT_EQUAL(static_cast<int>(btp::Error::Ok),
                      static_cast<int>(btp::decode(output, bytesWritten, btp::TransportProfile::EspNow,
                                                   &decodedEmpty)));
    TEST_ASSERT_EQUAL_UINT32(16U, decodedEmpty.payload.size);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_router_routes_every_unfragmented_valid_vector);
    RUN_TEST(test_router_preserves_header_fields_and_payload_bytes);
    RUN_TEST(test_router_holds_a_full_size_sealed_channel_c_message);
    RUN_TEST(test_router_rejects_every_invalid_vector);
    RUN_TEST(test_router_reassembles_two_concurrent_sources_out_of_order);
    RUN_TEST(test_command_request_vector_parses_with_matching_target);
    RUN_TEST(test_build_result_round_trips_through_parse_result);
    RUN_TEST(test_copy_shell_command_rejects_control_bytes);
    RUN_TEST(test_source_id_from_mac_matches_ecosystem_formula);
    RUN_TEST(test_sendLogical_fails_closed_when_seal_fails);
    RUN_TEST(test_sendLogical_seals_once_and_refragments_on_the_sealed_size);
    RUN_TEST(test_encodeSingleFrame_seals_and_respects_the_espnow_ceiling);
    return UNITY_END();
}
