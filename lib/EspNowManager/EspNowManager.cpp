#include "EspNowManager.h"

#include <WiFi.h>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// Active instance used by static C callbacks required by ESP-NOW API.
EspNowManager* EspNowManager::activeInstance_ = nullptr;

namespace {

constexpr std::size_t kTxPriorityCount = RadioTxScheduler::kPriorityCount;
constexpr UBaseType_t kTxQueueDepth[kTxPriorityCount] = {
    6U,  // Critical: heartbeat, COMMAND, CONTROL
    4U,  // Control: LOG/TERMINAL and ordinary management
    12U, // Data: TELEMETRY burst absorption
};
constexpr std::uint8_t kNoCompletionSlot = 0xFFU;
constexpr std::size_t kCompletionSlotCount = 4U;
constexpr std::uint32_t kAsyncCallbackTimeoutMs = 250U;

// Standard 802.11 MAC header: frame control at offset 0-1, then three 6-byte
// address fields for a management frame at offsets 4, 10, 16. Address2 (10)
// is the transmitter. Frame control byte 0 == 0xD0 is Version=0, Type=
// Management(00), Subtype=Action(1101) -- what every ESP-NOW frame is sent
// as, regardless of the flags in byte 1.
constexpr std::uint8_t kDot11FrameControlMgmtAction = 0xD0U;
constexpr std::size_t kDot11Addr2Offset = 10U;

struct TxRequest {
    std::uint8_t mac[6];
    std::uint16_t len;
    std::uint8_t data[EspNowManager::MAX_DATA_LEN];
    std::uint8_t completionSlot;
    std::uint32_t completionGeneration;
    std::uint32_t enqueuedMs;
    std::uint32_t timeoutMs;
};

struct DriverStatus {
    std::uint8_t mac[6];
    esp_now_send_status_t status;
};

struct CompletionSlot {
    SemaphoreHandle_t signal = nullptr;
    bool inUse = false;
    std::uint32_t generation = 0U;
    bool callbackReceived = false;
    bool delivered = false;
};

QueueHandle_t g_txQueues[kTxPriorityCount] = {nullptr, nullptr, nullptr};
QueueHandle_t g_driverStatusQueue = nullptr;
TaskHandle_t g_txWorkerTask = nullptr;
volatile bool g_txWorkerRunning = false;
CompletionSlot g_completionSlots[kCompletionSlotCount];
portMUX_TYPE g_completionMux = portMUX_INITIALIZER_UNLOCKED;
volatile std::uint32_t g_txEnqueued[kTxPriorityCount] = {0U, 0U, 0U};
volatile std::uint32_t g_txDroppedQueueFull[kTxPriorityCount] = {0U, 0U, 0U};
volatile std::uint32_t g_txDriverRejected = 0U;
volatile std::uint32_t g_txCallbackTimeouts = 0U;
volatile std::uint32_t g_txCallbacksReceived = 0U;

std::size_t priorityIndex(EspNowManager::TxPriority priority) noexcept {
    const std::size_t index = static_cast<std::size_t>(priority);
    return index < kTxPriorityCount ? index :
        static_cast<std::size_t>(EspNowManager::TxPriority::Control);
}

bool completionStillActive(const TxRequest& request) noexcept {
    if (request.completionSlot == kNoCompletionSlot ||
        request.completionSlot >= kCompletionSlotCount) {
        return true;
    }
    bool active = false;
    portENTER_CRITICAL(&g_completionMux);
    const CompletionSlot& slot = g_completionSlots[request.completionSlot];
    active = slot.inUse && slot.generation == request.completionGeneration;
    portEXIT_CRITICAL(&g_completionMux);
    return active;
}

void completeRequest(const TxRequest& request, bool callbackReceived, bool delivered) noexcept {
    if (request.completionSlot == kNoCompletionSlot ||
        request.completionSlot >= kCompletionSlotCount) {
        return;
    }

    SemaphoreHandle_t signal = nullptr;
    portENTER_CRITICAL(&g_completionMux);
    CompletionSlot& slot = g_completionSlots[request.completionSlot];
    if (slot.inUse && slot.generation == request.completionGeneration) {
        slot.callbackReceived = callbackReceived;
        slot.delivered = delivered;
        signal = slot.signal;
    }
    portEXIT_CRITICAL(&g_completionMux);

    if (signal != nullptr) {
        xSemaphoreGive(signal);
    }
}

bool dequeueScheduled(TxRequest& out, std::size_t& scheduleCursor) noexcept {
    bool available[kTxPriorityCount]{};
    for (std::size_t i = 0U; i < kTxPriorityCount; ++i) {
        available[i] = g_txQueues[i] != nullptr && uxQueueMessagesWaiting(g_txQueues[i]) > 0U;
    }

    const RadioTxScheduler::Selection selected =
        RadioTxScheduler::choose(available, scheduleCursor);
    scheduleCursor = selected.nextCursor;
    if (!selected.found) {
        return false;
    }
    return xQueueReceive(g_txQueues[priorityIndex(selected.priority)], &out, 0) == pdTRUE;
}

void txWorker(void*) {
    std::size_t scheduleCursor = 0U;
    while (g_txWorkerRunning) {
        TxRequest request{};
        if (!dequeueScheduled(request, scheduleCursor)) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        // A synchronous caller may have timed out while this request waited
        // behind earlier frames. Cancel before touching the driver.
        if (!completionStillActive(request)) {
            continue;
        }

        std::uint32_t callbackBudgetMs = kAsyncCallbackTimeoutMs;
        if (request.completionSlot != kNoCompletionSlot) {
            const std::uint32_t elapsed = millis() - request.enqueuedMs;
            if (elapsed >= request.timeoutMs) {
                completeRequest(request, false, false);
                continue;
            }
            callbackBudgetMs = request.timeoutMs - elapsed;
        }

        // There is exactly one in-flight driver send. Clearing stale status
        // before it starts makes a callback unambiguously belong to this MAC.
        xQueueReset(g_driverStatusQueue);
        if (esp_now_send(request.mac, request.data, request.len) != ESP_OK) {
            ++g_txDriverRejected;
            completeRequest(request, false, false);
            continue;
        }

        const std::uint32_t waitStartedMs = millis();
        bool callbackReceived = false;
        bool delivered = false;
        while ((millis() - waitStartedMs) < callbackBudgetMs) {
            const std::uint32_t elapsed = millis() - waitStartedMs;
            const std::uint32_t remainingMs = callbackBudgetMs - elapsed;
            DriverStatus status{};
            const TickType_t waitTicks = pdMS_TO_TICKS(remainingMs > 0U ? remainingMs : 1U);
            if (xQueueReceive(g_driverStatusQueue, &status, waitTicks) != pdTRUE) {
                break;
            }
            if (std::memcmp(status.mac, request.mac, sizeof(request.mac)) != 0) {
                continue; // defensive stale callback from a previous timeout
            }
            callbackReceived = true;
            delivered = status.status == ESP_NOW_SEND_SUCCESS;
            ++g_txCallbacksReceived;
            break;
        }
        if (!callbackReceived) {
            ++g_txCallbackTimeouts;
            completeRequest(request, false, false);

            // ESP-NOW does not carry an application token in its callback.
            // Starting another send now would let this late callback satisfy
            // a newer request to the same MAC. Keep only the TX worker
            // quarantined until the outstanding callback arrives; producers
            // and the main loop remain responsive and their bounded queues
            // expose backpressure instead of corrupting correlation.
            while (g_txWorkerRunning) {
                DriverStatus late{};
                if (xQueueReceive(g_driverStatusQueue, &late, pdMS_TO_TICKS(250U)) == pdTRUE &&
                    std::memcmp(late.mac, request.mac, sizeof(request.mac)) == 0) {
                    ++g_txCallbacksReceived;
                    break;
                }
            }
            continue;
        }
        completeRequest(request, callbackReceived, delivered);
    }
    vTaskDelete(nullptr);
}

void destroyTxSchedulerStorage() noexcept {
    for (std::size_t i = 0U; i < kTxPriorityCount; ++i) {
        if (g_txQueues[i] != nullptr) {
            vQueueDelete(g_txQueues[i]);
            g_txQueues[i] = nullptr;
        }
    }
    if (g_driverStatusQueue != nullptr) {
        vQueueDelete(g_driverStatusQueue);
        g_driverStatusQueue = nullptr;
    }
    for (CompletionSlot& slot : g_completionSlots) {
        if (slot.signal != nullptr) {
            vSemaphoreDelete(slot.signal);
        }
        slot = {};
    }
}

bool startTxScheduler() noexcept {
    destroyTxSchedulerStorage();
    for (std::size_t i = 0U; i < kTxPriorityCount; ++i) {
        g_txQueues[i] = xQueueCreate(kTxQueueDepth[i], sizeof(TxRequest));
        if (g_txQueues[i] == nullptr) {
            destroyTxSchedulerStorage();
            return false;
        }
        g_txEnqueued[i] = 0U;
        g_txDroppedQueueFull[i] = 0U;
    }
    g_driverStatusQueue = xQueueCreate(4U, sizeof(DriverStatus));
    if (g_driverStatusQueue == nullptr) {
        destroyTxSchedulerStorage();
        return false;
    }
    for (CompletionSlot& slot : g_completionSlots) {
        slot.signal = xSemaphoreCreateBinary();
        if (slot.signal == nullptr) {
            destroyTxSchedulerStorage();
            return false;
        }
    }

    g_txDriverRejected = 0U;
    g_txCallbackTimeouts = 0U;
    g_txCallbacksReceived = 0U;
    g_txWorkerRunning = true;
    if (xTaskCreate(txWorker, "espnow_tx", 4096U, nullptr, 3U, &g_txWorkerTask) != pdPASS) {
        g_txWorkerRunning = false;
        g_txWorkerTask = nullptr;
        destroyTxSchedulerStorage();
        return false;
    }
    return true;
}

void stopTxScheduler() noexcept {
    g_txWorkerRunning = false;
    if (g_txWorkerTask != nullptr) {
        vTaskDelete(g_txWorkerTask);
        g_txWorkerTask = nullptr;
    }
}

bool enqueueRequest(const TxRequest& request, EspNowManager::TxPriority priority) noexcept {
    const std::size_t index = priorityIndex(priority);
    if (!g_txWorkerRunning || g_txWorkerTask == nullptr || g_txQueues[index] == nullptr ||
        xQueueSend(g_txQueues[index], &request, 0) != pdTRUE) {
        ++g_txDroppedQueueFull[index];
        return false;
    }
    ++g_txEnqueued[index];
    xTaskNotifyGive(g_txWorkerTask);
    return true;
}

int acquireCompletionSlot(std::uint32_t& outGeneration) noexcept {
    int selected = -1;
    portENTER_CRITICAL(&g_completionMux);
    for (std::size_t i = 0U; i < kCompletionSlotCount; ++i) {
        CompletionSlot& slot = g_completionSlots[i];
        if (!slot.inUse && slot.signal != nullptr) {
            slot.inUse = true;
            ++slot.generation;
            if (slot.generation == 0U) ++slot.generation;
            slot.callbackReceived = false;
            slot.delivered = false;
            outGeneration = slot.generation;
            selected = static_cast<int>(i);
            break;
        }
    }
    portEXIT_CRITICAL(&g_completionMux);

    if (selected >= 0) {
        while (xSemaphoreTake(g_completionSlots[selected].signal, 0) == pdTRUE) {}
    }
    return selected;
}

void releaseCompletionSlot(std::size_t index, std::uint32_t generation) noexcept {
    if (index >= kCompletionSlotCount) return;
    portENTER_CRITICAL(&g_completionMux);
    CompletionSlot& slot = g_completionSlots[index];
    if (slot.inUse && slot.generation == generation) {
        slot.inUse = false;
        slot.callbackReceived = false;
        slot.delivered = false;
    }
    portEXIT_CRITICAL(&g_completionMux);
}

} // namespace

