#include <unity.h>

#include <SerialSession.h>
#include <SubscriptionRegistry.h>

#include <cstddef>
#include <cstdint>

// Exercises topico 17 (bally_protocol/topicos/17_assinaturas_controle_taxa.txt)
// on the dongle side: SubscriptionRegistry's aggregation of several desktop
// clients into a single upstream SUBSCRIBE per (source_id, topic_id), and
// SerialSession::buildStatusV2's status_version=2 serialization
// (COMMANDS_AND_ACTIONS.md section 8.1).
//
// Both modules are pure C++, so this suite runs under env:native exactly like
// test_protocol_router/test_serial_session. The ESP-NOW/Serial glue that sends
// the frames these decisions produce (EspNowConfig::requestUpstreamSubscribe,
// SerialMux::dispatchUpstreamAction) is Arduino/FreeRTOS code and is covered by
// pio run -e tdongle-s3 plus hardware verification, not here.
//
// Coverage maps to the topico's CRITERIOS DE ACEITE:
//   - "fechar um grafico reduz trafego quando nenhum outro consumidor usa o
//     topico" -> the unsubscribe/last-consumer and session-end tests;
//   - "pedido acima do maximo e limitado e informado ao cliente" -> the clamp
//     itself lives in SerialMux (ManifestCache max_rate_millihz, Arduino side);
//     what is testable here is that a second, slower client can never lower or
//     raise what the union already decided.

namespace {

using std::uint8_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;

// Two distinct desktop sessions. SerialMux uses the HELLO source_id as the
// opaque clientId, so any two non-equal values model two clients.
constexpr uint32_t kClientA = 0xAAAA0001U;
constexpr uint32_t kClientB = 0xBBBB0002U;

constexpr uint32_t kRobotOne = 0x11111111U;
constexpr uint32_t kRobotTwo = 0x22222222U;

constexpr uint32_t kTopicSeven = 7U;
constexpr uint32_t kTopicNine = 9U;

constexpr uint32_t kLeaseMs = 10000U;

uint16_t read_u16(const uint8_t* data) {
    return static_cast<uint16_t>(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8U));
}

