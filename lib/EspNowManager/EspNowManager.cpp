#include "EspNowManager.h"

#include <WiFi.h>
#include <cstring>

// Active instance used by static C callbacks required by ESP-NOW API.
EspNowManager* EspNowManager::activeInstance_ = nullptr;

// Build empty manager state; runtime init happens in begin().
EspNowManager::EspNowManager()
    : deviceCount_(0),
      initialized_(false),
      channel_(0),
      encrypt_(false),
      receiveCallback_(nullptr),
    sendCallback_(nullptr),
    sendWaitPending_(false),
    sendWaitCompleted_(false),
    sendWaitStatus_(ESP_NOW_SEND_FAIL),
    sendWaitMac_{0, 0, 0, 0, 0, 0},
    sendWaitHasMac_(false) {
}

// Initialize Wi-Fi station mode and register ESP-NOW callbacks.
bool EspNowManager::begin(uint8_t channel, bool encrypt) {
    channel_ = channel;
    encrypt_ = encrypt;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        initialized_ = false;
        return false;
    }

    initialized_ = true;
    activeInstance_ = this;

    // Bind static handlers, then restore already registered peers.
    esp_now_register_recv_cb(handleReceiveStatic);
    esp_now_register_send_cb(handleSendStatic);

    for (size_t i = 0; i < deviceCount_; ++i) {
        if (!addPeerToEspNow(devices_[i].mac)) {
            return false;
        }
    }

    return true;
}

// Deinitialize ESP-NOW and detach this instance from static callback dispatch.
void EspNowManager::end() {
    if (initialized_) {
        esp_now_deinit();
    }

    initialized_ = false;
    if (activeInstance_ == this) {
        activeInstance_ = nullptr;
    }
}

// Add one device to local registry and to ESP-NOW runtime when active.
bool EspNowManager::addDevice(const uint8_t mac[6], const char* name, const char* description) {
    if (mac == nullptr || deviceCount_ >= MAX_DEVICES) {
        return false;
    }

    if (findDeviceIndexByMac(mac) >= 0) {
        return false;
    }

    if (initialized_ && !addPeerToEspNow(mac)) {
        return false;
    }

    deviceInfo item = {};
    memcpy(item.mac, mac, sizeof(item.mac));
    copyText(item.name, sizeof(item.name), name);
    copyText(item.description, sizeof(item.description), description);

    devices_[deviceCount_] = item;
    ++deviceCount_;
    return true;
}

// Convenience overload to add from a prefilled struct.
bool EspNowManager::addDevice(const deviceInfo& device) {
    return addDevice(device.mac, device.name, device.description);
}

// Remove one device by index and compact local registry array.
bool EspNowManager::removeDeviceByIndex(size_t index) {
    if (index >= deviceCount_) {
        return false;
    }

    if (initialized_) {
        removePeerFromEspNow(devices_[index].mac);
    }

    for (size_t i = index; i + 1 < deviceCount_; ++i) {
        devices_[i] = devices_[i + 1];
    }

    --deviceCount_;
    devices_[deviceCount_] = {};
    return true;
}

// Remove one device by MAC when present.
bool EspNowManager::removeDeviceByMac(const uint8_t mac[6]) {
    const int index = findDeviceIndexByMac(mac);
    if (index < 0) {
        return false;
    }

    return removeDeviceByIndex(static_cast<size_t>(index));
}

// Update one device metadata by index.
bool EspNowManager::updateDeviceByIndex(size_t index, const char* name, const char* description) {
    if (index >= deviceCount_) {
        return false;
    }

    copyText(devices_[index].name, sizeof(devices_[index].name), name);
    copyText(devices_[index].description, sizeof(devices_[index].description), description);
    return true;
}

// Update one device metadata by MAC.
bool EspNowManager::updateDeviceByMac(const uint8_t mac[6], const char* name, const char* description) {
    const int index = findDeviceIndexByMac(mac);
    if (index < 0) {
        return false;
    }

    return updateDeviceByIndex(static_cast<size_t>(index), name, description);
}

