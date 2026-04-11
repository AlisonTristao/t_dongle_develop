#include "DonglePeripherals.h"

#include <SD_MMC.h>
#include <driver/gpio.h>

#include "../../include/config.h"

DonglePeripherals::DonglePeripherals()
    : tft_(
        BoardConfig::PIN_TFT_CS,
        BoardConfig::PIN_TFT_DC,
        BoardConfig::PIN_TFT_SDA,
        BoardConfig::PIN_TFT_SCL,
        BoardConfig::PIN_TFT_RES
    ),
      ledReady_(false),
      lcdReady_(false),
      sdReady_(false),
      sdOneBitMode_(false),
      sdFrequencyKHz_(0),
      lcdBacklightOn_(false),
      lcdBacklightActiveHigh_(false),
      lcdRotation_(1) {
}

void DonglePeripherals::begin() {
    beginLed();
    ledOff();
    beginLcd(lcdRotation_);
}

bool DonglePeripherals::beginLed() {
    pinMode(BoardConfig::PIN_LED_DI, OUTPUT);
    pinMode(BoardConfig::PIN_LED_CI, OUTPUT);
    digitalWrite(BoardConfig::PIN_LED_DI, LOW);
    digitalWrite(BoardConfig::PIN_LED_CI, LOW);
    ledReady_ = true;
    return true;
}

void DonglePeripherals::setLedColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness31) {
    if (!ledReady_) {
        beginLed();
    }

    if (brightness31 > 31) {
        brightness31 = 31;
    }

    writeLedFrame(brightness31, r, g, b);
}

void DonglePeripherals::ledOff() {
    setLedColor(0, 0, 0, 1);
}

bool DonglePeripherals::beginLcd(uint8_t rotation) {
    lcdRotation_ = static_cast<uint8_t>(rotation % 4);

    // Hardware reset sequence increases reliability on cold boot.
    pinMode(BoardConfig::PIN_TFT_RES, OUTPUT);
    digitalWrite(BoardConfig::PIN_TFT_RES, HIGH);
    delay(5);
    digitalWrite(BoardConfig::PIN_TFT_RES, LOW);
    delay(20);
    digitalWrite(BoardConfig::PIN_TFT_RES, HIGH);
    delay(120);

    // Keep backlight on while running init sequence.
    setLcdBacklight(true);

#if defined(INITR_MINI160x80)
    tft_.initR(INITR_MINI160x80);
#else
    tft_.initR(INITR_BLACKTAB);
#endif
    // Align panel RAM window to avoid shifted/garbled content.
    tft_.setPanelOffset(
        static_cast<int8_t>(BoardConfig::TFT_COL_START),
        static_cast<int8_t>(BoardConfig::TFT_ROW_START)
    );
    tft_.setRotation(lcdRotation_);
    tft_.fillScreen(ST77XX_WHITE);
    tft_.setTextColor(ST77XX_BLACK);
    tft_.setTextSize(1);
    tft_.setTextWrap(true);
    const char* bootMessage = "serial closed";
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t textW = 0;
    uint16_t textH = 0;
    tft_.getTextBounds(bootMessage, 0, 0, &x1, &y1, &textW, &textH);

    int16_t cursorX = static_cast<int16_t>((tft_.width() - static_cast<int16_t>(textW)) / 2);
    int16_t cursorY = static_cast<int16_t>((tft_.height() - static_cast<int16_t>(textH)) / 2);
    if (cursorX < 0) {
        cursorX = 0;
    }
    if (cursorY < 0) {
        cursorY = 0;
    }

    tft_.setCursor(cursorX, cursorY);
    tft_.print(bootMessage);

    lcdReady_ = true;
    return true;
}

bool DonglePeripherals::isLcdReady() const {
    return lcdReady_;
}

bool DonglePeripherals::reinitLcd(uint8_t rotation) {
    lcdReady_ = false;
    return beginLcd(rotation);
}

