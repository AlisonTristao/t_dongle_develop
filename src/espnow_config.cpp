#include "espnow_config.h"

#include <cstdio>

namespace {

// Shared output stream used by ESP-NOW callbacks.
Stream* g_io = nullptr;
EspNowManager* g_manager = nullptr;
DatabaseStore* g_database = nullptr;

void onDataRecv(const uint8_t* mac, const EspNowManager::message& incomingData) {
    // Keep RX callback lightweight: forward payload and persist when configured.
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

} // namespace EspNowConfig
