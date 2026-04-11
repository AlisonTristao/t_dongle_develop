#include "DonglePeripherals.h"

#include <SD_MMC.h>

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
        lcdBacklightOn_(false),
        lcdBacklightActiveHigh_(false) {
}

void DonglePeripherals::begin() {
    beginLed();
    ledOff();
    beginLcd(1);
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
    tft_.setRotation(rotation % 4);
    tft_.fillScreen(ST77XX_BLACK);
    tft_.setTextColor(ST77XX_WHITE);
    tft_.setTextSize(1);
    tft_.setTextWrap(true);
    tft_.setCursor(0, 0);
    tft_.println("LCD ready");

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
    if (!lcdReady_ && !beginLcd(1)) {
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
    if (!lcdReady_ && !beginLcd(1)) {
        return false;
    }

    setLcdBacklight(true);

    tft_.fillScreen(color);
    tft_.setCursor(0, 0);
    return true;
}

bool DonglePeripherals::beginSd(bool oneBitMode) {
#if defined(ESP32)
    if (!SD_MMC.setPins(
            BoardConfig::PIN_SDMMC_CLK,
            BoardConfig::PIN_SDMMC_CMD,
            BoardConfig::PIN_SDMMC_D0,
            BoardConfig::PIN_SDMMC_D1,
            BoardConfig::PIN_SDMMC_D2,
            BoardConfig::PIN_SDMMC_D3
        )) {
        sdReady_ = false;
        return false;
    }
#endif

    if (!SD_MMC.begin("/sdcard", oneBitMode)) {
        sdReady_ = false;
        return false;
    }

    sdReady_ = (SD_MMC.cardType() != CARD_NONE);
    return sdReady_;
}

bool DonglePeripherals::isSdReady() const {
    return sdReady_;
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