uint32_t read_u32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
           (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

uint64_t read_u64(const uint8_t* data) {
    uint64_t value = 0U;
    for (std::size_t i = 0U; i < 8U; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (8U * i);
    }
    return value;
}

// Finds a snapshot row by the (source_id, topic_id) pair the spec requires as
// the disambiguating key; returns -1 when absent.
int find_snapshot(const SubscriptionRegistry::TopicStatusEntry* entries, std::size_t count, uint32_t sourceId,
                  uint16_t topicId) {
    for (std::size_t i = 0U; i < count; ++i) {
        if (entries[i].sourceId == sourceId && entries[i].topicId == topicId) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// Aggregation across clients
// ---------------------------------------------------------------------------

void test_two_clients_on_same_topic_produce_one_row_at_the_union_rate() {
    SubscriptionRegistry::resetForTests();

    const auto first =
        SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotOne, kTopicSeven, 1000U, kLeaseMs, 1000U);
    TEST_ASSERT_TRUE(first.accepted);
    TEST_ASSERT_EQUAL(static_cast<int>(SubscriptionRegistry::UpstreamKind::Subscribe),
                      static_cast<int>(first.upstream.kind));
    TEST_ASSERT_EQUAL_UINT32(1000U, first.upstream.rateMillihz);
    TEST_ASSERT_EQUAL_UINT32(kLeaseMs, first.upstream.leaseMs);
    TEST_ASSERT_EQUAL_UINT32(kRobotOne, first.upstream.sourceId);
    TEST_ASSERT_EQUAL_UINT32(kTopicSeven, first.upstream.topicId);

    // Second client, same topic, faster: the robot must be re-asked at the
    // higher union rate -- once, not once per client.
    const auto second =
        SubscriptionRegistry::onDesktopSubscribe(kClientB, kRobotOne, kTopicSeven, 5000U, kLeaseMs, 1000U);
    TEST_ASSERT_TRUE(second.accepted);
    TEST_ASSERT_EQUAL(static_cast<int>(SubscriptionRegistry::UpstreamKind::Subscribe),
                      static_cast<int>(second.upstream.kind));
    TEST_ASSERT_EQUAL_UINT32(5000U, second.upstream.rateMillihz);
    TEST_ASSERT_TRUE(second.subscriptionId != first.subscriptionId);

    // One aggregated row, two subscribers.
    SubscriptionRegistry::TopicStatusEntry entries[SubscriptionRegistry::kMaxTopics];
    const std::size_t count = SubscriptionRegistry::topicStatusSnapshot(entries, SubscriptionRegistry::kMaxTopics);
    TEST_ASSERT_EQUAL(1U, count);
    TEST_ASSERT_EQUAL_UINT16(2U, entries[0].subscriberCount);
    TEST_ASSERT_TRUE(SubscriptionRegistry::isWanted(kRobotOne, kTopicSeven));
}

void test_slower_second_client_does_not_change_what_the_robot_was_asked_for() {
    SubscriptionRegistry::resetForTests();

    (void)SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotOne, kTopicSeven, 5000U, kLeaseMs, 1000U);
    const auto slower =
        SubscriptionRegistry::onDesktopSubscribe(kClientB, kRobotOne, kTopicSeven, 1000U, kLeaseMs, 1000U);

    TEST_ASSERT_TRUE(slower.accepted);
    // Accepted locally, but nothing to tell the robot: it is already
    // publishing at 5000 mHz, which more than satisfies this client.
    TEST_ASSERT_EQUAL(static_cast<int>(SubscriptionRegistry::UpstreamKind::None),
                      static_cast<int>(slower.upstream.kind));
}

void test_rate_falls_back_to_remaining_client_when_the_fastest_one_leaves() {
    SubscriptionRegistry::resetForTests();

    (void)SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotOne, kTopicSeven, 1000U, kLeaseMs, 1000U);
    const auto fast =
        SubscriptionRegistry::onDesktopSubscribe(kClientB, kRobotOne, kTopicSeven, 5000U, kLeaseMs, 1000U);

    const auto removed = SubscriptionRegistry::onDesktopUnsubscribe(kClientB, fast.subscriptionId, 2000U);

    TEST_ASSERT_TRUE(removed.found);
    // A consumer remains, so this is a rate *downgrade*, never an unsubscribe.
    TEST_ASSERT_EQUAL(static_cast<int>(SubscriptionRegistry::UpstreamKind::Subscribe),
                      static_cast<int>(removed.upstream.kind));
    TEST_ASSERT_EQUAL_UINT32(1000U, removed.upstream.rateMillihz);
    TEST_ASSERT_TRUE(SubscriptionRegistry::isWanted(kRobotOne, kTopicSeven));
}

