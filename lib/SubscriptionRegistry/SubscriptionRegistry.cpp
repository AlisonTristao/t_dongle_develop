#include "SubscriptionRegistry.h"

namespace SubscriptionRegistry {
namespace {

struct ClientSub {
    bool used = false;
    std::uint32_t clientId = 0U;
    std::uint32_t subscriptionId = 0U;  // dongle-local id handed back to the desktop
    std::uint32_t requestedRateMillihz = 0U;
    std::uint32_t requestedLeaseMs = 0U;
    std::uint32_t leaseDeadlineMs = 0U;
};

struct TopicRow {
    bool used = false;
    std::uint32_t sourceId = 0U;
    std::uint32_t topicId = 0U;
    ClientSub clients[kMaxClientsPerTopic];

    // Upstream (dongle -> robot) state.
    bool upstreamActive = false;                // an upstream SUBSCRIBE is currently asserted for this row
    std::uint32_t pendingRequestSequence = 0U;  // 0 = none outstanding
    std::uint32_t upstreamRequestedRateMillihz = 0U;
    std::uint32_t upstreamLeaseMs = 0U;         // lease last asked of the robot
    std::uint32_t upstreamRenewAtMs = 0U;       // re-send the SUBSCRIBE once nowMs reaches this
    std::uint32_t upstreamSubscriptionId = 0U;  // robot-assigned id, needed to build UNSUBSCRIBE
    std::uint32_t effectiveRateMillihz = 0U;  // last grant (or 0 if never granted / cleared)