// Remove all devices from both local storage and ESP-NOW peer table.
void EspNowManager::clearDevices() {
    if (initialized_) {
        for (size_t i = 0; i < deviceCount_; ++i) {
            removePeerFromEspNow(devices_[i].mac);
        }
    }

    for (size_t i = 0; i < MAX_DEVICES; ++i) {
        devices_[i] = {};
    }
    deviceCount_ = 0;
}

// Number of currently registered devices.
size_t EspNowManager::deviceCount() const {
    return deviceCount_;
}

// Read one device entry by index.
bool EspNowManager::deviceAt(size_t index, deviceInfo& outDevice) const {
    if (index >= deviceCount_) {
        return false;
    }

    outDevice = devices_[index];
    return true;
}

// Return pointer to internal list for read-only iteration.
const EspNowManager::deviceInfo* EspNowManager::deviceList() const {
    return devices_;
}

// Copy local registry into caller buffer with maxItems bound.
size_t EspNowManager::copyDeviceList(deviceInfo* outList, size_t maxItems) const {
    if (outList == nullptr || maxItems == 0) {
        return 0;
    }

    const size_t total = (deviceCount_ < maxItems) ? deviceCount_ : maxItems;
    for (size_t i = 0; i < total; ++i) {
        outList[i] = devices_[i];
    }

    return total;
}

// Public lookup helper for MAC address.
int EspNowManager::deviceIndexByMac(const uint8_t mac[6]) const {
    return findDeviceIndexByMac(mac);
}

// Send one message to every registered device.
bool EspNowManager::sendToAll(const message& outgoing) const {
    if (!initialized_ || deviceCount_ == 0) {
        return false;
    }

    bool sentAtLeastOne = false;
    for (size_t i = 0; i < deviceCount_; ++i) {
        if (sendToDevice(i, outgoing)) {
            sentAtLeastOne = true;
        }
    }

    return sentAtLeastOne;
}

// Send one message by device index.
bool EspNowManager::sendToDevice(size_t index, const message& outgoing) const {
    if (index >= deviceCount_) {
        return false;
    }

    return sendToMac(devices_[index].mac, outgoing);
}

// Send one message directly to target MAC.
bool EspNowManager::sendToMac(const uint8_t mac[6], const message& outgoing) const {
    if (!initialized_ || mac == nullptr) {
        return false;
    }

    return esp_now_send(mac, reinterpret_cast<const uint8_t*>(&outgoing), sizeof(outgoing)) == ESP_OK;
}

// Send to one index and wait for callback delivery status.
bool EspNowManager::sendToDeviceWithStatus(size_t index, const message& outgoing, bool& outDelivered, uint32_t timeoutMs) const {
    outDelivered = false;
    if (index >= deviceCount_) {
        return false;
    }

    return sendToMacWithStatus(devices_[index].mac, outgoing, outDelivered, timeoutMs);
}

// Send to one MAC and wait for callback delivery status.
bool EspNowManager::sendToMacWithStatus(const uint8_t mac[6], const message& outgoing, bool& outDelivered, uint32_t timeoutMs) const {
    outDelivered = false;
    if (!initialized_ || mac == nullptr) {
        return false;
    }

    sendWaitPending_ = true;
    sendWaitCompleted_ = false;
    sendWaitStatus_ = ESP_NOW_SEND_FAIL;
    memcpy(sendWaitMac_, mac, sizeof(sendWaitMac_));
    sendWaitHasMac_ = true;

    if (esp_now_send(mac, reinterpret_cast<const uint8_t*>(&outgoing), sizeof(outgoing)) != ESP_OK) {
        sendWaitPending_ = false;
        sendWaitHasMac_ = false;
        return false;
    }

    const uint32_t start = millis();
    while (sendWaitPending_) {
        if ((millis() - start) >= timeoutMs) {
            sendWaitPending_ = false;
            sendWaitHasMac_ = false;
            return false;
        }

        delay(1);
    }

    sendWaitHasMac_ = false;
    if (!sendWaitCompleted_) {
        return false;
    }

    outDelivered = (sendWaitStatus_ == ESP_NOW_SEND_SUCCESS);
    return true;
}