void test_unsubscribe_reaches_the_robot_only_for_the_last_consumer() {
    SubscriptionRegistry::resetForTests();

    const auto slow =
        SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotOne, kTopicSeven, 1000U, kLeaseMs, 1000U);
    const auto fast =
        SubscriptionRegistry::onDesktopSubscribe(kClientB, kRobotOne, kTopicSeven, 5000U, kLeaseMs, 1000U);
    SubscriptionRegistry::noteUpstreamRequestSent(kRobotOne, kTopicSeven, 42U);
    TEST_ASSERT_TRUE(SubscriptionRegistry::onUpstreamSubscribeResult(42U, 0x00U, 777U, 5000U));

    // The slower client leaving changes nothing at all upstream: the union is
    // still 5000 mHz and someone still wants the topic.
    const auto firstOut = SubscriptionRegistry::onDesktopUnsubscribe(kClientA, slow.subscriptionId, 2000U);
    TEST_ASSERT_TRUE(firstOut.found);
    TEST_ASSERT_EQUAL(static_cast<int>(SubscriptionRegistry::UpstreamKind::None),
                      static_cast<int>(firstOut.upstream.kind));
    TEST_ASSERT_TRUE(SubscriptionRegistry::isWanted(kRobotOne, kTopicSeven));

    // Last consumer gone -> now, and only now, release the robot's grant.
    const auto lastOut = SubscriptionRegistry::onDesktopUnsubscribe(kClientB, fast.subscriptionId, 2000U);
    TEST_ASSERT_TRUE(lastOut.found);
    TEST_ASSERT_EQUAL(static_cast<int>(SubscriptionRegistry::UpstreamKind::Unsubscribe),
                      static_cast<int>(lastOut.upstream.kind));
    TEST_ASSERT_EQUAL_UINT32(777U, lastOut.upstream.upstreamSubscriptionId);
    TEST_ASSERT_FALSE(SubscriptionRegistry::isWanted(kRobotOne, kTopicSeven));
}

void test_unsubscribing_an_absent_subscription_is_a_silent_no_op() {
    SubscriptionRegistry::resetForTests();

    const auto outcome = SubscriptionRegistry::onDesktopUnsubscribe(kClientA, 999U, 1000U);

    // COMMANDS_AND_ACTIONS.md section 7: retries are idempotent. The caller
    // still answers SUCCESS/NONE; nothing goes upstream.
    TEST_ASSERT_FALSE(outcome.found);
    TEST_ASSERT_EQUAL(static_cast<int>(SubscriptionRegistry::UpstreamKind::None),
                      static_cast<int>(outcome.upstream.kind));
}

void test_repeating_an_identical_subscribe_reuses_the_subscription_id() {
    SubscriptionRegistry::resetForTests();

    const auto first =
        SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotOne, kTopicSeven, 2000U, kLeaseMs, 1000U);
    const auto retry =
        SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotOne, kTopicSeven, 2000U, kLeaseMs, 1500U);

    TEST_ASSERT_TRUE(retry.accepted);
    TEST_ASSERT_EQUAL_UINT32(first.subscriptionId, retry.subscriptionId);
    // Well inside the renewal window (half of 10 s), so the retry costs no
    // ESP-NOW traffic at all.
    TEST_ASSERT_EQUAL(static_cast<int>(SubscriptionRegistry::UpstreamKind::None),
                      static_cast<int>(retry.upstream.kind));

    SubscriptionRegistry::TopicStatusEntry entries[SubscriptionRegistry::kMaxTopics];
    const std::size_t count = SubscriptionRegistry::topicStatusSnapshot(entries, SubscriptionRegistry::kMaxTopics);
    TEST_ASSERT_EQUAL(1U, count);
    TEST_ASSERT_EQUAL_UINT16(1U, entries[0].subscriberCount);  // renewed, not duplicated
}

// ---------------------------------------------------------------------------
// Session lifetime
// ---------------------------------------------------------------------------

