#include "EspNowConfig.h"
#include "LcdTerminal.h"
#include "ShellOutput.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <Esp.h>

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
volatile uint32_t g_overwrittenRxDisplayCount = 0;
volatile uint32_t g_droppedRxDbLogCount = 0;
bool g_asyncRxEnabled = false;
bool g_asyncDbLogEnabled = false;

struct RxDisplayLine {
    char header[96];
    char payload[EspNowManager::MESSAGE_TEXT_SIZE + 1];
    char closeSuffix[24];
    size_t payloadLen;
    uint16_t color;
    bool appendToPrevious;
    bool keepLineOpen;
};

bool g_rxLineOpen = false;
size_t g_rxContinuationPadding = 1;
bool g_rxAssemblyActive = false;
bool g_rxAssemblyHasMac = false;
uint8_t g_rxAssemblyMac[6] = {0};
uint8_t g_rxExpectedPacketIndex = 0;

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
    const unsigned long ms = static_cast<unsigned long>(millis() % 1000);
    if (nowEpoch > 0) {
        std::tm localTime = {};
        if (localtime_r(&nowEpoch, &localTime) != nullptr) {
            char out[32] = {0};
            std::strftime(out, sizeof(out), "%H:%M:%S", &localTime);
            char result[40] = {0};
            std::snprintf(result, sizeof(result), "%s.%03lu", out, ms);
            return String(result);
        }
    }

    char fallback[32] = {0};
    std::snprintf(fallback, sizeof(fallback), "ms%lu.%03lu", static_cast<unsigned long>(millis() / 1000), ms);
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

void writeRawToStream(Stream* io, const char* data, size_t len);

void resetRxAssemblyState() {
    g_rxAssemblyActive = false;
    g_rxAssemblyHasMac = false;
    g_rxExpectedPacketIndex = 0;
}

bool sameMacAddress(const uint8_t* lhs, const uint8_t* rhs) {
    return lhs != nullptr && rhs != nullptr && std::memcmp(lhs, rhs, 6) == 0;
}

void closeOpenRxLine(Stream* io) {
    if (io == nullptr || !g_rxLineOpen) {
        return;
    }

    io->write('\r');
    io->write('\n');
    g_rxLineOpen = false;
}

void writePaddingSpaces(Stream* io, size_t count) {
    if (io == nullptr || count == 0) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        io->write(' ');
    }
}