// Send to all peers and aggregate delivery status.
bool EspNowManager::sendToAllWithStatus(
    const message& outgoing,
    size_t& outDeliveredCount,
    size_t& outTriedCount,
    uint32_t timeoutMs
) const {
    outDeliveredCount = 0;
    outTriedCount = 0;

    if (!initialized_ || deviceCount_ == 0) {
        return false;
    }

    for (size_t i = 0; i < deviceCount_; ++i) {
        bool delivered = false;
        const bool gotStatus = sendToDeviceWithStatus(i, outgoing, delivered, timeoutMs);
        if (!gotStatus) {
            continue;
        }

        ++outTriedCount;
        if (delivered) {
            ++outDeliveredCount;
        }
    }

    return outTriedCount > 0;
}

// Register high-level receive callback.
void EspNowManager::setReceiveCallback(ReceiveCallback callback) {
    receiveCallback_ = callback;
}

// Register high-level send-status callback.
void EspNowManager::setSendCallback(SendCallback callback) {
    sendCallback_ = callback;
}

// Adapt raw ESP-NOW payload to typed message and dispatch to instance callback.
void EspNowManager::handleReceiveStatic(const uint8_t* mac, const uint8_t* incomingData, int len) {
    if (activeInstance_ == nullptr || activeInstance_->receiveCallback_ == nullptr || incomingData == nullptr || len <= 0) {
        return;
    }

    message incoming = {};

    // If sender used full struct, deserialize directly.
    if (len >= static_cast<int>(sizeof(message))) {
        memcpy(&incoming, incomingData, sizeof(message));
        incoming.msg[sizeof(incoming.msg) - 1] = '\0';
    } else {
        // Compatibility path for raw text payloads.
        const size_t copySize = (static_cast<size_t>(len) < (sizeof(incoming.msg) - 1))
            ? static_cast<size_t>(len)
            : (sizeof(incoming.msg) - 1);
        memcpy(incoming.msg, incomingData, copySize);
        incoming.msg[copySize] = '\0';
        incoming.timer = millis();
        incoming.type = logType::NONE;
    }

    activeInstance_->receiveCallback_(mac, incoming);
}

// Dispatch low-level send result to user callback.
void EspNowManager::handleSendStatic(const uint8_t* mac, esp_now_send_status_t status) {
    if (activeInstance_ == nullptr) {
        return;
    }

    if (activeInstance_->sendWaitPending_ && activeInstance_->sendWaitHasMac_ && mac != nullptr) {
        if (memcmp(activeInstance_->sendWaitMac_, mac, 6) == 0) {
            activeInstance_->sendWaitStatus_ = status;
            activeInstance_->sendWaitCompleted_ = true;
            activeInstance_->sendWaitPending_ = false;
        }
    }

    if (activeInstance_->sendCallback_ == nullptr) {
        return;
    }

    activeInstance_->sendCallback_(mac, status);
}

// Add peer in ESP-NOW runtime if not already present.
bool EspNowManager::addPeerToEspNow(const uint8_t mac[6]) const {
    if (esp_now_is_peer_exist(mac)) {
        return true;
    }

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = channel_;
    peerInfo.encrypt = encrypt_;

    return esp_now_add_peer(&peerInfo) == ESP_OK;
}

// Remove peer from ESP-NOW runtime table.
bool EspNowManager::removePeerFromEspNow(const uint8_t mac[6]) const {
    if (!esp_now_is_peer_exist(mac)) {
        return true;
    }

    return esp_now_del_peer(mac) == ESP_OK;
}

// Search local device list by MAC address.
int EspNowManager::findDeviceIndexByMac(const uint8_t mac[6]) const {
    if (mac == nullptr) {
        return -1;
    }

    for (size_t i = 0; i < deviceCount_; ++i) {
        if (memcmp(devices_[i].mac, mac, 6) == 0) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

// Safe copy helper for metadata fields.
void EspNowManager::copyText(char* dst, size_t dstSize, const char* src) {
    if (dst == nullptr || dstSize == 0) {
        return;
    }

    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}
