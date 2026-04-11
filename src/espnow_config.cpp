#include "espnow_config.h"

namespace {

Stream* g_io = nullptr;

void onDataRecv(const uint8_t* mac, const EspNowManager::message& incomingData) {
    (void)mac;
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
    g_io = &io;
    manager.setReceiveCallback(onDataRecv);
    manager.setSendCallback(onDataSent);
}

} // namespace EspNowConfig
