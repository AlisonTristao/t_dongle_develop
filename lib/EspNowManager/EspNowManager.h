#pragma once

#include <Arduino.h>
#include <esp_now.h>

/**
 * @brief EspNowManager wraps peer registration and messaging over ESP-NOW.
 *
 * Responsibilities:
 * - initialize/deinitialize ESP-NOW stack
 * - maintain a local device registry with metadata
 * - provide send helpers (broadcast over registered peers or direct target)
 * - expose callbacks for receive and send status events
 */
class EspNowManager final {
public:
    /** Maximum number of peers tracked by the manager. */
    static constexpr size_t MAX_DEVICES = 16;
    /** Maximum length for device display name, including terminator. */
    static constexpr size_t DEVICE_NAME_SIZE = 32;
    /** Maximum length for device description, including terminator. */
    static constexpr size_t DEVICE_DESCRIPTION_SIZE = 96;
    /** Payload text size compatible with message struct. */
    static constexpr size_t MESSAGE_TEXT_SIZE = 231;

    /**
     * @brief Message category used in ESP-NOW payloads.
     */
    enum class logType : uint8_t {
        NONE,
        INFO,
        TELEMETRY,
        ERROR,
        DEBUG
    };

    /**
     * @brief Wire payload structure shared between sender and receiver.
     */
    struct message {
        uint32_t timer;
        char msg[MESSAGE_TEXT_SIZE];
        logType type;
    };

    /**
     * @brief Local device registry record.
     */
    struct deviceInfo {
        uint8_t mac[6];
        char name[DEVICE_NAME_SIZE];
        char description[DEVICE_DESCRIPTION_SIZE];
    };

    /** Callback for incoming message payloads. */
    using ReceiveCallback = void (*)(const uint8_t* mac, const message& incoming);
    /** Callback for delivery result notifications. */
    using SendCallback = void (*)(const uint8_t* mac, esp_now_send_status_t status);

    /**
     * @brief Creates an empty manager instance.
     */
    EspNowManager();

    /**
     * @brief Initializes Wi-Fi station mode and ESP-NOW callbacks.
     * @param channel ESP-NOW channel used for peer registration.
     * @param encrypt Enables encrypted peer registration when true.
     * @return true when initialization succeeds.
     */
    bool begin(uint8_t channel = 0, bool encrypt = false);

    /**
     * @brief Deinitializes ESP-NOW and releases active instance binding.
     */
    void end();

    /**
     * @brief Registers one device in local list and in ESP-NOW if initialized.
     * @param mac Target MAC address.
     * @param name Friendly name.
     * @param description Device description text.
     * @return true when device is added.
     */
    bool addDevice(const uint8_t mac[6], const char* name, const char* description);

    /**
     * @brief Registers one device from a prefilled structure.
     */
    bool addDevice(const deviceInfo& device);

    /**
     * @brief Removes device by list index.
     */
    bool removeDeviceByIndex(size_t index);

    /**
     * @brief Removes device by MAC address.
     */
    bool removeDeviceByMac(const uint8_t mac[6]);

    /**
     * @brief Updates name and description for one device by index.
     */
    bool updateDeviceByIndex(size_t index, const char* name, const char* description);

    /**
     * @brief Updates name and description for one device by MAC.
     */
    bool updateDeviceByMac(const uint8_t mac[6], const char* name, const char* description);

    /**
     * @brief Removes all devices from local registry and ESP-NOW peers.
     */
    void clearDevices();

    /**
     * @brief Returns number of registered devices.
     */
    size_t deviceCount() const;

    /**
     * @brief Reads one device from the registry.
     */
    bool deviceAt(size_t index, deviceInfo& outDevice) const;

    /**
     * @brief Returns internal list pointer for read-only iteration.
     */
    const deviceInfo* deviceList() const;

    /**
     * @brief Copies up to maxItems devices to an output array.
     * @return Number of copied items.
     */
    size_t copyDeviceList(deviceInfo* outList, size_t maxItems) const;

    /**
     * @brief Returns index for MAC address, or -1 when not found.
     */
    int deviceIndexByMac(const uint8_t mac[6]) const;

    /**
     * @brief Sends one message to all registered devices.
     */
    bool sendToAll(const message& outgoing) const;

    /**
     * @brief Sends one message to a device identified by index.
     */
    bool sendToDevice(size_t index, const message& outgoing) const;

    /**
     * @brief Sends one message directly to a MAC address.
     */
    bool sendToMac(const uint8_t mac[6], const message& outgoing) const;

    /**
     * @brief Sends message and waits for callback status for target index.
     * @param index Device index in registry.
     * @param outgoing Payload to send.
     * @param outDelivered true when callback reports success.
     * @param timeoutMs Max wait time for callback.
     * @return true when send callback was received.
     */
    bool sendToDeviceWithStatus(size_t index, const message& outgoing, bool& outDelivered, uint32_t timeoutMs = 600) const;

    /**
     * @brief Sends message and waits for callback status for target MAC.
     * @param mac Target MAC.
     * @param outgoing Payload to send.
     * @param outDelivered true when callback reports success.
     * @param timeoutMs Max wait time for callback.
     * @return true when send callback was received.
     */
    bool sendToMacWithStatus(const uint8_t mac[6], const message& outgoing, bool& outDelivered, uint32_t timeoutMs = 600) const;

    /**
     * @brief Sends to all peers and aggregates callback delivery status.
     * @param outgoing Payload to send.
     * @param outDeliveredCount Number of peers confirmed as delivered.
     * @param outTriedCount Number of peers attempted.
     * @param timeoutMs Per-peer callback timeout.
     * @return true when at least one peer was attempted.
     */
    bool sendToAllWithStatus(
        const message& outgoing,
        size_t& outDeliveredCount,
        size_t& outTriedCount,
        uint32_t timeoutMs = 600
    ) const;

    /**
     * @brief Sets callback used for message receive events.
     */
    void setReceiveCallback(ReceiveCallback callback);

    /**
     * @brief Sets callback used for send status events.
     */
    void setSendCallback(SendCallback callback);

private:
    /** Singleton-like active instance used by static ESP-NOW callbacks. */
    static EspNowManager* activeInstance_;

    deviceInfo devices_[MAX_DEVICES];
    size_t deviceCount_;
    bool initialized_;
    uint8_t channel_;
    bool encrypt_;
    ReceiveCallback receiveCallback_;
    SendCallback sendCallback_;

    mutable volatile bool sendWaitPending_;
    mutable volatile bool sendWaitCompleted_;
    mutable esp_now_send_status_t sendWaitStatus_;
    mutable uint8_t sendWaitMac_[6];
    mutable bool sendWaitHasMac_;

    /** Static adapter for ESP-NOW receive callback. */
    static void handleReceiveStatic(const uint8_t* mac, const uint8_t* incomingData, int len);

    /** Static adapter for ESP-NOW send callback. */
    static void handleSendStatic(const uint8_t* mac, esp_now_send_status_t status);

    /** Adds one peer to ESP-NOW runtime table. */
    bool addPeerToEspNow(const uint8_t mac[6]) const;

    /** Removes one peer from ESP-NOW runtime table. */
    bool removePeerFromEspNow(const uint8_t mac[6]) const;

    /** Returns list index for given MAC, or -1 when not found. */
    int findDeviceIndexByMac(const uint8_t mac[6]) const;

    /** Safe C-string copy helper with guaranteed terminator. */
    static void copyText(char* dst, size_t dstSize, const char* src);
};