void test_session_end_drops_every_subscription_that_client_owned() {
    SubscriptionRegistry::resetForTests();

    (void)SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotOne, kTopicSeven, 5000U, kLeaseMs, 1000U);
    (void)SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotOne, kTopicNine, 4000U, kLeaseMs, 1000U);
    (void)SubscriptionRegistry::onDesktopSubscribe(kClientB, kRobotOne, kTopicSeven, 1000U, kLeaseMs, 1000U);
    SubscriptionRegistry::noteUpstreamRequestSent(kRobotOne, kTopicNine, 51U);
    TEST_ASSERT_TRUE(SubscriptionRegistry::onUpstreamSubscribeResult(51U, 0x00U, 900U, 4000U));

    SubscriptionRegistry::UpstreamAction actions[SubscriptionRegistry::kMaxTopics];
    const std::size_t count =
        SubscriptionRegistry::onClientDisconnected(kClientA, 2000U, actions, SubscriptionRegistry::kMaxTopics);

    // Topic 9 had no other consumer -> released. Topic 7 still has client B ->
    // kept, but downgraded from 5000 to 1000 mHz.
    TEST_ASSERT_EQUAL(2U, count);
    int sawTopicSevenSubscribe = 0;
    int sawTopicNineUnsubscribe = 0;
    for (std::size_t i = 0U; i < count; ++i) {
        if (actions[i].topicId == kTopicSeven &&
            actions[i].kind == SubscriptionRegistry::UpstreamKind::Subscribe && actions[i].rateMillihz == 1000U) {
            ++sawTopicSevenSubscribe;
        }
        if (actions[i].topicId == kTopicNine &&
            actions[i].kind == SubscriptionRegistry::UpstreamKind::Unsubscribe &&
            actions[i].upstreamSubscriptionId == 900U) {
            ++sawTopicNineUnsubscribe;
        }
    }
    TEST_ASSERT_EQUAL(1, sawTopicSevenSubscribe);
    TEST_ASSERT_EQUAL(1, sawTopicNineUnsubscribe);

    TEST_ASSERT_TRUE(SubscriptionRegistry::isWanted(kRobotOne, kTopicSeven));
    TEST_ASSERT_FALSE(SubscriptionRegistry::isWanted(kRobotOne, kTopicNine));

    // A second disconnect of the same (already gone) client is inert.
    TEST_ASSERT_EQUAL(
        0U, SubscriptionRegistry::onClientDisconnected(kClientA, 2100U, actions, SubscriptionRegistry::kMaxTopics));
}

void test_expired_lease_releases_the_topic_like_an_unsubscribe() {
    SubscriptionRegistry::resetForTests();

    const uint32_t startMs = 1000U;
    (void)SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotOne, kTopicSeven, 3000U, kLeaseMs, startMs);
    SubscriptionRegistry::noteUpstreamRequestSent(kRobotOne, kTopicSeven, 7U);
    TEST_ASSERT_TRUE(SubscriptionRegistry::onUpstreamSubscribeResult(7U, 0x00U, 1234U, 3000U));

    SubscriptionRegistry::UpstreamAction actions[SubscriptionRegistry::kMaxTopics];

    // One millisecond before the lease deadline the topic is still wanted, so
    // whatever the sweep emits (an upstream renewal is due by then) can never
    // be an Unsubscribe.
    const std::size_t beforeDeadline =
        SubscriptionRegistry::sweep(startMs + kLeaseMs - 1U, actions, SubscriptionRegistry::kMaxTopics);
    for (std::size_t i = 0U; i < beforeDeadline; ++i) {
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(SubscriptionRegistry::UpstreamKind::Unsubscribe),
                              static_cast<int>(actions[i].kind));
    }
    TEST_ASSERT_TRUE(SubscriptionRegistry::isWanted(kRobotOne, kTopicSeven));

    const std::size_t count =
        SubscriptionRegistry::sweep(startMs + kLeaseMs, actions, SubscriptionRegistry::kMaxTopics);
    TEST_ASSERT_EQUAL(1U, count);
    TEST_ASSERT_EQUAL(static_cast<int>(SubscriptionRegistry::UpstreamKind::Unsubscribe),
                      static_cast<int>(actions[0].kind));
    TEST_ASSERT_EQUAL_UINT32(1234U, actions[0].upstreamSubscriptionId);
    TEST_ASSERT_FALSE(SubscriptionRegistry::isWanted(kRobotOne, kTopicSeven));
}

