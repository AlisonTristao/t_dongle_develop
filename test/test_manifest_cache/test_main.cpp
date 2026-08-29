#include <unity.h>

#include <BtpTransport.h>
#include <ManifestCache.h>
#include <btp/codec.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Plano 36 fase 3.3: fixa o contrato do caminho do catalogo do robo no
// dongle -- ingestManifestData() -> ManifestCache -> buildTargetedResponse().
// Tudo pure C++. O prime em si (EspNowConfig, radio) e coberto por hardware +
// os contadores de `hub -manifest` (fase 0a).
//
// O layout do payload MANIFEST_DATA abaixo e o mesmo que bally_OS
// lib/ManifestResponder/ManifestResponder.cpp::build_manifest_data escreve:
// se uma dessas duas implementacoes andar um octeto, um destes testes quebra.

namespace {

using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint8_t;

constexpr uint32_t kDongleSrc = 0xD0D0D0D0U;
constexpr uint32_t kDongleBoot = 0xB0B0B0B0U;
constexpr uint32_t kRobotSrc = 0x9F442484U;
constexpr uint32_t kRobotBootA = 0x11111111U;
constexpr uint32_t kRobotBootB = 0x22222222U;

void put_u16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFFU));
    b.push_back(static_cast<uint8_t>(v >> 8U));
}
void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8U * i)) & 0xFFU));
}
uint16_t get_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
}

// One record_size-prefixed record: [u32 contentSize][contentSize bytes]. The
// dongle stores records verbatim, so any content of the right length exercises
// walkRecords() exactly like a real topic record does.
std::vector<uint8_t> fake_record(size_t contentLen) {
    std::vector<uint8_t> r;
    put_u32(r, static_cast<uint32_t>(contentLen));
    for (size_t i = 0; i < contentLen; ++i) r.push_back(static_cast<uint8_t>(i & 0xFFU));
    return r;
}

struct BuildOpts {
    uint32_t refSrc = kDongleSrc;      // offset 0: the requester (== dongle src in the real flow)
    uint32_t describedSrc = kRobotSrc;
    uint32_t describedBoot = kRobotBootA;
    uint32_t rev = 5U;
    bool online = true;
    uint8_t status = 0x00U;            // SUCCESS
    uint16_t formatVersion = 1U;
    uint16_t topicCount = 0U;
    std::vector<uint8_t> records{};
    std::string name = "robot";
};

std::vector<uint8_t> build_manifest_data(const BuildOpts& o) {
    std::vector<uint8_t> b;
    put_u32(b, o.refSrc);        // [0]  request source_id
    put_u32(b, 0xABCDEF01U);     // [4]  request boot_id
    put_u32(b, 7U);              // [8]  request sequence
    b.push_back(o.status);       // [12] status
    b.push_back(0x02U);          // [13] flags = CATALOG_COMPLETE
    put_u16(b, 0U);              // [14] error_code
    put_u16(b, o.formatVersion); // [16] format_version
    put_u16(b, 0U);              // [18] reserved
    put_u32(b, o.rev);           // [20] config_revision
    for (int i = 0; i < 16; ++i) b.push_back(0xAAU);  // [24] uuid
    put_u32(b, o.describedSrc);  // [40] described_source_id
    put_u32(b, o.describedBoot); // [44] described_boot_id
    b.push_back(0x01U);          // [48] role = ROBOT
    b.push_back(o.online ? 0x01U : 0x00U);  // [49] source_flags
    put_u16(b, 0U);              // [50] catalog_index
    put_u16(b, 1U);              // [52] catalog_count
    put_u16(b, o.topicCount);    // [54] topic_count
    put_u16(b, 0U);              // [56] action_count
    put_u16(b, static_cast<uint16_t>(o.name.size()));  // [58] name_len
    for (char c : o.name) b.push_back(static_cast<uint8_t>(c));
    for (uint8_t x : o.records) b.push_back(x);
    return b;
}

bool ingest(const std::vector<uint8_t>& payload, uint32_t nowMs = 1000U) {
    return ManifestCache::ingestManifestData({payload.data(), payload.size()}, nowMs);
}

