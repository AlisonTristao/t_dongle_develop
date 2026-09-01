#include <ManifestCache.h>

#include <BtpTransport.h>
#include <DonglePublisher.h>
#include <btp/messages.hpp>

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

// This dongle's own descriptor is a compile-time constant (DonglePublisher's
// schema tables), so a fixed revision satisfies "a revisao e monotonica e
// comeca em 1" trivially, same reasoning bally_software's ManifestResponder
// uses for its own kConfigRevision.
//
// Bumped 1 -> 2 by topico 27: revision 1 described a dongle with zero topics.
// Bumped 2 -> 3 by the downstream-relay counters added to hub.usb in schema
// version 2.  A client that cached revision 2 must not receive NOT_MODIFIED
// and decode the older six-field layout as the new twelve-field layout.
constexpr std::uint32_t kSelfConfigRevision = 3U;
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
    // Verbatim source_info block from this source's MANIFEST_DATA (commands.md
    // 3.12), info_count prefix included. Always >= 2 octets once used: a
    // format-1 source, or a format-2 one with no entries, both land here as a
    // bare "00 00". Re-emitted as-is.
    std::uint8_t sourceInfo[kMaxSourceInfoBytes] = {};
    std::uint16_t sourceInfoSize = 0U;
    std::uint32_t lastSeenMs = 0U;
};

struct PendingRequest {
    bool used = false;
    std::uint32_t sourceId = 0U;
    std::uint32_t bootId = 0U;
    std::uint32_t lastRequestedMs = 0U;
    // MANIFEST_REQUESTs sent for this exact (sourceId, bootId) chase. Drives
    // the fast->steady cadence in shouldRequestManifest and is surfaced by
    // `hub -manifest`. Reset to 1 on the first request of a fresh chase.
    std::uint32_t attempts = 0U;
};

Entry g_entries[kCapacity];
PendingRequest g_pending[kCapacity];
std::uint32_t g_catalogRevision = 1U;
std::uint8_t g_selfUuid[16] = {};

// This dongle's own serialized source_info block (commands.md 3.12), set by
// configure() from AppRuntime. Borrowed pointer; nullptr means "serve an
// empty block for self".
const std::uint8_t* g_selfSourceInfo = nullptr;
std::size_t g_selfSourceInfoSize = 0U;
// A valid empty block (info_count = 0), re-used for a format-1 robot and
// wherever a source has no info of its own.
const std::uint8_t kEmptySourceInfo[2] = {0U, 0U};

// Diagnostico (plano 36 fase 0a). volatile, mesmo estilo dos contadores de
// EspNowConfig -- RX roda na task WiFi, o shell le na main.
volatile std::uint32_t g_diagPrimeSent = 0U;
volatile std::uint32_t g_diagIngestOk = 0U;
volatile std::uint32_t g_diagIngestFail = 0U;
volatile std::uint32_t g_diagConsumeRejected = 0U;
volatile std::uint32_t g_diagTargetedRx = 0U;
volatile std::uint32_t g_diagTargetedHit = 0U;
volatile std::uint32_t g_diagTargetedMiss = 0U;

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
// ascending (commands.md section 3.1). N is at most kCapacity
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
                              const std::uint8_t* sourceInfo, std::size_t sourceInfoSize,
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

    // source_info block (commands.md 3.12) -- always present in format 2,
    // NOT_MODIFIED and error responses included. Emitted verbatim (the block
    // already carries its own info_count); a source with none gets "00 00".
    if (sourceInfo != nullptr && sourceInfoSize >= 2U) {
        if (!writer.bytes(sourceInfo, sourceInfoSize)) return 0U;
    } else if (!writer.u16(0U)) {
        return 0U;
    }

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

std::size_t serializeSourceInfo(const SourceInfoEntry* entries, std::size_t count,
                                std::uint8_t* output, std::size_t capacity) noexcept {
    Writer writer(output, capacity);
    std::size_t countOffset = 0U;
    if (!writer.reserveU16(&countOffset)) return 0U;

    std::uint16_t written = 0U;
    for (std::size_t i = 0U; i < count && written < 0xFFFFU; ++i) {
        const char* value = (entries == nullptr) ? nullptr : entries[i].value;
        if (value == nullptr || value[0] == '\0') continue;  // unset field: skipped, not emitted
        if (!writer.utf8(entries[i].key) || !writer.utf8(entries[i].label) || !writer.utf8(value)) {
            return 0U;
        }
        ++written;
    }
    writer.patchU16(countOffset, written);
    return writer.size();
}

