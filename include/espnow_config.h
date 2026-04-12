#pragma once

#include <Arduino.h>
#include <EspNowManager.h>
#include <DatabaseStore.h>

class LcdTerminal;

#if defined(DEBUG)
#define ESP_NOW_RX_LATENCY_DEBUG 1
#else
#define ESP_NOW_RX_LATENCY_DEBUG 0
#endif

namespace EspNowConfig {

struct RxMessageEvent {
	uint8_t mac[6];
	EspNowManager::message incoming;
#if ESP_NOW_RX_LATENCY_DEBUG
	uint32_t receivedAtUs;
#endif
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

size_t flushRxDisplayLines(size_t maxLines = 8);

uint32_t takeDroppedRxCount();

} // namespace EspNowConfig
