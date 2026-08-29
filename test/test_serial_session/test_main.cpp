#include <unity.h>

#include <BtpTransport.h>
#include <SerialSession.h>
#include <ShellLineEditor.h>
#include <btp/codec.hpp>
#include <btp/stream.hpp>

#include <algorithm>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Exercises SerialSession (topico 13's portable session/handshake logic) and
// the shared btp::SerialDecoder/COBS pipeline it sits on top of, against the
// four CRITERIOS DE ACEITE of
// bally_protocol/topicos/13_dongle_serial_mux_sessao.txt:
//   1. terminal never receives TELEMETRY bytes;
//   2. a payload containing CR/LF/zero survives the serial link intact;
//   3. a partial/noisy frame recovers at the next delimiter;
//   4. a plain human can still use the console after reboot (covered by
//      Session starting in State::Console and never leaving it on its own).
// SerialMux itself (Arduino/FreeRTOS glue: real queues, Serial.write) is not
// testable under env:native and is instead exercised by pio run -e
// tdongle-s3 (compile) and manual hardware verification.

namespace {

using std::uint8_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;

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

std::uint16_t read_u16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(input[0]) | static_cast<std::uint16_t>(input[1] << 8U);
}

std::uint32_t read_u32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) | (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) | (static_cast<std::uint32_t>(input[3]) << 24U);
}

std::vector<std::uint8_t> build_hello_payload(std::uint8_t role, std::uint32_t maxLogicalPayload,
                                              std::uint16_t maxInflight, std::uint16_t maxSubs,
                                              std::uint32_t maxDedup, std::uint32_t timeoutMs,
                                              const std::uint8_t uuid[16], std::uint32_t configRevision,
                                              const std::vector<std::uint8_t>& versions) {
    std::vector<std::uint8_t> out(40U + versions.size(), 0U);
    out[0] = role;
    out[1] = static_cast<std::uint8_t>(versions.size());
    write_u16(out.data() + 2U, 0U);
    write_u32(out.data() + 4U, maxLogicalPayload);
    write_u16(out.data() + 8U, maxInflight);
    write_u16(out.data() + 10U, maxSubs);
    write_u32(out.data() + 12U, maxDedup);
    write_u32(out.data() + 16U, timeoutMs);
    std::memcpy(out.data() + 20U, uuid, 16U);
    write_u32(out.data() + 36U, configRevision);
    for (std::size_t i = 0U; i < versions.size(); ++i) {
        out[40U + i] = versions[i];
    }
    return out;
}

// Encodes a full BTP frame (Serial transport profile) for the given header
// and payload.
std::vector<std::uint8_t> encode_frame(const btp::Header& header, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out(btp::kV1HeaderSize + btp::kV1CrcSize + payload.size());
    const btp::Frame frame{header, {payload.empty() ? nullptr : payload.data(), payload.size()}};
    std::size_t written = 0U;
    const btp::Error error = btp::encode(frame, btp::TransportProfile::Serial, out.data(), out.size(), &written);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(btp::Error::Ok), static_cast<int>(error), "encode_frame failed");
    out.resize(written);
    return out;
}

// Wraps encode_frame's bytes in "0x00 || COBS(frame) || 0x00" the same way
// the real wire link carries them (fragmentation-and-transports.md 3.2).
std::vector<std::uint8_t> to_serial_packet(const std::vector<std::uint8_t>& frameBytes) {
    std::vector<std::uint8_t> encoded(btp::kSerialMaxCobsBlockSize);
    std::size_t written = 0U;
    const btp::CobsError error =
        btp::cobs_encode(frameBytes.data(), frameBytes.size(), encoded.data(), encoded.size(), &written);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(btp::CobsError::Ok), static_cast<int>(error), "cobs_encode failed");
    encoded.resize(written);

    std::vector<std::uint8_t> packet;
    packet.reserve(encoded.size() + 2U);
    packet.push_back(0U);
    packet.insert(packet.end(), encoded.begin(), encoded.end());
    packet.push_back(0U);
    return packet;
}