// buildTargetedResponse with a plausible child request header.
size_t serve(uint32_t targetSrc, uint8_t* out, size_t cap, uint32_t knownRev = 0U) {
    btp::Header hdr{};
    hdr.source_id = 0x41770972U;  // a hub-child source_id
    hdr.boot_id = 0x0F0F0F0FU;
    hdr.sequence = 42U;
    return ManifestCache::buildTargetedResponse(targetSrc, /*targetBootId=*/0U, knownRev, hdr, out, cap);
}

}  // namespace

void setUp() {
    ManifestCache::resetForTests();
    BtpTransport::configureIdentity(kDongleSrc, kDongleBoot);
}
void tearDown() {}

// --------------------------------------------------------------------------

void test_ingest_then_serve_round_trips() {
    const std::vector<uint8_t> payload = build_manifest_data(BuildOpts{});
    TEST_ASSERT_TRUE(ingest(payload));

    ManifestCache::Diagnostics d = ManifestCache::diagnostics();
    TEST_ASSERT_EQUAL_UINT32(1U, d.ingestedOk);
    TEST_ASSERT_EQUAL_UINT32(0U, d.ingestFailed);
    TEST_ASSERT_EQUAL_size_t(1U, d.entryCount);
    TEST_ASSERT_EQUAL_HEX32(kRobotSrc, d.entries[0].sourceId);
    TEST_ASSERT_EQUAL_HEX32(kRobotBootA, d.entries[0].bootId);
    TEST_ASSERT_EQUAL_UINT32(5U, d.entries[0].configRevision);
    TEST_ASSERT_TRUE(d.entries[0].online);

    uint8_t out[512] = {0};
    const size_t n = serve(kRobotSrc, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(60U, n);
    TEST_ASSERT_EQUAL_UINT8(0x00U, out[12]);            // status SUCCESS
    TEST_ASSERT_EQUAL_UINT16(1U, get_u16(out + 16));    // format_version
    TEST_ASSERT_EQUAL_HEX32(kRobotSrc, get_u32(out + 40));
    TEST_ASSERT_EQUAL_HEX32(kRobotBootA, get_u32(out + 44));

    d = ManifestCache::diagnostics();
    TEST_ASSERT_EQUAL_UINT32(1U, d.targetedRequestsRx);
    TEST_ASSERT_EQUAL_UINT32(1U, d.targetedServedHit);
    TEST_ASSERT_EQUAL_UINT32(0U, d.targetedServedMiss);
}

void test_topic_records_survive_ingest_and_serve() {
    BuildOpts o;
    o.topicCount = 1U;
    o.records = fake_record(20U);
    const std::vector<uint8_t> payload = build_manifest_data(o);
    TEST_ASSERT_TRUE(ingest(payload));

    ManifestCache::Diagnostics d = ManifestCache::diagnostics();
    TEST_ASSERT_EQUAL_size_t(1U, d.entryCount);
    TEST_ASSERT_EQUAL_UINT16(1U, d.entries[0].topicCount);
    TEST_ASSERT_EQUAL_UINT16(24U, d.entries[0].recordsSize);  // 4-byte prefix + 20

    uint8_t out[512] = {0};
    const size_t n = serve(kRobotSrc, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(60U, n);
    TEST_ASSERT_EQUAL_UINT16(1U, get_u16(out + 54));  // topic_count echoed back
}

void test_unknown_source_is_served_as_miss() {
    uint8_t out[256] = {0};
    const size_t n = serve(0xDEADBEEFU, out, sizeof(out));  // never cached, not self
    TEST_ASSERT_GREATER_THAN(0U, n);                        // NOT_FOUND is still a valid frame
    TEST_ASSERT_EQUAL_UINT8(0x01U, out[12]);                // status REJECTED

    const ManifestCache::Diagnostics d = ManifestCache::diagnostics();
    TEST_ASSERT_EQUAL_UINT32(1U, d.targetedRequestsRx);
    TEST_ASSERT_EQUAL_UINT32(0U, d.targetedServedHit);
    TEST_ASSERT_EQUAL_UINT32(1U, d.targetedServedMiss);
}

void test_ingest_rejects_malformed() {
    // too short
    uint8_t tiny[40] = {0};
    TEST_ASSERT_FALSE(ManifestCache::ingestManifestData({tiny, sizeof(tiny)}, 1000U));

    // wrong format_version
    BuildOpts bad_ver;
    bad_ver.formatVersion = 99U;
    TEST_ASSERT_FALSE(ingest(build_manifest_data(bad_ver)));

    // described_source_id == 0
    BuildOpts zero_src;
    zero_src.describedSrc = 0U;
    TEST_ASSERT_FALSE(ingest(build_manifest_data(zero_src)));

    // topic_count says 1 but no record bytes follow -> walkRecords fails
    BuildOpts framing;
    framing.topicCount = 1U;  // records left empty
    TEST_ASSERT_FALSE(ingest(build_manifest_data(framing)));

    // non-SUCCESS status is ignored (not cached)
    BuildOpts rejected;
    rejected.status = 0x01U;
    TEST_ASSERT_FALSE(ingest(build_manifest_data(rejected)));

    const ManifestCache::Diagnostics d = ManifestCache::diagnostics();
    TEST_ASSERT_EQUAL_UINT32(0U, d.ingestedOk);
    TEST_ASSERT_EQUAL_UINT32(5U, d.ingestFailed);
    TEST_ASSERT_EQUAL_size_t(0U, d.entryCount);
}

void test_boot_change_replaces_the_entry() {
    BuildOpts a;
    a.describedBoot = kRobotBootA;
    a.rev = 1U;
    TEST_ASSERT_TRUE(ingest(build_manifest_data(a)));

    BuildOpts b;
    b.describedBoot = kRobotBootB;
    b.rev = 1U;
    TEST_ASSERT_TRUE(ingest(build_manifest_data(b)));

    const ManifestCache::Diagnostics d = ManifestCache::diagnostics();
    TEST_ASSERT_EQUAL_size_t(1U, d.entryCount);  // same slot, not a second one
    TEST_ASSERT_EQUAL_HEX32(kRobotBootB, d.entries[0].bootId);

    uint8_t out[512] = {0};
    TEST_ASSERT_GREATER_THAN(60U, serve(kRobotSrc, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX32(kRobotBootB, get_u32(out + 44));
}

void test_shouldRequest_gates_on_cached_boot() {
    // never cached -> ask
    TEST_ASSERT_TRUE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootA, 0U));
    // asked just now, still pending -> do not re-ask within the cooldown
    TEST_ASSERT_FALSE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootA, 100U));

    TEST_ASSERT_TRUE(ingest(build_manifest_data(BuildOpts{}), 200U));

    // have this exact boot -> never ask again for it
    TEST_ASSERT_FALSE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootA, 100000U));
    // peer rebooted -> ask for the new boot
    TEST_ASSERT_TRUE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootB, 200000U));
}