void configure(const std::uint8_t selfUuid[16], const std::uint8_t* selfSourceInfo,
               std::size_t selfSourceInfoSize) noexcept {
    if (selfUuid != nullptr) std::memcpy(g_selfUuid, selfUuid, 16U);
    if (selfSourceInfo != nullptr && selfSourceInfoSize >= 2U) {
        g_selfSourceInfo = selfSourceInfo;
        g_selfSourceInfoSize = selfSourceInfoSize;
    } else {
        g_selfSourceInfo = nullptr;
        g_selfSourceInfoSize = 0U;
    }
}

bool shouldRequestManifest(std::uint32_t sourceId, std::uint32_t bootId, std::uint32_t nowMs) noexcept {
    if (sourceId == 0U || bootId == 0U) return false;

    const Entry* entry = findEntry(sourceId);
    if (entry != nullptr && entry->bootId == bootId) {
        return false;  // already have a manifest for this exact boot
    }

    // `attempts` counts requests sent for this source since the last one that
    // got a usable answer (ingestManifestData clears the pending, which frees
    // the slot and resets the count). It deliberately does NOT reset on a
    // boot_id change: a robot that answers normally never accumulates, so the
    // fast cadence still covers first contact and a genuine reboot; a robot
    // that ignores every request -- or whose boot_id flaps because of a bug
    // -- accumulates and drops to the slow cadence instead of being polled
    // every second forever (which is what stalled the link).
    PendingRequest* pending = findPending(sourceId);
    if (pending != nullptr) {
        const std::uint32_t cooldown =
            (pending->attempts <= kFastRequestAttempts) ? kFastRequestCooldownMs : kRequestCooldownMs;
        if ((nowMs - pending->lastRequestedMs) < cooldown) {
            return false;  // asked recently, still waiting
        }
    }

    PendingRequest* slot = findOrAllocatePending(sourceId);
    const bool sameSource = (slot->used && slot->sourceId == sourceId);
    slot->used = true;
    slot->sourceId = sourceId;
    slot->bootId = bootId;
    slot->lastRequestedMs = nowMs;
    slot->attempts = sameSource ? (slot->attempts + 1U) : 1U;
    return true;
}

std::size_t buildRequest(std::uint32_t targetSourceId, std::uint32_t targetBootId, std::uint32_t knownRevision,
                         std::uint8_t* output, std::size_t outputCapacity) noexcept {
    btp::ManifestRequest request{};
    request.target_source_id = targetSourceId;
    request.target_boot_id = targetBootId;
    request.known_config_revision = knownRevision;

    std::size_t written = 0U;
    if (btp::encode_manifest_request(request, output, outputCapacity, &written) != btp::MessageError::Ok) {
        return 0U;
    }
    return written;
}