// Build empty manager state; runtime init happens in begin().
EspNowManager::EspNowManager()
    : deviceCount_(0),
      initialized_(false),
      channel_(0),
      encrypt_(false),
      receiveCallback_(nullptr),
      sendCallback_(nullptr) {
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

    // See handlePromiscuousRxStatic: this core's ESP-NOW recv callback has no
    // RSSI, so a promiscuous sniffer runs alongside it to recover one.
    // Management-frame-only filter, since ESP-NOW's Action frames are all
    // this dongle ever needs to see on its own fixed channel.
    wifi_promiscuous_filter_t promiscFilter{};
    promiscFilter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&promiscFilter);
    esp_wifi_set_promiscuous_rx_cb(handlePromiscuousRxStatic);
    esp_wifi_set_promiscuous(true);

    if (!startTxScheduler()) {
        esp_now_deinit();
        initialized_ = false;
        activeInstance_ = nullptr;
        return false;
    }

    for (size_t i = 0; i < deviceCount_; ++i) {
        if (!addPeerToEspNow(devices_[i].mac)) {
            stopTxScheduler();
            esp_now_deinit();
            destroyTxSchedulerStorage();
            initialized_ = false;
            activeInstance_ = nullptr;
            return false;
        }
    }

    return true;
}

// Deinitialize ESP-NOW and detach this instance from static callback dispatch.
void EspNowManager::end() {
    stopTxScheduler();
    if (initialized_) {
        esp_wifi_set_promiscuous(false);
        esp_now_deinit();
    }
    destroyTxSchedulerStorage();

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

// Send one datagram to every registered device.
bool EspNowManager::sendToAll(const uint8_t* data, size_t len) const {
    if (!initialized_ || deviceCount_ == 0) {
        return false;
    }

    bool sentAtLeastOne = false;
    for (size_t i = 0; i < deviceCount_; ++i) {
        if (sendToDevice(i, data, len)) {
            sentAtLeastOne = true;
        }
    }

    return sentAtLeastOne;
}

// Send one datagram by device index.
bool EspNowManager::sendToDevice(size_t index, const uint8_t* data, size_t len) const {
    if (index >= deviceCount_) {
        return false;
    }

    return sendToMac(devices_[index].mac, data, len);
}

// Enqueue one copied datagram. Only txWorker() calls esp_now_send(), so driver
// callbacks can never be consumed by another producer's wait slot.
bool EspNowManager::sendToMac(const uint8_t mac[6], const uint8_t* data, size_t len,
                              TxPriority priority) const {
    if (!initialized_ || mac == nullptr || data == nullptr || len == 0 || len > MAX_DATA_LEN) {
        return false;
    }

    TxRequest request{};
    std::memcpy(request.mac, mac, sizeof(request.mac));
    request.len = static_cast<std::uint16_t>(len);
    std::memcpy(request.data, data, len);
    request.completionSlot = kNoCompletionSlot;
    request.enqueuedMs = millis();
    request.timeoutMs = kAsyncCallbackTimeoutMs;
    return enqueueRequest(request, priority);
}

// Send to one index and wait for callback delivery status.
bool EspNowManager::sendToDeviceWithStatus(size_t index, const uint8_t* data, size_t len, bool& outDelivered, uint32_t timeoutMs) const {
    outDelivered = false;
    if (index >= deviceCount_) {
        return false;
    }

    return sendToMacWithStatus(devices_[index].mac, data, len, outDelivered, timeoutMs);
}

// Send to one MAC and wait for callback delivery status.
bool EspNowManager::sendToMacWithStatus(const uint8_t mac[6], const uint8_t* data, size_t len,
                                        bool& outDelivered, uint32_t timeoutMs,
                                        TxPriority priority) const {
    outDelivered = false;
    if (!initialized_ || mac == nullptr || data == nullptr || len == 0 ||
        len > MAX_DATA_LEN || timeoutMs == 0U) {
        return false;
    }

    std::uint32_t generation = 0U;
    const int slotIndex = acquireCompletionSlot(generation);
    if (slotIndex < 0) {
        return false;
    }

    TxRequest request{};
    std::memcpy(request.mac, mac, sizeof(request.mac));
    request.len = static_cast<std::uint16_t>(len);
    std::memcpy(request.data, data, len);
    request.completionSlot = static_cast<std::uint8_t>(slotIndex);
    request.completionGeneration = generation;
    request.enqueuedMs = millis();
    request.timeoutMs = timeoutMs;
    if (!enqueueRequest(request, priority)) {
        releaseCompletionSlot(static_cast<std::size_t>(slotIndex), generation);
        return false;
    }

    const TickType_t waitTicks = pdMS_TO_TICKS(timeoutMs) > 0U ? pdMS_TO_TICKS(timeoutMs) : 1U;
    if (xSemaphoreTake(g_completionSlots[slotIndex].signal, waitTicks) != pdTRUE) {
        releaseCompletionSlot(static_cast<std::size_t>(slotIndex), generation);
        return false;
    }

    bool callbackReceived = false;
    portENTER_CRITICAL(&g_completionMux);
    const CompletionSlot& slot = g_completionSlots[slotIndex];
    if (slot.inUse && slot.generation == generation) {
        callbackReceived = slot.callbackReceived;
        outDelivered = slot.delivered;
    }
    portEXIT_CRITICAL(&g_completionMux);
    releaseCompletionSlot(static_cast<std::size_t>(slotIndex), generation);
    return callbackReceived;
}

void EspNowManager::peekTxSchedulerCounters(TxSchedulerCounters& out) const {
    for (std::size_t i = 0U; i < kTxPriorityCount; ++i) {
        out.enqueued[i] = g_txEnqueued[i];
        out.droppedQueueFull[i] = g_txDroppedQueueFull[i];
    }
    out.driverRejected = g_txDriverRejected;
    out.callbackTimeouts = g_txCallbackTimeouts;
    out.callbacksReceived = g_txCallbacksReceived;
}

// Send to all peers and aggregate delivery status.
bool EspNowManager::sendToAllWithStatus(
    const uint8_t* data,
    size_t len,
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
        const bool gotStatus = sendToDeviceWithStatus(i, data, len, delivered, timeoutMs);
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

// Forward the raw ESP-NOW datagram to the instance callback unmodified; this
// class has no protocol knowledge, so bounds are the only thing checked here.
void EspNowManager::handleReceiveStatic(const uint8_t* mac, const uint8_t* incomingData, int len) {
    if (activeInstance_ == nullptr || activeInstance_->receiveCallback_ == nullptr ||
        incomingData == nullptr || len <= 0 || len > static_cast<int>(MAX_DATA_LEN)) {
        return;
    }

    const int index = activeInstance_->findDeviceIndexByMac(mac);
    const int8_t rssi = (index >= 0) ? activeInstance_->devices_[index].lastRssi : int8_t(-128);
    activeInstance_->receiveCallback_(mac, incomingData, static_cast<size_t>(len), rssi);
}

// See handlePromiscuousRxStatic's doc comment (EspNowManager.h): this stashes
// the RSSI of the last Action frame seen from each known peer, so
// handleReceiveStatic can read it back for that same over-the-air frame.
void EspNowManager::handlePromiscuousRxStatic(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (activeInstance_ == nullptr || type != WIFI_PKT_MGMT || buf == nullptr) {
        return;
    }

    const auto* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
    if (pkt->rx_ctrl.sig_len < kDot11Addr2Offset + 6U ||
        pkt->payload[0] != kDot11FrameControlMgmtAction) {
        return;
    }

    const uint8_t* sourceMac = pkt->payload + kDot11Addr2Offset;
    const int index = activeInstance_->findDeviceIndexByMac(sourceMac);
    if (index >= 0) {
        activeInstance_->devices_[index].lastRssi = static_cast<int8_t>(pkt->rx_ctrl.rssi);
    }
}

// Dispatch low-level send result to user callback.
void EspNowManager::handleSendStatic(const uint8_t* mac, esp_now_send_status_t status) {
    if (activeInstance_ == nullptr) {
        return;
    }

    if (g_driverStatusQueue != nullptr && mac != nullptr) {
        DriverStatus driverStatus{};
        std::memcpy(driverStatus.mac, mac, sizeof(driverStatus.mac));
        driverStatus.status = status;
        xQueueSend(g_driverStatusQueue, &driverStatus, 0);
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
