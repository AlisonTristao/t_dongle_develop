#include "StartupConfig.h"

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

    // No interactive ENTER/clock prompt here anymore -- the BTP client
    // (e.g. TraceView) queries and, if needed, corrects the clock itself via
    // the "dongle clock" / "dongle set_clock" shell commands once connected,
    // so boot goes straight from "port open" to the shell being ready.
    ShellOutput::printTagged(Serial, "startup", "iniciando...");

    showSerialStatusOnLcd(peripherals, true);

    peripherals.ledOff();
    delay(80);
}

} // namespace StartupConfig
