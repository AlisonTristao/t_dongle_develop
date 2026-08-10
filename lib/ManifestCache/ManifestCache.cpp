#include <ManifestCache.h>

#include <BtpTransport.h>

#include <cstring>

namespace ManifestCache {
namespace {

constexpr std::uint8_t kStatusSuccess = 0x00U;
constexpr std::uint8_t kStatusRejected = 0x01U;
constexpr std::uint16_t kErrorNone = 0x0000U;
constexpr std::uint16_t kErrorStaleTargetBoot = 0x0009U;
constexpr std::uint16_t kErrorNotFound = 0x000BU;
constexpr std::uint8_t kFlagNotModified = 0x01U;
constexpr std::uint8_t kFlagCatalogComplete = 0x02U;

// This dongle's own descriptor never changes within a build (no topics/
// actions of its own to publish yet); a fixed revision satisfies "a revisao
// e monotonica e comeca em 1" trivially, same reasoning bally_software's
// ManifestResponder uses for its own kConfigRevision.
constexpr std::uint32_t kSelfConfigRevision = 1U;
constexpr const char* kSelfName = "t_dongle_develop";

struct Entry {
    bool used = false;
    std::uint32_t sourceId = 0U;
    std::uint32_t bootId = 0U;
    std::uint8_t uuid[16] = {};
    std::uint8_t role = 0U;
    bool online = false;
    std::uint32_t configRevision = 0U;
    char name[kMaxNameLength + 1U] = {};
    std::uint16_t topicCount = 0U;
    std::uint16_t actionCount = 0U;
    std::uint8_t records[kMaxRecordsBytes];
    std::size_t recordsSize = 0U;
    std::uint32_t lastSeenMs = 0U;
};

struct PendingRequest {
    bool used = false;
    std::uint32_t sourceId = 0U;
    std::uint32_t bootId = 0U;
    std::uint32_t lastRequestedMs = 0U;
};

Entry g_entries[kCapacity];
PendingRequest g_pending[kCapacity];
std::uint32_t g_catalogRevision = 1U;
std::uint8_t g_selfUuid[16] = {};

std::uint16_t read_u16_le(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(data[0]) | (static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint32_t read_u32_le(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) | (static_cast<std::uint32_t>(data[3]) << 24U);
}

// Append-only cursor with two-phase (reserve + patch) support for the
// topic_count/action_count fields, whose real values are only known after
// the truncated records walk below. Every append returns false (and does not
// partially mutate the buffer) on overflow.
class Writer {
public:
    Writer(std::uint8_t* out, std::size_t capacity) noexcept : out_(out), capacity_(capacity) {}

    bool u8(std::uint8_t v) noexcept { return raw(&v, 1U); }
    bool u16(std::uint16_t v) noexcept {
        const std::uint8_t b[2] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8U)};
        return raw(b, 2U);
    }
    bool u32(std::uint32_t v) noexcept {
        const std::uint8_t b[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8U),
                                   static_cast<std::uint8_t>(v >> 16U), static_cast<std::uint8_t>(v >> 24U)};
        return raw(b, 4U);
    }
    bool bytes(const std::uint8_t* data, std::size_t n) noexcept { return raw(data, n); }
    bool utf8(const char* text) noexcept {
        const std::size_t len = (text == nullptr) ? 0U : std::strlen(text);
        if (len > 0xFFFFU) return false;
        if (!u16(static_cast<std::uint16_t>(len))) return false;
        return len == 0U || raw(reinterpret_cast<const std::uint8_t*>(text), len);
    }

    std::size_t size() const noexcept { return pos_; }