    // PASSO 8 counters, monotonic for this row's lifetime (never reset when
    // refcount hits zero -- only a dongle reboot resets them, same
    // "monotonic, saturates" convention as STATUS section 5).
    std::uint64_t bytesTotal = 0U;
    std::uint64_t samplesDroppedTotal = 0U;
};

TopicRow g_rows[kMaxTopics];
std::uint32_t g_nextSubscriptionId = 1U;

std::uint32_t nextSubscriptionId() noexcept {
    const std::uint32_t id = g_nextSubscriptionId++;
    if (g_nextSubscriptionId == 0U) g_nextSubscriptionId = 1U;  // never 0
    return id;
}

int findRow(std::uint32_t sourceId, std::uint32_t topicId) noexcept {
    for (std::size_t i = 0U; i < kMaxTopics; ++i) {
        if (g_rows[i].used && g_rows[i].sourceId == sourceId && g_rows[i].topicId == topicId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int findOrCreateRow(std::uint32_t sourceId, std::uint32_t topicId) noexcept {
    const int existing = findRow(sourceId, topicId);
    if (existing >= 0) return existing;

    for (std::size_t i = 0U; i < kMaxTopics; ++i) {
        if (!g_rows[i].used) {
            g_rows[i] = TopicRow{};
            g_rows[i].used = true;
            g_rows[i].sourceId = sourceId;
            g_rows[i].topicId = topicId;
            return static_cast<int>(i);
        }
    }
    return -1;  // capacity exhausted
}

std::uint16_t refcount(const TopicRow& row) noexcept {
    std::uint16_t count = 0U;
    for (std::size_t i = 0U; i < kMaxClientsPerTopic; ++i) {
        if (row.clients[i].used) ++count;
    }
    return count;
}

std::uint32_t unionRateMillihz(const TopicRow& row) noexcept {
    std::uint32_t maxRate = 0U;
    for (std::size_t i = 0U; i < kMaxClientsPerTopic; ++i) {
        if (row.clients[i].used && row.clients[i].requestedRateMillihz > maxRate) {
            maxRate = row.clients[i].requestedRateMillihz;
        }
    }
    return maxRate;
}

std::uint32_t unionLeaseMs(const TopicRow& row) noexcept {
    std::uint32_t maxLease = 0U;
    for (std::size_t i = 0U; i < kMaxClientsPerTopic; ++i) {
        if (row.clients[i].used && row.clients[i].requestedLeaseMs > maxLease) {
            maxLease = row.clients[i].requestedLeaseMs;
        }
    }
    return maxLease;
}

std::uint32_t clampLease(std::uint32_t requestedLeaseMs) noexcept {
    if (requestedLeaseMs < kMinLeaseMs) return kMinLeaseMs;
    if (requestedLeaseMs > kMaxLeaseMs) return kMaxLeaseMs;
    return requestedLeaseMs;
}

// The single decision point for "what, if anything, must this dongle now tell
// the robot about this topic". Every desktop-facing entry point below funnels
// through here after mutating a row, so the aggregation rules exist once:
//
//   * no consumers left  -> Unsubscribe, but only if we had actually asserted
//     a SUBSCRIBE upstream (PASSO 5: "remover assinatura somente quando nao
//     houver outro consumidor" -- a row that still has one live client never
//     produces an Unsubscribe, no matter how many others just left);
//   * consumers left     -> a single aggregated Subscribe carrying the union
//     (highest) rate and lease across them, emitted when the row is newly
//     active, when that union changed in EITHER direction (a departing fast
//     client must lower the robot's rate, not leave it pinned high), or when
//     the granted lease is half spent and needs renewing;
//   * otherwise          -> None, so a plain re-assertion of an unchanged
//     union costs no ESP-NOW traffic at all.
//
// Mutates the row's upstream bookkeeping to match whatever it returns, so the
// caller only has to send the frame.
UpstreamAction evaluateUpstream(TopicRow& row, std::uint32_t nowMs) noexcept {
    UpstreamAction action{};
    action.sourceId = row.sourceId;
    action.topicId = row.topicId;

    if (refcount(row) == 0U) {
        if (!row.upstreamActive) return action;  // kind stays None: nothing was ever asserted
        action.kind = UpstreamKind::Unsubscribe;
        action.upstreamSubscriptionId = row.upstreamSubscriptionId;
        row.upstreamActive = false;
        row.upstreamRequestedRateMillihz = 0U;
        row.upstreamLeaseMs = 0U;
        row.upstreamRenewAtMs = 0U;
        row.effectiveRateMillihz = 0U;
        row.upstreamSubscriptionId = 0U;
        return action;
    }

    const std::uint32_t rate = unionRateMillihz(row);
    const std::uint32_t lease = unionLeaseMs(row);
    const bool changed = !row.upstreamActive || rate != row.upstreamRequestedRateMillihz ||
                         lease != row.upstreamLeaseMs;
    // Unsigned wrap-safe comparison: millis() rolls over after ~49 days, so
    // compare the elapsed difference rather than the absolute instants.
    const bool dueForRenewal = row.upstreamActive && (nowMs - row.upstreamRenewAtMs) < 0x80000000U;
    if (!changed && !dueForRenewal) return action;  // kind stays None

    action.kind = UpstreamKind::Subscribe;
    action.rateMillihz = rate;
    action.leaseMs = lease;
    row.upstreamActive = true;
    row.upstreamRequestedRateMillihz = rate;
    row.upstreamLeaseMs = lease;
    row.upstreamRenewAtMs = nowMs + (lease / kUpstreamRenewDivisor);
    return action;
}

}  // namespace

DesktopSubscribeOutcome onDesktopSubscribe(std::uint32_t clientId, std::uint32_t sourceId, std::uint32_t topicId,
                                           std::uint32_t requestedRateMillihz, std::uint32_t requestedLeaseMs,
                                           std::uint32_t nowMs) noexcept {
    DesktopSubscribeOutcome outcome{};

    const int rowIndex = findOrCreateRow(sourceId, topicId);
    if (rowIndex < 0) return outcome;  // accepted stays false: no room for a new topic row
    TopicRow& row = g_rows[static_cast<std::size_t>(rowIndex)];

    const std::uint32_t clampedLeaseMs = clampLease(requestedLeaseMs);
    const std::uint32_t leaseDeadlineMs = nowMs + clampedLeaseMs;

    // Idempotent retry: same client, same topic, same requested rate/lease,
    // subscription still active -> same subscriptionId, no new row entry.
    for (std::size_t i = 0U; i < kMaxClientsPerTopic; ++i) {
        ClientSub& client = row.clients[i];
        if (client.used && client.clientId == clientId &&
            client.requestedRateMillihz == requestedRateMillihz && client.requestedLeaseMs == clampedLeaseMs) {
            client.leaseDeadlineMs = leaseDeadlineMs;  // renew
            outcome.accepted = true;
            outcome.subscriptionId = client.subscriptionId;
            // The union is unchanged, so evaluateUpstream only emits anything
            // here when the robot's own lease is half spent -- i.e. a desktop
            // renewal is what propagates upstream as a renewal, but not one
            // ESP-NOW frame per retry.
            outcome.upstream = evaluateUpstream(row, nowMs);
            return outcome;
        }
    }

    // Same client re-subscribing to the same topic with a *different*
    // rate/lease replaces its row entry atomically (commands.md
    // section 4: "a new sequence atomically creates or replaces the
    // subscription for that session and topic").
    int slot = -1;
    for (std::size_t i = 0U; i < kMaxClientsPerTopic; ++i) {
        if (row.clients[i].used && row.clients[i].clientId == clientId) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0) {
        for (std::size_t i = 0U; i < kMaxClientsPerTopic; ++i) {
            if (!row.clients[i].used) {
                slot = static_cast<int>(i);
                break;
            }
        }
    }
    if (slot < 0) return outcome;  // accepted stays false: this row's client table is full

    ClientSub& client = row.clients[static_cast<std::size_t>(slot)];
    client.used = true;
    client.clientId = clientId;
    client.subscriptionId = nextSubscriptionId();
    client.requestedRateMillihz = requestedRateMillihz;
    client.requestedLeaseMs = clampedLeaseMs;
    client.leaseDeadlineMs = leaseDeadlineMs;

    outcome.accepted = true;
    outcome.subscriptionId = client.subscriptionId;
    outcome.upstream = evaluateUpstream(row, nowMs);
    return outcome;
}

DesktopUnsubscribeOutcome onDesktopUnsubscribe(std::uint32_t clientId, std::uint32_t subscriptionId,
                                               std::uint32_t nowMs) noexcept {
    DesktopUnsubscribeOutcome outcome{};

    for (std::size_t r = 0U; r < kMaxTopics; ++r) {
        TopicRow& row = g_rows[r];
        if (!row.used) continue;
        for (std::size_t i = 0U; i < kMaxClientsPerTopic; ++i) {
            ClientSub& client = row.clients[i];
            if (client.used && client.clientId == clientId && client.subscriptionId == subscriptionId) {
                client = ClientSub{};
                outcome.found = true;
                outcome.upstream = evaluateUpstream(row, nowMs);
                return outcome;
            }
        }
    }
    // Not found anywhere: commands.md section 4 treats this as
    // SUCCESS/NONE (idempotent retry), not an error -- outcome.found stays
    // false and the caller does not turn that into a wire error.
    return outcome;
}

std::size_t onClientDisconnected(std::uint32_t clientId, std::uint32_t nowMs, UpstreamAction* out,
                                 std::size_t maxOut) noexcept {
    std::size_t written = 0U;
    for (std::size_t r = 0U; r < kMaxTopics; ++r) {
        TopicRow& row = g_rows[r];
        if (!row.used) continue;

        bool touched = false;
        for (std::size_t i = 0U; i < kMaxClientsPerTopic; ++i) {
            if (row.clients[i].used && row.clients[i].clientId == clientId) {
                row.clients[i] = ClientSub{};
                touched = true;
            }
        }
        if (!touched) continue;

        // Evaluated even when the output buffer is already full, so the row's
        // upstream bookkeeping never drifts from reality just because the
        // caller undersized `out`.
        const UpstreamAction action = evaluateUpstream(row, nowMs);
        if (action.kind == UpstreamKind::None) continue;
        if (out != nullptr && written < maxOut) out[written] = action;
        if (written < maxOut) ++written;
    }
    return written;
}

std::size_t sweep(std::uint32_t nowMs, UpstreamAction* out, std::size_t maxOut) noexcept {
    std::size_t written = 0U;
    for (std::size_t r = 0U; r < kMaxTopics; ++r) {
        TopicRow& row = g_rows[r];
        if (!row.used) continue;

        for (std::size_t i = 0U; i < kMaxClientsPerTopic; ++i) {
            ClientSub& client = row.clients[i];
            // Wrap-safe "deadline reached": millis() rolls over after ~49
            // days, so a plain nowMs >= deadline would expire every live
            // subscription for one lease-length window after the rollover.
            if (client.used && (nowMs - client.leaseDeadlineMs) < 0x80000000U) {
                client = ClientSub{};
            }
        }

        // Unconditional (not only when something expired): this is also where
        // an otherwise-idle row gets its upstream lease renewed.
        const UpstreamAction action = evaluateUpstream(row, nowMs);
        if (action.kind == UpstreamKind::None) continue;
        if (out != nullptr && written < maxOut) out[written] = action;
        if (written < maxOut) ++written;
    }
    return written;
}

void noteUpstreamRequestSent(std::uint32_t sourceId, std::uint32_t topicId, std::uint32_t sequence) noexcept {
    const int rowIndex = findRow(sourceId, topicId);
    if (rowIndex < 0) return;
    g_rows[static_cast<std::size_t>(rowIndex)].pendingRequestSequence = sequence;
}

bool onUpstreamSubscribeResult(std::uint32_t replyToSequence, std::uint8_t status,
                               std::uint32_t upstreamSubscriptionId, std::uint32_t effectiveRateMillihz) noexcept {
    constexpr std::uint8_t kStatusSuccess = 0x00U;
    for (std::size_t r = 0U; r < kMaxTopics; ++r) {
        TopicRow& row = g_rows[r];
        if (row.used && row.pendingRequestSequence != 0U && row.pendingRequestSequence == replyToSequence) {
            row.pendingRequestSequence = 0U;
            if (status == kStatusSuccess) {
                row.effectiveRateMillihz = effectiveRateMillihz;
                row.upstreamSubscriptionId = upstreamSubscriptionId;
            }
            return true;
        }
    }
    return false;
}

std::uint32_t upstreamSubscriptionId(std::uint32_t sourceId, std::uint32_t topicId) noexcept {
    const int rowIndex = findRow(sourceId, topicId);
    if (rowIndex < 0) return 0U;
    return g_rows[static_cast<std::size_t>(rowIndex)].upstreamSubscriptionId;
}

void recordForwarded(std::uint32_t sourceId, std::uint32_t topicId, std::size_t payloadBytes) noexcept {
    const int rowIndex = findRow(sourceId, topicId);
    if (rowIndex < 0) return;
    g_rows[static_cast<std::size_t>(rowIndex)].bytesTotal += payloadBytes;
}

void recordDropped(std::uint32_t sourceId, std::uint32_t topicId) noexcept {
    const int rowIndex = findRow(sourceId, topicId);
    if (rowIndex < 0) return;
    g_rows[static_cast<std::size_t>(rowIndex)].samplesDroppedTotal += 1U;
}

bool isWanted(std::uint32_t sourceId, std::uint32_t topicId) noexcept {
    const int rowIndex = findRow(sourceId, topicId);
    if (rowIndex < 0) return false;
    return refcount(g_rows[static_cast<std::size_t>(rowIndex)]) > 0U;
}

bool isKnownUnwanted(std::uint32_t sourceId, std::uint32_t topicId) noexcept {
    const int rowIndex = findRow(sourceId, topicId);
    // No row means this registry was never told anything about the topic, and
    // knowing nothing is not the same as knowing nobody wants it.
    if (rowIndex < 0) return false;
    return refcount(g_rows[static_cast<std::size_t>(rowIndex)]) == 0U;
}

std::size_t topicStatusSnapshot(TopicStatusEntry* out, std::size_t maxOut) noexcept {
    if (out == nullptr) return 0U;
    std::size_t written = 0U;
    for (std::size_t r = 0U; r < kMaxTopics && written < maxOut; ++r) {
        const TopicRow& row = g_rows[r];
        if (!row.used) continue;
        out[written].sourceId = row.sourceId;
        out[written].topicId = static_cast<std::uint16_t>(row.topicId);
        out[written].subscriberCount = refcount(row);
        out[written].effectiveRateMillihz = row.effectiveRateMillihz;
        out[written].bytesTotal = row.bytesTotal;
        out[written].samplesDroppedTotal = row.samplesDroppedTotal;
        ++written;
    }
    return written;
}

void resetForTests() noexcept {
    for (std::size_t i = 0U; i < kMaxTopics; ++i) {
        g_rows[i] = TopicRow{};
    }
    g_nextSubscriptionId = 1U;
}

}  // namespace SubscriptionRegistry