void test_shouldRequest_fast_then_steady_cadence() {
    // Attempt 1 at t=0.
    TEST_ASSERT_TRUE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootA, 0U));
    // Within the 1s fast cooldown -> no.
    TEST_ASSERT_FALSE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootA, 999U));
    // 1s later -> attempt 2 (fast phase).
    TEST_ASSERT_TRUE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootA, 1000U));
    // attempt 3
    TEST_ASSERT_TRUE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootA, 2000U));
    // attempt 4 -- still lets the boundary through, then the cadence widens
    TEST_ASSERT_TRUE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootA, 3000U));

    // Now past kFastRequestAttempts: 1s is no longer enough, 3s is.
    TEST_ASSERT_FALSE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootA, 4000U));
    TEST_ASSERT_FALSE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootA, 5999U));
    TEST_ASSERT_TRUE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootA, 6000U));

    const ManifestCache::Diagnostics d = ManifestCache::diagnostics();
    TEST_ASSERT_EQUAL_size_t(1U, d.pendingCount);
    TEST_ASSERT_EQUAL_HEX32(kRobotSrc, d.pending[0].sourceId);
    TEST_ASSERT_EQUAL_UINT32(5U, d.pending[0].attempts);
}

void test_shouldRequest_attempts_do_not_reset_on_boot_change() {
    // A robot that ignores every request: attempts accumulate.
    for (uint32_t t = 0U; t <= 20000U; t += 4000U) {
        ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootA, t);
    }
    ManifestCache::Diagnostics d = ManifestCache::diagnostics();
    TEST_ASSERT_EQUAL_size_t(1U, d.pendingCount);
    const uint32_t before = d.pending[0].attempts;
    TEST_ASSERT_GREATER_THAN_UINT32(ManifestCache::kFastRequestAttempts, before);

    // Its boot_id flaps (a robot bug) -- this must NOT drop back to the fast
    // 1s cadence, or the dongle would poll it every second forever. The
    // attempt count keeps climbing and the slow cadence holds.
    TEST_ASSERT_FALSE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootB, 21000U));  // <3s
    TEST_ASSERT_TRUE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootB, 23000U));   // 3s ok
    d = ManifestCache::diagnostics();
    TEST_ASSERT_EQUAL_HEX32(kRobotBootB, d.pending[0].bootId);
    TEST_ASSERT_EQUAL_UINT32(before + 1U, d.pending[0].attempts);
}

