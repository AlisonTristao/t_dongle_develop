#include "startup_config.h"

#include <sys/time.h>

#include <cstdio>
#include <ctime>

namespace {

void colorWheel(uint8_t position, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (position < 85U) {
        r = static_cast<uint8_t>(255U - position * 3U);
        g = static_cast<uint8_t>(position * 3U);
        b = 0;
        return;
    }

    if (position < 170U) {
        const uint8_t p = static_cast<uint8_t>(position - 85U);
        r = 0;
        g = static_cast<uint8_t>(255U - p * 3U);
        b = static_cast<uint8_t>(p * 3U);
        return;
    }

    const uint8_t p = static_cast<uint8_t>(position - 170U);
    r = static_cast<uint8_t>(p * 3U);
    g = 0;
    b = static_cast<uint8_t>(255U - p * 3U);
}

bool isSerialTerminalOpen() {
    return static_cast<bool>(Serial);
}

bool parseDateTime(const String& text, time_t& outEpoch) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
        return false;
    }

    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }

    struct tm tmValue = {};
    tmValue.tm_year = year - 1900;
    tmValue.tm_mon = month - 1;
    tmValue.tm_mday = day;
    tmValue.tm_hour = hour;
    tmValue.tm_min = minute;
    tmValue.tm_sec = second;

    const time_t epoch = mktime(&tmValue);
    if (epoch <= 0) {
        return false;
    }

    outEpoch = epoch;
    return true;
}

} // namespace

namespace StartupConfig {

void waitForSerialAndAnimateLed(DonglePeripherals& peripherals) {
    peripherals.beginLed();

    uint8_t wheel = 0;
    uint8_t brightness = 3;
    int8_t direction = 1;
    uint32_t openSinceMs = 0;

    // Require stable open for a short period to avoid false positives during upload/reset toggles.
    constexpr uint32_t kStableOpenMs = 1200;

    while (true) {
        const bool open = isSerialTerminalOpen();
        if (open) {
            if (openSinceMs == 0) {
                openSinceMs = millis();
            } else if ((millis() - openSinceMs) >= kStableOpenMs) {
                break;
            }
        } else {
            openSinceMs = 0;
        }

        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        colorWheel(wheel, r, g, b);
        peripherals.setLedColor(r, g, b, brightness);

        wheel = static_cast<uint8_t>(wheel + 2U);

        const int16_t nextBrightness = static_cast<int16_t>(brightness) + direction;
        if (nextBrightness >= 31) {
            brightness = 31;
            direction = -1;
        } else if (nextBrightness <= 2) {
            brightness = 2;
            direction = 1;
        } else {
            brightness = static_cast<uint8_t>(nextBrightness);
        }

        delay(24);
    }

    while (Serial.available()) {
        Serial.read();
    }

    Serial.println("[startup] monitor conectado. pressione ENTER para iniciar");
    Serial.print("[startup] > ");

    uint32_t lastPromptMs = millis();
    while (true) {
        if (Serial.available()) {
            const String line = Serial.readStringUntil('\n');
            (void)line;
            break;
        }

        if ((millis() - lastPromptMs) >= 2000) {
            Serial.print("\r[startup] > ");
            lastPromptMs = millis();
        }

        delay(10);
    }

    Serial.println();
    Serial.println("[startup] iniciando...");

    peripherals.ledOff();
    delay(80);
}

void promptAndSetDateTime(Stream& io, uint32_t timeoutMs) {
    io.println("[clock] informe data/hora: YYYY-MM-DD HH:MM:SS");
    io.println("[clock] ENTER vazio para pular (timeout 30s)");
    io.print("[clock] > ");

    const uint32_t start = millis();
    String line;

    while ((millis() - start) < timeoutMs) {
        if (Serial.available()) {
            line = Serial.readStringUntil('\n');
            break;
        }
        delay(10);
    }

    line.trim();
    if (line.isEmpty()) {
        io.println("[clock] horario mantido");
        return;
    }

    time_t epoch = 0;
    if (!parseDateTime(line, epoch)) {
        io.println("[clock] formato invalido, mantendo horario atual");
        return;
    }

    timeval tv = {};
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    char out[48] = {0};
    std::tm* now = std::localtime(&epoch);
    if (now != nullptr) {
        std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", now);
        io.print("[clock] horario ajustado para ");
        io.println(out);
    } else {
        io.println("[clock] horario ajustado");
    }
}

} // namespace StartupConfig