void test_upstream_lease_is_renewed_before_the_robot_would_drop_it() {
    SubscriptionRegistry::resetForTests();

    const uint32_t startMs = 1000U;
    (void)SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotOne, kTopicSeven, 3000U, kLeaseMs, startMs);

    SubscriptionRegistry::UpstreamAction actions[SubscriptionRegistry::kMaxTopics];

    // Just before half the lease has elapsed: nothing to say.
    TEST_ASSERT_EQUAL(0U, SubscriptionRegistry::sweep(startMs + (kLeaseMs / 2U) - 1U, actions,
                                                      SubscriptionRegistry::kMaxTopics));

    // At the halfway mark the SUBSCRIBE is re-asserted, at the same rate, so
    // the robot's own lease never lapses under a client that is still there.
    const std::size_t count =
        SubscriptionRegistry::sweep(startMs + (kLeaseMs / 2U), actions, SubscriptionRegistry::kMaxTopics);
    TEST_ASSERT_EQUAL(1U, count);
    TEST_ASSERT_EQUAL(static_cast<int>(SubscriptionRegistry::UpstreamKind::Subscribe),
                      static_cast<int>(actions[0].kind));
    TEST_ASSERT_EQUAL_UINT32(3000U, actions[0].rateMillihz);

    // And the renewal re-arms rather than firing every tick afterwards.
    TEST_ASSERT_EQUAL(0U, SubscriptionRegistry::sweep(startMs + (kLeaseMs / 2U) + 1U, actions,
                                                      SubscriptionRegistry::kMaxTopics));
}

// ---------------------------------------------------------------------------
// Counters and STATUS v2
// ---------------------------------------------------------------------------

void test_counters_are_disambiguated_by_source_and_topic() {
    SubscriptionRegistry::resetForTests();

    // Same topic_id published by two different robots: topic_id alone is not
    // globally unique (COMMANDS_AND_ACTIONS.md section 8.1).
    (void)SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotOne, kTopicSeven, 1000U, kLeaseMs, 1000U);
    (void)SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotTwo, kTopicSeven, 1000U, kLeaseMs, 1000U);

    SubscriptionRegistry::recordForwarded(kRobotOne, kTopicSeven, 100U);
    SubscriptionRegistry::recordForwarded(kRobotOne, kTopicSeven, 40U);
    SubscriptionRegistry::recordDropped(kRobotOne, kTopicSeven);
    SubscriptionRegistry::recordForwarded(kRobotTwo, kTopicSeven, 7U);

    SubscriptionRegistry::TopicStatusEntry entries[SubscriptionRegistry::kMaxTopics];
    const std::size_t count = SubscriptionRegistry::topicStatusSnapshot(entries, SubscriptionRegistry::kMaxTopics);
    TEST_ASSERT_EQUAL(2U, count);

    const int one = find_snapshot(entries, count, kRobotOne, static_cast<uint16_t>(kTopicSeven));
    const int two = find_snapshot(entries, count, kRobotTwo, static_cast<uint16_t>(kTopicSeven));
    TEST_ASSERT_TRUE(one >= 0);
    TEST_ASSERT_TRUE(two >= 0);
    TEST_ASSERT_EQUAL_UINT64(140U, entries[one].bytesTotal);
    TEST_ASSERT_EQUAL_UINT64(1U, entries[one].samplesDroppedTotal);
    TEST_ASSERT_EQUAL_UINT64(7U, entries[two].bytesTotal);
    TEST_ASSERT_EQUAL_UINT64(0U, entries[two].samplesDroppedTotal);
}

void test_counters_survive_the_topic_losing_all_subscribers() {
    SubscriptionRegistry::resetForTests();

    const auto sub =
        SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotOne, kTopicSeven, 1000U, kLeaseMs, 1000U);
    SubscriptionRegistry::recordForwarded(kRobotOne, kTopicSeven, 250U);
    (void)SubscriptionRegistry::onDesktopUnsubscribe(kClientA, sub.subscriptionId, 2000U);

    SubscriptionRegistry::TopicStatusEntry entries[SubscriptionRegistry::kMaxTopics];
    const std::size_t count = SubscriptionRegistry::topicStatusSnapshot(entries, SubscriptionRegistry::kMaxTopics);

    // Section 8.1: bytes_total is monotonic since the emitter's boot, and an
    // idle topic reports effective_rate_millihz = 0 ("nao esta sendo publicado
    // agora") rather than vanishing from the list.
    TEST_ASSERT_EQUAL(1U, count);
    TEST_ASSERT_EQUAL_UINT64(250U, entries[0].bytesTotal);
    TEST_ASSERT_EQUAL_UINT16(0U, entries[0].subscriberCount);
    TEST_ASSERT_EQUAL_UINT32(0U, entries[0].effectiveRateMillihz);
}

