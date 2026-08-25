#include "StartupConfig.h"

#include "ShellOutput.h"

namespace {

void showBootStatusOnLcd(DonglePeripherals& peripherals) {
    Adafruit_ST7735* lcd = peripherals.lcd();
    if (lcd == nullptr) {
        return;
    }

    // Keep the same calibrated color pair previously used in beginLcd
    // (panel on this board has inverted visual polarity in practice).
    constexpr uint16_t kStatusBg = ST77XX_WHITE;
    constexpr uint16_t kStatusFg = ST77XX_BLACK;

    const char* line1 = "bally dongle";
    const char* line2 = "iniciando...";

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

} // namespace

namespace StartupConfig{

void announceBoot(DonglePeripherals& peripherals) {
    // Previously blocked here (while(!Serial)) until the host asserted DTR
    // on the USB CDC port, so a terminal attaching after power-on wouldn't
    // miss early boot output. Only interactive terminals (Arduino Serial
    // Monitor, PuTTY, ...) assert DTR on open; an automated BTP client (e.g.
    // TraceView's QSerialPort) does not, so that wait never resolved and the
    // dongle sat here forever instead of ever reaching the shell/BTP
    // handshake. Boot now proceeds immediately once powered on -- no serial
    // connection required.
    peripherals.beginLed();
    peripherals.setLedColor(0, 255, 0, 8);

    showBootStatusOnLcd(peripherals);

    ShellOutput::printTagged(Serial, "startup", "iniciando...");

    peripherals.ledOff();
}

} // namespace StartupConfig