struct DecoderFixture {
    std::vector<std::uint8_t> encodedBuffer;
    std::vector<std::uint8_t> decodedBuffer;
    btp::SerialDecoder decoder;

    DecoderFixture()
        : encodedBuffer(btp::kSerialMaxCobsBlockSize),
          decodedBuffer(btp::kSerialMaxFrameSize),
          decoder(encodedBuffer.data(), encodedBuffer.size(), decodedBuffer.data(), decodedBuffer.size()) {}
};

// Feeds raw bytes (not necessarily framed) through the decoder and returns
// every successfully decoded frame's payload (copied out immediately, since
// DecodedFrame.payload only stays valid until the next candidate completes).
struct CapturedFrame {
    btp::Header header;
    std::vector<std::uint8_t> payload;
};

std::vector<CapturedFrame> feed_bytes(btp::SerialDecoder& decoder, const std::vector<std::uint8_t>& bytes,
                                      std::size_t* outCobsErrors = nullptr, std::size_t* outFrameErrors = nullptr) {
    std::vector<CapturedFrame> captured;
    for (const std::uint8_t byte : bytes) {
        btp::DecodedFrame decoded{};
        const btp::SerialDecodeResult result = decoder.push(byte, &decoded);
        if (result.event == btp::SerialDecodeEvent::Frame) {
            CapturedFrame frame{decoded.header, {}};
            frame.payload.assign(decoded.payload.data, decoded.payload.data + decoded.payload.size);
            captured.push_back(std::move(frame));
        } else if (result.event == btp::SerialDecodeEvent::CobsError ||
                  result.event == btp::SerialDecodeEvent::Overflow) {
            if (outCobsErrors != nullptr) {
                ++(*outCobsErrors);
            }
        } else if (result.event == btp::SerialDecodeEvent::FrameError) {
            if (outFrameErrors != nullptr) {
                ++(*outFrameErrors);
            }
        }
    }
    return captured;
}

const std::uint8_t kUuidA[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
const std::uint8_t kLocalUuid[16] = {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
                                     0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF};

}  // namespace

void setUp() {}
void tearDown() {}

// ---- HELLO parsing against the canonical vector ---------------------------

void test_parse_hello_matches_canonical_vector() {
    const std::vector<std::uint8_t> bytes = read_vector("valid/hello.bin");
    TEST_ASSERT_FALSE_MESSAGE(bytes.empty(), "missing BTP/test-vectors/v1/valid/hello.bin");

    btp::DecodedFrame decoded{};
    const btp::Error error = btp::decode(bytes.data(), bytes.size(), btp::TransportProfile::Serial, &decoded);
    TEST_ASSERT_EQUAL(static_cast<int>(btp::Error::Ok), static_cast<int>(error));
    TEST_ASSERT_EQUAL(static_cast<int>(btp::MessageType::Control), static_cast<int>(decoded.header.type));
    TEST_ASSERT_EQUAL_UINT16(SerialSession::kHelloObjectId, decoded.header.object_id);

    SerialSession::HelloView hello{};
    const SerialSession::HelloParseError parseError = SerialSession::parseHello(decoded.payload, &hello);
    TEST_ASSERT_EQUAL(static_cast<int>(SerialSession::HelloParseError::Ok), static_cast<int>(parseError));
    TEST_ASSERT_EQUAL_UINT8(0x03U, hello.role); // DESKTOP
    TEST_ASSERT_TRUE(hello.supportsVersion1);
    TEST_ASSERT_EQUAL_UINT32(53550U, hello.maxLogicalPayload);
    TEST_ASSERT_EQUAL_UINT16(4U, hello.maxInflightReassemblies);
    TEST_ASSERT_EQUAL_UINT16(32U, hello.maxSubscriptions);
    TEST_ASSERT_EQUAL_UINT32(128U, hello.maxDedupEntries);
    TEST_ASSERT_EQUAL_UINT32(5000U, hello.sessionTimeoutMs);
    TEST_ASSERT_EQUAL_UINT32(0U, hello.configRevision);
    TEST_ASSERT_EQUAL_UINT8(0, std::memcmp(hello.peerUuid, kUuidA, 16));
}

