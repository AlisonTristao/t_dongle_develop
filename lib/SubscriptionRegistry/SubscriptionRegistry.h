#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief Dongle-side aggregator for BTP v1 SUBSCRIBE/UNSUBSCRIBE
 * (BTP/docs/commands.md section 4), topico 17.
 *
 * One row per (source_id, topic_id) a desktop client has ever asked this
 * dongle for. Each row keeps its own small table of desktop-side
 * subscriptions (PASSO 3/5: "agrega pedidos de clientes"). This dongle
 * currently exposes exactly one serial port, so at most one desktop session
 * exists at a time in practice -- but the table is keyed by an opaque
 * `clientId` rather than hardcoded to "the" session, so a second transport
 * (a future second CDC port, TCP bridge, ...) would not need a rewrite, only
 * a second caller passing a different clientId. Refcounting across those
 * client rows is what decides whether this dongle still needs to ask the
 * robot for a topic at all (PASSO 5's "remover so quando ninguem mais quer").
 *
 * Same shape/style as ManifestCache: a plain namespace with fixed-capacity
 * internal arrays (no dynamic allocation), pure C++ so it links into
 * env:native.
 *
 * What this module does NOT do: talk to EspNowManager/Serial directly. It
 * only decides *whether* an upstream SUBSCRIBE/UNSUBSCRIBE is now needed and
 * *what* it should ask for (see UpstreamAction); the caller (EspNowConfig)
 * performs the actual send and tells this module the ESP-NOW sequence it
 * used (noteUpstreamRequestSent) so the eventual SUBSCRIBE_RESULT/
 * UNSUBSCRIBE_RESULT (correlated only by reply_to_sequence on the wire, see
 * commands.md section 4 -- there is no topic_id in *_RESULT) can
 * be matched back to a row.
 */
namespace SubscriptionRegistry {

constexpr std::uint16_t kSubscribeObjectId = 0x0005U;
constexpr std::uint16_t kSubscribeResultObjectId = 0x0006U;
constexpr std::uint16_t kUnsubscribeObjectId = 0x0007U;
constexpr std::uint16_t kUnsubscribeResultObjectId = 0x0008U;

// (source_id, topic_id) rows this dongle tracks at once. Generous for a
// single-robot deployment with a handful of topics (bally_software has two
// today); bounded, no dynamic allocation, same rationale as
// ManifestCache::kCapacity.
constexpr std::size_t kMaxTopics = 8U;
// Desktop-side subscriptions aggregated per row. Today there is only ever
// one live serial session, so in practice this is 0 or 1 -- sized for a
// handful of future concurrent clients without needing a redesign.
constexpr std::size_t kMaxClientsPerTopic = 4U;

constexpr std::uint32_t kMinLeaseMs = 1000U;
constexpr std::uint32_t kMaxLeaseMs = 300000U;  // 5 minutes, mirrors bally_software's ceiling

// The upstream SUBSCRIBE is re-sent once this fraction of the granted lease
// has elapsed, so the robot's own lease never expires under a desktop client
// that is still renewing (commands.md section 4: "a subscription expires
// after its lease unless renewed by another SUBSCRIBE"). Halfway gives
// one free retry before the robot would drop the topic, without turning every
// desktop renewal into an ESP-NOW frame (topico 17 PASSO 7: rate limiting
// must not flood).
constexpr std::uint32_t kUpstreamRenewDivisor = 2U;

enum class UpstreamKind : std::uint8_t {
    None = 0,     // nothing to send; the robot's current state is already correct
    Subscribe,    // (re)assert this topic at `rateMillihz`/`leaseMs`: new topic, rate change (up OR down), or lease renewal
    Unsubscribe,  // last consumer of this topic is gone; release `upstreamSubscriptionId`
};

// What the caller must do upstream (over ESP-NOW, toward the robot) as a
// consequence of a desktop-facing call below. `sourceId`/`topicId` name the
// target; for a Subscribe action, `rateMillihz`/`leaseMs` are the union
// (highest) request across every remaining client of that row.
struct UpstreamAction {
    UpstreamKind kind = UpstreamKind::None;
    std::uint32_t sourceId = 0U;
    std::uint32_t topicId = 0U;
    std::uint32_t rateMillihz = 0U;             // Subscribe only
    std::uint32_t leaseMs = 0U;                 // Subscribe only
    std::uint32_t upstreamSubscriptionId = 0U;  // Unsubscribe only: robot-assigned id to release (0 if never granted)
};

struct DesktopSubscribeOutcome {
    // false only when this dongle's own local capacity (kMaxTopics /
    // kMaxClientsPerTopic) is exhausted -- the caller answers the desktop
    // with REJECTED/CAPACITY_EXHAUSTED, distinct from "topic unknown" (which
    // this module cannot detect; the caller checks ManifestCache first).
    bool accepted = false;
    std::uint32_t subscriptionId = 0U;  // dongle-local id, handed back to the desktop client
    UpstreamAction upstream{};          // kind == None when the robot already has the right orders
};

// clientId identifies the desktop session (see class comment). Repeating the
// same (clientId, sourceId, topicId, requestedRateMillihz, requestedLeaseMs)
// while that exact subscription is still active returns the same
// subscriptionId, matching commands.md section 4's "returns the same
// subscription rather than creating another".
DesktopSubscribeOutcome onDesktopSubscribe(std::uint32_t clientId, std::uint32_t sourceId, std::uint32_t topicId,
                                           std::uint32_t requestedRateMillihz, std::uint32_t requestedLeaseMs,
                                           std::uint32_t nowMs) noexcept;

struct DesktopUnsubscribeOutcome {
    // Matches commands.md section 4's "removing an already-absent
    // subscription returns SUCCESS/NONE" -- `found` is informational only,
    // never turned into an error by the caller.
    bool found = false;
    // Unsubscribe only when the row lost its last consumer; Subscribe (with a
    // lower rateMillihz) when other consumers remain but the departing one was
    // the fastest -- topico 17 PASSO 5 is "remover somente quando nao houver
    // outro consumidor", not "stop adjusting the rate".
    UpstreamAction upstream{};
};

DesktopUnsubscribeOutcome onDesktopUnsubscribe(std::uint32_t clientId, std::uint32_t subscriptionId,
                                               std::uint32_t nowMs) noexcept;

// PASSO 6 (session disconnect): clears every desktop subscription clientId
// owns across all rows. Writes up to maxOut UpstreamAction entries into out
// (Unsubscribe for rows that lost their last consumer, Subscribe for rows
// whose union rate merely dropped); returns how many were written.
std::size_t onClientDisconnected(std::uint32_t clientId, std::uint32_t nowMs, UpstreamAction* out,
                                 std::size_t maxOut) noexcept;

// Per-tick maintenance: drops client subscriptions whose lease expired at
// nowMs, and re-asserts (renews) the upstream SUBSCRIBE of any row past
// kUpstreamRenewDivisor of its granted lease. Same output contract as
// onClientDisconnected. Call once per SerialMux tick.
std::size_t sweep(std::uint32_t nowMs, UpstreamAction* out, std::size_t maxOut) noexcept;

// Called immediately after sending an upstream SUBSCRIBE/UNSUBSCRIBE, so the
// eventual *_RESULT (correlated only by reply_to_sequence) can be matched
// back to this row.
void noteUpstreamRequestSent(std::uint32_t sourceId, std::uint32_t topicId, std::uint32_t sequence) noexcept;

// Correlates an incoming SUBSCRIBE_RESULT by replyToSequence and, on
// SUCCESS, records the robot's actual grant -- both effectiveRateMillihz
// (used for STATUS reporting and as this dongle's own effective-rate ceiling
// going forward) and the robot-assigned upstreamSubscriptionId (needed to
// address a later upstream UNSUBSCRIBE at the right subscription -- see
// commands.md section 4's UNSUBSCRIBE payload, which targets a
// subscription_id, not a topic_id). Returns false when no row's last
// outstanding request matches (stale/unsolicited reply, silently ignored by
// the caller).
bool onUpstreamSubscribeResult(std::uint32_t replyToSequence, std::uint8_t status,
                               std::uint32_t upstreamSubscriptionId, std::uint32_t effectiveRateMillihz) noexcept;

// The robot-assigned subscription_id last granted for (sourceId, topicId),
// or 0 if none is currently known (never subscribed upstream yet, or the
// grant was cleared by an unsubscribe/lease expiry). Used to build the
// upstream UNSUBSCRIBE payload.
std::uint32_t upstreamSubscriptionId(std::uint32_t sourceId, std::uint32_t topicId) noexcept;

// PASSO 8: forwarding counters, fed by whichever relay path actually moves
// TELEMETRY bytes for that topic (SerialMux::forwardRelay).
void recordForwarded(std::uint32_t sourceId, std::uint32_t topicId, std::size_t payloadBytes) noexcept;
void recordDropped(std::uint32_t sourceId, std::uint32_t topicId) noexcept;

// True when (sourceId, topicId) currently has at least one active desktop
// subscriber. Gates SerialMux::forwardRelay (PASSO 3/5: only relay what is
// actually wanted) -- a topic with zero subscribers is simply not forwarded,
// which is also how "closing the last chart stops the traffic" happens on
// this leg (the desktop-facing UNSUBSCRIBE already stopped the upstream ask;
// this is the belt-and-suspenders local gate in case the upstream UNSUBSCRIBE
// hasn't landed yet).
bool isWanted(std::uint32_t sourceId, std::uint32_t topicId) noexcept;

// True only when this registry KNOWS about (sourceId, topicId) and every
// subscriber for it has gone away. An unknown pair answers false, which is the
// whole difference from !isWanted().
//
// That difference is what the relay gate needs once subscriptions live on the
// endpoint channel instead of this one. A desktop that subscribes directly at
// a robot never tells this dongle about it, so every topic looks unknown here
// -- and !isWanted() would then drop ALL telemetry rather than the topic
// nobody asked for. Relaying by default is also the rule the hub follows
// everywhere else: what the hub was not told about goes up, where it is
// visible, instead of disappearing here.
//
// What this still catches is the case the gate was added for: a sample that
// was already in flight when the last chart closed, for a topic this dongle
// does know about. It also keeps working unchanged for topics subscribed
// through the dongle itself, such as its own hub.* ones.
bool isKnownUnwanted(std::uint32_t sourceId, std::uint32_t topicId) noexcept;

struct TopicStatusEntry {
    std::uint32_t sourceId = 0U;
    std::uint16_t topicId = 0U;
    std::uint16_t subscriberCount = 0U;
    std::uint32_t effectiveRateMillihz = 0U;
    std::uint64_t bytesTotal = 0U;
    std::uint64_t samplesDroppedTotal = 0U;
};

// Snapshot for STATUS v2 (commands.md section 5.1). Only rows
// that have ever seen at least one desktop subscription are included.
std::size_t topicStatusSnapshot(TopicStatusEntry* out, std::size_t maxOut) noexcept;

// Test-only: resets every row to the fresh-boot state. Declared here (not
// hidden in an anonymous namespace) so env:native tests can isolate cases
// from each other; production code never calls this.
void resetForTests() noexcept;

}  // namespace SubscriptionRegistry
