#include "espnow_config.h"
#include "LcdTerminal.h"
#include "shell_output.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace {

// Shared output stream used by ESP-NOW callbacks.
Stream* g_io = nullptr;
EspNowManager* g_manager = nullptr;
DatabaseStore* g_database = nullptr;
LcdTerminal* g_lcdTerminal = nullptr;
QueueHandle_t g_rxQueue = nullptr;
QueueHandle_t g_rxDisplayQueue = nullptr;
QueueHandle_t g_rxDbLogQueue = nullptr;
volatile uint32_t g_droppedRxCount = 0;
volatile uint32_t g_droppedRxDbLogCount = 0;
bool g_asyncRxEnabled = false;
bool g_asyncDbLogEnabled = false;

struct RxDisplayLine {
    char header[96];
    char payload[EspNowManager::MESSAGE_TEXT_SIZE + 1];
    size_t payloadLen;
    uint16_t color;
};

const char* logTypeToText(EspNowManager::logType type) {
    switch (type) {
    case EspNowManager::logType::INFO:
        return "INFO";
    case EspNowManager::logType::TELEMETRY:
        return "TELEMETRY";
    case EspNowManager::logType::ERROR:
        return "ERROR";
    case EspNowManager::logType::DEBUG:
        return "DEBUG";
    case EspNowManager::logType::NONE:
    default:
        return "NONE";
    }
}

uint16_t logTypeToLcdColor(EspNowManager::logType type) {
    switch (type) {
    case EspNowManager::logType::ERROR:
        return ST77XX_RED;
    case EspNowManager::logType::TELEMETRY:
        return ST77XX_GREEN;
    case EspNowManager::logType::DEBUG:
        return ST77XX_YELLOW;
    case EspNowManager::logType::INFO:
    case EspNowManager::logType::NONE:
    default:
        return ST77XX_BLACK;
    }
}

String arrivalTimeText() {
    const time_t nowEpoch = time(nullptr);
    if (nowEpoch > 0) {
        std::tm localTime = {};
        if (localtime_r(&nowEpoch, &localTime) != nullptr) {
            char out[24] = {0};
            std::strftime(out, sizeof(out), "%H:%M:%S", &localTime);
            return String(out);
        }
    }

    char fallback[20] = {0};
    std::snprintf(fallback, sizeof(fallback), "ms%lu", static_cast<unsigned long>(millis()));
    return String(fallback);
}

size_t copyIncomingPayload(const EspNowManager::message& incomingData, char* outPayload, size_t outSize) {
    if (outPayload == nullptr || outSize == 0) {
        return 0;
    }

    size_t length = 0;
    while (length < EspNowManager::MESSAGE_TEXT_SIZE && incomingData.msg[length] != '\0') {
        ++length;
    }

    const size_t copyLen = (length < (outSize - 1)) ? length : (outSize - 1);
    if (copyLen > 0) {
        std::memcpy(outPayload, incomingData.msg, copyLen);
    }
    outPayload[copyLen] = '\0';
    return copyLen;
}

void writeRawToStream(Stream* io, const char* data, size_t len) {
    if (io == nullptr || data == nullptr || len == 0) {
        return;
    }

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data);
    size_t remaining = len;
    uint32_t retries = 0;

    while (remaining > 0) {
        const size_t sent = io->write(ptr, remaining);
        if (sent == 0) {
            ++retries;
            if (retries > 8) {
                break;
            }
            delay(1);
            continue;
        }

        ptr += sent;
        remaining -= sent;
    }
}

void ensurePayloadTerminator(Stream* io, const char* payload, size_t payloadLen) {
    if (io == nullptr) {
        return;
    }

    if (payloadLen == 0) {
        io->write('\r');
        io->write('\n');
        return;
    }

    const char last = payload[payloadLen - 1];
    if (last != '\n' && last != '\r') {
        io->write('\r');
        io->write('\n');
    }
}

