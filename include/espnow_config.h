#pragma once

#include <Arduino.h>
#include <EspNowManager.h>
#include <DatabaseStore.h>

class LcdTerminal;

#ifndef HIGH_FREQUENCY_INCOMMING_ESPNOW
#define HIGH_FREQUENCY_INCOMMING_ESPNOW 0
#endif

#ifndef RX_ASYNC_QUEUE_DEPTH
#define RX_ASYNC_QUEUE_DEPTH 24
#endif

#ifndef RX_DB_LOG_QUEUE_DEPTH
#if HIGH_FREQUENCY_INCOMMING_ESPNOW
#define RX_DB_LOG_QUEUE_DEPTH 128
#else
#define RX_DB_LOG_QUEUE_DEPTH RX_ASYNC_QUEUE_DEPTH
#endif
#endif

#ifndef RX_DB_LOG_DYNAMIC_ALLOC
#if HIGH_FREQUENCY_INCOMMING_ESPNOW
#define RX_DB_LOG_DYNAMIC_ALLOC 1
#else
#define RX_DB_LOG_DYNAMIC_ALLOC 0
#endif
#endif

#ifndef RX_DB_LOG_HEAP_RESERVE_BYTES
#define RX_DB_LOG_HEAP_RESERVE_BYTES 65536
#endif

#if defined(RX_DB_AUTO_FLUSH_PERCENT) && !defined(RX_DB_WARNING_PERCENT)
#define RX_DB_WARNING_PERCENT RX_DB_AUTO_FLUSH_PERCENT
#endif

#ifndef RX_DB_WARNING_PERCENT
#define RX_DB_WARNING_PERCENT 80
#endif

#ifndef RX_DB_AUTO_FLUSH_PERCENT
#define RX_DB_AUTO_FLUSH_PERCENT RX_DB_WARNING_PERCENT
#endif

namespace EspNowConfig {

struct RxMessageEvent {
	uint8_t mac[6];
	EspNowManager::message incoming;
};

struct RxDbLogEvent {
	uint8_t mac[6];
	EspNowManager::message incoming;
};

void attachCallbacks(
	EspNowManager& manager,
	Stream& io,
	DatabaseStore* database = nullptr,
	LcdTerminal* lcdTerminal = nullptr
);

bool enableAsyncRx(size_t queueDepth = 24);

void disableAsyncRx();

bool dequeueRxMessage(RxMessageEvent& outEvent, uint32_t timeoutMs = 0);

void processRxMessage(const RxMessageEvent& event);

bool dequeueRxDbLog(RxDbLogEvent& outEvent, uint32_t timeoutMs = 0);

void processRxDbLog(const RxDbLogEvent& event);

void setAsyncDbLogEnabled(bool enabled);

size_t flushRxDbLogBuffer(size_t maxItems = 0);

size_t pendingRxDbLogCount();

size_t rxDbLogCapacity();

size_t flushRxDisplayLines(size_t maxLines = 8);

uint32_t takeDroppedRxCount();

uint32_t takeDroppedRxDbLogCount();

} // namespace EspNowConfig