void DonglePeripherals::setLcdRotation(uint8_t rotation) {
    lcdRotation_ = static_cast<uint8_t>(rotation % 4);

    if (!lcdReady_) {
        return;
    }

    tft_.setRotation(lcdRotation_);
    tft_.fillScreen(ST77XX_BLACK);
    tft_.setCursor(0, 0);
}

uint8_t DonglePeripherals::lcdRotation() const {
    return lcdRotation_;
}

Adafruit_ST7735* DonglePeripherals::lcd() {
    if (!lcdReady_ && !beginLcd(lcdRotation_)) {
        return nullptr;
    }

    return &tft_;
}

void DonglePeripherals::setLcdBacklight(bool on) {
    pinMode(BoardConfig::PIN_TFT_LED, OUTPUT);
    lcdBacklightOn_ = on;
    const bool pinLevel = lcdBacklightActiveHigh_ ? lcdBacklightOn_ : !lcdBacklightOn_;
    digitalWrite(BoardConfig::PIN_TFT_LED, pinLevel ? HIGH : LOW);
}

void DonglePeripherals::setLcdBacklightPolarity(bool activeHigh) {
    lcdBacklightActiveHigh_ = activeHigh;
    setLcdBacklight(lcdBacklightOn_);
}

bool DonglePeripherals::isLcdBacklightOn() const {
    return lcdBacklightOn_;
}

bool DonglePeripherals::isLcdBacklightActiveHigh() const {
    return lcdBacklightActiveHigh_;
}

bool DonglePeripherals::writeLcd(const String& text, bool clearFirst, uint16_t color) {
    if (!lcdReady_ && !beginLcd(lcdRotation_)) {
        return false;
    }

    setLcdBacklight(true);

    if (clearFirst) {
        tft_.fillScreen(ST77XX_BLACK);
        tft_.setCursor(0, 0);
    }

    tft_.setTextColor(color);
    tft_.println(text);
    return true;
}

bool DonglePeripherals::clearLcd(uint16_t color) {
    if (!lcdReady_ && !beginLcd(lcdRotation_)) {
        return false;
    }

    setLcdBacklight(true);

    tft_.fillScreen(color);
    tft_.setCursor(0, 0);
    return true;
}