// ---- Session: full AwaitingHello -> Protocolled handshake ------------------

void test_session_accepts_hello_and_negotiates_minimum_limits() {
    SerialSession::LocalLimits local{};
    local.maxLogicalPayload = 700U;      // smaller than the vector's 53550
    local.maxInflightReassemblies = 1U;  // smaller than the vector's 4
    local.maxSubscriptions = 8U;         // smaller than the vector's 32
    local.maxDedupEntries = 32U;         // smaller than the vector's 128
    local.sessionTimeoutMs = 15000U;     // larger than the vector's 5000

    SerialSession::Session session(local);
    session.setLocalUuid(kLocalUuid);
    TEST_ASSERT_TRUE(session.isConsole());

    session.beginNegotiation(1000U);
    TEST_ASSERT_EQUAL(static_cast<int>(SerialSession::State::AwaitingHello), static_cast<int>(session.state()));

    const std::vector<std::uint8_t> helloPayload =
        build_hello_payload(0x03U, 53550U, 4U, 32U, 128U, 5000U, kUuidA, 0U, {1U});
    const btp::Header helloHeader{
        btp::MessageType::Control, 0U, 0x0C0D0E0FU, 0x10203040U, 1U, 1000U, SerialSession::kHelloObjectId, 0U, 1U};

    btp::DecodedFrame decoded{};
    decoded.header = helloHeader;
    decoded.payload = {helloPayload.data(), helloPayload.size()};
    decoded.crc32 = 0U;

    std::uint8_t outPayload[64];
    const auto result = session.onFrame(decoded, 1500U, outPayload, sizeof(outPayload));

    TEST_ASSERT_EQUAL(static_cast<int>(SerialSession::Session::FrameOutcome::HelloAccepted),
                      static_cast<int>(result.outcome));
    TEST_ASSERT_EQUAL(static_cast<int>(SerialSession::State::Protocolled), static_cast<int>(session.state()));
    TEST_ASSERT_TRUE(session.isProtocolled());
    TEST_ASSERT_EQUAL_UINT32(0x0C0D0E0FU, session.peerSourceId());
    TEST_ASSERT_EQUAL_UINT32(0x10203040U, session.peerBootId());

    // HELLO_RESULT payload layout (session-and-terminal.md section 2).
    TEST_ASSERT_EQUAL_UINT32(0x0C0D0E0FU, read_u32(outPayload)); // request_source_id
    TEST_ASSERT_EQUAL_UINT32(0x10203040U, read_u32(outPayload + 4U)); // request_boot_id
    TEST_ASSERT_EQUAL_UINT32(1U, read_u32(outPayload + 8U)); // reply_to_sequence
    TEST_ASSERT_EQUAL_UINT8(0x00U, outPayload[12]); // SUCCESS
    TEST_ASSERT_EQUAL_UINT8(0x01U, outPayload[13]); // selected_version
    TEST_ASSERT_EQUAL_UINT16(0x0000U, read_u16(outPayload + 14U)); // error_code NONE
    TEST_ASSERT_EQUAL_UINT32(700U, read_u32(outPayload + 16U)); // min(53550, 700)
    TEST_ASSERT_EQUAL_UINT16(1U, read_u16(outPayload + 20U)); // min(4, 1)
    TEST_ASSERT_EQUAL_UINT16(8U, read_u16(outPayload + 22U)); // min(32, 8)
    TEST_ASSERT_EQUAL_UINT32(32U, read_u32(outPayload + 24U)); // min(128, 32)
    TEST_ASSERT_EQUAL_UINT32(5000U, read_u32(outPayload + 28U)); // min(15000, 5000)
    TEST_ASSERT_EQUAL_UINT8(0, std::memcmp(outPayload + 32U, kLocalUuid, 16U));
}

