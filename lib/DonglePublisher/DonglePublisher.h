#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief This dongle's own BTP telemetry: the three `hub.*` topics it
 * publishes about itself, plus the topic records that describe them in its
 * own manifest (topico 27).
 *
 * Before this, the dongle was a cable: it relayed robot telemetry and had no
 * way to say anything about its own health. It now behaves like any other BTP
 * source -- a manifest with topics, subscribable, published only while
 * somebody is subscribed -- so a desktop client can plot the dongle itself
 * with no client-side change at all. Nothing here is a new message, a new
 * object_id or a new BTP field: peer discovery in particular is just the
 * `hub.peers` topic, not a protocol extension (PLANO_GERAL decision D9).
 *
 * Mirrors bally_software's lib/TelemetryPublisher + lib/ManifestResponder
 * split rather than inventing a second design: one static schema table is the
 * single source of truth for both the packers and the manifest records, so
 * the manifest can never drift from the bytes actually put on the wire.
 * Unlike the robot's version there is no queue and no flush() here -- the
 * outbound path already exists (SerialMux's per-priority-class queues), so
 * this module hands finished payloads straight to the injected EmitFn and
 * SerialMux's existing enqueueOwn() does the framing.
 *
 * Pure C++ (no Arduino/FreeRTOS), same shape as ManifestCache/
 * SubscriptionRegistry/BtpTransport: a plain namespace over fixed-capacity
 * static state, no dynamic allocation, so it links and is unit-testable
 * under env:native (test/test_dongle_publisher).
 *
 * Layering: this library must not include ManifestCache.h or SerialMux.h --
 * ManifestCache includes *this* (to serve the dongle's own topic records) and
 * SerialMux includes both, so the dependency edge only ever points inward
 * (CONTRIBUTING.md section 3). The two things this module cannot reach on its
 * own are therefore injected: the counters/peer table live behind
 * setSnapshotProvider() (bound by AppRuntime, exactly like
 * EspNowCommands::setStatsProvider) and the wire behind tick()'s EmitFn
 * (bound by SerialMux).
 */
namespace DonglePublisher {

// ---------------------------------------------------------------------------
// Topic ids
// ---------------------------------------------------------------------------
// A topic_id is local to a source's namespace (telemetry.md section 1: "Two
// different sources may use the same number for unrelated topics"), so a
// collision with a robot's ids is impossible on the wire and the choice below
// is not about avoiding one. It is about the humans: a desktop client shows
// the robots' catalogs and the dongle's side by side, and a dongle topic
// numbered 0x0003 would read like "the third robot topic" in every screenshot
// and bug report.
//
// 0x0F00-0x0FFF is therefore reserved for `hub.*` -- topics whose subject is
// this dongle itself. Far above the low block the robots actually use
// (bally_software: 0x0001 protocol.test, 0x0002 robot.state), with the rest of
// the block free for the relay/bind topics of topicos 28-31 so they stay
// visibly part of the same family instead of being scattered.
constexpr std::uint16_t kHubTopicBlockBase = 0x0F00U;
constexpr std::uint16_t kHubTopicBlockEnd = 0x0FFFU;

constexpr std::uint16_t kLinkTopicId = kHubTopicBlockBase + 0x01U;   // hub.link
constexpr std::uint16_t kUsbTopicId = kHubTopicBlockBase + 0x02U;    // hub.usb
constexpr std::uint16_t kPeersTopicId = kHubTopicBlockBase + 0x03U;  // hub.peers

// Version 2 adds the five downstream-relay outcome counters to hub.usb.
// Any change to a layout or to the meaning of a field needs a new value here
// *and* a bump of ManifestCache::kSelfConfigRevision (commands.md section
// 3.3), otherwise a client can retain and decode the old layout as if it were
// still current.
constexpr std::uint16_t kSchemaVersion = 2U;

// Peers this dongle can describe at once. Must equal
// BtpTransport::kPeerIdentityCapacity -- static_assert'ed in the .cpp -- and
// is what hub.peers declares as `max_element_count` for every one of its
// variable arrays.
constexpr std::size_t kMaxPeers = 16U;

// Priority classes SerialMux splits its TX drops across. Must equal
// SerialSession::PriorityClass::kCount -- static_assert'ed in SerialMux.cpp,
// which owns that enum; duplicating the number here (instead of including
// SerialSession.h) is what keeps this library free of any dependency that
// points back at SerialMux.
constexpr std::size_t kUsbDropClassCount = 4U;

// ---------------------------------------------------------------------------
// Samples
// ---------------------------------------------------------------------------

/**
 * @brief hub.link body: the ESP-NOW ingress counters, field-for-field the
 * same twelve values as EspNowConfig::RxCounters and in the same order.
 *
 * Deliberately a separate struct instead of reusing RxCounters: that header
 * pulls in Arduino/EspNowManager, and this library has to stay native-buildable.
 * AppRuntime copies field by field, the same way it already feeds
 * EspNowCommands::StatsSnapshot from the same source.
 *
 * All twelve are cumulative since boot and monotonic, so a client plots the
 * *difference* between two samples. That is the whole reason these are worth
 * publishing: `espnow -stats` could already print them to a console nobody
 * was watching, and STATUS collapses every loss into one `frames_dropped`,
 * so until now there was no way to see *which* stage lost traffic over time.
 */
struct LinkCounters {
    std::uint32_t datagrams;
    std::uint32_t fragmentsAccepted;
    std::uint32_t routedTelemetry;
    std::uint32_t routedLog;
    std::uint32_t routedCommand;
    std::uint32_t routedControl;
    std::uint32_t routedTerminal;
    std::uint32_t droppedRx;
    std::uint32_t droppedDecode;
    std::uint32_t droppedCrc;
    std::uint32_t droppedReassembly;
    std::uint32_t droppedQueueFull;
};

/**
 * @brief hub.usb body: the dongle -> desktop counters, field-for-field
 * SerialMux::TxCounters.
 *
 * `droppedByClass` stays an array of four and is never summed. That split is
 * the entire diagnostic value of this topic: a drop in the kTelemetry class
 * means the link is saturated, a drop in kSession/kTerminal means the main
 * loop stalled, and those two point at opposite fixes. A single total says
 * only "something was lost".
 */
struct UsbCounters {
    std::uint64_t framesRx;
    std::uint64_t framesTx;
    std::uint64_t crcErrors;
    std::uint64_t decodeErrors;
    std::uint64_t reassemblyRejected;
    std::uint64_t telemetryDropped;
    std::uint64_t droppedByClass[kUsbDropClassCount];
    // Downstream relay outcomes by reason (SerialMux::TxCounters). Published
    // because "my command never reaches the robot" is otherwise undiagnosable
    // from the desktop: unbound says the binding is missing, no_peer says the
    // robot has not been heard from, oversized says the child encoded on the
    // wrong transport profile. Each one points at a different repository.
    std::uint64_t relayDownOk;
    std::uint64_t relayDownUnbound;
    std::uint64_t relayDownNoPeer;
    std::uint64_t relayDownOversized;
    std::uint64_t relayDownSendFailed;
};

/**
 * @brief One hub.peers entry, already reduced to wire values.
 *
 * ================== READ THIS BEFORE USING `channel` ==================
 * The `channel` published for a peer is a DISPLAY INDEX that this dongle
 * assigns in the order it first hears each peer. It is NOT an identity, and
 * it is NOT stable across dongle reboots: reboot the dongle, let the robots
 * come up in a different order, and channel 0 now names a different robot.
 * The address of a peer is `sourceId` (with `bootId` distinguishing one boot
 * of that robot from the next).
 *
 * The consequence of confusing the two is silent and expensive: anything that
 * persists a *selection* -- a saved chart, a field binding, the bind table a
 * later topico adds -- and persists the channel instead of the sourceId will
 * come back after a reboot pointing at a different robot, plotting the wrong
 * data, with no error raised anywhere.
 *
 * This topic only ever PUBLISHES the index, as a convenience for a UI that
 * wants short labels. Nothing in this module treats it as identity, and
 * nothing downstream should either.
 * ======================================================================
 */
struct PeerRecord {
    std::uint32_t sourceId;
    std::uint32_t bootId;
    std::uint8_t mac[6];
    // Wire field `last_seen_ms`: age, in milliseconds, since the last frame
    // received from this peer -- not an absolute timestamp. An age is what a
    // client can use directly; an absolute millis() value would be relative
    // to a boot the client knows nothing about.
    std::uint32_t lastSeenAgeMs;
    // Topico 30: means "authenticated", not "heard" -- BtpTransport::
    // notePeerLinkResult's latest verdict for this MAC, which now comes from
    // two combined signals, both gated on channel C's key L: this dongle's
    // own sealed heartbeat getting a radio-layer ACK (necessary but not
    // proof the peer holds L too -- it never replies), AND, more decisively,
    // the last CONSUMED inbound frame from that peer actually opening under
    // L (RadioSeal::open in EspNowConfig.cpp) -- a peer that cannot produce
    // one is marked not-online even while its heartbeat keeps ACKing. Before
    // this topico this bit only ever reflected the heartbeat's radio ACK,
    // which is why an unauthenticated peer could still show up here; it
    // cannot any more. False for a peer that has never been probed or
    // opened -- absence of evidence, not evidence of absence, which is why
    // `lastSeenAgeMs` (unaffected by any of this -- see
    // rememberAuthenticatedPeer's own
    // comment) is still the field to judge mere freshness by.
    bool online;
};

/** Everything one publish cycle needs, gathered in one call (see
 * setSnapshotProvider). */
struct Snapshot {
    LinkCounters link;
    UsbCounters usb;
    PeerRecord peers[kMaxPeers];
    std::size_t peerCount;
};

// ---------------------------------------------------------------------------
// Payload sizes
// ---------------------------------------------------------------------------
// Every payload is `schema_version:uint16_le` followed by a PACKED_LE body
// (telemetry.md sections 2 and 4.1). None of the fields is nullable, so no
// payload carries a presence bitmap.

constexpr std::size_t kSchemaVersionPrefixSize = 2U;
constexpr std::size_t kLinkPayloadSize = kSchemaVersionPrefixSize + 12U * 4U;  // 50
constexpr std::size_t kUsbPayloadSize =
    kSchemaVersionPrefixSize + 6U * 8U + kUsbDropClassCount * 8U + 5U * 8U;  // 122

// hub.peers is six variable arrays, each prefixed by its own
// `element_count:uint16_le` (telemetry.md 4.1). Per peer that is
// 1 (channel) + 4 (source_id) + 4 (boot_id) + 6 (mac) + 4 (last_seen_ms)
// + 1 (online) = 20 octets, on top of a fixed 2 + 6*2 = 14.
constexpr std::size_t kPeersPayloadOverhead = kSchemaVersionPrefixSize + 6U * 2U;  // 14
constexpr std::size_t kPeersPayloadBytesPerPeer = 20U;
constexpr std::size_t kMaxPeersPayloadSize =
    kPeersPayloadOverhead + kPeersPayloadBytesPerPeer * kMaxPeers;  // 334

// ---------------------------------------------------------------------------
// Serialization (pure, no state -- what env:native covers)
// ---------------------------------------------------------------------------

/** Writes the hub.link payload. Returns bytes written, or 0 if `capacity`
 * is smaller than kLinkPayloadSize. */
std::size_t packLink(const LinkCounters& counters, std::uint8_t* output,
                     std::size_t capacity) noexcept;

/** Writes the hub.usb payload. Returns bytes written, or 0 on capacity. */
std::size_t packUsb(const UsbCounters& counters, std::uint8_t* output,
                    std::size_t capacity) noexcept;

/** Writes the hub.peers payload for the first `count` records (capped at
 * kMaxPeers). `channel` is emitted as the record's own index in this array,
 * i.e. arrival order -- see PeerRecord's warning. Returns bytes written, or 0
 * on capacity. A count of zero is valid and yields the 14-octet empty form. */
std::size_t packPeers(const PeerRecord* peers, std::size_t count, std::uint8_t* output,
                      std::size_t capacity) noexcept;

// ---------------------------------------------------------------------------
// Manifest (commands.md section 3.3)
// ---------------------------------------------------------------------------

/**
 * @brief The dongle's own topic records: `topic_count` records, each already
 * prefixed with its own `record_size:uint32_le`, concatenated exactly as
 * MANIFEST_DATA wants them and sorted by ascending topic_id.
 *
 * Built once, on first call, into a static buffer (no allocation; the table
 * it is built from is compile-time constant, so the bytes never change during
 * a run). ManifestCache hands this straight to its self entry, which is why
 * the dongle needs no separate "responder" of its own: the existing
 * MANIFEST_REQUEST path -- enumeration with target_source_id = 0 and a
 * targeted request for BtpTransport::sourceId() -- already answers from that
 * entry.
 *
 * Never returns nullptr; *sizeOut is 0 only if the static buffer were too
 * small, which is a compile-time-checked impossibility.
 */
const std::uint8_t* topicRecords(std::size_t* sizeOut, std::uint16_t* topicCountOut) noexcept;

/** The declared `max_rate_millihz` of one hub.* topic. False when topicId is
 * not one of this dongle's own. Read from the same schema table the manifest
 * is built from, not by re-parsing the records, so the two cannot disagree. */
bool lookupTopicMaxRateMillihz(std::uint16_t topicId, std::uint32_t* outMaxRateMillihz) noexcept;

// ---------------------------------------------------------------------------
// Subscription state and publishing
// ---------------------------------------------------------------------------

/**
 * @brief Applies a subscription decision SubscriptionRegistry already made
 * for one of this dongle's own topics.
 *
 * A hub.* topic has no radio hop: the "upstream" producer of hub.link is this
 * very firmware. SerialMux therefore routes any UpstreamAction whose
 * sourceId equals BtpTransport::sourceId() here instead of to
 * EspNowConfig::requestUpstreamSubscribe (which would look up a MAC for the
 * dongle itself and find none). `rateMillihz` is the registry's union rate
 * over every desktop subscriber of that topic, already clamped to the
 * schema's declared maximum by SerialMux's SUBSCRIBE handler -- so honouring
 * it here is what keeps the effective rate at or below what was granted
 * (commands.md section 4).
 */
void onLocalSubscribe(std::uint16_t topicId, std::uint32_t rateMillihz) noexcept;

/** Last subscriber of a hub.* topic is gone (unsubscribe, lease expiry or
 * session end). The topic stops being published and stops costing anything. */
void onLocalUnsubscribe(std::uint16_t topicId) noexcept;

/** True while at least one hub.* topic has a subscriber. */
bool hasSubscribers() noexcept;

/** Hands one finished payload to the wire. Returns false when it could not be
 * queued (counted as a dropped sample). Bound by SerialMux to its existing
 * enqueueOwn(), so this module never becomes a second TX path. */
using EmitFn = bool (*)(std::uint16_t topicId, const std::uint8_t* payload, std::size_t size);

/** Fills `out` with the current counters and peer table. */
using SnapshotFn = void (*)(Snapshot& out);

void setSnapshotProvider(SnapshotFn provider) noexcept;

/**
 * @brief Publishes whatever is due at `nowMs`.
 *
 * Called once per SerialMux tick, and only while a session is protocolled --
 * SerialMux's own gate, which is also why this module never has to ask about
 * session state. Returns immediately, without even calling the snapshot
 * provider, when no hub.* topic has a subscriber: an unsubscribed topic must
 * cost neither radio nor cable, and reading the counters and walking the peer
 * table is exactly the cost being avoided.
 *
 * @return how many payloads were emitted (0, 1, 2 or 3).
 */
std::size_t tick(std::uint32_t nowMs, EmitFn emit) noexcept;

/** Test-only: back to the fresh-boot state (no subscribers, nothing
 * published yet). Declared here, like SubscriptionRegistry::resetForTests, so
 * env:native cases can isolate each other; production code never calls it. */
void resetForTests() noexcept;

}  // namespace DonglePublisher
