#pragma once

#include <Arduino.h>
#include <Adafruit_ST7735.h>

#include <DonglePeripherals.h>

/**
 * @brief Minimal terminal-like UI rendered on ST7735.
 *
 * Visual style:
 * - 2px border on all sides
 * - gray background inside border
 * - colored text lines with automatic wrap/scroll reset
 */
class LcdTerminal final {
public:
    static constexpr uint8_t BORDER_PX = 2;

    LcdTerminal();

    /**
     * @brief Attaches to DonglePeripherals LCD and draws terminal frame.
     */
    bool begin(DonglePeripherals& peripherals);

    /**
     * @brief Redraws border/background and resets cursor.
     */
    void clear();

    /**
     * @brief Writes text block, splitting by newline.
     */
    void writeText(const String& text, uint16_t color);

    /**
     * @brief Writes one line in default color.
     */
    void writeInfo(const String& text);

    /**
     * @brief Writes one line in warning color.
     */
    void writeWarning(const String& text);

    /**
     * @brief Writes one line in error color.
     */
    void writeError(const String& text);

    /**
     * @brief Returns true when terminal is attached to a ready LCD.
     */
    bool isReady() const;

private:
    DonglePeripherals* peripherals_;
    Adafruit_ST7735* tft_;
    bool ready_;

    uint16_t borderColor_;
    uint16_t backgroundColor_;
    uint16_t infoColor_;
    uint16_t warningColor_;
    uint16_t errorColor_;

    int16_t textX_;
    int16_t textY_;
    int16_t textW_;
    int16_t textH_;
    int16_t cursorY_;
    uint8_t lineH_;

    void drawFrame();
    void writeLineInternal(const String& line, uint16_t color);
    String fitLine(const String& line) const;
};
