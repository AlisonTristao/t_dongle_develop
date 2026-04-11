#include "espnow_config.h"

#include <cstdio>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace {

// Shared output stream used by ESP-NOW callbacks.
Stream* g_io = nullptr;
EspNowManager* g_manager = nullptr;
DatabaseStore* g_database = nullptr;
QueueHandle_t g_rxQueue = nullptr;
volatile uint32_t g_droppedRxCount = 0;
bool g_asyncRxEnabled = false;

void processRxMessageInternal(const uint8_t mac[6], const EspNowManager::message& incomingData) {
    if (g_io != nullptr) {
        g_io->println(incomingData.msg);
    }

    if (mac != nullptr && g_manager != nullptr && g_manager->deviceIndexByMac(mac) < 0) {
        char autoName[24] = {0};
        std::snprintf(autoName, sizeof(autoName), "peer-%02X%02X", mac[4], mac[5]);
        g_manager->addDevice(mac, autoName, "adicionado automaticamente por RX ESP-NOW");
    }

    if (g_database != nullptr && g_database->isReady() && mac != nullptr) {
        g_database->logIncomingEspNow(mac, incomingData);
    }
}

void onDataRecv(const uint8_t* mac, const EspNowManager::message& incomingData) {
    if (mac == nullptr) {
        return;
    }

    if (g_asyncRxEnabled && g_rxQueue != nullptr) {
        EspNowConfig::RxMessageEvent event = {};
        std::memcpy(event.mac, mac, sizeof(event.mac));
        event.incoming = incomingData;

        if (xQueueSend(g_rxQueue, &event, 0) == pdTRUE) {
            return;
        }

        ++g_droppedRxCount;
        return;
    }

    processRxMessageInternal(mac, incomingData);
}

void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    (void)mac_addr;
    (void)status;
}

} // namespace

namespace EspNowConfig {

void attachCallbacks(EspNowManager& manager, Stream& io, DatabaseStore* database) {
    // Bind callbacks once and keep stream pointer for runtime logging.
    g_io = &io;
    g_manager = &manager;
    g_database = database;
    manager.setReceiveCallback(onDataRecv);
    manager.setSendCallback(onDataSent);
}

bool enableAsyncRx(size_t queueDepth) {
    if (queueDepth == 0) {
        queueDepth = 24;
    }

    if (g_rxQueue == nullptr) {
        g_rxQueue = xQueueCreate(static_cast<UBaseType_t>(queueDepth), sizeof(RxMessageEvent));
        if (g_rxQueue == nullptr) {
            g_asyncRxEnabled = false;
            return false;
        }
    }

    xQueueReset(g_rxQueue);
    g_droppedRxCount = 0;
    g_asyncRxEnabled = true;
    return true;
}

void disableAsyncRx() {
    g_asyncRxEnabled = false;
    if (g_rxQueue != nullptr) {
        xQueueReset(g_rxQueue);
    }
}

bool dequeueRxMessage(RxMessageEvent& outEvent, uint32_t timeoutMs) {
    if (g_rxQueue == nullptr) {
        return false;
    }

    const TickType_t waitTicks = (timeoutMs == 0)
        ? static_cast<TickType_t>(0)
        : pdMS_TO_TICKS(timeoutMs);

    return xQueueReceive(g_rxQueue, &outEvent, waitTicks) == pdTRUE;
}

void processRxMessage(const RxMessageEvent& event) {
    processRxMessageInternal(event.mac, event.incoming);
}

uint32_t takeDroppedRxCount() {
    const uint32_t dropped = g_droppedRxCount;
    g_droppedRxCount = 0;
    return dropped;
}

} // namespace EspNowConfig