void test_session_rejects_hello_without_common_version() {
    SerialSession::Session session((SerialSession::LocalLimits{}));
    session.setLocalUuid(kLocalUuid);
    session.beginNegotiation(0U);

    const std::vector<std::uint8_t> helloPayload =
        build_hello_payload(0x03U, 1024U, 1U, 1U, 1U, 1000U, kUuidA, 0U, {2U}); // only version 2

    const btp::Header helloHeader{
        btp::MessageType::Control, 0U, 42U, 43U, 7U, 0U, SerialSession::kHelloObjectId, 0U, 1U};
    btp::DecodedFrame decoded{};
    decoded.header = helloHeader;
    decoded.payload = {helloPayload.data(), helloPayload.size()};

    std::uint8_t outPayload[64];
    const auto result = session.onFrame(decoded, 100U, outPayload, sizeof(outPayload));

    TEST_ASSERT_EQUAL(static_cast<int>(SerialSession::Session::FrameOutcome::HelloRejected),
                      static_cast<int>(result.outcome));
    TEST_ASSERT_TRUE(result.consoleTransition);
    TEST_ASSERT_EQUAL_STRING("BTP/1 CONSOLE\r\n", result.consoleLine);
    TEST_ASSERT_TRUE(session.isConsole());
    TEST_ASSERT_EQUAL_UINT8(0x05U, outPayload[12]); // UNSUPPORTED
    TEST_ASSERT_EQUAL_UINT8(0x00U, outPayload[13]); // selected_version = 0
}

void test_session_hello_deadline_times_out_back_to_console() {
    SerialSession::Session session((SerialSession::LocalLimits{}));
    session.setLocalUuid(kLocalUuid);
    session.beginNegotiation(1000U);

    char consoleLine[SerialSession::kConsoleLineCapacity];
    TEST_ASSERT_FALSE(session.pollTimeout(1000U + SerialSession::kHelloDeadlineMs - 1U, consoleLine));
    TEST_ASSERT_TRUE(session.isConsole() == false);
    TEST_ASSERT_TRUE(session.pollTimeout(1000U + SerialSession::kHelloDeadlineMs, consoleLine));
    TEST_ASSERT_TRUE(session.isConsole());
    TEST_ASSERT_EQUAL_STRING("BTP/1 CONSOLE\r\n", consoleLine);
}

// ---- SESSION_CLOSE ----------------------------------------------------------

void test_session_close_returns_to_console() {
    SerialSession::Session session((SerialSession::LocalLimits{}));
    session.setLocalUuid(kLocalUuid);
    session.beginNegotiation(0U);

    const std::vector<std::uint8_t> helloPayload =
        build_hello_payload(0x03U, 1024U, 1U, 1U, 1U, 20000U, kUuidA, 0U, {1U});
    btp::DecodedFrame helloFrame{};
    helloFrame.header = {btp::MessageType::Control, 0U, 5U, 6U, 1U, 0U, SerialSession::kHelloObjectId, 0U, 1U};
    helloFrame.payload = {helloPayload.data(), helloPayload.size()};
    std::uint8_t scratch[64];
    TEST_ASSERT_EQUAL(static_cast<int>(SerialSession::Session::FrameOutcome::HelloAccepted),
                      static_cast<int>(session.onFrame(helloFrame, 10U, scratch, sizeof(scratch)).outcome));

    std::uint8_t closePayload[8] = {0};
    closePayload[0] = 0x02U; // CLIENT_SHUTDOWN
    write_u32(closePayload + 4U, 500U);

    btp::DecodedFrame closeFrame{};
    closeFrame.header = {
        btp::MessageType::Control, 0U, 5U, 6U, 2U, 0U, SerialSession::kSessionCloseObjectId, 0U, 1U};
    closeFrame.payload = {closePayload, sizeof(closePayload)};

    const auto result = session.onFrame(closeFrame, 20U, scratch, sizeof(scratch));
    TEST_ASSERT_EQUAL(static_cast<int>(SerialSession::Session::FrameOutcome::SessionClosed),
                      static_cast<int>(result.outcome));
    TEST_ASSERT_TRUE(session.isConsole());
    TEST_ASSERT_EQUAL_STRING("BTP/1 CONSOLE\r\n", result.consoleLine);
    TEST_ASSERT_EQUAL_UINT32(5U, read_u32(scratch));
    TEST_ASSERT_EQUAL_UINT32(6U, read_u32(scratch + 4U));
    TEST_ASSERT_EQUAL_UINT32(2U, read_u32(scratch + 8U));
    TEST_ASSERT_EQUAL_UINT8(0x00U, scratch[12]); // SUCCESS
}

