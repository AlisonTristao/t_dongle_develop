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
    std::uint32_t pendingRequestSequence = 0U;  // 0 = none outstanding
    std::uint32_t upstreamRequestedRateMillihz = 0U;
    std::uint32_t upstreamSubscriptionId = 0U;  // robot-assigned id, needed to build UNSUBSCRIBE
    std::uint32_t effectiveRateMillihz = 0U;  // last grant (or 0 if never granted / cleared)

    // PASSO 8 counters, monotonic for this row's lifetime (never reset when
    // refcount hits zero -- only a dongle reboot resets them, same
    // "monotonic, saturates" convention as STATUS section 8).
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

std::uint32_t clampLease(std::uint32_t requestedLeaseMs) noexcept {
    if (requestedLeaseMs < kMinLeaseMs) return kMinLeaseMs;
    if (requestedLeaseMs > kMaxLeaseMs) return kMaxLeaseMs;
    return requestedLeaseMs;
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
            // Union rate cannot have changed (this client's own contribution
            // is unchanged and nobody else was touched), so no upstream
            // action is needed for a pure renewal.
            return outcome;
        }
    }

    const std::uint32_t rateBeforeUnion = unionRateMillihz(row);
    const bool wasActive = refcount(row) > 0U;

    // Same client re-subscribing to the same topic with a *different*
    // rate/lease replaces its row entry atomically (COMMANDS_AND_ACTIONS.md
    // section 7: "uma nova sequencia cria ou substitui... a assinatura da
    // mesma sessao e do mesmo topico").
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

    const std::uint32_t rateAfterUnion = unionRateMillihz(row);
    if (!wasActive || rateAfterUnion > rateBeforeUnion) {
        outcome.needsUpstreamSubscribe = true;
        outcome.upstream = UpstreamAction{sourceId, topicId, rateAfterUnion, clampedLeaseMs};
        row.upstreamRequestedRateMillihz = rateAfterUnion;
    }
    return outcome;
}

DesktopUnsubscribeOutcome onDesktopUnsubscribe(std::uint32_t clientId, std::uint32_t subscriptionId) noexcept {
    DesktopUnsubscribeOutcome outcome{};

    for (std::size_t r = 0U; r < kMaxTopics; ++r) {
        TopicRow& row = g_rows[r];
        if (!row.used) continue;
        for (std::size_t i = 0U; i < kMaxClientsPerTopic; ++i) {
            ClientSub& client = row.clients[i];
            if (client.used && client.clientId == clientId && client.subscriptionId == subscriptionId) {
                client = ClientSub{};
                outcome.found = true;
                if (refcount(row) == 0U) {
                    outcome.needsUpstreamUnsubscribe = true;
                    outcome.upstream = UpstreamAction{row.sourceId, row.topicId, 0U, 0U, row.upstreamSubscriptionId};
                    row.upstreamRequestedRateMillihz = 0U;
                    row.effectiveRateMillihz = 0U;
                    row.upstreamSubscriptionId = 0U;
                }
                return outcome;
            }
        }
    }
    // Not found anywhere: COMMANDS_AND_ACTIONS.md section 7 treats this as
    // SUCCESS/NONE (idempotent retry), not an error -- outcome.found stays
    // false and the caller does not turn that into a wire error.
    return outcome;
}

std::size_t onClientDisconnected(std::uint32_t clientId, UpstreamAction* out, std::size_t maxOut) noexcept {
    std::size_t written = 0U;
    for (std::size_t r = 0U; r < kMaxTopics && written < maxOut; ++r) {
        TopicRow& row = g_rows[r];
        if (!row.used) continue;

        bool touched = false;
        for (std::size_t i = 0U; i < kMaxClientsPerTopic; ++i) {
            if (row.clients[i].used && row.clients[i].clientId == clientId) {
                row.clients[i] = ClientSub{};
                touched = true;
            }
        }
        if (touched && refcount(row) == 0U) {
            if (out != nullptr) {
                out[written] = UpstreamAction{row.sourceId, row.topicId, 0U, 0U, row.upstreamSubscriptionId};
            }
            ++written;
            row.upstreamRequestedRateMillihz = 0U;
            row.effectiveRateMillihz = 0U;
            row.upstreamSubscriptionId = 0U;
        }
    }
    return written;
}

std::size_t expireLeases(std::uint32_t nowMs, UpstreamAction* out, std::size_t maxOut) noexcept {
    std::size_t written = 0U;
    for (std::size_t r = 0U; r < kMaxTopics && written < maxOut; ++r) {
        TopicRow& row = g_rows[r];
        if (!row.used) continue;

        bool touched = false;
        for (std::size_t i = 0U; i < kMaxClientsPerTopic; ++i) {
            ClientSub& client = row.clients[i];
            if (client.used && nowMs >= client.leaseDeadlineMs) {
                client = ClientSub{};
                touched = true;
            }
        }
        if (touched && refcount(row) == 0U) {
            if (out != nullptr) {
                out[written] = UpstreamAction{row.sourceId, row.topicId, 0U, 0U, row.upstreamSubscriptionId};
            }
            ++written;
            row.upstreamRequestedRateMillihz = 0U;
            row.effectiveRateMillihz = 0U;
            row.upstreamSubscriptionId = 0U;
        }
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
