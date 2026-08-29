#pragma once

#include <Arduino.h>
#include <EspNowManager.h>
#include <DatabaseStore.h>
#include <ProtocolRouter.h>

class LcdDashboard;

#ifndef RX_ASYNC_QUEUE_DEPTH
#define RX_ASYNC_QUEUE_DEPTH 24
#endif

// Each slot of these two queues is a ProtocolRouter::RoutedMessage, which
// carries a payload[kMaxPayloadSize=616] buffer -- ~680 B/slot. 32 slots
// each was ~44 KB of heap for the pair, reserved up front in
// enableAsyncRx() during a boot sequence that was already running out of
// internal RAM (topico 33 / topico 34 section 1B). 16 is still four beats
// of burst headroom at the main loop's drain rate.
#ifndef RX_LOG_QUEUE_DEPTH
#define RX_LOG_QUEUE_DEPTH 16
#endif

#ifndef RX_TELEMETRY_QUEUE_DEPTH
#define RX_TELEMETRY_QUEUE_DEPTH 16
#endif

#ifndef RX_TERMINAL_QUEUE_DEPTH
#define RX_TERMINAL_QUEUE_DEPTH 16
#endif

#ifndef RX_CONTROL_QUEUE_DEPTH
#define RX_CONTROL_QUEUE_DEPTH 8
#endif

#ifndef RX_COMMAND_QUEUE_DEPTH
#define RX_COMMAND_QUEUE_DEPTH 8
#endif

#ifndef HEARTBEAT_INTERVAL_MS
#define HEARTBEAT_INTERVAL_MS 200 // 5x/s
#endif

#ifndef HEARTBEAT_SEND_TIMEOUT_MS
#define HEARTBEAT_SEND_TIMEOUT_MS 150 // must stay under HEARTBEAT_INTERVAL_MS
#endif