void test_transport_loss_abandons_active_session_once() {
    SerialSession::Session session((SerialSession::LocalLimits{}));
    session.beginNegotiation(100U);

    TEST_ASSERT_FALSE(session.isConsole());
    TEST_ASSERT_TRUE(session.onTransportLost());
    TEST_ASSERT_TRUE(session.isConsole());
    TEST_ASSERT_FALSE(session.onTransportLost());
}

// ---- ENTER/READY console handshake text ------------------------------------

void test_enter_line_produces_lowercase_ready_line() {
    char readyLine[SerialSession::kReadyLineCapacity];
    TEST_ASSERT_TRUE(SerialSession::tryParseEnterLine("BTP/1 ENTER 0123456789ABCDEF", readyLine));
    TEST_ASSERT_EQUAL_STRING("BTP/1 READY 0123456789abcdef\r\n", readyLine);

    TEST_ASSERT_FALSE(SerialSession::tryParseEnterLine("BTP/1 ENTER 0123456789ABCDE", readyLine)); // too short
    TEST_ASSERT_FALSE(SerialSession::tryParseEnterLine("btp/1 enter 0123456789abcdef", readyLine)); // wrong case prefix
    TEST_ASSERT_FALSE(SerialSession::tryParseEnterLine("dongle -ping", readyLine));
}

void test_ready_line_from_nonce_is_16_lowercase_hex_digits() {
    char readyLine[SerialSession::kReadyLineCapacity];
    SerialSession::buildReadyLineFromNonce(0x0123456789ABCDEFULL, readyLine);
    TEST_ASSERT_EQUAL_STRING("BTP/1 READY 0123456789abcdef\r\n", readyLine);
}

// ---- classify(): the structural guarantee behind CRITERIO 1 ----------------

void test_classify_never_mixes_telemetry_and_terminal() {
    TEST_ASSERT_EQUAL(static_cast<int>(SerialSession::PriorityClass::kTelemetry),
                      static_cast<int>(SerialSession::classify(btp::MessageType::Telemetry, 0U)));
    TEST_ASSERT_EQUAL(static_cast<int>(SerialSession::PriorityClass::kTerminal),
                      static_cast<int>(SerialSession::classify(btp::MessageType::Terminal,
                                                                SerialSession::kTerminalInObjectId)));
    TEST_ASSERT_EQUAL(static_cast<int>(SerialSession::PriorityClass::kTerminal),
                      static_cast<int>(SerialSession::classify(btp::MessageType::Terminal,
                                                                SerialSession::kTerminalOutObjectId)));
    TEST_ASSERT_TRUE(SerialSession::classify(btp::MessageType::Telemetry, 0U) !=
                     SerialSession::classify(btp::MessageType::Terminal, SerialSession::kTerminalInObjectId));
}

// ---- CRITERIO 2: payload with 0x00/CR/LF survives the link intact ---------

