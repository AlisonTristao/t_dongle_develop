#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief Dongle-side aggregator for BTP v1 SUBSCRIBE/UNSUBSCRIBE
 * (bally_protocol/docs/COMMANDS_AND_ACTIONS.md section 7), topico 17.
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
 * COMMANDS_AND_ACTIONS.md section 7 -- there is no topic_id in *_RESULT) can
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

// What the caller must do upstream (over ESP-NOW, toward the robot) as a
// consequence of a desktop-facing call below. `sourceId`/`topicId` name the
// target; for a Subscribe action, `rateMillihz`/`leaseMs` are the union
// (highest) request across every remaining client of that row.
struct UpstreamAction {
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
    bool needsUpstreamSubscribe = false;
    UpstreamAction upstream{};
};

// clientId identifies the desktop session (see class comment). Repeating the
// same (clientId, sourceId, topicId, requestedRateMillihz, requestedLeaseMs)
// while that exact subscription is still active returns the same
// subscriptionId, matching COMMANDS_AND_ACTIONS.md section 7's "retorna a
// mesma assinatura sem criar outra".
DesktopSubscribeOutcome onDesktopSubscribe(std::uint32_t clientId, std::uint32_t sourceId, std::uint32_t topicId,
                                           std::uint32_t requestedRateMillihz, std::uint32_t requestedLeaseMs,
                                           std::uint32_t nowMs) noexcept;

struct DesktopUnsubscribeOutcome {
    // Matches COMMANDS_AND_ACTIONS.md section 7's "remover uma assinatura ja
    // ausente retorna SUCCESS/NONE" -- `found` is informational only, never
    // turned into an error by the caller.
    bool found = false;
    bool needsUpstreamUnsubscribe = false;
    UpstreamAction upstream{};
};

DesktopUnsubscribeOutcome onDesktopUnsubscribe(std::uint32_t clientId, std::uint32_t subscriptionId) noexcept;

// PASSO 6 (session disconnect): clears every desktop subscription clientId
// owns across all rows. Writes up to maxOut UpstreamAction entries (topics
// whose refcount just hit zero) into out; returns how many were written.
std::size_t onClientDisconnected(std::uint32_t clientId, UpstreamAction* out, std::size_t maxOut) noexcept;

// Sweeps every row for client leases past nowMs; same output contract as
// onClientDisconnected. Call once per SerialMux tick.
std::size_t expireLeases(std::uint32_t nowMs, UpstreamAction* out, std::size_t maxOut) noexcept;

// Called immediately after sending an upstream SUBSCRIBE/UNSUBSCRIBE, so the
// eventual *_RESULT (correlated only by reply_to_sequence) can be matched
// back to this row.
void noteUpstreamRequestSent(std::uint32_t sourceId, std::uint32_t topicId, std::uint32_t sequence) noexcept;

// Correlates an incoming SUBSCRIBE_RESULT by replyToSequence and, on
// SUCCESS, records the robot's actual grant -- both effectiveRateMillihz
// (used for STATUS reporting and as this dongle's own effective-rate ceiling
// going forward) and the robot-assigned upstreamSubscriptionId (needed to
// address a later upstream UNSUBSCRIBE at the right subscription -- see
// COMMANDS_AND_ACTIONS.md section 7's UNSUBSCRIBE payload, which targets a
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

struct TopicStatusEntry {
    std::uint32_t sourceId = 0U;
    std::uint16_t topicId = 0U;
    std::uint16_t subscriberCount = 0U;
    std::uint32_t effectiveRateMillihz = 0U;
    std::uint64_t bytesTotal = 0U;
    std::uint64_t samplesDroppedTotal = 0U;
};

// Snapshot for STATUS v2 (COMMANDS_AND_ACTIONS.md section 8.1). Only rows
// that have ever seen at least one desktop subscription are included.
std::size_t topicStatusSnapshot(TopicStatusEntry* out, std::size_t maxOut) noexcept;

// Test-only: resets every row to the fresh-boot state. Declared here (not
// hidden in an anonymous namespace) so env:native tests can isolate cases
// from each other; production code never calls this.
void resetForTests() noexcept;

}  // namespace SubscriptionRegistry
