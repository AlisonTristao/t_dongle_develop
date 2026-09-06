#include "DonglePublisher.h"

#include <BtpTransport.h>
#include <SubscriptionRegistry.h>

#include <cstring>

namespace DonglePublisher {
namespace {

static_assert(kMaxPeers == BtpTransport::kPeerIdentityCapacity,
              "hub.peers declares max_element_count from the real peer table size");

// Field type codes, commands.md section 3.3 (same order as telemetry.md
// section 5's wire types). Only the four this dongle actually uses.
constexpr std::uint8_t kTypeUint8 = 0x01U;
constexpr std::uint8_t kTypeUint32 = 0x03U;
constexpr std::uint8_t kTypeUint64 = 0x04U;
constexpr std::uint8_t kTypeInt8 = 0x05U;

constexpr std::uint8_t kEncodingPackedLe = 0x05U;
constexpr std::uint8_t kTopicFlagSubscribable = 0x01U;
constexpr std::uint8_t kFieldFlagVariableCount = 0x02U;  // bit 1

// hub.peers publishes on change *and* on a fixed keep-alive, so a client that
// attaches mid-run gets the list without waiting for a peer to appear or
// disappear.
constexpr std::uint32_t kPeersKeepAliveMs = 1000U;

// Declared publish ceilings (`max_rate_millihz`, commands.md section 3.3).
// The robot declares 50 Hz for protocol.test because that is a control-loop
// signal sampled at loop rate. These three are cumulative health counters and
// a discovery list: a client plots the *difference* between samples, so a few
// Hz already resolves any spike worth reacting to, and asking for more would
// only spend USB frames and queue slots on redundant totals.
constexpr std::uint32_t kLinkMaxRateMillihz = 5000U;   // 5 Hz
constexpr std::uint32_t kUsbMaxRateMillihz = 5000U;    // 5 Hz
// hub.peers is capped at 2 Hz rather than 1 Hz on purpose: the topic is
// specified as "publish on change and at least once per second", and a change
// arriving just after a keep-alive would otherwise have to wait a full second
// or exceed the declared maximum. 2 Hz is the honest ceiling of what the
// change path can actually produce (see tick(): a change can only be
// published once the granted period has elapsed), so the manifest never
// promises a rate the publisher then breaks.
constexpr std::uint32_t kPeersMaxRateMillihz = 2000U;  // 2 Hz

struct FieldDesc {
    std::uint16_t fieldId;
    std::uint16_t order;
    const char* name;
    std::uint8_t type;
    const char* unit;
    // A fixed array declares its width here and leaves maxElementCount at 0.
    // A variable array declares elementCount = 0, kFieldFlagVariableCount and
    // a non-zero maxElementCount -- the shape telemetry.md section 4.1
    // requires and the one a client uses to tell the two apart.
    std::uint16_t elementCount;
    std::uint16_t maxElementCount;
    std::uint8_t flags;
};

struct TopicDesc {
    std::uint16_t topicId;
    const char* name;
    std::uint32_t maxRateMillihz;
    const FieldDesc* fields;
    std::size_t fieldCount;
};

// The single source of truth: the packers below and the manifest records are
// both generated from these tables, so a field added to one can never be
// missing from the other (same reason bally_software's ManifestResponder reads
// TelemetryPublisher::schemas() instead of keeping its own copy).
//
// Order matters twice over: it is the PACKED_LE order on the wire and the
// `order` a client decodes by. Every field is non-nullable, so no payload
// carries a presence bitmap.
constexpr FieldDesc kLinkFields[] = {
    {1U, 0U, "datagrams", kTypeUint32, "1", 1U, 0U, 0U},
    {2U, 1U, "fragments_accepted", kTypeUint32, "1", 1U, 0U, 0U},
    {3U, 2U, "routed_telemetry", kTypeUint32, "1", 1U, 0U, 0U},
    {4U, 3U, "routed_log", kTypeUint32, "1", 1U, 0U, 0U},
    {5U, 4U, "routed_command", kTypeUint32, "1", 1U, 0U, 0U},
    {6U, 5U, "routed_control", kTypeUint32, "1", 1U, 0U, 0U},
    {7U, 6U, "routed_terminal", kTypeUint32, "1", 1U, 0U, 0U},
    {8U, 7U, "dropped_rx", kTypeUint32, "1", 1U, 0U, 0U},
    {9U, 8U, "dropped_decode", kTypeUint32, "1", 1U, 0U, 0U},
    {10U, 9U, "dropped_crc", kTypeUint32, "1", 1U, 0U, 0U},
    {11U, 10U, "dropped_reassembly", kTypeUint32, "1", 1U, 0U, 0U},
    {12U, 11U, "dropped_queue_full", kTypeUint32, "1", 1U, 0U, 0U},
};

constexpr FieldDesc kUsbFields[] = {
    {1U, 0U, "frames_rx", kTypeUint64, "1", 1U, 0U, 0U},
    {2U, 1U, "frames_tx", kTypeUint64, "1", 1U, 0U, 0U},
    {3U, 2U, "crc_errors", kTypeUint64, "1", 1U, 0U, 0U},
    {4U, 3U, "decode_errors", kTypeUint64, "1", 1U, 0U, 0U},
    {5U, 4U, "reassembly_rejected", kTypeUint64, "1", 1U, 0U, 0U},
    {6U, 5U, "telemetry_dropped", kTypeUint64, "1", 1U, 0U, 0U},
    // Fixed array of four, index-aligned with SerialSession::PriorityClass
    // (0 session, 1 terminal, 2 log/status, 3 telemetry). A fixed array
    // carries no count on the wire and keeps every class separately
    // addressable as (field_id, element_index) -- collapsing it into one
    // total would destroy the only thing it is for.
    {7U, 6U, "dropped_by_class", kTypeUint64, "1",
     static_cast<std::uint16_t>(kUsbDropClassCount), 0U, 0U},
    // Downstream relay, one field per refusal reason. Separate fields rather
    // than one "failed" total for the same reason dropped_by_class is an
    // array and not a sum: each reason names a different thing to go fix.
    {8U, 7U, "relay_down_ok", kTypeUint64, "1", 1U, 0U, 0U},
    {9U, 8U, "relay_down_unbound", kTypeUint64, "1", 1U, 0U, 0U},
    {10U, 9U, "relay_down_no_peer", kTypeUint64, "1", 1U, 0U, 0U},
    {11U, 10U, "relay_down_oversized", kTypeUint64, "1", 1U, 0U, 0U},
    {12U, 11U, "relay_down_send_failed", kTypeUint64, "1", 1U, 0U, 0U},
};

// hub.peers is a variable-length list, and PACKED_LE has no nested-record
// type: the only shape that keeps every value individually addressable is one
// variable array per column, each carrying its own element_count. Peer i is
// therefore read across the eight arrays at element_index i (and at 6i..6i+5 for
// the flat `mac` bytes). That is exactly the (source_id, topic_id, field_id,
// element_index) binding telemetry.md section 8 defines, so a client can chart
// one peer's last_seen_ms without any new concept. The alternative --
// OPAQUE_BYTES for the whole body -- would have made the topic unplottable,
// which defeats its purpose.
constexpr FieldDesc kPeersFields[] = {
    {1U, 0U, "channel", kTypeUint8, "1", 0U, static_cast<std::uint16_t>(kMaxPeers),
     kFieldFlagVariableCount},
    {2U, 1U, "source_id", kTypeUint32, "1", 0U, static_cast<std::uint16_t>(kMaxPeers),
     kFieldFlagVariableCount},
    {3U, 2U, "boot_id", kTypeUint32, "1", 0U, static_cast<std::uint16_t>(kMaxPeers),
     kFieldFlagVariableCount},
    {4U, 3U, "mac", kTypeUint8, "1", 0U, static_cast<std::uint16_t>(kMaxPeers * 6U),
     kFieldFlagVariableCount},
    {5U, 4U, "last_seen_ms", kTypeUint32, "ms", 0U, static_cast<std::uint16_t>(kMaxPeers),
     kFieldFlagVariableCount},
    {6U, 5U, "online", kTypeUint8, "1", 0U, static_cast<std::uint16_t>(kMaxPeers),
     kFieldFlagVariableCount},
    {7U, 6U, "rssi", kTypeInt8, "dBm", 0U, static_cast<std::uint16_t>(kMaxPeers),
     kFieldFlagVariableCount},
    {8U, 7U, "rtt_ms", kTypeUint32, "ms", 0U, static_cast<std::uint16_t>(kMaxPeers),
     kFieldFlagVariableCount},
};

// Sorted by ascending topic_id: MANIFEST_DATA does not require it, but a
// stable order makes the response bytes reproducible, which is what lets a
// test compare offsets at all.
constexpr TopicDesc kTopics[] = {
    {kLinkTopicId, "hub.link", kLinkMaxRateMillihz, kLinkFields,
     sizeof(kLinkFields) / sizeof(kLinkFields[0])},
    {kUsbTopicId, "hub.usb", kUsbMaxRateMillihz, kUsbFields,
     sizeof(kUsbFields) / sizeof(kUsbFields[0])},
    {kPeersTopicId, "hub.peers", kPeersMaxRateMillihz, kPeersFields,
     sizeof(kPeersFields) / sizeof(kPeersFields[0])},
};

constexpr std::size_t kTopicCount = sizeof(kTopics) / sizeof(kTopics[0]);

// hub.usb grew in schema version 2 with five relay counters.  The old 1500
// byte buffer then wrote hub.link + hub.usb and silently stopped before
// hub.peers.  The manifest is the discovery path for the whole hub, so a
// partial prefix is not an acceptable degraded result.  2048 leaves room for
// all three current topic records and stays below SerialMux's matching
// 2048-byte MANIFEST_DATA payload ceiling.
constexpr std::size_t kRecordsCapacity = 2048U;

// ---------------------------------------------------------------------------
// Little-endian append cursor. Same shape as ManifestCache's Writer (reserve +
// patch for the record_size prefixes that are only known after the fact); kept
// local rather than shared because the two write different things and neither
// wants the other's methods.
// ---------------------------------------------------------------------------
class Writer {
public:
    Writer(std::uint8_t* out, std::size_t capacity) noexcept : out_(out), capacity_(capacity) {}