void test_payload_with_nul_cr_lf_round_trips_through_cobs_and_decoder() {
    const std::vector<std::uint8_t> payload = {0x00, 0x0A, 0x0D, 0xFF, 0x00, 0x00, 0x0D, 0x0A, 0x41};
    const btp::Header header{
        btp::MessageType::Terminal, 0U, 99U, 100U, 3U, 123456789U, SerialSession::kTerminalInObjectId, 0U, 1U};
    const std::vector<std::uint8_t> frameBytes = encode_frame(header, payload);
    const std::vector<std::uint8_t> packet = to_serial_packet(frameBytes);

    DecoderFixture fixture;
    const auto captured = feed_bytes(fixture.decoder, packet);

    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(1U), static_cast<uint32_t>(captured.size()));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(payload.size()), static_cast<uint32_t>(captured[0].payload.size()));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload.data(), captured[0].payload.data(), payload.size());
    TEST_ASSERT_EQUAL_UINT32(99U, captured[0].header.source_id);
    TEST_ASSERT_EQUAL_UINT64(123456789U, captured[0].header.timestamp_us);
}

// ---- CRITERIO 3: noise / partial frame recovers at the next delimiter -----

void test_recovers_after_noise_with_no_leading_delimiter() {
    const std::vector<std::uint8_t> payload = {0x01, 0x02, 0x03};
    const btp::Header header{
        btp::MessageType::Log, 0U, 1U, 2U, 3U, 0U, 0x0001U, 0U, 1U};
    const std::vector<std::uint8_t> packet = to_serial_packet(encode_frame(header, payload));

    std::vector<std::uint8_t> stream = {0x11, 0x22, 0x33, 0xFF, 0xAA}; // noise, no leading 0x00
    stream.insert(stream.end(), packet.begin(), packet.end());

    DecoderFixture fixture;
    const auto captured = feed_bytes(fixture.decoder, stream);

    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(1U), static_cast<uint32_t>(captured.size()));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload.data(), captured[0].payload.data(), payload.size());
}

void test_recovers_after_truncated_frame_followed_by_valid_one() {
    const std::vector<std::uint8_t> payloadA = {0xAA, 0xBB, 0xCC, 0xDD};
    const btp::Header headerA{btp::MessageType::Log, 0U, 1U, 2U, 1U, 0U, 0x0001U, 0U, 1U};
    std::vector<std::uint8_t> packetA = to_serial_packet(encode_frame(headerA, payloadA));

    // Truncate the first packet after its leading delimiter + a few encoded
    // bytes (drop the trailing delimiter and remaining bytes), then let a
    // second, complete, valid frame follow immediately.
    std::vector<std::uint8_t> truncated(packetA.begin(), packetA.begin() + 5);

    const std::vector<std::uint8_t> payloadB = {0x10, 0x20, 0x30};
    const btp::Header headerB{btp::MessageType::Log, 0U, 1U, 2U, 2U, 0U, 0x0001U, 0U, 1U};
    const std::vector<std::uint8_t> packetB = to_serial_packet(encode_frame(headerB, payloadB));

    std::vector<std::uint8_t> stream = truncated;
    stream.push_back(0x00); // delimiter that ends the (invalid, truncated) candidate
    stream.insert(stream.end(), packetB.begin() + 1, packetB.end()); // packetB already starts with 0x00

    DecoderFixture fixture;
    std::size_t cobsErrors = 0U;
    const auto captured = feed_bytes(fixture.decoder, stream, &cobsErrors, nullptr);

    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(1U), static_cast<uint32_t>(captured.size()));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payloadB.data(), captured[0].payload.data(), payloadB.size());
}

void test_consecutive_delimiters_produce_no_empty_frame() {
    DecoderFixture fixture;
    const std::vector<std::uint8_t> stream = {0x00, 0x00, 0x00, 0x00};
    std::size_t cobsErrors = 0U;
    std::size_t frameErrors = 0U;
    const auto captured = feed_bytes(fixture.decoder, stream, &cobsErrors, &frameErrors);
    TEST_ASSERT_TRUE(captured.empty());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(0U), static_cast<uint32_t>(cobsErrors));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(0U), static_cast<uint32_t>(frameErrors));
}

// ---- PASSO 12: stress test, telemetry and terminal interleaved ------------

