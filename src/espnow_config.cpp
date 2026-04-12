#include "espnow_config.h"
#include "LcdTerminal.h"

#include <cctype>
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
volatile uint32_t g_droppedRxCount = 0;
bool g_asyncRxEnabled = false;

constexpr size_t kMaxDisplayPayload = 96;

struct RxDisplayLine {
    char text[320];
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

void sanitizeIncomingText(const EspNowManager::message& incomingData, char* outText, size_t outSize) {
    if (outText == nullptr || outSize == 0) {
        return;
    }

    const size_t payloadLimit = (outSize - 1 < EspNowManager::MESSAGE_TEXT_SIZE)
        ? (outSize - 1)
        : EspNowManager::MESSAGE_TEXT_SIZE;

    std::memcpy(outText, incomingData.msg, payloadLimit);
    outText[payloadLimit] = '\0';

    for (size_t i = 0; i < payloadLimit; ++i) {
        char& ch = outText[i];
        if (ch == '\0') {
            break;
        }
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
    }

    size_t start = 0;
    while (outText[start] != '\0' && std::isspace(static_cast<unsigned char>(outText[start])) != 0) {
        ++start;
    }

    size_t end = std::strlen(outText);
    while (end > start && std::isspace(static_cast<unsigned char>(outText[end - 1])) != 0) {
        --end;
    }

    if (start > 0) {
        std::memmove(outText, outText + start, end - start);
    }
    outText[end - start] = '\0';

    const size_t len = std::strlen(outText);
    if (len == 0) {
        std::snprintf(outText, outSize, "(vazio)");
        return;
    }

    if (len > kMaxDisplayPayload && kMaxDisplayPayload + 1 < outSize) {
        outText[kMaxDisplayPayload] = '\0';
        if (kMaxDisplayPayload >= 3) {
            outText[kMaxDisplayPayload - 3] = '.';
            outText[kMaxDisplayPayload - 2] = '.';
            outText[kMaxDisplayPayload - 1] = '.';
        }
    }
}

void writeLineToStream(Stream* io, const char* line) {
    if (io == nullptr || line == nullptr) {
        return;
    }

    const uint8_t* data = reinterpret_cast<const uint8_t*>(line);
    size_t remaining = std::strlen(line);
    uint32_t retries = 0;

    while (remaining > 0) {
        const size_t sent = io->write(data, remaining);
        if (sent == 0) {
            ++retries;
            if (retries > 8) {
                break;
            }
            delay(1);
            continue;
        }

        data += sent;
        remaining -= sent;
    }

    io->write('\r');
    io->write('\n');
}

bool enqueueDisplayLine(const char* line, uint16_t color) {
    if (line == nullptr) {
        return false;
    }

    if (g_rxDisplayQueue == nullptr) {
        return false;
    }

    RxDisplayLine item = {};
    std::snprintf(item.text, sizeof(item.text), "%s", line);
    item.color = color;

    return xQueueSend(g_rxDisplayQueue, &item, 0) == pdTRUE;
}

void processRxMessageInternal(const uint8_t mac[6], const EspNowManager::message& incomingData) {
    const String arrivedAt = arrivalTimeText();
    const char* typeText = logTypeToText(incomingData.type);
    char payload[EspNowManager::MESSAGE_TEXT_SIZE + 1] = {0};
    sanitizeIncomingText(incomingData, payload, sizeof(payload));

    // Keep line compact to avoid monitor visual wrap corruption on long terminals.
    char line[320] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "[espnow][rx] t=%s tipo=%s msg=%s",
        arrivedAt.c_str(),
        typeText,
        payload
    );

    const uint16_t color = logTypeToLcdColor(incomingData.type);
    if (!enqueueDisplayLine(line, color)) {
        // Fallback path when queue is unavailable/full.
        if (g_io != nullptr) {
            writeLineToStream(g_io, line);
        }

        if (g_lcdTerminal != nullptr && g_lcdTerminal->isReady()) {
            g_lcdTerminal->writeText(String(line), color);
        }
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
        g_rxDisplayQueue = xQueueCreate(static_cast<UBaseType_t>(queueDepth * 2), sizeof(RxDisplayLine));
        if (g_rxDisplayQueue == nullptr) {
            g_asyncRxEnabled = false;
            return false;
        }
    }

    xQueueReset(g_rxQueue);
    xQueueReset(g_rxDisplayQueue);
    g_droppedRxCount = 0;
    g_asyncRxEnabled = true;
    return true;
}

void disableAsyncRx() {
    g_asyncRxEnabled = false;
    if (g_rxQueue != nullptr) {
        xQueueReset(g_rxQueue);
    }
    if (g_rxDisplayQueue != nullptr) {
        xQueueReset(g_rxDisplayQueue);
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

void flushRxDisplayLines(size_t maxLines) {
    if (g_rxDisplayQueue == nullptr || maxLines == 0) {
        return;
    }

    RxDisplayLine item = {};
    size_t drained = 0;
    while (drained < maxLines && xQueueReceive(g_rxDisplayQueue, &item, 0) == pdTRUE) {
        if (g_io != nullptr) {
            writeLineToStream(g_io, item.text);
        }

        if (g_lcdTerminal != nullptr && g_lcdTerminal->isReady()) {
            g_lcdTerminal->writeText(String(item.text), item.color);
        }

        ++drained;
    }
}

uint32_t takeDroppedRxCount() {
    const uint32_t dropped = g_droppedRxCount;
    g_droppedRxCount = 0;
    return dropped;
}

} // namespace EspNowConfig