bool enqueueDisplayLine(const char* header, const char* payload, size_t payloadLen, uint16_t color) {
    if (header == nullptr || payload == nullptr) {
        return false;
    }

    if (g_rxDisplayQueue == nullptr) {
        return false;
    }

    RxDisplayLine item = {};
    std::snprintf(item.header, sizeof(item.header), "%s", header);
    std::snprintf(item.payload, sizeof(item.payload), "%s", payload);
    item.payloadLen = payloadLen;
    item.color = color;

    return xQueueSend(g_rxDisplayQueue, &item, 0) == pdTRUE;
}

void processRxMessageInternal(const uint8_t mac[6], const EspNowManager::message& incomingData) {
    const String arrivedAt = arrivalTimeText();
    const char* typeText = logTypeToText(incomingData.type);
    char payload[EspNowManager::MESSAGE_TEXT_SIZE + 1] = {0};
    const size_t payloadLen = copyIncomingPayload(incomingData, payload, sizeof(payload));

    char header[96] = {0};
    std::snprintf(
        header,
        sizeof(header),
        "[espnow][rx] t=%s tipo=%s",
        arrivedAt.c_str(),
        typeText
    );

    const uint16_t color = logTypeToLcdColor(incomingData.type);
    if (!enqueueDisplayLine(header, payload, payloadLen, color)) {
        // Fallback path when queue is unavailable/full.
        if (g_io != nullptr) {
            ShellOutput::writeLine(*g_io, header);
            writeRawToStream(g_io, payload, payloadLen);
            ensurePayloadTerminator(g_io, payload, payloadLen);
        }

#if !HIGH_FREQUENCY_INCOMMING_ESPNOW
        if (g_lcdTerminal != nullptr && g_lcdTerminal->isReady()) {
            g_lcdTerminal->writeText(String(header), color);
            if (payloadLen > 0) {
                g_lcdTerminal->writeText(String(payload), color);
            }
        }
#endif
    }

    if (mac != nullptr && g_manager != nullptr && g_manager->deviceIndexByMac(mac) < 0) {
        char autoName[24] = {0};
        std::snprintf(autoName, sizeof(autoName), "peer-%02X%02X", mac[4], mac[5]);
        g_manager->addDevice(mac, autoName, "adicionado automaticamente por RX ESP-NOW");
    }

    if (g_database != nullptr && g_database->isReady() && mac != nullptr) {
        if (g_asyncDbLogEnabled && g_rxDbLogQueue != nullptr) {
            EspNowConfig::RxDbLogEvent dbLogEvent = {};
            std::memcpy(dbLogEvent.mac, mac, sizeof(dbLogEvent.mac));
            dbLogEvent.incoming = incomingData;
            if (xQueueSend(g_rxDbLogQueue, &dbLogEvent, 0) != pdTRUE) {
                ++g_droppedRxDbLogCount;
            }
        } else if (!g_database->logIncomingEspNow(mac, incomingData)) {
            ++g_droppedRxDbLogCount;
        }
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

void attachCallbacks(EspNowManager& manager, Stream& io, DatabaseStore* database, LcdTerminal* lcdTerminal) {
    // Bind callbacks once and keep stream pointer for runtime logging.
    g_io = &io;
    g_manager = &manager;
    g_database = database;
    g_lcdTerminal = lcdTerminal;
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

    if (g_rxDisplayQueue == nullptr) {
        g_rxDisplayQueue = xQueueCreate(static_cast<UBaseType_t>(queueDepth), sizeof(RxDisplayLine));
        if (g_rxDisplayQueue == nullptr) {
            g_asyncRxEnabled = false;
            return false;
        }
    }

    if (g_rxDbLogQueue == nullptr) {
        g_rxDbLogQueue = xQueueCreate(static_cast<UBaseType_t>(RX_DB_LOG_QUEUE_DEPTH), sizeof(RxDbLogEvent));
    }

    xQueueReset(g_rxQueue);
    xQueueReset(g_rxDisplayQueue);
    if (g_rxDbLogQueue != nullptr) {
        xQueueReset(g_rxDbLogQueue);
    }
    g_droppedRxCount = 0;
    g_droppedRxDbLogCount = 0;
    g_asyncDbLogEnabled = false;
    g_asyncRxEnabled = true;
    return true;
}

void disableAsyncRx() {
    g_asyncRxEnabled = false;
    g_asyncDbLogEnabled = false;
    if (g_rxQueue != nullptr) {
        xQueueReset(g_rxQueue);
    }
    if (g_rxDisplayQueue != nullptr) {
        xQueueReset(g_rxDisplayQueue);
    }
    if (g_rxDbLogQueue != nullptr) {
        xQueueReset(g_rxDbLogQueue);
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

bool dequeueRxDbLog(RxDbLogEvent& outEvent, uint32_t timeoutMs) {
    if (g_rxDbLogQueue == nullptr) {
        return false;
    }

    const TickType_t waitTicks = (timeoutMs == 0)
        ? static_cast<TickType_t>(0)
        : pdMS_TO_TICKS(timeoutMs);

    return xQueueReceive(g_rxDbLogQueue, &outEvent, waitTicks) == pdTRUE;
}

void processRxDbLog(const RxDbLogEvent& event) {
    if (g_database != nullptr && g_database->isReady()) {
        if (!g_database->logIncomingEspNow(event.mac, event.incoming)) {
            ++g_droppedRxDbLogCount;
        }
    }
}

void setAsyncDbLogEnabled(bool enabled) {
    g_asyncDbLogEnabled = enabled && (g_rxDbLogQueue != nullptr);
}

size_t flushRxDbLogBuffer(size_t maxItems) {
    if (g_rxDbLogQueue == nullptr || g_database == nullptr || !g_database->isReady()) {
        return 0;
    }

    const size_t limit = (maxItems == 0) ? static_cast<size_t>(-1) : maxItems;
    const bool wantsBatch = (maxItems == 0) || (maxItems > 1);
    const bool hasTransaction = wantsBatch && g_database->beginTransaction();
    size_t flushed = 0;
    RxDbLogEvent event = {};

    while (flushed < limit && dequeueRxDbLog(event, 0)) {
        processRxDbLog(event);
        ++flushed;
    }

    if (hasTransaction) {
        if (!g_database->commitTransaction()) {
            (void)g_database->rollbackTransaction();
            g_droppedRxDbLogCount += static_cast<uint32_t>(flushed);
        }
    }

    return flushed;
}

size_t pendingRxDbLogCount() {
    if (g_rxDbLogQueue == nullptr) {
        return 0;
    }

    return static_cast<size_t>(uxQueueMessagesWaiting(g_rxDbLogQueue));
}

size_t rxDbLogCapacity() {
    if (g_rxDbLogQueue == nullptr) {
        return 0;
    }

    const size_t pending = static_cast<size_t>(uxQueueMessagesWaiting(g_rxDbLogQueue));
    const size_t available = static_cast<size_t>(uxQueueSpacesAvailable(g_rxDbLogQueue));
    return pending + available;
}

size_t flushRxDisplayLines(size_t maxLines) {
    if (g_rxDisplayQueue == nullptr || maxLines == 0) {
        return 0;
    }

    RxDisplayLine item = {};
    size_t drained = 0;
    while (drained < maxLines && xQueueReceive(g_rxDisplayQueue, &item, 0) == pdTRUE) {
        if (g_io != nullptr) {
            ShellOutput::writeLine(*g_io, item.header);
            writeRawToStream(g_io, item.payload, item.payloadLen);
            ensurePayloadTerminator(g_io, item.payload, item.payloadLen);
        }

#if !HIGH_FREQUENCY_INCOMMING_ESPNOW
    if (g_lcdTerminal != nullptr && g_lcdTerminal->isReady()) {
            g_lcdTerminal->writeText(String(item.header), item.color);
            if (item.payloadLen > 0) {
                g_lcdTerminal->writeText(String(item.payload), item.color);
            }
        }
#endif

        ++drained;
    }

    return drained;
}

uint32_t takeDroppedRxCount() {
    const uint32_t dropped = g_droppedRxCount;
    g_droppedRxCount = 0;
    return dropped;
}

uint32_t takeDroppedRxDbLogCount() {
    const uint32_t dropped = g_droppedRxDbLogCount;
    g_droppedRxDbLogCount = 0;
    return dropped;
}

} // namespace EspNowConfig