void test_stress_interleaved_telemetry_and_terminal_never_cross_classes() {
    SerialSession::Session session((SerialSession::LocalLimits{}));
    session.setLocalUuid(kLocalUuid);
    session.beginNegotiation(0U);

    const std::vector<std::uint8_t> helloPayload =
        build_hello_payload(0x03U, 4096U, 1U, 1U, 1U, 20000U, kUuidA, 0U, {1U});
    btp::DecodedFrame helloFrame{};
    helloFrame.header = {btp::MessageType::Control, 0U, 7U, 8U, 1U, 0U, SerialSession::kHelloObjectId, 0U, 1U};
    helloFrame.payload = {helloPayload.data(), helloPayload.size()};
    std::uint8_t scratch[64];
    TEST_ASSERT_EQUAL(static_cast<int>(SerialSession::Session::FrameOutcome::HelloAccepted),
                      static_cast<int>(session.onFrame(helloFrame, 1U, scratch, sizeof(scratch)).outcome));

    constexpr int kIterations = 200;
    int terminalOutcomes = 0;
    int telemetryOutcomes = 0;

    for (int i = 0; i < kIterations; ++i) {
        const bool isTerminal = (i % 2) == 0;
        std::vector<std::uint8_t> payload(16, static_cast<std::uint8_t>(i & 0xFF));

        btp::DecodedFrame frame{};
        if (isTerminal) {
            frame.header = {btp::MessageType::Terminal, 0U, 7U, 8U, static_cast<std::uint32_t>(10 + i), 0U,
                            SerialSession::kTerminalInObjectId, 0U, 1U};
        } else {
            frame.header = {btp::MessageType::Telemetry, 0U, 7U, 8U, static_cast<std::uint32_t>(10 + i), 0U,
                            0x0001U, 0U, 1U};
        }
        frame.payload = {payload.data(), payload.size()};

        const auto result = session.onFrame(frame, static_cast<std::uint64_t>(2U + i), scratch, sizeof(scratch));

        if (isTerminal) {
            TEST_ASSERT_EQUAL(static_cast<int>(SerialSession::Session::FrameOutcome::TerminalIn),
                              static_cast<int>(result.outcome));
            ++terminalOutcomes;
        } else {
            // TELEMETRY has no defined inbound meaning on this session (the
            // desktop is not expected to send it to the dongle); it MUST
            // NOT be classified as TerminalIn (CRITERIO 1) or as anything
            // that would route it toward the shell/terminal path.
            TEST_ASSERT_TRUE(result.outcome != SerialSession::Session::FrameOutcome::TerminalIn);
            TEST_ASSERT_TRUE(result.outcome != SerialSession::Session::FrameOutcome::CommandRequest);
            ++telemetryOutcomes;
        }
        TEST_ASSERT_TRUE(session.isProtocolled());
    }

    TEST_ASSERT_EQUAL(kIterations / 2, terminalOutcomes);
    TEST_ASSERT_EQUAL(kIterations / 2, telemetryOutcomes);
}

// ---- topico 19: ShellLineEditor (server-side line editor, shared with the
//      USB console -- lives in the TinyShell package now) -----------------
//
// The behaviour of the editor itself is covered in the TinyShell repo
// (test/test_shell_line_editor.cpp). What matters here is the shape SerialMux
// drives it with: feed(TERMINAL_IN bytes) -> poll() out echo + completed
// lines -> chunk the echoed std::string into TERMINAL_OUT-sized frames.

void test_shell_line_editor_feed_poll_echoes_and_completes_a_line() {
    ShellLineEditor editor;
    std::string out;
    editor.setPrompt("$ ", out);

    const std::string keystrokes = "ls -l\r";
    editor.feed(reinterpret_cast<const std::uint8_t*>(keystrokes.data()), keystrokes.size());

    std::string line;
    std::vector<std::string> lines;
    out.clear();
    while (editor.poll(out, &line)) {
        lines.push_back(line);
    }

    TEST_ASSERT_EQUAL(1U, lines.size());
    TEST_ASSERT_EQUAL_STRING("ls -l", lines[0].c_str());
    // Every typed character was echoed back before the CRLF.
    TEST_ASSERT_TRUE(out.find("ls -l") != std::string::npos);
    TEST_ASSERT_TRUE(out.find("\r\n") != std::string::npos);
}

