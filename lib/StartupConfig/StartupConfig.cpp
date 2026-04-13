#include "StartupConfig.h"

#include <sys/time.h>

#include <cctype>
#include <cstring>
#include <cstdio>
#include <ctime>

#include "ShellOutput.h"

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

void showSerialStatusOnLcd(DonglePeripherals& peripherals, bool connected) {
    Adafruit_ST7735* lcd = peripherals.lcd();
    if (lcd == nullptr) {
        return;
    }

    // Keep the same calibrated color pair previously used in beginLcd
    // (panel on this board has inverted visual polarity in practice).
    constexpr uint16_t kStatusBg = ST77XX_WHITE;
    constexpr uint16_t kStatusFg = ST77XX_BLACK;

    const char* line1 = connected ? "serial conectado" : "serial desconectado";
    const char* line2 = connected ? "iniciando..." : "aguardando monitor";

    peripherals.setLcdBacklight(true);
    lcd->fillScreen(kStatusBg);
    lcd->setTextWrap(false);
    lcd->setTextSize(1);
    lcd->setTextColor(kStatusFg, kStatusBg);

    const char* lines[2] = {line1, line2};
    uint16_t widths[2] = {0, 0};
    uint16_t heights[2] = {0, 0};
    int16_t x1 = 0;
    int16_t y1 = 0;

    for (size_t i = 0; i < 2; ++i) {
        lcd->getTextBounds(lines[i], 0, 0, &x1, &y1, &widths[i], &heights[i]);
        if (heights[i] == 0) {
            heights[i] = 8;
        }
    }

    constexpr int16_t kLineSpacing = 4;
    const int16_t totalHeight = static_cast<int16_t>(heights[0] + heights[1] + kLineSpacing);
    int16_t y = static_cast<int16_t>((static_cast<int16_t>(lcd->height()) - totalHeight) / 2);
    if (y < 0) {
        y = 0;
    }

    for (size_t i = 0; i < 2; ++i) {
        int16_t x = static_cast<int16_t>((static_cast<int16_t>(lcd->width()) - static_cast<int16_t>(widths[i])) / 2);
        if (x < 0) {
            x = 0;
        }

        lcd->setCursor(x, y);
        lcd->print(lines[i]);
        y = static_cast<int16_t>(y + heights[i] + kLineSpacing);
    }
}

bool isSerialTerminalOpen() {
    return static_cast<bool>(Serial);
}

void drainSerialInput() {
    while (Serial.available()) {
        Serial.read();
    }
}

bool readLineWithEcho(Stream& io, String& outLine, uint32_t timeoutMs) {
    outLine = "";
    const uint32_t start = millis();

    while ((millis() - start) < timeoutMs) {
        while (Serial.available()) {
            const int value = Serial.read();
            if (value < 0) {
                break;
            }

            const char c = static_cast<char>(value);

            if (c == '\r' || c == '\n') {
                io.println();

                // Consume optional paired line ending to avoid leaking a leftover '\n'/'\r'.
                while (Serial.available()) {
                    const int next = Serial.peek();
                    if (next == '\r' || next == '\n') {
                        Serial.read();
                    } else {
                        break;
                    }
                }

                return true;
            }

            if (c == '\b' || c == 127) {
                if (!outLine.isEmpty()) {
                    outLine.remove(outLine.length() - 1);
                    io.print("\b \b");
                }
                continue;
            }

            if (std::isprint(static_cast<unsigned char>(c)) != 0) {
                outLine += c;
                io.print(c);
            }
        }

        delay(10);
    }

    return false;
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

namespace StartupConfig{

void waitForSerialAndAnimateLed(DonglePeripherals& peripherals) {
    peripherals.beginLed();
    showSerialStatusOnLcd(peripherals, false);

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

    drainSerialInput();

    ShellOutput::printTagged(Serial, "startup", "monitor conectado. pressione ENTER para iniciar");
    Serial.print(ShellOutput::commandPrefix());

    bool enterConfirmed = false;
    uint32_t lastPromptMs = millis();
    while (!enterConfirmed) {
        if (Serial.available()) {
            const int value = Serial.read();
            if (value >= 0) {
                const char c = static_cast<char>(value);
                if (c == '\r' || c == '\n') {
                    ShellOutput::writeRawLine(Serial, "");

                    while (Serial.available()) {
                        const int next = Serial.peek();
                        if (next == '\r' || next == '\n') {
                            Serial.read();
                        } else {
                            break;
                        }
                    }

                    enterConfirmed = true;
                    continue;
                }

                if (std::isprint(static_cast<unsigned char>(c)) != 0) {
                    Serial.print(c);
                }
            }
        }

        if ((millis() - lastPromptMs) >= 2000) {
            Serial.print("\r");
            Serial.print(ShellOutput::commandPrefix());
            lastPromptMs = millis();
        }

        delay(10);
    }

    ShellOutput::writeRawLine(Serial, "");
    ShellOutput::printTagged(Serial, "startup", "iniciando...");

    showSerialStatusOnLcd(peripherals, true);

    peripherals.ledOff();
    delay(80);
}

void promptAndSetDateTime(Stream& io, uint32_t timeoutMs) {
    drainSerialInput();

    ShellOutput::printTagged(io, "clock", "informe data/hora: YYYY-MM-DD HH:MM:SS");
    ShellOutput::printTagged(io, "clock", "ENTER vazio para pular (timeout 30s)");
    io.print(ShellOutput::commandPrefix());

    String line;

    const bool submitted = readLineWithEcho(io, line, timeoutMs);
    if (!submitted) {
        ShellOutput::printTagged(io, "clock", "timeout sem confirmacao, mantendo horario atual");
        return;
    }

    line.trim();
    if (line.isEmpty()) {
        ShellOutput::printTagged(io, "clock", "horario mantido");
        return;
    }

    time_t epoch = 0;
    if (!parseDateTime(line, epoch)) {
        ShellOutput::printTagged(io, "clock", "formato invalido, mantendo horario atual");
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
        ShellOutput::printTagged(io, "clock", String("horario ajustado para ") + out);
    } else {
        ShellOutput::printTagged(io, "clock", "horario ajustado");
    }
}

} // namespace StartupConfig
