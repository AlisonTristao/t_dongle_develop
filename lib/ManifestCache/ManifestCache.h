#pragma once

#include <btp/codec.hpp>

#include <cstddef>
#include <cstdint>

/**
 * @brief Dongle-side cache/aggregator for BTP v1 MANIFEST_DATA
 * (BTP/docs/commands.md section 3), topico 16.
 *
 * One entry per robot source_id, keyed by the identity actually observed
 * over ESP-NOW (BtpTransport::rememberAuthenticatedPeer already tracks
 * authenticated (mac, source_id,
 * boot_id); this cache adds "and what topics/actions does that source_id
 * publish"). Topic/action records are stored **verbatim** as the raw,
 * already-length-prefixed bytes the robot sent -- this dongle relays a
 * catalog, it does not need to understand field internals (scale/offset/
 * enum labels/...) to forward them correctly, only to walk each record's
 * own `record_size` prefix to find its boundary (commands.md
 * section 3.3: "Every record begins with record_size..."). That is also why
 * this cache never needs its own parallel field-schema representation.
 *
 * In-memory only, rebuilt across a dongle reboot rather than persisted to
 * DatabaseStore (topico 16 PASSO 4's "decida o que persistir" decision --
 * see this repo's topico 16 RESULTADO for the full rationale): a robot's
 * boot_id is itself not persisted anywhere and normally changes every robot
 * boot, so a manifest cached from a previous dongle boot would need
 * revalidating on first contact anyway. Rediscovery is cheap and automatic:
 * EspNowConfig calls shouldRequestManifest() for every peer it hears from,
 * and the existing device registry (DatabaseStore, already persisted)
 * means a known peer is recognized immediately, before any manifest
 * exchange completes.
 *
 * Pure C++ (no Arduino/FreeRTOS), same shape as ProtocolRouter/BtpTransport,
 * so it can run under env:native if a future topico adds tests here.
 */