namespace EspNowConfig {

/** One raw ESP-NOW datagram, exactly as received from the radio (no BTP
 * decoding yet -- see ProtocolRouter for that). */
struct RxDatagramEvent {
	uint8_t mac[6];
	uint8_t data[EspNowManager::MAX_DATA_LEN];
	size_t len;
};

void attachCallbacks(
	EspNowManager& manager,
	Stream& io,
	DatabaseStore* database = nullptr,
	LcdDashboard* lcdDashboard = nullptr
);

bool enableAsyncRx(size_t queueDepth = RX_ASYNC_QUEUE_DEPTH);

void disableAsyncRx();

bool dequeueRxDatagram(RxDatagramEvent& outEvent, uint32_t timeoutMs = 0);

/**
 * @brief Classifies one raw datagram and branches (topico 28).
 *
 * Reads the BTP envelope and relays non-candidates verbatim, fragment by
 * fragment, with no reassembly and no re-encoding (SerialMux::relayUp, D5).
 * COMMAND, MANIFEST_DATA and STATUS are candidates: they are reassembled and
 * opened with key L before bally::dongle_consumes() reads their authenticated
 * plaintext reference. A failed L open is endpoint traffic and its original
 * raw fragments are relayed, so E-key ciphertext is never inspected.
 *
 * Everything while the port is still console-owned (no client to relay to)
 * also takes the pre-hub path: decode + reassemble via ProtocolRouter, then
 * dispatch -- COMMAND synchronously here
 * (remote execution shouldn't wait for the next drainRoutedQueues() call),
 * LOG/TELEMETRY/TERMINAL/CONTROL into their own bounded queue.
 */
void processRxDatagram(const RxDatagramEvent& event);

/**
 * @brief Drains up to maxItemsPerQueue entries from each of the CONTROL/LOG/
 * TELEMETRY/TERMINAL queues, in that priority order.
 *
 * Since topico 28 these queues only ever see what the dongle consumes, plus
 * everything at all while the port is console-owned: with a client attached,
 * the rest is relayed straight off the radio and never routed. LOG is printed
 * to the console (source_id-tagged); CONTROL/MANIFEST_DATA feeds
 * ManifestCache; TELEMETRY and TERMINAL are drained and discarded on this
 * path, since their only real consumer is the relay.
 * @return total entries drained across all four queues.
 */
size_t drainRoutedQueues(size_t maxItemsPerQueue = 8);

/** Each returns what accumulated since its own last call and is consumed by
 * AppRuntime::flushPendingEspNowOutput every tick to feed the LCD's dropped
 * tile. Use peekRxCounters() instead when you need a value that survives
 * being read. */
uint32_t takeDroppedRxCount();
uint32_t takeDroppedDecodeCount();
uint32_t takeDroppedCrcCount();
uint32_t takeDroppedReassemblyCount();
uint32_t takeDroppedQueueFullCount();
/** Topico 30: a consumed (channel C) frame that did not open under key L --
 * no key configured, missing ENCRYPTED, wrong cipher, or a forged/corrupted
 * tag. Not part of RxCounters/hub.link (see the .cpp comment); read by
 * `espnow -stats` and by AppRuntime's dropped-LCD-tile tally. */
uint32_t takeDroppedAuthCount();

/** Non-destructive cumulative-since-boot read of the same counter
 * takeDroppedAuthCount() drains -- what `espnow -stats` prints, since that
 * command (like the rest of StatsSnapshot) reports running totals a user
 * diffs across two calls, not a delta consumed by a single reader. */
uint32_t peekDroppedAuthCount();

/** Radio datagrams dropped because async RX never initialized (heap-starved
 * boot -- see the .cpp comment on onDataRecv's fallback). Any nonzero value
 * means the dongle booted degraded and is deaf to the radio; `espnow -stats`
 * surfaces it. Not part of RxCounters/hub.link. */
uint32_t peekSyncFallbackDropCount();

/**
 * @brief Cumulative RX counters for the ESP-NOW hop, counted since the last
 * enableAsyncRx() (in practice, since boot).
 *
 * The takeDropped*Count() family only ever answered "what was lost", and it
 * answers it destructively -- which is enough to light up an LCD tile and not
 * enough to compute a rate. These totals add the missing half ("what came
 * through") and clear nothing, so two snapshots plus the elapsed time give the
 * real ingress rate of the radio hop, per message type.
 *
 * That number is the one thing the throughput investigation had no way to
 * measure: the dongle could see the USB side via the STATUS heartbeat's
 * frames_tx/bytes_total, but nothing observed how much ESP-NOW actually
 * delivered, so it was impossible to tell a saturated radio from a saturated
 * USB link. `espnow -stats` prints these; see EspNowCommands.
 *
 * `datagrams` counts every datagram handed to ProtocolRouter, so
 * `datagrams - fragmentsAccepted - (every dropped* field)` is the number that
 * should match the sum of the routed* fields.
 */
struct RxCounters {
    uint32_t datagrams;
    uint32_t fragmentsAccepted;
    uint32_t routedTelemetry;
    uint32_t routedLog;
    uint32_t routedCommand;
    uint32_t routedControl;
    uint32_t routedTerminal;
    uint32_t droppedRx;
    uint32_t droppedDecode;
    uint32_t droppedCrc;
    uint32_t droppedReassembly;
    uint32_t droppedQueueFull;
};

void peekRxCounters(RxCounters& out);

/** Cumulative counters from the single-owner radio TX scheduler. */
void peekTxSchedulerCounters(EspNowManager::TxSchedulerCounters& out);

/**
 * @brief Sends one heartbeat probe (BTP CONTROL/STATUS, object_id 0x0009,
 * empty payload -- "spontaneous and gets no response" per
 * BTP/docs/commands.md section 5) to the most recently seen
 * ESP-NOW peer (the last one we received real data from, not another
 * heartbeat) and updates the dashboard's link tile with the delivery result.
 * No-op when no peer has sent us anything yet. Meant to be called on a fixed
 * timer, e.g. every HEARTBEAT_INTERVAL_MS from its own task (this call
 * blocks up to HEARTBEAT_SEND_TIMEOUT_MS waiting for the ESP-NOW send
 * callback).
 *
 * Topico 30: sealed with key L (channel C, RadioSeal::seal) like every other
 * message this dongle originates over the radio. Without a key configured
 * (DongleKeyStore::hasKeyL() false) the seal fails and this returns before
 * ever calling notePeerLinkResult(), so a peer's `linkOk`/hub.peers `online`
 * simply never turns true from this path alone -- fail-closed, not a stale
 * "last known good".
 *
 * This probe gets no response (BTP/docs/commands.md section 5), so a
 * successful notePeerLinkResult() from here is only "the radio ACK'd my
 * sealed probe to that MAC", never proof the PEER holds key L too -- an
 * ESP-NOW send-status callback is a link-layer fact about the local radio,
 * not an application-level answer. The stronger, genuinely bidirectional
 * signal is EspNowConfig.cpp's processRxDatagramInternal: every CONSUMED
 * inbound frame from a peer is opened with RadioSeal::open(), and that
 * outcome (success or TagMismatch) also feeds notePeerLinkResult() -- a
 * peer that cannot produce a frame this dongle can open under key L gets
 * marked NOT online even if its heartbeat replies keep ACKing at the radio
 * layer. `online` is the AND of both signals settling on "yes".
 */
void heartbeatTick();

/**
 * @brief Topico 28: writes one already-encoded BTP frame to a peer MAC,
 * unchanged. The whole point is that it takes octets and not a message: it is
 * bound to SerialMux::RelayToRadioFn, whose caller (relayDown) has a frame
 * that belongs to somebody else and must not be re-originated -- rewriting
 * source_id/boot_id/sequence would rewrite the AEAD nonce
 * (BTP/docs/encryption.md section 4).
 *
 * This replaces topico 17's requestUpstreamSubscribe/requestUpstreamUnsubscribe,
 * removed here: a robot's subscriptions moved to channel B, so the dongle no
 * longer merges its clients into a SUBSCRIBE of its own toward the radio.
 */
bool sendRawToMac(const uint8_t mac[6], const uint8_t* frame, size_t frameSize);

} // namespace EspNowConfig
