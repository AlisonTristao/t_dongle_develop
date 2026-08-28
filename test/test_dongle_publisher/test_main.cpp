#include <unity.h>

#include <DonglePublisher.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

// Exercises topico 27 on the dongle side: the dongle stops being a cable and
// starts being a BTP device of its own -- own manifest, own topics, own
// publishing -- so the health of the hub itself can be plotted in a chart
// like any robot's telemetry.
//
// The acceptance criterion of that topico is that NOTHING in the desktop had
// to change for this to work. That is what makes the byte layout, and not the
// behavior, the thing worth pinning here: the desktop reads these payloads
// with the PACKED_LE decoder it already had, so a field silently moving by
// one octet is the whole failure mode. Every assertion below is written
// against absolute offsets for that reason -- a test that re-derived the
// offsets from the same constants the encoder uses would move along with the
// bug.
//
// Everything covered here is pure C++. The Arduino/FreeRTOS glue that feeds
// these payloads (EspNowConfig::peekRxCounters, SerialMux::peekTxCounters,
// the heartbeat that decides `online`) is covered by pio run -e tdongle-s3
// plus hardware verification, not here.

namespace {

using std::size_t;
using std::uint8_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;

uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readU64(const uint8_t* p) {
    uint64_t value = 0U;
    for (size_t i = 0U; i < 8U; ++i) {
        value |= static_cast<uint64_t>(p[i]) << (8U * i);
    }
    return value;
}

// Distinct, non-zero, non-sequential values in every field, so a pair of
// fields swapped by the encoder cannot pass by coincidence.
DonglePublisher::LinkCounters sampleLink() {
    DonglePublisher::LinkCounters c = {};
    c.datagrams = 0x11111111U;
    c.fragmentsAccepted = 0x22222222U;
    c.routedTelemetry = 0x33333333U;
    c.routedLog = 0x44444444U;
    c.routedCommand = 0x55555555U;
    c.routedControl = 0x66666666U;
    c.routedTerminal = 0x77777777U;
    c.droppedRx = 0x88888888U;
    c.droppedDecode = 0x99999999U;
    c.droppedCrc = 0xAAAAAAAAU;
    c.droppedReassembly = 0xBBBBBBBBU;
    c.droppedQueueFull = 0xCCCCCCCCU;
    return c;
}

DonglePublisher::UsbCounters sampleUsb() {
    DonglePublisher::UsbCounters c = {};
    c.framesRx = 0x1111111122222222ULL;
    c.framesTx = 0x3333333344444444ULL;
    c.crcErrors = 0x5555555566666666ULL;
    c.decodeErrors = 0x7777777788888888ULL;
    c.reassemblyRejected = 0x9999999900000001ULL;
    c.telemetryDropped = 0xAAAAAAAA00000002ULL;
    for (size_t i = 0U; i < DonglePublisher::kUsbDropClassCount; ++i) {
        // Deliberately distinct AND deliberately not summable back into any
        // of the fields above -- see the droppedByClass test.
        c.droppedByClass[i] = 0xD0000000ULL + i;
    }
    c.relayDownOk = 0xE000000000000001ULL;
    c.relayDownUnbound = 0xE000000000000002ULL;
    c.relayDownNoPeer = 0xE000000000000003ULL;
    c.relayDownOversized = 0xE000000000000004ULL;
    c.relayDownSendFailed = 0xE000000000000005ULL;
    return c;
}

DonglePublisher::PeerRecord makePeer(uint32_t sourceId, uint32_t bootId,
                                     uint8_t macTail, uint32_t ageMs,
                                     bool online) {
    DonglePublisher::PeerRecord p = {};
    p.sourceId = sourceId;
    p.bootId = bootId;
    const uint8_t mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, macTail};
    std::memcpy(p.mac, mac, sizeof(mac));
    p.lastSeenAgeMs = ageMs;
    p.online = online;
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// hub.link
// ---------------------------------------------------------------------------

// The twelve counters EspNowConfig has been accumulating and never publishing.
// The order is the contract: schema_version, then the twelve in declaration
// order. A field inserted in the middle without a schema_version bump would
// silently reinterpret every field after it on the desktop.
void test_link_payload_layout_is_exact() {
    const DonglePublisher::LinkCounters counters = sampleLink();
    uint8_t buffer[DonglePublisher::kLinkPayloadSize + 8U] = {0};

    const size_t written =
        DonglePublisher::packLink(counters, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_UINT32(DonglePublisher::kLinkPayloadSize, written);
    TEST_ASSERT_EQUAL_UINT32(50U, written);  // 2 + 12*4, spelled out

    TEST_ASSERT_EQUAL_UINT16(DonglePublisher::kSchemaVersion, readU16(buffer));

    const uint32_t expected[12] = {
        counters.datagrams,        counters.fragmentsAccepted,
        counters.routedTelemetry,  counters.routedLog,
        counters.routedCommand,    counters.routedControl,
        counters.routedTerminal,   counters.droppedRx,
        counters.droppedDecode,    counters.droppedCrc,
        counters.droppedReassembly, counters.droppedQueueFull};
    for (size_t i = 0U; i < 12U; ++i) {
        TEST_ASSERT_EQUAL_UINT32(expected[i], readU32(buffer + 2U + i * 4U));
    }

    // Nothing written past the declared size.
    TEST_ASSERT_EQUAL_UINT8(0U, buffer[DonglePublisher::kLinkPayloadSize]);
}

void test_link_refuses_a_buffer_one_octet_short() {
    const DonglePublisher::LinkCounters counters = sampleLink();
    uint8_t buffer[DonglePublisher::kLinkPayloadSize] = {0};
    TEST_ASSERT_EQUAL_UINT32(
        0U, DonglePublisher::packLink(counters, buffer,
                                      DonglePublisher::kLinkPayloadSize - 1U));
    // Refused means refused: not a partial write.
    for (size_t i = 0U; i < sizeof(buffer); ++i) {
        TEST_ASSERT_EQUAL_UINT8(0U, buffer[i]);
    }
}

// ---------------------------------------------------------------------------
// hub.usb
// ---------------------------------------------------------------------------

void test_usb_payload_layout_is_exact() {
    const DonglePublisher::UsbCounters counters = sampleUsb();
    uint8_t buffer[DonglePublisher::kUsbPayloadSize + 8U] = {0};

    const size_t written =
        DonglePublisher::packUsb(counters, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_UINT32(DonglePublisher::kUsbPayloadSize, written);
    TEST_ASSERT_EQUAL_UINT32(122U, written);  // 2 + 6*8 + 4*8 + 5*8

    TEST_ASSERT_EQUAL_UINT16(DonglePublisher::kSchemaVersion, readU16(buffer));

    const uint64_t expected[6] = {counters.framesRx,           counters.framesTx,
                                  counters.crcErrors,          counters.decodeErrors,
                                  counters.reassemblyRejected, counters.telemetryDropped};
    for (size_t i = 0U; i < 6U; ++i) {
        TEST_ASSERT_EQUAL_UINT64(expected[i], readU64(buffer + 2U + i * 8U));
    }

    const size_t relayBase = 2U + 6U * 8U + DonglePublisher::kUsbDropClassCount * 8U;
    const uint64_t relayExpected[5] = {
        counters.relayDownOk, counters.relayDownUnbound, counters.relayDownNoPeer,
        counters.relayDownOversized, counters.relayDownSendFailed};
    for (size_t i = 0U; i < 5U; ++i) {
        TEST_ASSERT_EQUAL_UINT64(relayExpected[i], readU64(buffer + relayBase + i * 8U));
    }
}

// The single most important assertion about hub.usb, and the reason the topic
// exists at all: droppedByClass travels as one entry PER PRIORITY CLASS and is
// never collapsed into a total. A drop in the telemetry class means the link
// is saturated; a drop in the session or terminal class means the main loop
// stalled. Those point at opposite fixes, and a single total says only that
// something was lost -- which is exactly what STATUS already said.
void test_usb_keeps_dropped_by_class_split_never_summed() {
    const DonglePublisher::UsbCounters counters = sampleUsb();
    uint8_t buffer[DonglePublisher::kUsbPayloadSize] = {0};
    TEST_ASSERT_EQUAL_UINT32(
        DonglePublisher::kUsbPayloadSize,
        DonglePublisher::packUsb(counters, buffer, sizeof(buffer)));

    const size_t base = 2U + 6U * 8U;
    uint64_t total = 0U;
    for (size_t i = 0U; i < DonglePublisher::kUsbDropClassCount; ++i) {
        const uint64_t value = readU64(buffer + base + i * 8U);
        TEST_ASSERT_EQUAL_UINT64(counters.droppedByClass[i], value);
        total += counters.droppedByClass[i];
    }

    // And the collapsed form is provably absent: the sum appears nowhere in
    // the payload. This is what would fail if someone "simplified" the four
    // entries into one.
    for (size_t offset = 0U; offset + 8U <= DonglePublisher::kUsbPayloadSize;
         ++offset) {
        TEST_ASSERT_NOT_EQUAL_UINT64(total, readU64(buffer + offset));
    }
}

// ---------------------------------------------------------------------------
// hub.peers
// ---------------------------------------------------------------------------

// Six parallel arrays, each self-prefixed with its own element_count
// (telemetry.md section 4.1). The MAC array counts OCTETS, not peers, because
// PACKED_LE has no fixed-width blob type -- that asymmetry is easy to get
// wrong in both the encoder and a hand-written decoder, so it is pinned.
void test_peers_payload_layout_is_exact() {
    const DonglePublisher::PeerRecord peers[3] = {
        makePeer(0xA1A1A1A1U, 0x0B0B0B0BU, 0x11U, 250U, true),
        makePeer(0xB2B2B2B2U, 0x0C0C0C0CU, 0x22U, 4000U, false),
        makePeer(0xC3C3C3C3U, 0x0D0D0D0DU, 0x33U, 0U, true),
    };
    uint8_t buffer[DonglePublisher::kMaxPeersPayloadSize] = {0};

    const size_t written =
        DonglePublisher::packPeers(peers, 3U, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_UINT32(
        DonglePublisher::kPeersPayloadOverhead +
            DonglePublisher::kPeersPayloadBytesPerPeer * 3U,
        written);
    TEST_ASSERT_EQUAL_UINT32(14U + 20U * 3U, written);  // 74, spelled out

    size_t at = 0U;
    TEST_ASSERT_EQUAL_UINT16(DonglePublisher::kSchemaVersion, readU16(buffer));
    at += 2U;

    // channel: u8 per peer, and the value IS the array index.
    TEST_ASSERT_EQUAL_UINT16(3U, readU16(buffer + at));
    at += 2U;
    for (uint8_t i = 0U; i < 3U; ++i) {
        TEST_ASSERT_EQUAL_UINT8(i, buffer[at + i]);
    }
    at += 3U;

    TEST_ASSERT_EQUAL_UINT16(3U, readU16(buffer + at));
    at += 2U;
    for (size_t i = 0U; i < 3U; ++i) {
        TEST_ASSERT_EQUAL_UINT32(peers[i].sourceId, readU32(buffer + at + i * 4U));
    }
    at += 12U;

    TEST_ASSERT_EQUAL_UINT16(3U, readU16(buffer + at));
    at += 2U;
    for (size_t i = 0U; i < 3U; ++i) {
        TEST_ASSERT_EQUAL_UINT32(peers[i].bootId, readU32(buffer + at + i * 4U));
    }
    at += 12U;

    // The MAC count is 6 * peers, in octets -- not 3.
    TEST_ASSERT_EQUAL_UINT16(18U, readU16(buffer + at));
    at += 2U;
    for (size_t i = 0U; i < 3U; ++i) {
        TEST_ASSERT_EQUAL_UINT8_ARRAY(peers[i].mac, buffer + at + i * 6U, 6);
    }
    at += 18U;

    TEST_ASSERT_EQUAL_UINT16(3U, readU16(buffer + at));
    at += 2U;
    for (size_t i = 0U; i < 3U; ++i) {
        TEST_ASSERT_EQUAL_UINT32(peers[i].lastSeenAgeMs,
                                 readU32(buffer + at + i * 4U));
    }
    at += 12U;

    TEST_ASSERT_EQUAL_UINT16(3U, readU16(buffer + at));
    at += 2U;
    TEST_ASSERT_EQUAL_UINT8(1U, buffer[at + 0U]);
    TEST_ASSERT_EQUAL_UINT8(0U, buffer[at + 1U]);
    TEST_ASSERT_EQUAL_UINT8(1U, buffer[at + 2U]);
    at += 3U;

    TEST_ASSERT_EQUAL_UINT32(written, at);
}

// An empty peer list is a valid payload, not a refusal: a hub that hears no
// robot has to be able to say so. A client that only ever saw a non-empty
// list would have no way to distinguish "no peers" from "publisher stopped".
void test_peers_with_no_peers_is_a_valid_empty_payload() {
    uint8_t buffer[DonglePublisher::kMaxPeersPayloadSize] = {0};
    const size_t written =
        DonglePublisher::packPeers(nullptr, 0U, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_UINT32(DonglePublisher::kPeersPayloadOverhead, written);
    TEST_ASSERT_EQUAL_UINT16(DonglePublisher::kSchemaVersion, readU16(buffer));
    // All six element_counts are zero, and the six of them are the whole body.
    for (size_t i = 0U; i < 6U; ++i) {
        TEST_ASSERT_EQUAL_UINT16(0U, readU16(buffer + 2U + i * 2U));
    }
}

void test_peers_at_capacity_fits_the_declared_maximum() {
    DonglePublisher::PeerRecord peers[DonglePublisher::kMaxPeers];
    for (size_t i = 0U; i < DonglePublisher::kMaxPeers; ++i) {
        peers[i] = makePeer(0x1000U + static_cast<uint32_t>(i),
                            0x2000U + static_cast<uint32_t>(i),
                            static_cast<uint8_t>(i), static_cast<uint32_t>(i),
                            (i % 2U) == 0U);
    }
    uint8_t buffer[DonglePublisher::kMaxPeersPayloadSize] = {0};

    const size_t written = DonglePublisher::packPeers(
        peers, DonglePublisher::kMaxPeers, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_UINT32(DonglePublisher::kMaxPeersPayloadSize, written);
    TEST_ASSERT_EQUAL_UINT32(334U, written);  // 14 + 20*16, spelled out

    // The declared ceiling is the real ceiling: a full list still fits in one
    // ESP-NOW-profile logical payload's worth of relay budget upstream.
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(DonglePublisher::kMaxPeersPayloadSize,
                                     written);
}

// More peers than the table holds is clamped, not overflowed. kMaxPeers
// mirrors BtpTransport::kPeerIdentityCapacity, so this is the boundary where a
// 17th robot on the fleet would arrive.
void test_peers_beyond_capacity_is_clamped_not_overflowed() {
    DonglePublisher::PeerRecord peers[DonglePublisher::kMaxPeers + 4U];
    for (size_t i = 0U; i < DonglePublisher::kMaxPeers + 4U; ++i) {
        peers[i] = makePeer(static_cast<uint32_t>(0x9000U + i), 1U,
                            static_cast<uint8_t>(i), 0U, true);
    }
    uint8_t buffer[DonglePublisher::kMaxPeersPayloadSize] = {0};

    const size_t written = DonglePublisher::packPeers(
        peers, DonglePublisher::kMaxPeers + 4U, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_UINT32(DonglePublisher::kMaxPeersPayloadSize, written);
    TEST_ASSERT_EQUAL_UINT16(DonglePublisher::kMaxPeers, readU16(buffer + 2U));
}

// ===========================================================================
// The D8 trap.
//
// `channel` is a DISPLAY INDEX assigned in the order the dongle first hears
// each peer. It is not identity and it is not stable across dongle reboots.
// This test exists to DOCUMENT that volatility, not to demand stability: the
// same fleet heard in a different order publishes different channel numbers
// for the same robots, while every sourceId stays put.
//
// The reason it is worth a test of its own is the shape of the failure it
// guards. Anything that persists a selection -- a saved chart, a field
// binding, the bind table topico 28 adds -- and persists the channel instead
// of the sourceId comes back after a reboot pointing at a different robot,
// plotting the wrong data, raising no error anywhere.
// ===========================================================================
void test_channel_is_arrival_order_and_sourceid_is_the_address() {
    const uint32_t robotA = 0xA1A1A1A1U;
    const uint32_t robotB = 0xB2B2B2B2U;

    // Boot 1: A came up first.
    const DonglePublisher::PeerRecord bootOne[2] = {
        makePeer(robotA, 1U, 0x11U, 10U, true),
        makePeer(robotB, 1U, 0x22U, 20U, true),
    };
    // Boot 2: same fleet, B came up first. Nothing about the robots changed.
    const DonglePublisher::PeerRecord bootTwo[2] = {
        makePeer(robotB, 2U, 0x22U, 15U, true),
        makePeer(robotA, 2U, 0x11U, 25U, true),
    };

    uint8_t one[DonglePublisher::kMaxPeersPayloadSize] = {0};
    uint8_t two[DonglePublisher::kMaxPeersPayloadSize] = {0};
    TEST_ASSERT_TRUE(DonglePublisher::packPeers(bootOne, 2U, one, sizeof(one)) > 0U);
    TEST_ASSERT_TRUE(DonglePublisher::packPeers(bootTwo, 2U, two, sizeof(two)) > 0U);

    const size_t channelAt = 4U;         // schema_version + element_count
    const size_t sourceAt = 4U + 2U + 2U;  // + channels + element_count

    // Channel 0 names robot A after one boot and robot B after the other.
    // Identical index, different robot: this is the whole warning.
    TEST_ASSERT_EQUAL_UINT8(0U, one[channelAt]);
    TEST_ASSERT_EQUAL_UINT8(0U, two[channelAt]);
    TEST_ASSERT_EQUAL_UINT32(robotA, readU32(one + sourceAt));
    TEST_ASSERT_EQUAL_UINT32(robotB, readU32(two + sourceAt));

    // The address is stable in both directions: robot A is 0xA1A1A1A1 in both
    // payloads, it just moved from channel 0 to channel 1.
    TEST_ASSERT_EQUAL_UINT8(1U, one[channelAt + 1U]);
    TEST_ASSERT_EQUAL_UINT8(1U, two[channelAt + 1U]);
    TEST_ASSERT_EQUAL_UINT32(robotB, readU32(one + sourceAt + 4U));
    TEST_ASSERT_EQUAL_UINT32(robotA, readU32(two + sourceAt + 4U));
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

// The dongle's three topics live in one reserved block so they can never
// collide with a robot's topic ids, and each record is self-prefixed with its
// own record_size (commands.md section 3.3) -- which is what lets a client
// skip a record it does not understand instead of losing the rest.
void test_manifest_topic_records_are_well_formed() {
    size_t size = 0U;
    uint16_t topicCount = 0U;
    const uint8_t* records = DonglePublisher::topicRecords(&size, &topicCount);

    TEST_ASSERT_NOT_NULL(records);
    TEST_ASSERT_EQUAL_UINT16(3U, topicCount);
    TEST_ASSERT_TRUE(size > 0U);

    // Walk by the self-declared record_size and require the walk to land
    // exactly on the end. Any record whose prefix lies leaves a remainder.
    //
    // record_size is uint32_le and counts the octets AFTER itself, not
    // including itself (commands.md section 3.3) -- so a record occupies
    // 4 + record_size. Getting that inclusive/exclusive boundary wrong is the
    // classic way to write a decoder that works on a one-record manifest and
    // desynchronizes on the second, which is why the walk below is checked
    // against the total rather than just per record.
    size_t at = 0U;
    uint16_t seen = 0U;
    while (at < size) {
        TEST_ASSERT_TRUE(at + 4U <= size);
        const uint32_t recordSize = readU32(records + at);
        // A topic record's fixed part alone is topic_id(2) + schema_version(2)
        // + encoding(1) + flags(1) + field_count(2) + max_rate(4) + two
        // utf8_u16 length prefixes(4) = 16, so anything smaller is malformed.
        TEST_ASSERT_TRUE(recordSize >= 16U);
        TEST_ASSERT_TRUE(at + 4U + recordSize <= size);
        at += 4U + recordSize;
        ++seen;
    }
    TEST_ASSERT_EQUAL_UINT32(size, at);
    TEST_ASSERT_EQUAL_UINT16(topicCount, seen);
}

void test_topic_ids_are_inside_the_reserved_hub_block() {
    const uint16_t ids[3] = {DonglePublisher::kLinkTopicId,
                             DonglePublisher::kUsbTopicId,
                             DonglePublisher::kPeersTopicId};
    for (size_t i = 0U; i < 3U; ++i) {
        TEST_ASSERT_TRUE(ids[i] >= DonglePublisher::kHubTopicBlockBase);
        TEST_ASSERT_TRUE(ids[i] <= DonglePublisher::kHubTopicBlockEnd);
    }
    // Three distinct topics, not one id used twice.
    TEST_ASSERT_NOT_EQUAL_UINT16(ids[0], ids[1]);
    TEST_ASSERT_NOT_EQUAL_UINT16(ids[1], ids[2]);
    TEST_ASSERT_NOT_EQUAL_UINT16(ids[0], ids[2]);
}

// Every topic the manifest declares has to be answerable, or a client that
// subscribes at the declared rate gets nothing and has no way to tell whether
// it asked wrong or the hub is silent.
void test_every_declared_topic_has_a_max_rate() {
    const uint16_t ids[3] = {DonglePublisher::kLinkTopicId,
                             DonglePublisher::kUsbTopicId,
                             DonglePublisher::kPeersTopicId};
    for (size_t i = 0U; i < 3U; ++i) {
        uint32_t maxRate = 0U;
        TEST_ASSERT_TRUE(DonglePublisher::lookupTopicMaxRateMillihz(ids[i], &maxRate));
        TEST_ASSERT_TRUE(maxRate > 0U);
    }

    uint32_t unused = 0U;
    TEST_ASSERT_FALSE(DonglePublisher::lookupTopicMaxRateMillihz(0x0001U, &unused));
}

// ---------------------------------------------------------------------------
// Publishing is subscription-gated
// ---------------------------------------------------------------------------

namespace {
uint16_t g_emitted[16];
size_t g_emittedCount = 0U;

bool captureEmit(uint16_t topicId, const uint8_t* payload, size_t size) {
    (void)payload;
    (void)size;
    if (g_emittedCount < 16U) {
        g_emitted[g_emittedCount++] = topicId;
    }
    return true;
}

void fillSnapshot(DonglePublisher::Snapshot& out) {
    out.link = sampleLink();
    out.usb = sampleUsb();
    out.peers[0] = makePeer(0xA1A1A1A1U, 1U, 0x11U, 5U, true);
    out.peerCount = 1U;
}
}  // namespace

// A topic nobody subscribed to must cost neither radio nor cable. This is the
// local half of "closing a chart reduces traffic": the hub is a BTP device
// like any other, so it obeys the same subscribe model it relays for robots.
void test_nothing_is_published_without_a_subscriber() {
    DonglePublisher::resetForTests();
    DonglePublisher::setSnapshotProvider(fillSnapshot);
    g_emittedCount = 0U;

    TEST_ASSERT_FALSE(DonglePublisher::hasSubscribers());
    DonglePublisher::tick(1000U, captureEmit);
    DonglePublisher::tick(5000U, captureEmit);
    TEST_ASSERT_EQUAL_UINT32(0U, g_emittedCount);
}

void test_subscribing_starts_and_unsubscribing_stops_that_topic() {
    DonglePublisher::resetForTests();
    DonglePublisher::setSnapshotProvider(fillSnapshot);
    g_emittedCount = 0U;

    DonglePublisher::onLocalSubscribe(DonglePublisher::kPeersTopicId, 1000U);
    TEST_ASSERT_TRUE(DonglePublisher::hasSubscribers());

    DonglePublisher::tick(1000U, captureEmit);
    DonglePublisher::tick(2000U, captureEmit);
    TEST_ASSERT_TRUE(g_emittedCount > 0U);
    for (size_t i = 0U; i < g_emittedCount; ++i) {
        // Only the subscribed topic -- subscribing to one does not turn the
        // other two on.
        TEST_ASSERT_EQUAL_UINT16(DonglePublisher::kPeersTopicId, g_emitted[i]);
    }

    DonglePublisher::onLocalUnsubscribe(DonglePublisher::kPeersTopicId);
    TEST_ASSERT_FALSE(DonglePublisher::hasSubscribers());
    g_emittedCount = 0U;
    DonglePublisher::tick(3000U, captureEmit);
    DonglePublisher::tick(4000U, captureEmit);
    TEST_ASSERT_EQUAL_UINT32(0U, g_emittedCount);
}

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_link_payload_layout_is_exact);
    RUN_TEST(test_link_refuses_a_buffer_one_octet_short);

    RUN_TEST(test_usb_payload_layout_is_exact);
    RUN_TEST(test_usb_keeps_dropped_by_class_split_never_summed);

    RUN_TEST(test_peers_payload_layout_is_exact);
    RUN_TEST(test_peers_with_no_peers_is_a_valid_empty_payload);
    RUN_TEST(test_peers_at_capacity_fits_the_declared_maximum);
    RUN_TEST(test_peers_beyond_capacity_is_clamped_not_overflowed);
    RUN_TEST(test_channel_is_arrival_order_and_sourceid_is_the_address);

    RUN_TEST(test_manifest_topic_records_are_well_formed);
    RUN_TEST(test_topic_ids_are_inside_the_reserved_hub_block);
    RUN_TEST(test_every_declared_topic_has_a_max_rate);

    RUN_TEST(test_nothing_is_published_without_a_subscriber);
    RUN_TEST(test_subscribing_starts_and_unsubscribing_stops_that_topic);

    return UNITY_END();
}