static bool ingestManifestDataImpl(btp::ByteView payload, std::uint32_t nowMs) noexcept {
    // The MANIFEST_DATA fixed head (commands.md section 3.3) is now
    // btp::ManifestReader::header(): the request-reference, the status/role
    // enums, manifest_format_version {1,2}, the reserved word, the section-6
    // count limits and the source_name bounds all move into the library. Only
    // the topic/action records past source_name stay opaque here -- this cache
    // relays them verbatim (walkRecords / appendRecordsTruncated below), it
    // never parses a field.
    btp::ManifestReader reader(payload.data, payload.size);
    btp::ManifestHeader header{};
    if (reader.header(&header) != btp::MessageError::Ok) return false;

    if (header.status != static_cast<std::uint8_t>(btp::ResultStatus::Success)) {
        return false;  // only interested in successful descriptors
    }

    const bool notModified = (header.flags & btp::kManifestNotModified) != 0U;
    // btp::ManifestReader already accepted only format 1 or 2 (format 1 is
    // treated as an empty source_info block, so a fleet mid-rollout still
    // populates the cache).
    const std::uint16_t formatVersion = header.manifest_format_version;
    const std::uint32_t configRevision = header.config_revision;
    const std::uint8_t* uuid = header.source_uuid;
    const std::uint32_t describedSourceId = header.described_source_id;
    const std::uint32_t describedBootId = header.described_boot_id;
    const std::uint8_t role = header.source_role;
    const std::uint8_t sourceFlags = header.source_flags;
    const std::uint16_t topicCount = header.topic_count;
    const std::uint16_t actionCount = header.action_count;

    if (describedSourceId == 0U || describedBootId == 0U) return false;

    // source_name is a ByteView into `payload`; the source_info block (format 2)
    // or the records (format 1) begin right after it.
    const std::uint16_t nameLen = static_cast<std::uint16_t>(header.source_name.size);
    const std::size_t nameStart = static_cast<std::size_t>(header.source_name.data - payload.data);

    // source_info block sits between source_name and the records in format 2
    // (commands.md 3.12): info_count:u16 then that many { key, label, value }
    // utf8_u16 triples. Walk it to find where the records begin -- info_count
    // makes its end unambiguous even with records right after it. Format 1
    // has no block; synthesize an empty one.
    const std::size_t infoStart = nameStart + nameLen;
    std::size_t recordsStart = infoStart;
    const std::uint8_t* infoBlob = kEmptySourceInfo;
    std::size_t infoSize = sizeof(kEmptySourceInfo);
    if (formatVersion >= 2U) {
        if (infoStart + 2U > payload.size) return false;
        const std::uint16_t infoCount = read_u16_le(payload.data + infoStart);
        std::size_t pos = infoStart + 2U;
        for (std::uint16_t entryIdx = 0U; entryIdx < infoCount; ++entryIdx) {
            for (int field = 0; field < 3; ++field) {
                if (pos + 2U > payload.size) return false;
                const std::uint16_t len = read_u16_le(payload.data + pos);
                pos += 2U;
                if (pos + len > payload.size || pos + len < pos) return false;  // overflow-safe
                pos += len;
            }
        }
        recordsStart = pos;
        infoSize = recordsStart - infoStart;
        if (infoSize > kMaxSourceInfoBytes) return false;  // beyond this cache's budget
        infoBlob = payload.data + infoStart;
    }
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

    // source_info is not gated by config_revision (commands.md 3.12): refresh
    // it from every response, NOT_MODIFIED included, the same as uuid/online
    // above and unlike name/records below.
    std::memcpy(entry->sourceInfo, infoBlob, infoSize);
    entry->sourceInfoSize = static_cast<std::uint16_t>(infoSize);

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

bool ingestManifestData(btp::ByteView payload, std::uint32_t nowMs) noexcept {
    const bool ok = ingestManifestDataImpl(payload, nowMs);
    if (ok) {
        ++g_diagIngestOk;
    } else {
        ++g_diagIngestFail;
    }
    return ok;
}

void notePrimeSent() noexcept { ++g_diagPrimeSent; }
void noteConsumeRejected() noexcept { ++g_diagConsumeRejected; }

std::uint32_t ingestFailedTotal() noexcept { return g_diagIngestFail; }
std::uint32_t consumeRejectedTotal() noexcept { return g_diagConsumeRejected; }

void resetForTests() noexcept {
    for (Entry& entry : g_entries) entry = Entry{};
    for (PendingRequest& pending : g_pending) pending = PendingRequest{};
    g_catalogRevision = 1U;
    std::memset(g_selfUuid, 0, sizeof(g_selfUuid));
    g_selfSourceInfo = nullptr;
    g_selfSourceInfoSize = 0U;
    g_diagPrimeSent = 0U;
    g_diagIngestOk = 0U;
    g_diagIngestFail = 0U;
    g_diagConsumeRejected = 0U;
    g_diagTargetedRx = 0U;
    g_diagTargetedHit = 0U;
    g_diagTargetedMiss = 0U;
}

Diagnostics diagnostics() noexcept {
    Diagnostics out{};
    out.primeRequestsSent = g_diagPrimeSent;
    out.ingestedOk = g_diagIngestOk;
    out.ingestFailed = g_diagIngestFail;
    out.consumeRejected = g_diagConsumeRejected;
    out.targetedRequestsRx = g_diagTargetedRx;
    out.targetedServedHit = g_diagTargetedHit;
    out.targetedServedMiss = g_diagTargetedMiss;
    out.entryCount = 0U;
    for (const Entry& entry : g_entries) {
        if (!entry.used) continue;
        DiagnosticEntry& e = out.entries[out.entryCount++];
        e.sourceId = entry.sourceId;
        e.bootId = entry.bootId;
        e.configRevision = entry.configRevision;
        e.topicCount = entry.topicCount;
        e.actionCount = entry.actionCount;
        e.recordsSize = static_cast<std::uint16_t>(entry.recordsSize);
        e.online = entry.online;
    }

    out.pendingCount = 0U;
    for (const PendingRequest& pending : g_pending) {
        if (!pending.used) continue;
        PendingDiagnostic& p = out.pending[out.pendingCount++];
        p.sourceId = pending.sourceId;
        p.bootId = pending.bootId;
        p.attempts = pending.attempts;
    }
    return out;
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
        // Topico 27: index 0 is still this dongle, but it is no longer an
        // empty descriptor -- it now carries the hub.* topic records, so a
        // plain target_source_id = 0 enumeration is all a client needs to
        // discover the dongle's own topics alongside the robots'. No special
        // branch anywhere: the dongle became one more source in the catalog.
        std::size_t selfRecordsSize = 0U;
        std::uint16_t selfTopicCount = 0U;
        const std::uint8_t* selfRecords = DonglePublisher::topicRecords(&selfRecordsSize, &selfTopicCount);

        return writeManifestData(requestHeader, kStatusSuccess, flags, kErrorNone,
                                 static_cast<std::uint16_t>(index), static_cast<std::uint16_t>(total),
                                 kSelfConfigRevision, g_selfUuid, BtpTransport::sourceId(), BtpTransport::bootId(),
                                 kSourceRoleDongle, /*online=*/true, kSelfName, g_selfSourceInfo, g_selfSourceInfoSize,
                                 selfRecords, selfRecordsSize, selfTopicCount, 0U, output, outputCapacity);
    }

    Entry* sorted[kCapacity];
    const std::size_t used = collectSortedEntries(sorted);
    const std::size_t sortedIndex = index - 1U;
    if (sortedIndex >= used) return 0U;
    const Entry* entry = sorted[sortedIndex];

    return writeManifestData(requestHeader, kStatusSuccess, flags, kErrorNone, static_cast<std::uint16_t>(index),
                             static_cast<std::uint16_t>(total), entry->configRevision, entry->uuid, entry->sourceId,
                             entry->bootId, entry->role, entry->online, entry->name, entry->sourceInfo,
                             entry->sourceInfoSize, entry->records, entry->recordsSize, entry->topicCount,
                             entry->actionCount, output, outputCapacity);
}