void test_status_v1_payload_is_untouched_by_this_topico() {
    SerialSession::StatusCounters counters{};
    counters.uptimeUs = 5U;
    counters.framesRx = 6U;

    uint8_t payload[SerialSession::kStatusPayloadSize];
    const std::size_t size = SerialSession::buildStatus(counters, payload, sizeof(payload));

    TEST_ASSERT_EQUAL(SerialSession::kStatusPayloadSize, size);
    TEST_ASSERT_EQUAL_UINT16(1U, read_u16(payload));  // status_version stays 1
}

void test_status_v2_serializes_one_fixed_record_per_topic() {
    SerialSession::StatusCounters counters{};
    counters.uptimeUs = 123456U;
    counters.framesRx = 10U;
    counters.framesTx = 11U;
    counters.framesDropped = 12U;
    counters.telemetryDropped = 13U;

    SerialSession::TopicStatusRecord records[2];
    records[0].sourceId = kRobotOne;
    records[0].topicId = 7U;
    records[0].subscriberCount = 2U;
    records[0].effectiveRateMillihz = 50000U;
    records[0].bytesTotal = 0x0102030405060708ULL;
    records[0].samplesDroppedTotal = 9U;
    records[1].sourceId = kRobotTwo;
    records[1].topicId = 9U;
    records[1].subscriberCount = 1U;
    records[1].effectiveRateMillihz = 1000U;
    records[1].bytesTotal = 4U;
    records[1].samplesDroppedTotal = 0U;

    uint8_t payload[SerialSession::kStatusPayloadSize + 2U + 2U * SerialSession::kTopicStatusRecordSize];
    const std::size_t size = SerialSession::buildStatusV2(counters, records, 2U, payload, sizeof(payload));

    // 92-byte v1 block + uint16 count + 2 fixed-size records, with no
    // record_size prefix of its own (section 8.1).
    TEST_ASSERT_EQUAL(SerialSession::kStatusPayloadSize + 2U + 2U * SerialSession::kTopicStatusRecordSize, size);
    TEST_ASSERT_EQUAL_UINT16(2U, read_u16(payload));  // status_version bumped to 2
    TEST_ASSERT_EQUAL_UINT64(123456U, read_u64(payload + 4U));   // v1 fields keep their offsets
    TEST_ASSERT_EQUAL_UINT64(13U, read_u64(payload + 84U));      // telemetry_dropped, last v1 field
    TEST_ASSERT_EQUAL_UINT16(2U, read_u16(payload + 92U));       // topic_status_count

    const uint8_t* first = payload + 94U;
    TEST_ASSERT_EQUAL_UINT32(kRobotOne, read_u32(first));
    TEST_ASSERT_EQUAL_UINT16(7U, read_u16(first + 4U));
    TEST_ASSERT_EQUAL_UINT16(2U, read_u16(first + 6U));
    TEST_ASSERT_EQUAL_UINT32(50000U, read_u32(first + 8U));
    TEST_ASSERT_EQUAL_UINT64(0x0102030405060708ULL, read_u64(first + 12U));
    TEST_ASSERT_EQUAL_UINT64(9U, read_u64(first + 20U));

    // The second record must start exactly one stride later -- the original
    // draft wrote 28 bytes per record while advancing 24, silently corrupting
    // every record after the first.
    const uint8_t* second = first + SerialSession::kTopicStatusRecordSize;
    TEST_ASSERT_EQUAL_UINT32(kRobotTwo, read_u32(second));
    TEST_ASSERT_EQUAL_UINT16(9U, read_u16(second + 4U));
    TEST_ASSERT_EQUAL_UINT16(1U, read_u16(second + 6U));
    TEST_ASSERT_EQUAL_UINT32(1000U, read_u32(second + 8U));
    TEST_ASSERT_EQUAL_UINT64(4U, read_u64(second + 12U));
    TEST_ASSERT_EQUAL_UINT64(0U, read_u64(second + 20U));
}

