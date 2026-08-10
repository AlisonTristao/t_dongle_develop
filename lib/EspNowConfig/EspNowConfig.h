#pragma once

#include <Arduino.h>
#include <EspNowManager.h>
#include <DatabaseStore.h>
#include <ProtocolRouter.h>

class LcdDashboard;

#ifndef RX_ASYNC_QUEUE_DEPTH
#define RX_ASYNC_QUEUE_DEPTH 24
#endif

#ifndef RX_LOG_QUEUE_DEPTH
#define RX_LOG_QUEUE_DEPTH 32
#endif

#ifndef RX_TELEMETRY_QUEUE_DEPTH
#define RX_TELEMETRY_QUEUE_DEPTH 32
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
 * @brief Decodes+routes one raw datagram (ProtocolRouter), then dispatches
 * the result: COMMAND is handled synchronously here (remote execution
 * shouldn't wait for the next drainRoutedQueues() call); LOG/TELEMETRY/
 * TERMINAL/CONTROL are pushed into their own bounded queue for
 * drainRoutedQueues() to consume from the main loop.
 */
void processRxDatagram(const RxDatagramEvent& event);

/**
 * @brief Drains up to maxItemsPerQueue entries from each of the CONTROL/LOG/
 * TELEMETRY/TERMINAL queues, in that priority order. LOG entries are printed
 * (source_id-tagged, no packet/arrival framing). TELEMETRY/TERMINAL/CONTROL
 * have no consumer yet in this topic (bytes only, no String/printf
 * formatting is applied to them) so they are drained and discarded, freeing
 * queue capacity; only their drop/processed counters are observable.
 * @return total entries drained across all four queues.
 */
size_t drainRoutedQueues(size_t maxItemsPerQueue = 8);

uint32_t takeDroppedRxCount();
uint32_t takeDroppedDecodeCount();
uint32_t takeDroppedCrcCount();
uint32_t takeDroppedReassemblyCount();
uint32_t takeDroppedQueueFullCount();

/**
 * @brief Sends one heartbeat probe (BTP CONTROL/STATUS, object_id 0x0009,
 * empty payload -- "publicação espontânea, sem resposta" per
 * bally_protocol/docs/COMMANDS_AND_ACTIONS.md) to the most recently seen
 * ESP-NOW peer (the last one we received real data from, not another
 * heartbeat) and updates the dashboard's link tile with the delivery result.
 * No-op when no peer has sent us anything yet. Meant to be called on a fixed
 * timer, e.g. every HEARTBEAT_INTERVAL_MS from its own task (this call
 * blocks up to HEARTBEAT_SEND_TIMEOUT_MS waiting for the ESP-NOW send
 * callback).
 */
void heartbeatTick();

/**
 * @brief Topico 17 (PASSO 3): sends a SUBSCRIBE toward the robot identified
 * by sourceId, over ESP-NOW, so it starts (or adjusts) publishing topicId at
 * (up to) rateMillihz. Fire-and-forget, matching every other best-effort
 * CONTROL exchange in this dongle (primeManifestIfNeeded, heartbeatTick) --
 * SerialMux already answered the desktop client synchronously from its own
 * cached knowledge (see SerialMux.cpp's handleSubscribeRequest), so this call
 * is not on that reply's critical path. No-op if this dongle has never heard
 * a BTP frame from sourceId yet (BtpTransport::lookupPeerMacBySourceId
 * fails) -- matches the same "can only target an already-observed peer"
 * limitation topico 12 documented for COMMAND_REQUEST. Meant to be passed as
 * SerialMux::RequestUpstreamSubscribeFn.
 */
void requestUpstreamSubscribe(uint32_t sourceId, uint32_t topicId, uint32_t rateMillihz, uint32_t leaseMs);

/**
 * @brief Topico 17 (PASSO 5): sends an UNSUBSCRIBE toward the robot,
 * releasing upstreamSubscriptionId (the id the robot itself granted, from
 * SubscriptionRegistry::upstreamSubscriptionId -- COMMANDS_AND_ACTIONS.md
 * section 7's UNSUBSCRIBE targets a subscription_id, not a topic_id). Same
 * fire-and-forget/no-op-if-unknown-peer contract as requestUpstreamSubscribe.
 * Meant to be passed as SerialMux::RequestUpstreamUnsubscribeFn.
 */
void requestUpstreamUnsubscribe(uint32_t sourceId, uint32_t topicId, uint32_t upstreamSubscriptionId);

} // namespace EspNowConfig
