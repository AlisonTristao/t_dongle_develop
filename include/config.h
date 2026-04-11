#pragma once

#include <Arduino.h>

// LILYGO T-Dongle-S3 pin map (based on board silk/pinout image)
namespace BoardConfig {

// Buttons
static constexpr uint8_t PIN_BOOT_BUTTON = 0;   // GPIO0

// USB native pins
static constexpr uint8_t PIN_USB_DN = 19;       // GPIO19
static constexpr uint8_t PIN_USB_DP = 20;       // GPIO20

// UART header pins
static constexpr uint8_t PIN_UART_TX = 43;      // GPIO43 (TX)
static constexpr uint8_t PIN_UART_RX = 44;      // GPIO44 (RX)

// LCD ST7735 (SPI)
static constexpr uint8_t PIN_TFT_CS   = 4;      // GPIO4
static constexpr uint8_t PIN_TFT_SDA  = 3;      // GPIO3 (MOSI)
static constexpr uint8_t PIN_TFT_SCL  = 5;      // GPIO5 (SCLK)
static constexpr uint8_t PIN_TFT_DC   = 2;      // GPIO2
static constexpr uint8_t PIN_TFT_RES  = 1;      // GPIO1
static constexpr uint8_t PIN_TFT_LED  = 38;     // GPIO38 (backlight)

// TF card (SDMMC bus on this board)
static constexpr uint8_t PIN_SDMMC_D0  = 14;    // GPIO14
static constexpr uint8_t PIN_SDMMC_D1  = 17;    // GPIO17
static constexpr uint8_t PIN_SDMMC_D2  = 18;    // GPIO18
static constexpr uint8_t PIN_SDMMC_D3  = 21;    // GPIO21
static constexpr uint8_t PIN_SDMMC_CLK = 12;    // GPIO12
static constexpr uint8_t PIN_SDMMC_CMD = 16;    // GPIO16

// Onboard clock/data LED driver lines
static constexpr uint8_t PIN_LED_DI = 40;       // GPIO40
static constexpr uint8_t PIN_LED_CI = 39;       // GPIO39

// Configure all board GPIO directions and default levels.
// Keep lcdBacklightOn=false for safe startup with screen off.
inline void initBoardPins(bool lcdBacklightOn = false) {
	// Inputs
	pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
	pinMode(PIN_UART_RX, INPUT);

	// USB pins are controlled by USB peripheral; set as inputs here.
	pinMode(PIN_USB_DN, INPUT);
	pinMode(PIN_USB_DP, INPUT);

	// Outputs
	pinMode(PIN_UART_TX, OUTPUT);
	digitalWrite(PIN_UART_TX, HIGH); // UART idle level

	pinMode(PIN_TFT_CS, OUTPUT);
	pinMode(PIN_TFT_SDA, OUTPUT);
	pinMode(PIN_TFT_SCL, OUTPUT);
	pinMode(PIN_TFT_DC, OUTPUT);
	pinMode(PIN_TFT_RES, OUTPUT);
	pinMode(PIN_TFT_LED, OUTPUT);

	// Safe defaults for SPI LCD lines before display init
	digitalWrite(PIN_TFT_CS, HIGH);
	digitalWrite(PIN_TFT_SDA, LOW);
	digitalWrite(PIN_TFT_SCL, LOW);
	digitalWrite(PIN_TFT_DC, LOW);
	digitalWrite(PIN_TFT_RES, HIGH);
	digitalWrite(PIN_TFT_LED, lcdBacklightOn ? HIGH : LOW);

	// SDMMC lines: keep pulled-up until SD/MMC driver starts
	pinMode(PIN_SDMMC_D0, INPUT_PULLUP);
	pinMode(PIN_SDMMC_D1, INPUT_PULLUP);
	pinMode(PIN_SDMMC_D2, INPUT_PULLUP);
	pinMode(PIN_SDMMC_D3, INPUT_PULLUP);
	pinMode(PIN_SDMMC_CLK, INPUT_PULLUP);
	pinMode(PIN_SDMMC_CMD, INPUT_PULLUP);

	// LED data/clock lines
	pinMode(PIN_LED_DI, OUTPUT);
	pinMode(PIN_LED_CI, OUTPUT);
	digitalWrite(PIN_LED_DI, LOW);
	digitalWrite(PIN_LED_CI, LOW);
}

} // namespace BoardConfig