void writePayloadWithContinuation(Stream* io, const char* payload, size_t payloadLen, size_t continuationPadding) {
    if (io == nullptr || payload == nullptr || payloadLen == 0) {
        return;
    }

    size_t start = 0;
    for (size_t i = 0; i < payloadLen; ++i) {
        const char ch = payload[i];
        if (ch != '\r' && ch != '\n') {
            continue;
        }

        if (i > start) {
            writeRawToStream(io, payload + start, i - start);
        }

        if (ch == '\r' && (i + 1) < payloadLen && payload[i + 1] == '\n') {
            ++i;
        }

        io->write('\r');
        io->write('\n');
        if ((i + 1) < payloadLen) {
            writePaddingSpaces(io, continuationPadding);
        }

        start = i + 1;
    }

    if (start < payloadLen) {
        writeRawToStream(io, payload + start, payloadLen - start);
    }
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

void writeCloseSuffix(Stream* io, const char* closeSuffix) {
    if (io == nullptr || closeSuffix == nullptr || closeSuffix[0] == '\0') {
        return;
    }

    io->write(' ');
    writeRawToStream(io, closeSuffix, std::strlen(closeSuffix));
}

void writeRxLineToStream(
    Stream* io,
    const char* header,
    const char* payload,
    size_t payloadLen,
    bool keepLineOpen,
    const char* closeSuffix
) {
    if (io == nullptr || header == nullptr) {
        return;
    }

    closeOpenRxLine(io);

    const size_t headerLen = std::strlen(header);
    const size_t continuationPadding = headerLen + 1;
    g_rxContinuationPadding = continuationPadding;

    // Ensure the log starts from the beginning of the terminal line.
    io->write('\r');
    writeRawToStream(io, header, headerLen);
    if (payload != nullptr && payloadLen > 0) {
        io->write(' ');
        writePayloadWithContinuation(io, payload, payloadLen, continuationPadding);
    }

    if (keepLineOpen) {
        g_rxLineOpen = true;
        return;
    }

    writeCloseSuffix(io, closeSuffix);
    io->write('\r');
    io->write('\n');
    g_rxLineOpen = false;
}

void writeRxContinuationToStream(
    Stream* io,
    const char* payload,
    size_t payloadLen,
    bool keepLineOpen,
    const char* closeSuffix
) {
    if (io == nullptr || payload == nullptr || payloadLen == 0) {
        return;
    }

    if (!g_rxLineOpen) {
        io->write('\r');
    }

    writePayloadWithContinuation(io, payload, payloadLen, g_rxContinuationPadding);

    if (keepLineOpen) {
        g_rxLineOpen = true;
        return;
    }

    writeCloseSuffix(io, closeSuffix);
    io->write('\r');
    io->write('\n');
    g_rxLineOpen = false;
}

bool enqueueDisplayLine(
    const char* header,
    const char* payload,
    size_t payloadLen,
    uint16_t color,
    bool appendToPrevious,
    bool keepLineOpen,
    const char* closeSuffix
) {
    if (header == nullptr || payload == nullptr) {
        return false;
    }

    if (g_rxDisplayQueue == nullptr) {
        return false;
    }

    RxDisplayLine item = {};
    std::snprintf(item.header, sizeof(item.header), "%s", header);
    std::snprintf(item.payload, sizeof(item.payload), "%s", payload);
    std::snprintf(item.closeSuffix, sizeof(item.closeSuffix), "%s", (closeSuffix != nullptr) ? closeSuffix : "");
    item.payloadLen = payloadLen;
    item.color = color;
    item.appendToPrevious = appendToPrevious;
    item.keepLineOpen = keepLineOpen;

    if (xQueueSend(g_rxDisplayQueue, &item, 0) == pdTRUE) {
        return true;
    }

    // Cyclic queue behavior: when full, drop oldest and keep newest.
    RxDisplayLine oldest = {};
    if (xQueueReceive(g_rxDisplayQueue, &oldest, 0) != pdTRUE) {
        return false;
    }

    if (xQueueSend(g_rxDisplayQueue, &item, 0) == pdTRUE) {
        ++g_overwrittenRxDisplayCount;
        return true;
    }

    return false;
}

bool createDbLogQueue() {
    if (g_rxDbLogQueue != nullptr) {
        return true;
    }

    const size_t minDepth = (RX_DB_LOG_QUEUE_DEPTH > 0U)
        ? static_cast<size_t>(RX_DB_LOG_QUEUE_DEPTH)
        : static_cast<size_t>(1U);

#if RX_DB_LOG_DYNAMIC_ALLOC
    const size_t entrySize = sizeof(EspNowConfig::RxDbLogEvent);
    const size_t heapReserve = static_cast<size_t>(RX_DB_LOG_HEAP_RESERVE_BYTES);
    const size_t freeHeap = static_cast<size_t>(ESP.getFreeHeap());

    size_t targetDepth = minDepth;
    if (freeHeap > heapReserve + entrySize) {
        targetDepth = (freeHeap - heapReserve) / entrySize;
        if (targetDepth < minDepth) {
            targetDepth = minDepth;
        }
    }

    size_t candidateDepth = targetDepth;
    while (candidateDepth >= minDepth) {
        g_rxDbLogQueue = xQueueCreate(static_cast<UBaseType_t>(candidateDepth), sizeof(EspNowConfig::RxDbLogEvent));
        if (g_rxDbLogQueue != nullptr) {
            return true;
        }

        if (candidateDepth == minDepth) {
            break;
        }

        const size_t reducedDepth = (candidateDepth * 9U) / 10U;
        candidateDepth = (reducedDepth < minDepth) ? minDepth : reducedDepth;
    }

    return g_rxDbLogQueue != nullptr;
#else
    g_rxDbLogQueue = xQueueCreate(static_cast<UBaseType_t>(minDepth), sizeof(EspNowConfig::RxDbLogEvent));
    return g_rxDbLogQueue != nullptr;
#endif
}

void processRxMessageInternal(const uint8_t mac[6], const EspNowManager::message& incomingData) {
    char payload[EspNowManager::MESSAGE_TEXT_SIZE + 1] = {0};
    const size_t copiedPayloadLen = copyIncomingPayload(incomingData, payload, sizeof(payload));
    const size_t payloadLen = copiedPayloadLen;

    const bool isPackageContinuation = (incomingData.type == EspNowManager::logType::PAKG);
    const uint8_t packetIndex = getPacketIndex(incomingData.packetInfo);
    const bool packetIsLast = ::isLastPacket(incomingData.packetInfo);
    const bool keepLineOpen = (!packetIsLast) && (packetIndex < MESSAGE_PACKET_MAX_INDEX);

    if (g_rxAssemblyActive && g_rxAssemblyHasMac && mac != nullptr && !sameMacAddress(g_rxAssemblyMac, mac)) {
        closeOpenRxLine(g_io);
        resetRxAssemblyState();
    }

    bool appendToPrevious = false;
    if (isPackageContinuation) {
        appendToPrevious = g_rxAssemblyActive &&
                           packetIndex == g_rxExpectedPacketIndex &&
                           (!g_rxAssemblyHasMac || sameMacAddress(g_rxAssemblyMac, mac));

        if (!appendToPrevious) {
            closeOpenRxLine(g_io);
        }
    } else {
        closeOpenRxLine(g_io);
    }

    char packetProgress[16] = {0};
    if (packetIsLast) {
        std::snprintf(
            packetProgress,
            sizeof(packetProgress),
            "%u/%u",
            static_cast<unsigned>(packetIndex + 1U),
            static_cast<unsigned>(packetIndex + 1U)
        );
    } else {
        std::snprintf(
            packetProgress,
            sizeof(packetProgress),
            "%u/?",
            static_cast<unsigned>(packetIndex + 1U)
        );
    }

    char header[96] = {0};
    if (!appendToPrevious) {
        const String arrivedAt = arrivalTimeText();
        const char* typeText = logTypeToString(incomingData.type);
        std::snprintf(
            header,
            sizeof(header),
            "[%s][%s][%s]",
            typeText,
            arrivedAt.c_str(),
            packetProgress
        );
    }

    char closeSuffix[24] = {0};
    if (appendToPrevious && packetIsLast) {
        std::snprintf(
            closeSuffix,
            sizeof(closeSuffix),
            "[%u/%u]",
            static_cast<unsigned>(packetIndex + 1U),
            static_cast<unsigned>(packetIndex + 1U)
        );
    }

    if (keepLineOpen) {
        g_rxAssemblyActive = true;
        g_rxExpectedPacketIndex = static_cast<uint8_t>(packetIndex + 1U);
        if (mac != nullptr) {
            std::memcpy(g_rxAssemblyMac, mac, sizeof(g_rxAssemblyMac));
            g_rxAssemblyHasMac = true;
        } else {
            g_rxAssemblyHasMac = false;
        }
    } else {
        resetRxAssemblyState();
    }

    const uint16_t color = logTypeToLcdColor(incomingData.type);
    const bool displayQueueReady = (g_rxDisplayQueue != nullptr);
    if (!enqueueDisplayLine(header, payload, payloadLen, color, appendToPrevious, keepLineOpen, closeSuffix)) {
        // Fallback only when queue is unavailable.
        if (!displayQueueReady) {
            if (g_io != nullptr) {
                if (appendToPrevious) {
                    writeRxContinuationToStream(g_io, payload, payloadLen, keepLineOpen, closeSuffix);
                } else {
                    writeRxLineToStream(g_io, header, payload, payloadLen, keepLineOpen, closeSuffix);
                }
            }

#if !HIGH_FREQUENCY_INCOMMING_ESPNOW
            if (g_lcdTerminal != nullptr && g_lcdTerminal->isReady()) {
                String lcdLine;
                if (payloadLen > 0) {
                    String lcdPayload = String(payload);
                    lcdPayload.replace('\r', ' ');
                    lcdPayload.replace('\n', ' ');
                    lcdPayload.replace('\t', ' ');
                    lcdPayload.trim();
                    if (appendToPrevious) {
                        lcdLine = lcdPayload;
                    } else {
                        lcdLine = String(header);
                        lcdLine += " ";
                        lcdLine += lcdPayload;
                    }
                } else if (!appendToPrevious) {
                    lcdLine = String(header);
                }

                if (!keepLineOpen && closeSuffix[0] != '\0') {
                    if (lcdLine.length() > 0) {
                        lcdLine += " ";
                    }
                    lcdLine += closeSuffix;
                }

                if (lcdLine.length() > 0) {
                    g_lcdTerminal->writeText(lcdLine, color);
                }
            }
#endif
        }
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
        } else {
#if HIGH_FREQUENCY_INCOMMING_ESPNOW
            // High-frequency mode persists only through manual flush.
            ++g_droppedRxDbLogCount;
#else
            if (!g_database->logIncomingEspNow(mac, incomingData)) {
                ++g_droppedRxDbLogCount;
            }
#endif
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

    xQueueReset(g_rxQueue);
    xQueueReset(g_rxDisplayQueue);
    if (g_rxDbLogQueue != nullptr) {
        xQueueReset(g_rxDbLogQueue);
    }
    g_droppedRxCount = 0;
    g_overwrittenRxDisplayCount = 0;
    g_droppedRxDbLogCount = 0;
    g_rxLineOpen = false;
    g_rxContinuationPadding = 1;
    resetRxAssemblyState();
    g_asyncDbLogEnabled = false;
    g_asyncRxEnabled = true;
    return true;
}

void disableAsyncRx() {
    closeOpenRxLine(g_io);
    resetRxAssemblyState();
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
    if (enabled && g_rxDbLogQueue == nullptr) {
        (void)createDbLogQueue();
    }

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
            if (item.appendToPrevious) {
                writeRxContinuationToStream(
                    g_io,
                    item.payload,
                    item.payloadLen,
                    item.keepLineOpen,
                    item.closeSuffix
                );
            } else {
                writeRxLineToStream(
                    g_io,
                    item.header,
                    item.payload,
                    item.payloadLen,
                    item.keepLineOpen,
                    item.closeSuffix
                );
            }
        }

#if !HIGH_FREQUENCY_INCOMMING_ESPNOW
        if (g_lcdTerminal != nullptr && g_lcdTerminal->isReady()) {
            String lcdLine;
            if (item.payloadLen > 0) {
                String lcdPayload = String(item.payload);
                lcdPayload.replace('\r', ' ');
                lcdPayload.replace('\n', ' ');
                lcdPayload.replace('\t', ' ');
                lcdPayload.trim();
                if (item.appendToPrevious) {
                    lcdLine = lcdPayload;
                } else {
                    lcdLine = String(item.header);
                    lcdLine += " ";
                    lcdLine += lcdPayload;
                }
            } else if (!item.appendToPrevious) {
                lcdLine = String(item.header);
            }

            if (!item.keepLineOpen && item.closeSuffix[0] != '\0') {
                if (lcdLine.length() > 0) {
                    lcdLine += " ";
                }
                lcdLine += item.closeSuffix;
            }

            if (lcdLine.length() > 0) {
                g_lcdTerminal->writeText(lcdLine, item.color);
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

uint32_t takeOverwrittenRxDisplayCount() {
    const uint32_t overwritten = g_overwrittenRxDisplayCount;
    g_overwrittenRxDisplayCount = 0;
    return overwritten;
}

uint32_t takeDroppedRxDisplayCount() {
    // Backward-compatible alias.
    return takeOverwrittenRxDisplayCount();
}

uint32_t takeDroppedRxDbLogCount() {
    const uint32_t dropped = g_droppedRxDbLogCount;
    g_droppedRxDbLogCount = 0;
    return dropped;
}

} // namespace EspNowConfig