    bool u8(std::uint8_t value) noexcept { return raw(&value, 1U); }

    bool u16(std::uint16_t value) noexcept {
        const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(value),
                                       static_cast<std::uint8_t>(value >> 8U)};
        return raw(bytes, 2U);
    }

    bool u32(std::uint32_t value) noexcept {
        const std::uint8_t bytes[4] = {
            static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8U),
            static_cast<std::uint8_t>(value >> 16U), static_cast<std::uint8_t>(value >> 24U)};
        return raw(bytes, 4U);
    }

    bool u64(std::uint64_t value) noexcept {
        std::uint8_t bytes[8];
        for (std::size_t i = 0U; i < 8U; ++i) {
            bytes[i] = static_cast<std::uint8_t>(value >> (8U * i));
        }
        return raw(bytes, 8U);
    }

    bool f64(double value) noexcept {
        std::uint8_t bytes[8];
        // Host is little-endian on both targets this builds for (ESP32-S3 and
        // the x86 test host), so the IEEE-754 bytes are already in wire order.
        std::memcpy(bytes, &value, sizeof(bytes));
        return raw(bytes, 8U);
    }

    bool utf8(const char* text) noexcept {
        const std::size_t len = (text == nullptr) ? 0U : std::strlen(text);
        if (len > 0xFFFFU) return false;
        if (!u16(static_cast<std::uint16_t>(len))) return false;
        return len == 0U || raw(reinterpret_cast<const std::uint8_t*>(text), len);
    }