namespace ManifestCache {

constexpr std::uint16_t kManifestRequestObjectId = 0x0003U;
constexpr std::uint16_t kManifestDataObjectId = 0x0004U;
constexpr std::uint16_t kManifestFormatVersion = 1U;

constexpr std::uint8_t kSourceRoleRobot = 0x01U;
constexpr std::uint8_t kSourceRoleDongle = 0x02U;

// How many distinct robot source_ids this dongle caches manifests for.
// Generous for the current one-robot deployments; raise if a multi-robot
// setup needs more (bounded, no dynamic allocation).
constexpr std::size_t kCapacity = 8U;

// Per-source raw records (topic + action records, concatenated, each
// self-delimited by its own record_size) budget. Comfortably covers
// bally_software's current two-topic schema (~200 octets) with headroom for
// a handful more fields/topics; well under the manifest's 49152-octet wire
// ceiling -- see this repo's topico 16 RESULTADO for why a bigger ceiling
// was not built out now.
constexpr std::size_t kMaxRecordsBytes = 2048U;
constexpr std::size_t kMaxNameLength = 64U;

// Minimum time between two MANIFEST_REQUESTs this dongle sends to the same
// (source_id, boot_id) while waiting for a reply, so a 50Hz TELEMETRY stream
// from a not-yet-cached robot does not flood it with duplicate requests.
//
// The first kFastRequestAttempts go out at the faster kFastRequestCooldownMs
// cadence: a robot that just powered on (or one the dongle evicted, or one a
// hub child asked about before the dongle had heard it) should show its
// catalog in a second or two, not wait out a full 3s cooldown per try. After
// the fast burst it falls back to kRequestCooldownMs and keeps retrying at
// that rate for as long as the robot stays uncached -- there is no attempt
// cap: a robot whose ManifestResponder is wedged but whose STATUS still
// arrives must not be given up on, only asked about less often.
constexpr std::uint32_t kRequestCooldownMs = 3000U;
constexpr std::uint32_t kFastRequestCooldownMs = 1000U;
constexpr std::uint32_t kFastRequestAttempts = 3U;

void configure(const std::uint8_t selfUuid[16]) noexcept;

// True if this dongle has no usable manifest for (sourceId, bootId) yet
// (never cached, or cached under a different boot_id -- i.e. the peer
// rebooted) and the last request for it (if any) is old enough to retry.
// Has the side effect of arming the cooldown, i.e. treat a `true` result as
// "and I am about to send one now" -- matches
// BtpTransport::rememberAuthenticatedPeer's
// "called once per routed frame" contract.
bool shouldRequestManifest(std::uint32_t sourceId, std::uint32_t bootId, std::uint32_t nowMs) noexcept;

// Builds a 12-byte MANIFEST_REQUEST payload. Returns 0 on failure (capacity
// too small).
std::size_t buildRequest(std::uint32_t targetSourceId, std::uint32_t targetBootId, std::uint32_t knownRevision,
                         std::uint8_t* output, std::size_t outputCapacity) noexcept;

// Parses a MANIFEST_DATA payload (from a robot, over ESP-NOW, answering a
// request this dongle sent) and updates the cache entry for its
// described_source_id. Only SUCCESS responses (optionally NOT_MODIFIED)
// update anything; malformed payloads, non-SUCCESS statuses and payloads
// whose record framing doesn't validate are ignored. Returns true if the
// cache was updated (or confirmed unchanged via NOT_MODIFIED).
bool ingestManifestData(btp::ByteView payload, std::uint32_t nowMs) noexcept;

// This dongle's own aggregate manifest-catalog revision: starts at 1,
// increments whenever a cached source is added or its content changes.
// Reported in this dongle's own HELLO_RESULT (PASSO 5) so a client can tell
// whether its cached view of *this dongle's* catalog might be stale --
// distinct from any individual robot's own config_revision, which lives
// inside that robot's cache entry / MANIFEST_DATA.
std::uint32_t catalogRevision() noexcept;

// Number of entries a target_source_id=0 enumeration would produce: this
// dongle's own self-description (always index 0, role=DONGLE, carrying the
// hub.* topic records DonglePublisher declares -- topico 27) plus one per
// cached robot source, sorted by source_id (commands.md section 3.1: "sorts
// them by source_id").
std::size_t enumerationCount() noexcept;

// Builds ONE MANIFEST_DATA response (SUCCESS, CATALOG_COMPLETE set only on
// the last index) for enumeration index `index` (0 = self, per above).
// requestHeader supplies the request-reference triple to echo back
// (commands.md section 1). Whole topic/action records that do
// not fit in outputCapacity are dropped (never emitted partially); the
// response's own topic_count/action_count reflect what was actually
// written, not the cached count -- see this repo's topico 16 RESULTADO.
// Returns 0 on failure (bad index or capacity too small even for the
// fixed prefix).
std::size_t buildEnumerationResponse(std::size_t index, const btp::Header& requestHeader, std::uint8_t* output,
                                     std::size_t outputCapacity) noexcept;

// Builds the MANIFEST_DATA response to a single targeted MANIFEST_REQUEST
// (target_source_id != 0): NOT_FOUND if unknown, STALE_TARGET_BOOT if
// targetBootId is non-zero and does not match the cached boot, NOT_MODIFIED
// if knownRevision matches the cached revision, else a full SUCCESS
// descriptor (same truncation rule as buildEnumerationResponse). Returns 0
// on failure (capacity too small even for the fixed prefix).
std::size_t buildTargetedResponse(std::uint32_t targetSourceId, std::uint32_t targetBootId,
                                  std::uint32_t knownRevision, const btp::Header& requestHeader,
                                  std::uint8_t* output, std::size_t outputCapacity) noexcept;

// Topico 17: walks the cached, verbatim topic records for sourceId looking
// for topicId, and reads its max_rate_millihz (commands.md section 3.3's
// per-topic
// record, offset 8 of the record's content -- topic_id/schema_version(2
// each), encoding/flags(1 each), field_count(2), then max_rate_millihz(4)).
// Returns false when the source or topic is not cached; SubscriptionRegistry
// callers treat that as "cannot grant a subscription for an unknown topic"
// (REJECTED/NOT_FOUND), the same way a MANIFEST_REQUEST for an unknown topic
// would be handled by the source itself.
//
// Topico 27: when sourceId is this dongle's own, the answer comes from
// DonglePublisher's schema tables instead of the cache, so a desktop client
// subscribes to hub.link/hub.usb/hub.peers through the exact same
// SUBSCRIBE handler it uses for a robot topic.
bool lookupTopicMaxRateMillihz(std::uint32_t sourceId, std::uint32_t topicId,
                               std::uint32_t* outMaxRateMillihz) noexcept;

// --- Diagnostico do caminho do manifesto (plano 36, fase 0a) --------------
// Contadores cumulativos desde o boot + um resumo das entradas em cache, para
// o comando "hub -manifest" dizer em qual elo a cadeia
// prime -> resposta -> ingest -> serve quebra, sem depender de log async (que
// e engolido quando uma sessao BTP e dona da porta).

struct DiagnosticEntry {
    std::uint32_t sourceId;
    std::uint32_t bootId;
    std::uint32_t configRevision;
    std::uint16_t topicCount;
    std::uint16_t actionCount;
    std::uint16_t recordsSize;
    bool online;
};

// A (source_id, boot_id) this dongle is currently chasing a manifest for --
// prime sent, no usable MANIFEST_DATA back yet. `attempts` is how many
// MANIFEST_REQUESTs have gone out for this exact chase (resets to 1 when the
// peer reboots). A row here with a climbing `attempts` and no matching entry
// above is "the robot hears us but never answers".
struct PendingDiagnostic {
    std::uint32_t sourceId;
    std::uint32_t bootId;
    std::uint32_t attempts;
};

struct Diagnostics {
    std::uint32_t primeRequestsSent;      // notePrimeSent() em EspNowConfig
    std::uint32_t ingestedOk;             // ingestManifestData() -> true
    std::uint32_t ingestFailed;           // ingestManifestData() -> false
    std::uint32_t consumeRejected;        // noteConsumeRejected() em EspNowConfig
    std::uint32_t targetedRequestsRx;     // buildTargetedResponse() chamado
    std::uint32_t targetedServedHit;      // ... com a entrada em cache
    std::uint32_t targetedServedMiss;     // ... sem a entrada (NOT_FOUND)
    std::size_t entryCount;
    DiagnosticEntry entries[kCapacity];
    std::size_t pendingCount;
    PendingDiagnostic pending[kCapacity];
};

Diagnostics diagnostics() noexcept;

// Cheap cumulative-since-boot reads of the two manifest-path failure counters,
// for a per-tick watcher (AppRuntime::processAsyncWarnings) that only wants
// the delta and must not copy the whole Diagnostics struct every loop.
std::uint32_t ingestFailedTotal() noexcept;
std::uint32_t consumeRejectedTotal() noexcept;

// Chamados de fora (EspNowConfig) para registrar eventos que a cache em si
// nao ve.
void notePrimeSent() noexcept;
void noteConsumeRejected() noexcept;

// Zera cache, pendencias, catalogRevision e todos os contadores de
// diagnostico. So para o env:native (test_manifest_cache), mesmo padrao de
// DonglePublisher::resetForTests().
void resetForTests() noexcept;

}  // namespace ManifestCache