std::size_t buildTargetedResponse(std::uint32_t targetSourceId, std::uint32_t targetBootId,
                                  std::uint32_t knownRevision, const btp::Header& requestHeader, std::uint8_t* output,
                                  std::size_t outputCapacity) noexcept {
    const bool isSelf = (targetSourceId == BtpTransport::sourceId());
    const Entry* entry = isSelf ? nullptr : findEntry(targetSourceId);

    // Diagnostico (plano 36 fase 0a): so as consultas direcionadas a um robo
    // contam -- "self" e enumeracao passam por outros caminhos.
    ++g_diagTargetedRx;
    if (isSelf || entry != nullptr) {
        ++g_diagTargetedHit;
    } else {
        ++g_diagTargetedMiss;
    }

    if (!isSelf && entry == nullptr) {
        return writeManifestData(requestHeader, kStatusRejected, kFlagCatalogComplete, kErrorNotFound, 0U, 1U, 0U,
                                 nullptr, 0U, 0U, 0U, false, "unknown source", nullptr, 0U, nullptr, 0U, 0U, 0U,
                                 output, outputCapacity);
    }

    const std::uint32_t foundBootId = isSelf ? BtpTransport::bootId() : entry->bootId;
    if (targetBootId != 0U && targetBootId != foundBootId) {
        return writeManifestData(requestHeader, kStatusRejected, kFlagCatalogComplete, kErrorStaleTargetBoot, 0U, 1U,
                                 0U, nullptr, 0U, 0U, 0U, false, "boot mismatch", nullptr, 0U, nullptr, 0U, 0U, 0U,
                                 output, outputCapacity);
    }

    const std::uint32_t foundRevision = isSelf ? kSelfConfigRevision : entry->configRevision;
    const std::uint8_t* foundUuid = isSelf ? g_selfUuid : entry->uuid;
    const std::uint8_t foundRole = isSelf ? kSourceRoleDongle : entry->role;
    const bool foundOnline = isSelf ? true : entry->online;
    const char* foundName = isSelf ? kSelfName : entry->name;
    const std::uint8_t* foundSourceInfo = isSelf ? g_selfSourceInfo : entry->sourceInfo;
    const std::size_t foundSourceInfoSize = isSelf ? g_selfSourceInfoSize : entry->sourceInfoSize;

    if (knownRevision != 0U && knownRevision == foundRevision) {
        return writeManifestData(requestHeader, kStatusSuccess,
                                 static_cast<std::uint8_t>(kFlagNotModified | kFlagCatalogComplete), kErrorNone, 0U,
                                 1U, foundRevision, foundUuid, targetSourceId, foundBootId, foundRole, foundOnline,
                                 foundName, foundSourceInfo, foundSourceInfoSize, nullptr, 0U, 0U, 0U, output,
                                 outputCapacity);
    }

    // Topico 27: a MANIFEST_REQUEST targeted at BtpTransport::sourceId() is
    // answered from DonglePublisher's schema tables, through this same
    // writeManifestData() call the robots' cached manifests use -- the
    // dongle's own catalog is served by the existing path, not by a parallel
    // responder object like bally_software's ManifestResponder.
    std::size_t selfRecordsSize = 0U;
    std::uint16_t selfTopicCount = 0U;
    const std::uint8_t* selfRecords =
        isSelf ? DonglePublisher::topicRecords(&selfRecordsSize, &selfTopicCount) : nullptr;

    const std::uint8_t* records = isSelf ? selfRecords : entry->records;
    const std::size_t recordsSize = isSelf ? selfRecordsSize : entry->recordsSize;
    const std::uint16_t topicCount = isSelf ? selfTopicCount : entry->topicCount;
    const std::uint16_t actionCount = isSelf ? 0U : entry->actionCount;

    return writeManifestData(requestHeader, kStatusSuccess, kFlagCatalogComplete, kErrorNone, 0U, 1U, foundRevision,
                             foundUuid, targetSourceId, foundBootId, foundRole, foundOnline, foundName, foundSourceInfo,
                             foundSourceInfoSize, records, recordsSize, topicCount, actionCount, output,
                             outputCapacity);
}