    bool reserveU16(std::size_t* offset_out) noexcept {
        *offset_out = pos_;
        return u16(0U);
    }
    void patchU16(std::size_t offset, std::uint16_t value) noexcept {
        out_[offset] = static_cast<std::uint8_t>(value);
        out_[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    }

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

// Walks `totalRecords` (topicCount + actionCount) record_size-prefixed
// records inside `blob`, requiring exact consumption of `blobSize`. Used
// both to validate a freshly-received manifest before caching it (strict:
// any short read invalidates the whole thing) and, via
// appendRecordsTruncated below, to copy whole records into a response.
bool walkRecords(const std::uint8_t* blob, std::size_t blobSize, std::uint32_t totalRecords,
                 std::size_t* consumed_out) noexcept {
    std::size_t pos = 0U;
    for (std::uint32_t i = 0U; i < totalRecords; ++i) {
        if (pos + 4U > blobSize) return false;
        const std::uint32_t contentSize = read_u32_le(blob + pos);
        const std::size_t recordTotalLen = 4U + static_cast<std::size_t>(contentSize);
        if (pos + recordTotalLen > blobSize || pos + recordTotalLen < pos) return false;  // overflow-safe
        pos += recordTotalLen;
    }
    *consumed_out = pos;
    return true;
}

// Copies as many whole records (topics first, then actions) as fit in
// `writer`'s remaining capacity, stopping before any partial record. Reports
// how many of each were actually written -- callers patch topic_count/
// action_count with these, never with the cached totals.
void appendRecordsTruncated(Writer& writer, const std::uint8_t* blob, std::size_t blobSize,
                            std::uint16_t topicCount, std::uint16_t actionCount, std::uint16_t* topicsWritten_out,
                            std::uint16_t* actionsWritten_out) noexcept {
    std::uint16_t topicsWritten = 0U;
    std::uint16_t actionsWritten = 0U;
    std::size_t pos = 0U;
    const std::uint32_t totalRecords = static_cast<std::uint32_t>(topicCount) + static_cast<std::uint32_t>(actionCount);

    for (std::uint32_t i = 0U; i < totalRecords; ++i) {
        if (pos + 4U > blobSize) break;
        const std::uint32_t contentSize = read_u32_le(blob + pos);
        const std::size_t recordTotalLen = 4U + static_cast<std::size_t>(contentSize);
        if (pos + recordTotalLen > blobSize) break;  // corrupt cache; stop defensively
        if (!writer.bytes(blob + pos, recordTotalLen)) break;  // doesn't fit the response; stop
        pos += recordTotalLen;
        if (i < topicCount) {
            ++topicsWritten;
        } else {
            ++actionsWritten;
        }
    }

    *topicsWritten_out = topicsWritten;
    *actionsWritten_out = actionsWritten;
}

Entry* findEntry(std::uint32_t sourceId) noexcept {
    for (Entry& entry : g_entries) {
        if (entry.used && entry.sourceId == sourceId) return &entry;
    }
    return nullptr;
}

// Finds the entry for sourceId, or allocates a free slot, or (cache full)
// evicts the least-recently-seen entry. Never returns nullptr.
Entry* findOrAllocateEntry(std::uint32_t sourceId) noexcept {
    if (Entry* existing = findEntry(sourceId)) return existing;

    Entry* free_slot = nullptr;
    Entry* oldest = nullptr;
    for (Entry& entry : g_entries) {
        if (!entry.used) {
            free_slot = &entry;
            break;
        }
        if (oldest == nullptr || entry.lastSeenMs < oldest->lastSeenMs) {
            oldest = &entry;
        }
    }
    return (free_slot != nullptr) ? free_slot : oldest;
}

PendingRequest* findPending(std::uint32_t sourceId) noexcept {
    for (PendingRequest& pending : g_pending) {
        if (pending.used && pending.sourceId == sourceId) return &pending;
    }
    return nullptr;
}

PendingRequest* findOrAllocatePending(std::uint32_t sourceId) noexcept {
    if (PendingRequest* existing = findPending(sourceId)) return existing;

    PendingRequest* free_slot = nullptr;
    PendingRequest* oldest = nullptr;
    for (PendingRequest& pending : g_pending) {
        if (!pending.used) {
            free_slot = &pending;
            break;
        }
        if (oldest == nullptr || pending.lastRequestedMs < oldest->lastRequestedMs) {
            oldest = &pending;
        }
    }
    return (free_slot != nullptr) ? free_slot : oldest;
}

// Collects pointers to used entries, insertion-sorted by source_id
// ascending (COMMANDS_AND_ACTIONS.md section 6.1). N is at most kCapacity
// (8), so an O(n^2) insertion sort is simplest and cheap enough.
std::size_t collectSortedEntries(Entry* out[kCapacity]) noexcept {
    std::size_t count = 0U;
    for (Entry& entry : g_entries) {
        if (entry.used) out[count++] = &entry;
    }
    for (std::size_t i = 1U; i < count; ++i) {
        Entry* key = out[i];
        std::size_t j = i;
        while (j > 0U && out[j - 1U]->sourceId > key->sourceId) {
            out[j] = out[j - 1U];
            --j;
        }
        out[j] = key;
    }
    return count;
}

std::size_t writeManifestData(const btp::Header& requestHeader, std::uint8_t status, std::uint8_t flags,
                              std::uint16_t errorCode, std::uint16_t catalogIndex, std::uint16_t catalogCount,
                              std::uint32_t configRevision, const std::uint8_t* uuid, std::uint32_t describedSourceId,
                              std::uint32_t describedBootId, std::uint8_t role, bool online, const char* name,
                              const std::uint8_t* recordsBlob, std::size_t recordsBlobSize,
                              std::uint16_t cachedTopicCount, std::uint16_t cachedActionCount, std::uint8_t* output,
                              std::size_t capacity) noexcept {
    Writer writer(output, capacity);

    bool ok = writer.u32(requestHeader.source_id) && writer.u32(requestHeader.boot_id) &&
             writer.u32(requestHeader.sequence) && writer.u8(status) && writer.u8(flags) && writer.u16(errorCode) &&
             writer.u16(kManifestFormatVersion) && writer.u16(0U) /*reserved*/ && writer.u32(configRevision);
    if (!ok) return 0U;

    static const std::uint8_t kZeroUuid[16] = {0};
    ok = writer.bytes(uuid != nullptr ? uuid : kZeroUuid, 16U) && writer.u32(describedSourceId) &&
        writer.u32(describedBootId) && writer.u8(role) && writer.u8(online ? 0x01U : 0x00U) &&
        writer.u16(catalogIndex) && writer.u16(catalogCount);
    if (!ok) return 0U;

    std::size_t topicCountOffset = 0U;
    std::size_t actionCountOffset = 0U;
    if (!writer.reserveU16(&topicCountOffset) || !writer.reserveU16(&actionCountOffset)) return 0U;
    if (!writer.utf8(name != nullptr ? name : "")) return 0U;

    std::uint16_t topicsWritten = 0U;
    std::uint16_t actionsWritten = 0U;
    if (recordsBlob != nullptr && recordsBlobSize > 0U) {
        appendRecordsTruncated(writer, recordsBlob, recordsBlobSize, cachedTopicCount, cachedActionCount,
                               &topicsWritten, &actionsWritten);
    }
    writer.patchU16(topicCountOffset, topicsWritten);
    writer.patchU16(actionCountOffset, actionsWritten);
    return writer.size();
}

}  // namespace

void configure(const std::uint8_t selfUuid[16]) noexcept {
    if (selfUuid != nullptr) std::memcpy(g_selfUuid, selfUuid, 16U);
}

bool shouldRequestManifest(std::uint32_t sourceId, std::uint32_t bootId, std::uint32_t nowMs) noexcept {
    if (sourceId == 0U || bootId == 0U) return false;

    const Entry* entry = findEntry(sourceId);
    if (entry != nullptr && entry->bootId == bootId) {
        return false;  // already have a manifest for this exact boot
    }

    PendingRequest* pending = findPending(sourceId);
    if (pending != nullptr && pending->bootId == bootId && (nowMs - pending->lastRequestedMs) < kRequestCooldownMs) {
        return false;  // asked recently, still waiting
    }

    PendingRequest* slot = findOrAllocatePending(sourceId);
    slot->used = true;
    slot->sourceId = sourceId;
    slot->bootId = bootId;
    slot->lastRequestedMs = nowMs;
    return true;
}

std::size_t buildRequest(std::uint32_t targetSourceId, std::uint32_t targetBootId, std::uint32_t knownRevision,
                         std::uint8_t* output, std::size_t outputCapacity) noexcept {
    Writer writer(output, outputCapacity);
    if (!writer.u32(targetSourceId) || !writer.u32(targetBootId) || !writer.u32(knownRevision)) return 0U;
    return writer.size();
}

bool ingestManifestData(btp::ByteView payload, std::uint32_t nowMs) noexcept {
    if (payload.data == nullptr || payload.size < 60U) return false;

    const std::uint8_t status = payload.data[12];
    if (status != kStatusSuccess) return false;  // only interested in successful descriptors

    const std::uint8_t flags = payload.data[13];
    const bool notModified = (flags & kFlagNotModified) != 0U;
    const std::uint16_t formatVersion = read_u16_le(payload.data + 16U);
    if (formatVersion != kManifestFormatVersion) return false;

    const std::uint32_t configRevision = read_u32_le(payload.data + 20U);
    const std::uint8_t* uuid = payload.data + 24U;
    const std::uint32_t describedSourceId = read_u32_le(payload.data + 40U);
    const std::uint32_t describedBootId = read_u32_le(payload.data + 44U);
    const std::uint8_t role = payload.data[48];
    const std::uint8_t sourceFlags = payload.data[49];
    const std::uint16_t topicCount = read_u16_le(payload.data + 54U);
    const std::uint16_t actionCount = read_u16_le(payload.data + 56U);

    if (describedSourceId == 0U || describedBootId == 0U) return false;

    const std::uint16_t nameLen = read_u16_le(payload.data + 58U);
    const std::size_t nameStart = 60U;
    if (nameStart + static_cast<std::size_t>(nameLen) > payload.size) return false;
    const std::size_t recordsStart = nameStart + nameLen;
    const std::size_t recordsSize = payload.size - recordsStart;

    if (notModified) {
        if (recordsSize != 0U || topicCount != 0U || actionCount != 0U) return false;  // malformed NOT_MODIFIED
    } else {
        if (recordsSize > kMaxRecordsBytes) return false;  // beyond this cache's budget
        std::size_t consumed = 0U;
        if (!walkRecords(payload.data + recordsStart, recordsSize,
                         static_cast<std::uint32_t>(topicCount) + static_cast<std::uint32_t>(actionCount),
                         &consumed) ||
            consumed != recordsSize) {
            return false;  // record framing does not exactly consume the payload
        }
    }

    Entry* entry = findEntry(describedSourceId);
    const bool isNew = (entry == nullptr);
    if (isNew && notModified) {
        return false;  // NOT_MODIFIED with nothing previously cached makes no sense; reject
    }
    if (isNew) {
        entry = findOrAllocateEntry(describedSourceId);
    }

    const bool changed = isNew || entry->bootId != describedBootId || entry->configRevision != configRevision;

    entry->used = true;
    entry->sourceId = describedSourceId;
    entry->bootId = describedBootId;
    std::memcpy(entry->uuid, uuid, 16U);
    entry->role = role;
    entry->online = (sourceFlags & 0x01U) != 0U;
    entry->configRevision = configRevision;
    entry->lastSeenMs = nowMs;

    if (!notModified) {
        const std::size_t copyLen = (nameLen < kMaxNameLength) ? nameLen : kMaxNameLength;
        std::memcpy(entry->name, payload.data + nameStart, copyLen);
        entry->name[copyLen] = '\0';

        std::memcpy(entry->records, payload.data + recordsStart, recordsSize);
        entry->recordsSize = recordsSize;
        entry->topicCount = topicCount;
        entry->actionCount = actionCount;
    }

    // Clear the cooldown so a subsequent boot-mismatch (peer rebooted again
    // right away) is not throttled by a now-irrelevant timestamp.
    if (PendingRequest* pending = findPending(describedSourceId)) {
        pending->used = false;
    }

    if (changed) ++g_catalogRevision;
    return true;
}

std::uint32_t catalogRevision() noexcept { return g_catalogRevision; }

std::size_t enumerationCount() noexcept {
    std::size_t count = 1U;  // self
    for (const Entry& entry : g_entries) {
        if (entry.used) ++count;
    }
    return count;
}

std::size_t buildEnumerationResponse(std::size_t index, const btp::Header& requestHeader, std::uint8_t* output,
                                     std::size_t outputCapacity) noexcept {
    const std::size_t total = enumerationCount();
    if (index >= total) return 0U;

    const std::uint8_t flags = (index + 1U == total) ? kFlagCatalogComplete : static_cast<std::uint8_t>(0U);

    if (index == 0U) {
        return writeManifestData(requestHeader, kStatusSuccess, flags, kErrorNone,
                                 static_cast<std::uint16_t>(index), static_cast<std::uint16_t>(total),
                                 kSelfConfigRevision, g_selfUuid, BtpTransport::sourceId(), BtpTransport::bootId(),
                                 kSourceRoleDongle, /*online=*/true, kSelfName, nullptr, 0U, 0U, 0U, output,
                                 outputCapacity);
    }

    Entry* sorted[kCapacity];
    const std::size_t used = collectSortedEntries(sorted);
    const std::size_t sortedIndex = index - 1U;
    if (sortedIndex >= used) return 0U;
    const Entry* entry = sorted[sortedIndex];

    return writeManifestData(requestHeader, kStatusSuccess, flags, kErrorNone, static_cast<std::uint16_t>(index),
                             static_cast<std::uint16_t>(total), entry->configRevision, entry->uuid, entry->sourceId,
                             entry->bootId, entry->role, entry->online, entry->name, entry->records,
                             entry->recordsSize, entry->topicCount, entry->actionCount, output, outputCapacity);
}

std::size_t buildTargetedResponse(std::uint32_t targetSourceId, std::uint32_t targetBootId,
                                  std::uint32_t knownRevision, const btp::Header& requestHeader, std::uint8_t* output,
                                  std::size_t outputCapacity) noexcept {
    const bool isSelf = (targetSourceId == BtpTransport::sourceId());
    const Entry* entry = isSelf ? nullptr : findEntry(targetSourceId);

    if (!isSelf && entry == nullptr) {
        return writeManifestData(requestHeader, kStatusRejected, kFlagCatalogComplete, kErrorNotFound, 0U, 1U, 0U,
                                 nullptr, 0U, 0U, 0U, false, "unknown source", nullptr, 0U, 0U, 0U, output,
                                 outputCapacity);
    }

    const std::uint32_t foundBootId = isSelf ? BtpTransport::bootId() : entry->bootId;
    if (targetBootId != 0U && targetBootId != foundBootId) {
        return writeManifestData(requestHeader, kStatusRejected, kFlagCatalogComplete, kErrorStaleTargetBoot, 0U, 1U,
                                 0U, nullptr, 0U, 0U, 0U, false, "boot mismatch", nullptr, 0U, 0U, 0U, output,
                                 outputCapacity);
    }

    const std::uint32_t foundRevision = isSelf ? kSelfConfigRevision : entry->configRevision;
    const std::uint8_t* foundUuid = isSelf ? g_selfUuid : entry->uuid;
    const std::uint8_t foundRole = isSelf ? kSourceRoleDongle : entry->role;
    const bool foundOnline = isSelf ? true : entry->online;
    const char* foundName = isSelf ? kSelfName : entry->name;

    if (knownRevision != 0U && knownRevision == foundRevision) {
        return writeManifestData(requestHeader, kStatusSuccess,
                                 static_cast<std::uint8_t>(kFlagNotModified | kFlagCatalogComplete), kErrorNone, 0U,
                                 1U, foundRevision, foundUuid, targetSourceId, foundBootId, foundRole, foundOnline,
                                 foundName, nullptr, 0U, 0U, 0U, output, outputCapacity);
    }

    const std::uint8_t* records = isSelf ? nullptr : entry->records;
    const std::size_t recordsSize = isSelf ? 0U : entry->recordsSize;
    const std::uint16_t topicCount = isSelf ? 0U : entry->topicCount;
    const std::uint16_t actionCount = isSelf ? 0U : entry->actionCount;

    return writeManifestData(requestHeader, kStatusSuccess, kFlagCatalogComplete, kErrorNone, 0U, 1U, foundRevision,
                             foundUuid, targetSourceId, foundBootId, foundRole, foundOnline, foundName, records,
                             recordsSize, topicCount, actionCount, output, outputCapacity);
}

}  // namespace ManifestCache