void test_status_v2_refuses_to_overrun_a_short_buffer() {
    SerialSession::StatusCounters counters{};
    SerialSession::TopicStatusRecord record{};
    record.sourceId = kRobotOne;
    record.topicId = 7U;

    // One byte short of the exact requirement: the caller must fall back to the
    // v1 payload rather than get a truncated/overrun v2 one.
    uint8_t payload[SerialSession::kStatusPayloadSize + 2U + SerialSession::kTopicStatusRecordSize];
    const std::size_t size = SerialSession::buildStatusV2(counters, &record, 1U, payload, sizeof(payload) - 1U);

    TEST_ASSERT_EQUAL(0U, size);
}

void test_status_v2_with_no_topics_is_just_the_count_field() {
    SerialSession::StatusCounters counters{};

    uint8_t payload[SerialSession::kStatusPayloadSize + 2U];
    const std::size_t size = SerialSession::buildStatusV2(counters, nullptr, 0U, payload, sizeof(payload));

    // Section 8.1 explicitly allows topic_status_count = 0.
    TEST_ASSERT_EQUAL(SerialSession::kStatusPayloadSize + 2U, size);
    TEST_ASSERT_EQUAL_UINT16(2U, read_u16(payload));
    TEST_ASSERT_EQUAL_UINT16(0U, read_u16(payload + 92U));
}

// The registry snapshot feeds buildStatusV2 through a field-by-field copy in
// SerialMux; this pins that the two structs really do line up.
void test_registry_snapshot_feeds_status_v2_end_to_end() {
    SubscriptionRegistry::resetForTests();

    (void)SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotOne, kTopicSeven, 2000U, kLeaseMs, 1000U);
    (void)SubscriptionRegistry::onDesktopSubscribe(kClientB, kRobotOne, kTopicSeven, 2000U, kLeaseMs, 1000U);
    SubscriptionRegistry::noteUpstreamRequestSent(kRobotOne, kTopicSeven, 5U);
    TEST_ASSERT_TRUE(SubscriptionRegistry::onUpstreamSubscribeResult(5U, 0x00U, 3141U, 1500U));
    SubscriptionRegistry::recordForwarded(kRobotOne, kTopicSeven, 64U);
    SubscriptionRegistry::recordDropped(kRobotOne, kTopicSeven);

    SubscriptionRegistry::TopicStatusEntry entries[SubscriptionRegistry::kMaxTopics];
    const std::size_t count = SubscriptionRegistry::topicStatusSnapshot(entries, SubscriptionRegistry::kMaxTopics);
    TEST_ASSERT_EQUAL(1U, count);

    SerialSession::TopicStatusRecord records[SubscriptionRegistry::kMaxTopics];
    for (std::size_t i = 0U; i < count; ++i) {
        records[i].sourceId = entries[i].sourceId;
        records[i].topicId = entries[i].topicId;
        records[i].subscriberCount = entries[i].subscriberCount;
        records[i].effectiveRateMillihz = entries[i].effectiveRateMillihz;
        records[i].bytesTotal = entries[i].bytesTotal;
        records[i].samplesDroppedTotal = entries[i].samplesDroppedTotal;
    }

    SerialSession::StatusCounters counters{};
    uint8_t payload[SerialSession::kStatusPayloadSize + 2U +
                    SubscriptionRegistry::kMaxTopics * SerialSession::kTopicStatusRecordSize];
    const std::size_t size = SerialSession::buildStatusV2(counters, records, count, payload, sizeof(payload));
    TEST_ASSERT_EQUAL(SerialSession::kStatusPayloadSize + 2U + SerialSession::kTopicStatusRecordSize, size);

    const uint8_t* record = payload + 94U;
    TEST_ASSERT_EQUAL_UINT32(kRobotOne, read_u32(record));
    TEST_ASSERT_EQUAL_UINT16(7U, read_u16(record + 4U));
    TEST_ASSERT_EQUAL_UINT16(2U, read_u16(record + 6U));
    // The rate the robot actually granted (1500), not the 2000 that was asked.
    TEST_ASSERT_EQUAL_UINT32(1500U, read_u32(record + 8U));
    TEST_ASSERT_EQUAL_UINT64(64U, read_u64(record + 12U));
    TEST_ASSERT_EQUAL_UINT64(1U, read_u64(record + 20U));
    TEST_ASSERT_EQUAL_UINT32(3141U, SubscriptionRegistry::upstreamSubscriptionId(kRobotOne, kTopicSeven));
}