bool lookupTopicMaxRateMillihz(std::uint32_t sourceId, std::uint32_t topicId,
                               std::uint32_t* outMaxRateMillihz) noexcept {
    if (outMaxRateMillihz == nullptr || topicId == 0U || topicId > 0xFFFFU) return false;

    // Topico 27: the dongle's own topics are never in g_entries (that table
    // only holds manifests received over ESP-NOW), so a SUBSCRIBE aimed at
    // this dongle would have been answered NOT_FOUND. Answering it from
    // DonglePublisher here is what makes hub.* subscribable at all -- and it
    // keeps SerialMux::handleSubscribeRequest untouched, which is why a
    // desktop client needs no change to subscribe to the dongle.
    if (sourceId == BtpTransport::sourceId()) {
        return DonglePublisher::lookupTopicMaxRateMillihz(static_cast<std::uint16_t>(topicId),
                                                           outMaxRateMillihz);
    }

    const Entry* entry = findEntry(sourceId);
    if (entry == nullptr) return false;

    std::size_t pos = 0U;
    for (std::uint16_t i = 0U; i < entry->topicCount; ++i) {
        if (pos + 4U > entry->recordsSize) return false;  // corrupt cache; defensive
        const std::uint32_t contentSize = read_u32_le(entry->records + pos);
        const std::size_t recordTotalLen = 4U + static_cast<std::size_t>(contentSize);
        if (pos + recordTotalLen > entry->recordsSize || contentSize < 12U) return false;

        const std::uint8_t* content = entry->records + pos + 4U;
        const std::uint16_t recordTopicId = read_u16_le(content);
        if (recordTopicId == static_cast<std::uint16_t>(topicId)) {
            *outMaxRateMillihz = read_u32_le(content + 8U);
            return true;
        }
        pos += recordTotalLen;
    }
    return false;
}

}  // namespace ManifestCache
