#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

class DongleSt7735 final : public Adafruit_ST7735 {
public:
    DongleSt7735(int8_t cs, int8_t dc, int8_t mosi, int8_t sclk, int8_t rst = -1)
        : Adafruit_ST7735(cs, dc, mosi, sclk, rst) {
    }

    void setPanelOffset(int8_t colStart, int8_t rowStart) {
        setColRowStart(colStart, rowStart);
    }
};

/**
 * @brief Unified peripheral manager for LILYGO T-Dongle-S3.
 *
 * Covered peripherals:
 * - onboard RGB LED driver (DI/CI)
 * - ST7735 LCD display
 * - TF card through SD_MMC
 */
class DonglePeripherals final {
public:
    DonglePeripherals();

    /**
     * @brief Initializes LED and LCD with safe defaults.
     */
    void begin();

    /**
     * @brief Initializes LED interface only.
     */
    bool beginLed();

    /**
     * @brief Sets RGB color for onboard LED.
     * @param r Red channel 0..255.
     * @param g Green channel 0..255.
     * @param b Blue channel 0..255.
     * @param brightness31 Global brightness 0..31.
     */
    void setLedColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness31 = 31);

    /**
     * @brief Turns LED off.
     */
    void ledOff();

    /**
     * @brief Initializes ST7735 LCD.
     * @param rotation Display rotation 0..3.
     */
    bool beginLcd(uint8_t rotation = 1);

    /**
     * @brief Returns true when LCD is initialized.
     */
    bool isLcdReady() const;

    /**
     * @brief Reinitializes LCD and clears the screen.
     */
    bool reinitLcd(uint8_t rotation = 1);

    /**
     * @brief Updates LCD rotation at runtime.
     * @param rotation Orientation index 0..3.
     */
    void setLcdRotation(uint8_t rotation);

    /**
     * @brief Returns current LCD rotation index.
     */
    uint8_t lcdRotation() const;

    /**
     * @brief Returns LCD object pointer, ensuring init when possible.
     */
    Adafruit_ST7735* lcd();

    /**
     * @brief Controls LCD backlight pin.
     */
    void setLcdBacklight(bool on);

    /**
     * @brief Sets backlight electrical polarity.
     * @param activeHigh true when HIGH means ON, false when LOW means ON.
     */
    void setLcdBacklightPolarity(bool activeHigh);

    /**
     * @brief Returns current software state of LCD backlight.
     */
    bool isLcdBacklightOn() const;

    /**
     * @brief Returns configured backlight polarity.
     */
    bool isLcdBacklightActiveHigh() const;

    /**
     * @brief Clears screen and writes text at top-left.
     * @param text Text to print.
     * @param clearFirst Clear screen before print.
     * @param color ST77XX text color.
     */
    bool writeLcd(const String& text, bool clearFirst = true, uint16_t color = ST77XX_WHITE);

    /**
     * @brief Fills LCD screen with one color.
     */
    bool clearLcd(uint16_t color = ST77XX_BLACK);

    /**
     * @brief Initializes SD card over SD_MMC pins.
     * @param oneBitMode true for 1-bit mode, false for 4-bit mode.
     */
    bool beginSd(bool oneBitMode = false);

    /**
     * @brief Returns true when SD card is mounted and ready.
     */
    bool isSdReady() const;

    /**
     * @brief Returns active SD bus width mode.
     * @return true when operating in 1-bit mode, false for 4-bit mode.
     */
    bool sdOneBitMode() const;

    /**
     * @brief Returns SD host clock frequency in kHz for the active mount.
     */
    uint32_t sdFrequencyKHz() const;

    /**
     * @brief Returns SD card type as text.
     */
    String sdCardTypeName() const;

    /**
     * @brief Total SD capacity in megabytes.
     */
    uint64_t sdTotalMB() const;

    /**
     * @brief Used SD bytes in megabytes.
     */
    uint64_t sdUsedMB() const;

private:
    DongleSt7735 tft_;
    bool ledReady_;
    bool lcdReady_;
    bool sdReady_;
    bool sdOneBitMode_;
    uint32_t sdFrequencyKHz_;
    bool lcdBacklightOn_;
    bool lcdBacklightActiveHigh_;
    uint8_t lcdRotation_;

    void sendLedByte(uint8_t value) const;
    void writeLedFrame(uint8_t brightness31, uint8_t r, uint8_t g, uint8_t b) const;
};