void test_shell_line_editor_output_chunks_respect_outbound_cap() {
    // Mirrors SerialMux::flushTerminalPtyOutput(): the editor's echoed
    // std::string is chopped into <= kOutboundPayloadCap pieces and their
    // concatenation reproduces the original with no loss or reordering.
    constexpr std::size_t kOutboundPayloadCap = 2048U;

    ShellLineEditor editor;
    std::string out;
    editor.setPrompt("$ ", out);

    // A long response the editor lays out onto the line.
    std::string big;
    big.reserve(5000U);
    for (std::size_t i = 0U; i < 5000U; ++i) {
        big += static_cast<char>('a' + (i % 26U));
    }
    out.clear();
    editor.writeResponse(big, out);
    TEST_ASSERT_TRUE(out.size() > kOutboundPayloadCap);

    std::string reassembled;
    std::size_t offset = 0U;
    while (offset < out.size()) {
        const std::size_t chunkSize = std::min(kOutboundPayloadCap, out.size() - offset);
        TEST_ASSERT_TRUE(chunkSize <= kOutboundPayloadCap);
        reassembled.append(out, offset, chunkSize);
        offset += chunkSize;
    }
    TEST_ASSERT_TRUE(reassembled == out);
}

void test_shell_line_editor_reset_drops_pending_and_input() {
    ShellLineEditor editor;
    std::string out;
    editor.setPrompt("$ ", out);

    const std::string partial = "half a comm";
    editor.feed(reinterpret_cast<const std::uint8_t*>(partial.data()), partial.size());

    editor.reset();  // same discard-pending-work rule SerialMux applies on session teardown

    std::string line;
    out.clear();
    TEST_ASSERT_FALSE(editor.poll(out, &line));
    TEST_ASSERT_EQUAL(0U, out.size());
}

void test_shell_line_editor_history_survives_reset() {
    ShellLineEditor editor;
    std::string out;
    editor.setPrompt("$ ", out);
    editor.addHistory("link -stats");

    editor.reset();

    TEST_ASSERT_EQUAL(1U, editor.historyCount());
    std::string h;
    TEST_ASSERT_TRUE(editor.historyAt(0, h));
    TEST_ASSERT_EQUAL_STRING("link -stats", h.c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_hello_matches_canonical_vector);
    RUN_TEST(test_session_accepts_hello_and_negotiates_minimum_limits);
    RUN_TEST(test_session_rejects_hello_without_common_version);
    RUN_TEST(test_session_hello_deadline_times_out_back_to_console);
    RUN_TEST(test_session_close_returns_to_console);
    RUN_TEST(test_transport_loss_abandons_active_session_once);
    RUN_TEST(test_enter_line_produces_lowercase_ready_line);
    RUN_TEST(test_ready_line_from_nonce_is_16_lowercase_hex_digits);
    RUN_TEST(test_classify_never_mixes_telemetry_and_terminal);
    RUN_TEST(test_payload_with_nul_cr_lf_round_trips_through_cobs_and_decoder);
    RUN_TEST(test_recovers_after_noise_with_no_leading_delimiter);
    RUN_TEST(test_recovers_after_truncated_frame_followed_by_valid_one);
    RUN_TEST(test_consecutive_delimiters_produce_no_empty_frame);
    RUN_TEST(test_stress_interleaved_telemetry_and_terminal_never_cross_classes);
    RUN_TEST(test_shell_line_editor_feed_poll_echoes_and_completes_a_line);
    RUN_TEST(test_shell_line_editor_output_chunks_respect_outbound_cap);
    RUN_TEST(test_shell_line_editor_reset_drops_pending_and_input);
    RUN_TEST(test_shell_line_editor_history_survives_reset);
    return UNITY_END();
}
