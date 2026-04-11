#include "LcdTerminal.h"

LcdTerminal::LcdTerminal()
    : peripherals_(nullptr),
      tft_(nullptr),
      ready_(false),
            borderColor_(ST77XX_WHITE),
            backgroundColor_(ST77XX_WHITE),
            infoColor_(ST77XX_BLACK),
      warningColor_(ST77XX_YELLOW),
      errorColor_(ST77XX_RED),
      textX_(0),
      textY_(0),
      textW_(0),
      textH_(0),
      cursorY_(0),
      lineH_(8) {
}

bool LcdTerminal::begin(DonglePeripherals& peripherals) {
    peripherals_ = &peripherals;
    tft_ = peripherals_->lcd();
    if (tft_ == nullptr) {
        ready_ = false;
        return false;
    }

    peripherals_->setLcdBacklight(true);

    textX_ = BORDER_PX + 2;
    textY_ = BORDER_PX + 2;
    textW_ = static_cast<int16_t>(tft_->width() - (2 * (BORDER_PX + 2)));
    textH_ = static_cast<int16_t>(tft_->height() - (2 * (BORDER_PX + 2)));
    cursorY_ = textY_;

    tft_->setTextSize(1);
    tft_->setTextWrap(false);

    drawFrame();
    ready_ = true;
    return true;
}

void LcdTerminal::clear() {
    if (!ready_ || tft_ == nullptr) {
        return;
    }

    drawFrame();
}

void LcdTerminal::writeText(const String& text, uint16_t color) {
    if (!ready_ || tft_ == nullptr) {
        return;
    }

    int32_t start = 0;
    while (start <= static_cast<int32_t>(text.length())) {
        const int32_t newline = text.indexOf('\n', start);
        if (newline < 0) {
            writeLineInternal(text.substring(start), color);
            break;
        }

        writeLineInternal(text.substring(start, newline), color);
        start = newline + 1;

        if (start == static_cast<int32_t>(text.length())) {
            writeLineInternal("", color);
            break;
        }
    }
}

void LcdTerminal::writeInfo(const String& text) {
    writeText(text, infoColor_);
}

void LcdTerminal::writeWarning(const String& text) {
    writeText(text, warningColor_);
}

void LcdTerminal::writeError(const String& text) {
    writeText(text, errorColor_);
}

bool LcdTerminal::isReady() const {
    return ready_;
}

void LcdTerminal::drawFrame() {
    tft_->fillScreen(borderColor_);
    tft_->fillRect(
        BORDER_PX,
        BORDER_PX,
        static_cast<int16_t>(tft_->width() - (2 * BORDER_PX)),
        static_cast<int16_t>(tft_->height() - (2 * BORDER_PX)),
        backgroundColor_
    );
    cursorY_ = textY_;
}

void LcdTerminal::writeLineInternal(const String& line, uint16_t color) {
    if (cursorY_ + lineH_ > (textY_ + textH_)) {
        tft_->fillRect(
            BORDER_PX,
            BORDER_PX,
            static_cast<int16_t>(tft_->width() - (2 * BORDER_PX)),
            static_cast<int16_t>(tft_->height() - (2 * BORDER_PX)),
            backgroundColor_
        );
        cursorY_ = textY_;
    }

    tft_->fillRect(textX_, cursorY_, textW_, lineH_, backgroundColor_);
    tft_->setCursor(textX_, cursorY_);
    tft_->setTextColor(color, backgroundColor_);
    tft_->print(fitLine(line));

    cursorY_ += lineH_;
}

String LcdTerminal::fitLine(const String& line) const {
    if (line.length() == 0) {
        return "";
    }

    const int16_t charWidth = 6;
    const int16_t maxChars = (textW_ > 0) ? (textW_ / charWidth) : 0;
    if (maxChars <= 0) {
        return "";
    }

    if (line.length() <= static_cast<unsigned>(maxChars)) {
        return line;
    }

    if (maxChars <= 3) {
        return line.substring(0, maxChars);
    }

    return line.substring(0, maxChars - 3) + "...";
}