void test_successful_ingest_clears_the_pending_chase_and_next_chase_is_fast_again() {
    // Wear the source down to the slow cadence.
    for (uint32_t t = 0U; t <= 20000U; t += 4000U) {
        ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootA, t);
    }
    TEST_ASSERT_GREATER_THAN_UINT32(1U, ManifestCache::diagnostics().pending[0].attempts);

    // The robot finally answers: pending cleared.
    TEST_ASSERT_TRUE(ingest(build_manifest_data(BuildOpts{}), 21000U));
    TEST_ASSERT_EQUAL_size_t(0U, ManifestCache::diagnostics().pendingCount);

    // It reboots later -> a brand-new chase, fast cadence again (attempts
    // start from 1, so 1s is enough).
    BuildOpts rebooted;
    rebooted.describedBoot = kRobotBootB;
    TEST_ASSERT_TRUE(ingest(build_manifest_data(rebooted), 30000U));  // cache the new boot to clear entry-match
    // (entry now on bootB; ask about a THIRD boot to exercise a fresh chase)
    const uint32_t kRobotBootC = 0x33333333U;
    TEST_ASSERT_TRUE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootC, 40000U));
    ManifestCache::Diagnostics d = ManifestCache::diagnostics();
    TEST_ASSERT_EQUAL_UINT32(1U, d.pending[0].attempts);
    TEST_ASSERT_TRUE(ManifestCache::shouldRequestManifest(kRobotSrc, kRobotBootC, 41000U));  // 1s ok
}

void test_external_counters() {
    ManifestCache::notePrimeSent();
    ManifestCache::notePrimeSent();
    ManifestCache::notePrimeSent();
    ManifestCache::noteConsumeRejected();

    const ManifestCache::Diagnostics d = ManifestCache::diagnostics();
    TEST_ASSERT_EQUAL_UINT32(3U, d.primeRequestsSent);
    TEST_ASSERT_EQUAL_UINT32(1U, d.consumeRejected);
}

void test_reference_source_id_is_not_checked_here() {
    // ingestManifestData() itself does NOT validate offset 0 -- the
    // reference_source_id == self_source_id gate lives upstream in
    // bally::dongle_consumes (EspNowConfig). A payload whose offset-0 names
    // someone else still parses fine here; the guard is elsewhere on purpose.
    BuildOpts o;
    o.refSrc = 0x12345678U;  // not the dongle
    TEST_ASSERT_TRUE(ingest(build_manifest_data(o)));
    TEST_ASSERT_EQUAL_size_t(1U, ManifestCache::diagnostics().entryCount);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ingest_then_serve_round_trips);
    RUN_TEST(test_topic_records_survive_ingest_and_serve);
    RUN_TEST(test_unknown_source_is_served_as_miss);
    RUN_TEST(test_ingest_rejects_malformed);
    RUN_TEST(test_boot_change_replaces_the_entry);
    RUN_TEST(test_shouldRequest_gates_on_cached_boot);
    RUN_TEST(test_shouldRequest_fast_then_steady_cadence);
    RUN_TEST(test_shouldRequest_attempts_do_not_reset_on_boot_change);
    RUN_TEST(test_successful_ingest_clears_the_pending_chase_and_next_chase_is_fast_again);
    RUN_TEST(test_external_counters);
    RUN_TEST(test_reference_source_id_is_not_checked_here);
    return UNITY_END();
}