bool DonglePeripherals::beginSd(bool oneBitMode) {
    sdReady_ = false;
    sdOneBitMode_ = oneBitMode;
    sdFrequencyKHz_ = 0;

    // SD_MMC.setPins only works before begin(), so always unmount first.
    SD_MMC.end();
    delay(20);

    // Software pull-ups help, but external pull-ups are still recommended.
    pinMode(BoardConfig::PIN_SDMMC_CMD, INPUT_PULLUP);
    pinMode(BoardConfig::PIN_SDMMC_D0, INPUT_PULLUP);
    pinMode(BoardConfig::PIN_SDMMC_D1, INPUT_PULLUP);
    pinMode(BoardConfig::PIN_SDMMC_D2, INPUT_PULLUP);
    pinMode(BoardConfig::PIN_SDMMC_D3, INPUT_PULLUP);

    gpio_pullup_en(static_cast<gpio_num_t>(BoardConfig::PIN_SDMMC_CMD));
    gpio_pullup_en(static_cast<gpio_num_t>(BoardConfig::PIN_SDMMC_D0));
    gpio_pullup_en(static_cast<gpio_num_t>(BoardConfig::PIN_SDMMC_D1));
    gpio_pullup_en(static_cast<gpio_num_t>(BoardConfig::PIN_SDMMC_D2));
    gpio_pullup_en(static_cast<gpio_num_t>(BoardConfig::PIN_SDMMC_D3));

#if defined(ESP32)
    auto setSdPins = [](bool mode1bit) -> bool {
        if (mode1bit) {
            return SD_MMC.setPins(
                BoardConfig::PIN_SDMMC_CLK,
                BoardConfig::PIN_SDMMC_CMD,
                BoardConfig::PIN_SDMMC_D0
            );
        }

        return SD_MMC.setPins(
            BoardConfig::PIN_SDMMC_CLK,
            BoardConfig::PIN_SDMMC_CMD,
            BoardConfig::PIN_SDMMC_D0,
            BoardConfig::PIN_SDMMC_D1,
            BoardConfig::PIN_SDMMC_D2,
            BoardConfig::PIN_SDMMC_D3
        );
    };
#endif

    struct SdAttempt {
        bool mode1bit;
        int frequencyKHz;
    };

    SdAttempt attempts[4] = {};
    size_t attemptsCount = 0;

    if (oneBitMode) {
        attempts[0] = {true, SDMMC_FREQ_DEFAULT};
        attempts[1] = {true, SDMMC_FREQ_PROBING};
        attemptsCount = 2;
    } else {
        // Try stable 1-bit first to avoid noisy init failures on weak signal cards.
        attempts[0] = {true, SDMMC_FREQ_DEFAULT};
        attempts[1] = {false, SDMMC_FREQ_DEFAULT};
        attempts[2] = {true, SDMMC_FREQ_PROBING};
        attempts[3] = {false, SDMMC_FREQ_PROBING};
        attemptsCount = 4;
    }

    for (size_t i = 0; i < attemptsCount; ++i) {
        const SdAttempt& attempt = attempts[i];

        SD_MMC.end();
        delay(20);

#if defined(ESP32)
        if (!setSdPins(attempt.mode1bit)) {
            continue;
        }
#endif

        if (!SD_MMC.begin("/sdcard", attempt.mode1bit, false, attempt.frequencyKHz)) {
            continue;
        }

        if (SD_MMC.cardType() == CARD_NONE) {
            SD_MMC.end();
            continue;
        }

        sdReady_ = true;
        sdOneBitMode_ = attempt.mode1bit;
        sdFrequencyKHz_ = static_cast<uint32_t>(attempt.frequencyKHz);
        return true;
    }

    Serial.println("[dongle] SD init falhou (tentou 4-bit/1-bit e clock reduzido)");
    return false;
}

bool DonglePeripherals::isSdReady() const {
    return sdReady_;
}

bool DonglePeripherals::sdOneBitMode() const {
    return sdOneBitMode_;
}

uint32_t DonglePeripherals::sdFrequencyKHz() const {
    return sdFrequencyKHz_;
}

String DonglePeripherals::sdCardTypeName() const {
    if (!sdReady_) {
        return "NONE";
    }

    const uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_MMC) {
        return "MMC";
    }
    if (cardType == CARD_SD) {
        return "SDSC";
    }
    if (cardType == CARD_SDHC) {
        return "SDHC";
    }

    return "UNKNOWN";
}

uint64_t DonglePeripherals::sdTotalMB() const {
    if (!sdReady_) {
        return 0;
    }
    return SD_MMC.totalBytes() / (1024ULL * 1024ULL);
}

uint64_t DonglePeripherals::sdUsedMB() const {
    if (!sdReady_) {
        return 0;
    }
    return SD_MMC.usedBytes() / (1024ULL * 1024ULL);
}

void DonglePeripherals::sendLedByte(uint8_t value) const {
    for (int8_t bit = 7; bit >= 0; --bit) {
        digitalWrite(BoardConfig::PIN_LED_DI, (value & (1U << bit)) ? HIGH : LOW);
        digitalWrite(BoardConfig::PIN_LED_CI, HIGH);
        digitalWrite(BoardConfig::PIN_LED_CI, LOW);
    }
}

void DonglePeripherals::writeLedFrame(uint8_t brightness31, uint8_t r, uint8_t g, uint8_t b) const {
    for (uint8_t i = 0; i < 4; ++i) {
        sendLedByte(0x00);
    }

    sendLedByte(static_cast<uint8_t>(0xE0 | (brightness31 & 0x1F)));
    sendLedByte(b);
    sendLedByte(g);
    sendLedByte(r);

    for (uint8_t i = 0; i < 4; ++i) {
        sendLedByte(0xFF);
    }
}
