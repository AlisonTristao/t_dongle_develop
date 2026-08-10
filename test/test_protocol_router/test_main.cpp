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
// BtpTransport (identity, command envelope) against bally_protocol's
// canonical v1 test vectors, on the host, without any ESP32/Arduino
// dependency. This is what topico 12 step 14 calls "testes com vetores
// canonicos".

namespace {

std::vector<std::uint8_t> read_vector(const char* relative_path) {
    const std::string candidates[] = {
        std::string("../bally_protocol/test-vectors/v1/") + relative_path,
        std::string("../../bally_protocol/test-vectors/v1/") + relative_path,
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

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_router_routes_every_unfragmented_valid_vector);
    RUN_TEST(test_router_preserves_header_fields_and_payload_bytes);
    RUN_TEST(test_router_rejects_every_invalid_vector);
    RUN_TEST(test_router_reassembles_two_concurrent_sources_out_of_order);
    RUN_TEST(test_command_request_vector_parses_with_matching_target);
    RUN_TEST(test_build_result_round_trips_through_parse_result);
    RUN_TEST(test_copy_shell_command_rejects_control_bytes);
    RUN_TEST(test_source_id_from_mac_matches_ecosystem_formula);
    return UNITY_END();
}