void test_stale_subscribe_result_is_ignored() {
    SubscriptionRegistry::resetForTests();

    (void)SubscriptionRegistry::onDesktopSubscribe(kClientA, kRobotOne, kTopicSeven, 2000U, kLeaseMs, 1000U);
    SubscriptionRegistry::noteUpstreamRequestSent(kRobotOne, kTopicSeven, 11U);

    // A reply correlating to a sequence this dongle never has outstanding.
    TEST_ASSERT_FALSE(SubscriptionRegistry::onUpstreamSubscribeResult(12U, 0x00U, 55U, 2000U));
    TEST_ASSERT_EQUAL_UINT32(0U, SubscriptionRegistry::upstreamSubscriptionId(kRobotOne, kTopicSeven));

    // A rejection is correlated but must not be recorded as a grant.
    TEST_ASSERT_TRUE(SubscriptionRegistry::onUpstreamSubscribeResult(11U, 0x01U, 55U, 2000U));
    TEST_ASSERT_EQUAL_UINT32(0U, SubscriptionRegistry::upstreamSubscriptionId(kRobotOne, kTopicSeven));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_two_clients_on_same_topic_produce_one_row_at_the_union_rate);
    RUN_TEST(test_slower_second_client_does_not_change_what_the_robot_was_asked_for);
    RUN_TEST(test_rate_falls_back_to_remaining_client_when_the_fastest_one_leaves);
    RUN_TEST(test_unsubscribe_reaches_the_robot_only_for_the_last_consumer);
    RUN_TEST(test_unsubscribing_an_absent_subscription_is_a_silent_no_op);
    RUN_TEST(test_repeating_an_identical_subscribe_reuses_the_subscription_id);
    RUN_TEST(test_session_end_drops_every_subscription_that_client_owned);
    RUN_TEST(test_expired_lease_releases_the_topic_like_an_unsubscribe);
    RUN_TEST(test_upstream_lease_is_renewed_before_the_robot_would_drop_it);
    RUN_TEST(test_counters_are_disambiguated_by_source_and_topic);
    RUN_TEST(test_counters_survive_the_topic_losing_all_subscribers);
    RUN_TEST(test_status_v1_payload_is_untouched_by_this_topico);
    RUN_TEST(test_status_v2_serializes_one_fixed_record_per_topic);
    RUN_TEST(test_status_v2_refuses_to_overrun_a_short_buffer);
    RUN_TEST(test_status_v2_with_no_topics_is_just_the_count_field);
    RUN_TEST(test_registry_snapshot_feeds_status_v2_end_to_end);
    RUN_TEST(test_stale_subscribe_result_is_ignored);
    return UNITY_END();
}
