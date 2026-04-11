#pragma once

#include <Arduino.h>
#include <EspNowManager.h>
#include <DatabaseStore.h>

namespace EspNowConfig {

struct RxMessageEvent {
	uint8_t mac[6];
	EspNowManager::message incoming;
};

void attachCallbacks(EspNowManager& manager, Stream& io, DatabaseStore* database = nullptr);

bool enableAsyncRx(size_t queueDepth = 24);

void disableAsyncRx();

bool dequeueRxMessage(RxMessageEvent& outEvent, uint32_t timeoutMs = 0);

void processRxMessage(const RxMessageEvent& event);

uint32_t takeDroppedRxCount();

} // namespace EspNowConfig
