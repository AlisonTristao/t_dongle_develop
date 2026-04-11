#include "espnow_config.h"

namespace {

// Shared output stream used by ESP-NOW callbacks.
Stream* g_io = nullptr;

void onDataRecv(const uint8_t* mac, const EspNowManager::message& incomingData) {
    (void)mac;
    // Keep RX callback lightweight: just forward received payload to serial.
    if (g_io != nullptr) {
        g_io->println(incomingData.msg);
    }
}

void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    (void)mac_addr;
    (void)status;
}

} // namespace

namespace EspNowConfig {

void attachCallbacks(EspNowManager& manager, Stream& io) {
    // Bind callbacks once and keep stream pointer for runtime logging.
    g_io = &io;
    manager.setReceiveCallback(onDataRecv);
    manager.setSendCallback(onDataSent);
}

} // namespace EspNowConfig