    bool reserveU32(std::size_t* offsetOut) noexcept {
        *offsetOut = pos_;
        return u32(0U);
    }

    void patchU32(std::size_t offset, std::uint32_t value) noexcept {
        out_[offset] = static_cast<std::uint8_t>(value);
        out_[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
        out_[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
        out_[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
    }

    std::size_t size() const noexcept { return pos_; }

private:
    bool raw(const std::uint8_t* data, std::size_t n) noexcept {
        if (pos_ + n > capacity_) return false;
        std::memcpy(out_ + pos_, data, n);
        pos_ += n;
        return true;
    }

    std::uint8_t* out_;
    std::size_t capacity_;
    std::size_t pos_ = 0U;
};

bool writeFieldRecord(Writer& writer, const FieldDesc& field) noexcept {
    std::size_t sizeOffset = 0U;
    if (!writer.reserveU32(&sizeOffset)) return false;
    const std::size_t contentStart = writer.size();

    const bool ok = writer.u16(field.fieldId) && writer.u16(field.order) && writer.u8(field.type) &&
                    writer.u8(field.flags) && writer.u16(field.elementCount) &&
                    writer.u16(field.maxElementCount) && writer.f64(1.0) /*scale*/ &&
                    writer.f64(0.0) /*offset*/ && writer.u16(0U) /*enum_count*/ &&
                    writer.utf8(field.name) && writer.utf8(field.unit) && writer.utf8("");
    if (!ok) return false;

    writer.patchU32(sizeOffset, static_cast<std::uint32_t>(writer.size() - contentStart));
    return true;
}

bool writeTopicRecord(Writer& writer, const TopicDesc& topic) noexcept {
    std::size_t sizeOffset = 0U;
    if (!writer.reserveU32(&sizeOffset)) return false;
    const std::size_t contentStart = writer.size();

    const bool ok = writer.u16(topic.topicId) && writer.u16(kSchemaVersion) &&
                    writer.u8(kEncodingPackedLe) && writer.u8(kTopicFlagSubscribable) &&
                    writer.u16(static_cast<std::uint16_t>(topic.fieldCount)) &&
                    writer.u32(topic.maxRateMillihz) && writer.utf8(topic.name) && writer.utf8("");
    if (!ok) return false;

    for (std::size_t i = 0U; i < topic.fieldCount; ++i) {
        if (!writeFieldRecord(writer, topic.fields[i])) return false;
    }

    writer.patchU32(sizeOffset, static_cast<std::uint32_t>(writer.size() - contentStart));
    return true;
}

std::uint8_t g_records[kRecordsCapacity];
std::size_t g_recordsSize = 0U;
std::uint16_t g_recordsTopicCount = 0U;
bool g_recordsBuilt = false;

// ---------------------------------------------------------------------------
// Publish scheduling
// ---------------------------------------------------------------------------

struct TopicState {
    bool subscribed = false;
    std::uint32_t periodMs = 0U;
    std::uint32_t lastPublishMs = 0U;
    bool published = false;
};

TopicState g_link;
TopicState g_usb;
TopicState g_peers;
std::uint32_t g_peersFingerprint = 0U;
bool g_peersFingerprintValid = false;
SnapshotFn g_snapshotProvider = nullptr;

TopicState* stateFor(std::uint16_t topicId) noexcept {
    switch (topicId) {
        case kLinkTopicId: return &g_link;
        case kUsbTopicId: return &g_usb;
        case kPeersTopicId: return &g_peers;
        default: return nullptr;
    }
}

std::uint32_t schemaMaxRateFor(std::uint16_t topicId) noexcept {
    for (const TopicDesc& topic : kTopics) {
        if (topic.topicId == topicId) return topic.maxRateMillihz;
    }
    return 0U;
}

// millihz -> milliseconds. 1 Hz is 1000 millihz, so the period is
// 1e6 / millihz. Never returns 0: a rate so high it rounds to a zero-length
// period would otherwise publish on every single tick.
std::uint32_t periodMsFromRate(std::uint32_t rateMillihz) noexcept {
    if (rateMillihz == 0U) return 0U;
    const std::uint32_t period = 1000000U / rateMillihz;
    return (period == 0U) ? 1U : period;
}

// Excludes lastSeenAgeMs on purpose: that value changes on every single tick,
// so folding it in would make "the peer list changed" always true and turn the
// change-triggered publish into a busy loop at the granted rate. What counts
// as a change is the *membership* of the list -- a peer appearing, going away,
// rebooting (new boot_id) or flipping its heartbeat verdict.
std::uint32_t peersFingerprint(const PeerRecord* peers, std::size_t count) noexcept {
    std::uint32_t hash = 2166136261U;  // FNV-1a
    const auto mix = [&hash](std::uint8_t byte) noexcept {
        hash ^= byte;
        hash *= 16777619U;
    };
    mix(static_cast<std::uint8_t>(count));
    for (std::size_t i = 0U; i < count; ++i) {
        for (std::size_t b = 0U; b < 4U; ++b) {
            mix(static_cast<std::uint8_t>(peers[i].sourceId >> (8U * b)));
            mix(static_cast<std::uint8_t>(peers[i].bootId >> (8U * b)));
        }
        for (std::size_t b = 0U; b < 6U; ++b) mix(peers[i].mac[b]);
        mix(peers[i].online ? 1U : 0U);
    }
    return hash;
}

// One place that touches the wire, so the per-topic accounting can never be
// forgotten on one path and not the other. Uses the same counters
// SubscriptionRegistry already keeps for relayed robot topics, so this
// dongle's own topics show up in the STATUS v2 per-topic extension exactly
// like everything else it forwards.
bool publish(std::uint16_t topicId, const std::uint8_t* payload, std::size_t size,
             EmitFn emit) noexcept {
    const std::uint32_t selfSourceId = BtpTransport::sourceId();
    if (emit(topicId, payload, size)) {
        SubscriptionRegistry::recordForwarded(selfSourceId, topicId, size);
        return true;
    }
    SubscriptionRegistry::recordDropped(selfSourceId, topicId);
    return false;
}

bool duePeriodic(const TopicState& state, std::uint32_t nowMs) noexcept {
    if (!state.subscribed) return false;
    if (!state.published) return true;  // first sample right after subscribing
    return (nowMs - state.lastPublishMs) >= state.periodMs;
}

}  // namespace

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::size_t packLink(const LinkCounters& counters, std::uint8_t* output,
                     std::size_t capacity) noexcept {
    if (output == nullptr || capacity < kLinkPayloadSize) return 0U;

    Writer writer(output, capacity);
    const bool ok = writer.u16(kSchemaVersion) && writer.u32(counters.datagrams) &&
                    writer.u32(counters.fragmentsAccepted) && writer.u32(counters.routedTelemetry) &&
                    writer.u32(counters.routedLog) && writer.u32(counters.routedCommand) &&
                    writer.u32(counters.routedControl) && writer.u32(counters.routedTerminal) &&
                    writer.u32(counters.droppedRx) && writer.u32(counters.droppedDecode) &&
                    writer.u32(counters.droppedCrc) && writer.u32(counters.droppedReassembly) &&
                    writer.u32(counters.droppedQueueFull);
    return ok ? writer.size() : 0U;
}

std::size_t packUsb(const UsbCounters& counters, std::uint8_t* output,
                    std::size_t capacity) noexcept {
    if (output == nullptr || capacity < kUsbPayloadSize) return 0U;

    Writer writer(output, capacity);
    bool ok = writer.u16(kSchemaVersion) && writer.u64(counters.framesRx) &&
              writer.u64(counters.framesTx) && writer.u64(counters.crcErrors) &&
              writer.u64(counters.decodeErrors) && writer.u64(counters.reassemblyRejected) &&
              writer.u64(counters.telemetryDropped);
    for (std::size_t i = 0U; ok && i < kUsbDropClassCount; ++i) {
        ok = writer.u64(counters.droppedByClass[i]);
    }
    ok = ok && writer.u64(counters.relayDownOk) && writer.u64(counters.relayDownUnbound) &&
         writer.u64(counters.relayDownNoPeer) && writer.u64(counters.relayDownOversized) &&
         writer.u64(counters.relayDownSendFailed);
    return ok ? writer.size() : 0U;
}

std::size_t packPeers(const PeerRecord* peers, std::size_t count, std::uint8_t* output,
                      std::size_t capacity) noexcept {
    if (output == nullptr) return 0U;
    if (peers == nullptr) count = 0U;
    if (count > kMaxPeers) count = kMaxPeers;

    const std::size_t needed = kPeersPayloadOverhead + kPeersPayloadBytesPerPeer * count;
    if (capacity < needed) return 0U;

    const std::uint16_t elementCount = static_cast<std::uint16_t>(count);
    Writer writer(output, capacity);
    bool ok = writer.u16(kSchemaVersion);

    // Field 1: channel. The index in this array *is* the channel -- see
    // PeerRecord's warning: a display index in arrival order, never an
    // identity. It travels anyway so that a client binding a single element
    // still knows which channel that element is.
    ok = ok && writer.u16(elementCount);
    for (std::size_t i = 0U; ok && i < count; ++i) {
        ok = writer.u8(static_cast<std::uint8_t>(i));
    }

    ok = ok && writer.u16(elementCount);
    for (std::size_t i = 0U; ok && i < count; ++i) ok = writer.u32(peers[i].sourceId);

    ok = ok && writer.u16(elementCount);
    for (std::size_t i = 0U; ok && i < count; ++i) ok = writer.u32(peers[i].bootId);

    // Flat 6 octets per peer: PACKED_LE has no fixed-width blob type, so the
    // MAC is a variable uint8 array whose count is 6 * peers.
    ok = ok && writer.u16(static_cast<std::uint16_t>(count * 6U));
    for (std::size_t i = 0U; ok && i < count; ++i) {
        for (std::size_t b = 0U; ok && b < 6U; ++b) ok = writer.u8(peers[i].mac[b]);
    }

    ok = ok && writer.u16(elementCount);
    for (std::size_t i = 0U; ok && i < count; ++i) ok = writer.u32(peers[i].lastSeenAgeMs);

    ok = ok && writer.u16(elementCount);
    for (std::size_t i = 0U; ok && i < count; ++i) {
        ok = writer.u8(peers[i].online ? 1U : 0U);
    }

    ok = ok && writer.u16(elementCount);
    for (std::size_t i = 0U; ok && i < count; ++i) {
        ok = writer.u8(static_cast<std::uint8_t>(peers[i].rssi));
    }

    ok = ok && writer.u16(elementCount);
    for (std::size_t i = 0U; ok && i < count; ++i) ok = writer.u32(peers[i].rttMs);

    return ok ? writer.size() : 0U;
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

const std::uint8_t* topicRecords(std::size_t* sizeOut, std::uint16_t* topicCountOut) noexcept {
    if (!g_recordsBuilt) {
        Writer writer(g_records, sizeof(g_records));
        std::uint16_t written = 0U;
        for (const TopicDesc& topic : kTopics) {
            if (!writeTopicRecord(writer, topic)) break;  // buffer too small; emit whole records only
            ++written;
        }
        g_recordsSize = writer.size();
        g_recordsTopicCount = written;
        g_recordsBuilt = true;
    }

    if (sizeOut != nullptr) *sizeOut = g_recordsSize;
    if (topicCountOut != nullptr) *topicCountOut = g_recordsTopicCount;
    return g_records;
}

bool lookupTopicMaxRateMillihz(std::uint16_t topicId, std::uint32_t* outMaxRateMillihz) noexcept {
    if (outMaxRateMillihz == nullptr) return false;
    for (const TopicDesc& topic : kTopics) {
        if (topic.topicId == topicId) {
            *outMaxRateMillihz = topic.maxRateMillihz;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Subscription state and publishing
// ---------------------------------------------------------------------------

void onLocalSubscribe(std::uint16_t topicId, std::uint32_t rateMillihz) noexcept {
    TopicState* state = stateFor(topicId);
    if (state == nullptr) return;

    // A zero rate should not reach here (SerialMux rejects a SUBSCRIBE with
    // requested_rate_millihz == 0 as INVALID_ARGUMENT), but falling back to
    // the schema's own maximum is the only answer that cannot turn into a
    // divide-by-zero or a publish-every-tick loop.
    const std::uint32_t effective = (rateMillihz != 0U) ? rateMillihz : schemaMaxRateFor(topicId);
    state->subscribed = true;
    state->periodMs = periodMsFromRate(effective);
    if (state->periodMs == 0U) state->periodMs = 1000U;
}

void onLocalUnsubscribe(std::uint16_t topicId) noexcept {
    TopicState* state = stateFor(topicId);
    if (state == nullptr) return;

    state->subscribed = false;
    state->published = false;
    if (topicId == kPeersTopicId) g_peersFingerprintValid = false;
}

bool hasSubscribers() noexcept {
    return g_link.subscribed || g_usb.subscribed || g_peers.subscribed;
}

void setSnapshotProvider(SnapshotFn provider) noexcept { g_snapshotProvider = provider; }

std::size_t tick(std::uint32_t nowMs, EmitFn emit) noexcept {
    if (emit == nullptr || g_snapshotProvider == nullptr) return 0U;
    // The "no subscriber costs nothing" guarantee, and the reason this check
    // comes before the provider call: gathering the snapshot is what reads the
    // ESP-NOW counters and walks the peer table.
    if (!hasSubscribers()) return 0U;

    Snapshot snapshot{};
    g_snapshotProvider(snapshot);

    std::size_t emitted = 0U;

    if (duePeriodic(g_link, nowMs)) {
        std::uint8_t payload[kLinkPayloadSize];
        const std::size_t size = packLink(snapshot.link, payload, sizeof(payload));
        if (size > 0U && publish(kLinkTopicId, payload, size, emit)) ++emitted;
        // The cadence advances even when the queue refused the frame: retrying
        // on the very next tick would fight SerialMux's backpressure instead
        // of respecting it, and the counters are cumulative so the next sample
        // loses nothing.
        g_link.lastPublishMs = nowMs;
        g_link.published = true;
    }

    if (duePeriodic(g_usb, nowMs)) {
        std::uint8_t payload[kUsbPayloadSize];
        const std::size_t size = packUsb(snapshot.usb, payload, sizeof(payload));
        if (size > 0U && publish(kUsbTopicId, payload, size, emit)) ++emitted;
        g_usb.lastPublishMs = nowMs;
        g_usb.published = true;
    }

    if (g_peers.subscribed) {
        const std::size_t count =
            (snapshot.peerCount < kMaxPeers) ? snapshot.peerCount : kMaxPeers;
        const std::uint32_t fingerprint = peersFingerprint(snapshot.peers, count);
        const bool changed = !g_peersFingerprintValid || (fingerprint != g_peersFingerprint);
        const std::uint32_t elapsed = nowMs - g_peers.lastPublishMs;

        // Both triggers stay inside the granted period, so "publish on change"
        // never pushes the effective rate above what SUBSCRIBE granted
        // (commands.md section 4). With the rate a client normally asks for
        // (the declared 2 Hz maximum) the period is 500 ms, which makes a
        // change visible within half a second and the keep-alive land at 1 Hz.
        const bool due = !g_peers.published ||
                         (elapsed >= g_peers.periodMs &&
                          (changed || elapsed >= kPeersKeepAliveMs));
        if (due) {
            std::uint8_t payload[kMaxPeersPayloadSize];
            const std::size_t size = packPeers(snapshot.peers, count, payload, sizeof(payload));
            if (size > 0U && publish(kPeersTopicId, payload, size, emit)) ++emitted;
            g_peers.lastPublishMs = nowMs;
            g_peers.published = true;
            g_peersFingerprint = fingerprint;
            g_peersFingerprintValid = true;
        }
    }

    return emitted;
}

void resetForTests() noexcept {
    g_link = TopicState{};
    g_usb = TopicState{};
    g_peers = TopicState{};
    g_peersFingerprint = 0U;
    g_peersFingerprintValid = false;
    g_snapshotProvider = nullptr;
}

}  // namespace DonglePublisher
