#include "LcdDashboard.h"

#include <cstdio>
#include <ctime>

namespace {

constexpr uint16_t kRawWhite = ST77XX_WHITE;
constexpr uint16_t kRawBlack = ST77XX_BLACK;
constexpr uint16_t kRawGridLine = 0xC618; // light gray, structural chrome (not panel-corrected)
constexpr uint16_t kNeutralGray565 = 0x8410;
constexpr int16_t kBarHeight = 16;
constexpr int16_t kClockWidth = 36;

} // namespace

LcdDashboard::LcdDashboard()
    : tft_(nullptr),
      ready_(false),
      screenW_(0),
      screenH_(0),
      messageRect_{0, 0, 0, 0},
      clockRect_{0, 0, 0, 0},
      linkTile_{0, 0, 0, 0},
      rxTile_{0, 0, 0, 0},
      txTile_{0, 0, 0, 0},
      stateTile_{0, 0, 0, 0},
      errTile_{0, 0, 0, 0},
      lastRefreshMs_(0),
      lastDrawnClock_(""),
      rxPulseUntilMs_(0),
      rxHotDrawn_(false),
      txPulseUntilMs_(0),
      txHasResult_(false),
      txLastOk_(false),
      txHotDrawnHot_(false),
      txHotDrawnHasResult_(false),
      txHotDrawnOk_(false),
      droppedTotal_(0),
      errCacheValid_(false),
      errCacheTotal_(0),
      hasHeartbeatResult_(false),
      heartbeatOk_(false),
      linkCacheValid_(false),
      linkCacheHasResult_(false),
      linkCacheOk_(false) {
}

bool LcdDashboard::begin(DonglePeripherals& peripherals) {
    tft_ = peripherals.lcd();
    if (tft_ == nullptr) {
        ready_ = false;
        return false;
    }

    peripherals.setLcdBacklight(true);

    layoutGrid();
    resetCaches();
    drawStaticChrome();

    ready_ = true;
    lastRefreshMs_ = millis() - REFRESH_INTERVAL_MS;
    return true;
}

void LcdDashboard::clear() {
    if (!ready_ || tft_ == nullptr) {
        return;
    }

    layoutGrid();
    resetCaches();
    drawStaticChrome();
}

bool LcdDashboard::isReady() const {
    return ready_;
}

void LcdDashboard::layoutGrid() {
    screenW_ = tft_->width();
    screenH_ = tft_->height();

    clockRect_ = {static_cast<int16_t>(screenW_ - kClockWidth), 0, kClockWidth, kBarHeight};
    messageRect_ = {0, 0, static_cast<int16_t>(screenW_ - kClockWidth), kBarHeight};

    const int16_t gridY = static_cast<int16_t>(kBarHeight + 1);
    const int16_t gridH = static_cast<int16_t>(screenH_ - gridY);
    const int16_t rowH = static_cast<int16_t>(gridH / 2);
    const int16_t lastRowH = static_cast<int16_t>(gridH - rowH);
    const int16_t colW = static_cast<int16_t>(screenW_ / 3);
    const int16_t lastColW = static_cast<int16_t>(screenW_ - (colW * 2));

    linkTile_ = {0, gridY, colW, rowH};
    rxTile_ = {colW, gridY, colW, rowH};
    txTile_ = {static_cast<int16_t>(colW * 2), gridY, lastColW, rowH};

    const int16_t row2Y = static_cast<int16_t>(gridY + rowH);
    stateTile_ = {0, row2Y, static_cast<int16_t>(colW * 2), lastRowH};
    errTile_ = {static_cast<int16_t>(colW * 2), row2Y, lastColW, lastRowH};
}

void LcdDashboard::drawStaticChrome() {
    tft_->fillScreen(kRawWhite);
    tft_->drawFastHLine(0, kBarHeight, screenW_, kRawGridLine);
    tft_->drawFastHLine(0, stateTile_.y, screenW_, kRawGridLine);

    // top row: 3 equal columns
    tft_->drawFastVLine(rxTile_.x, linkTile_.y, static_cast<int16_t>(stateTile_.y - linkTile_.y), kRawGridLine);
    tft_->drawFastVLine(txTile_.x, linkTile_.y, static_cast<int16_t>(stateTile_.y - linkTile_.y), kRawGridLine);

    // bottom row: STATE (double-wide) | ERR
    tft_->drawFastVLine(errTile_.x, stateTile_.y, static_cast<int16_t>(screenH_ - stateTile_.y), kRawGridLine);

    drawLabel(linkTile_, "LINK");
    drawLabel(rxTile_, "RX");
    drawLabel(txTile_, "TX");
    drawLabel(stateTile_, "STATE");
    drawLabel(errTile_, "ERR");
}

void LcdDashboard::resetCaches() {
    // Forces the next tick()/showMessage() to repaint from current values,
    // without touching running accumulators (droppedTotal_, last TX outcome,
    // pulse timers) — a clear() shouldn't erase history, just the pixels.
    lastDrawnClock_ = "";
    linkCacheValid_ = false;
    errCacheValid_ = false;
    rxHotDrawn_ = false;
    txHotDrawnHot_ = false;
    txHotDrawnHasResult_ = false;
    txHotDrawnOk_ = false;
}

uint16_t LcdDashboard::toPanelColor(uint16_t desiredColor) const {
    // Mirrors ShellCommandSupport's status-color compensation for this same
    // ST7735 panel's inverted polarity. Only the tiles this class owns use
    // it; "dongle -lcd" free text keeps sending raw colors, same as before.
    const uint16_t inverted = static_cast<uint16_t>(~desiredColor);
    const uint16_t r = static_cast<uint16_t>((inverted >> 11) & 0x1F);
    const uint16_t g = static_cast<uint16_t>((inverted >> 5) & 0x3F);
    const uint16_t b = static_cast<uint16_t>(inverted & 0x1F);
    return static_cast<uint16_t>((b << 11) | (g << 5) | r);
}

LcdDashboard::Rect LcdDashboard::valueArea(const Rect& tile) const {
    return Rect{
        static_cast<int16_t>(tile.x + 1),
        static_cast<int16_t>(tile.y + 10),
        static_cast<int16_t>(tile.w - 2),
        static_cast<int16_t>(tile.h - 11)
    };
}

void LcdDashboard::drawLabel(const Rect& tile, const char* label) {
    tft_->setTextSize(1);
    tft_->setTextColor(kRawBlack, kRawWhite);
    tft_->setCursor(static_cast<int16_t>(tile.x + 3), static_cast<int16_t>(tile.y + 2));
    tft_->print(label);
}

void LcdDashboard::drawCenteredValue(const Rect& tile, const String& text, uint16_t color, uint8_t textSize) {
    const Rect area = valueArea(tile);
    tft_->fillRect(area.x, area.y, area.w, area.h, kRawWhite);
    if (text.length() == 0) {
        return;
    }

    tft_->setTextSize(textSize);
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t tw = 0;
    uint16_t th = 0;
    tft_->getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);

    const int16_t cx = static_cast<int16_t>(area.x + (area.w - static_cast<int16_t>(tw)) / 2 - x1);
    const int16_t cy = static_cast<int16_t>(area.y + (area.h - static_cast<int16_t>(th)) / 2 - y1);

    tft_->setCursor(cx, cy);
    tft_->setTextColor(color, kRawWhite);
    tft_->print(text);
}

void LcdDashboard::drawActivityDot(const Rect& tile, uint16_t color) {
    const Rect area = valueArea(tile);
    tft_->fillRect(area.x, area.y, area.w, area.h, kRawWhite);

    const int16_t cx = static_cast<int16_t>(area.x + area.w / 2);
    const int16_t cy = static_cast<int16_t>(area.y + area.h / 2);
    const int16_t shorterSide = (area.w < area.h) ? area.w : area.h;
    int16_t radius = static_cast<int16_t>(shorterSide / 2 - 2);
    if (radius < 2) {
        radius = 2;
    }

    tft_->fillCircle(cx, cy, radius, color);
}

void LcdDashboard::showMessage(const String& text, uint16_t color) {
    if (!ready_) {
        return;
    }

    String oneLine = text;
    oneLine.replace('\r', ' ');
    oneLine.replace('\n', ' ');
    oneLine.trim();

    const int16_t charWidth = 6;
    const int16_t maxChars = (messageRect_.w > 4) ? static_cast<int16_t>((messageRect_.w - 4) / charWidth) : 0;
    if (maxChars > 0 && oneLine.length() > static_cast<unsigned>(maxChars)) {
        if (maxChars > 3) {
            oneLine = oneLine.substring(0, maxChars - 3) + "...";
        } else {
            oneLine = oneLine.substring(0, maxChars);
        }
    }

    tft_->fillRect(messageRect_.x, messageRect_.y, messageRect_.w, messageRect_.h, kRawWhite);
    tft_->setTextSize(1);
    tft_->setCursor(static_cast<int16_t>(messageRect_.x + 2), static_cast<int16_t>(messageRect_.y + (messageRect_.h - 8) / 2));
    tft_->setTextColor(color, kRawWhite);
    tft_->print(oneLine);
}

void LcdDashboard::notifyRx() {
    rxPulseUntilMs_ = millis() + PULSE_HOLD_MS;
}

void LcdDashboard::notifyTx(bool delivered) {
    txPulseUntilMs_ = millis() + PULSE_HOLD_MS;
    txLastOk_ = delivered;
    txHasResult_ = true;
}

void LcdDashboard::notifyHeartbeat(bool delivered) {
    heartbeatOk_ = delivered;
    hasHeartbeatResult_ = true;
}

void LcdDashboard::notifyDropped(uint32_t additionalCount) {
    if (additionalCount == 0) {
        return;
    }

    droppedTotal_ += additionalCount;
}

void LcdDashboard::notifyRobotState(const String& state) {
    if (!ready_) {
        return;
    }

    String text = state;
    text.trim();
    if (text.length() == 0) {
        text = "?";
    }

    String lower = text;
    lower.toLowerCase();
    uint16_t logicalColor = kNeutralGray565;
    if (lower.indexOf("erro") >= 0 || lower.indexOf("error") >= 0 || lower.indexOf("fault") >= 0) {
        logicalColor = ST77XX_RED;
    } else if (lower.indexOf("warn") >= 0 || lower.indexOf("aviso") >= 0) {
        logicalColor = ST77XX_YELLOW;
    } else if (lower.indexOf("ok") >= 0 || lower.indexOf("pronto") >= 0 || lower.indexOf("ready") >= 0 ||
               lower.indexOf("idle") >= 0 || lower.indexOf("run") >= 0) {
        logicalColor = ST77XX_GREEN;
    }
    const uint16_t color = toPanelColor(logicalColor);

    // Try the bigger size first; fall back to size 1, then truncate with "..."
    // if the state name is still too wide for the double tile.
    const Rect area = valueArea(stateTile_);
    uint8_t size = 2;
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t tw = 0;
    uint16_t th = 0;
    tft_->setTextSize(size);
    tft_->getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);

    if (tw > static_cast<uint16_t>(area.w)) {
        size = 1;
        tft_->setTextSize(size);
        tft_->getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);

        if (tw > static_cast<uint16_t>(area.w)) {
            const int16_t charWidth = 6;
            const int16_t maxChars = (area.w > 0) ? static_cast<int16_t>(area.w / charWidth) : 0;
            if (maxChars > 3 && text.length() > static_cast<unsigned>(maxChars)) {
                text = text.substring(0, maxChars - 3) + "...";
            } else if (maxChars > 0) {
                text = text.substring(0, maxChars);
            }
        }
    }

    drawCenteredValue(stateTile_, text, color, size);
}

void LcdDashboard::tick() {
    if (!ready_) {
        return;
    }

    const uint32_t now = millis();
    if (static_cast<uint32_t>(now - lastRefreshMs_) < REFRESH_INTERVAL_MS) {
        return;
    }
    lastRefreshMs_ = now;

    refreshClock();
    refreshLink();
    refreshDropped();
    refreshRxTile(now);
    refreshTxTile(now);
}

void LcdDashboard::refreshClock() {
    String text = "--:--";
    const time_t nowEpoch = time(nullptr);
    if (nowEpoch > 0) {
        std::tm localTime = {};
        if (localtime_r(&nowEpoch, &localTime) != nullptr) {
            char buf[8] = {0};
            std::strftime(buf, sizeof(buf), "%H:%M", &localTime);
            text = buf;
        }
    }

    if (text == lastDrawnClock_) {
        return;
    }
    lastDrawnClock_ = text;

    tft_->fillRect(clockRect_.x, clockRect_.y, clockRect_.w, clockRect_.h, kRawWhite);
    tft_->setTextSize(1);
    tft_->setCursor(static_cast<int16_t>(clockRect_.x + 1), static_cast<int16_t>(clockRect_.y + (clockRect_.h - 8) / 2));
    tft_->setTextColor(kRawBlack, kRawWhite);
    tft_->print(text);
}

void LcdDashboard::refreshLink() {
    const bool hasResult = hasHeartbeatResult_;
    const bool ok = heartbeatOk_;

    if (linkCacheValid_ && linkCacheHasResult_ == hasResult && linkCacheOk_ == ok) {
        return;
    }
    linkCacheValid_ = true;
    linkCacheHasResult_ = hasResult;
    linkCacheOk_ = ok;

    // No result yet (no peer heard from since boot): neutral. Otherwise a
    // steady green/red reflecting the latest heartbeat probe outcome.
    const uint16_t logicalColor = !hasResult ? kNeutralGray565 : (ok ? ST77XX_GREEN : ST77XX_RED);
    drawActivityDot(linkTile_, toPanelColor(logicalColor));
}

void LcdDashboard::refreshDropped() {
    const uint32_t total = droppedTotal_;
    if (errCacheValid_ && errCacheTotal_ == total) {
        return;
    }
    errCacheValid_ = true;
    errCacheTotal_ = total;

    char buf[12] = {0};
    std::snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(total));
    const uint16_t color = toPanelColor((total > 0) ? ST77XX_RED : kNeutralGray565);
    drawCenteredValue(errTile_, buf, color, 2);
}

void LcdDashboard::refreshRxTile(uint32_t now) {
    const bool hot = static_cast<int32_t>(rxPulseUntilMs_ - now) > 0;
    if (hot == rxHotDrawn_) {
        return;
    }
    rxHotDrawn_ = hot;

    const uint16_t color = toPanelColor(hot ? ST77XX_CYAN : kNeutralGray565);
    drawActivityDot(rxTile_, color);
}

void LcdDashboard::refreshTxTile(uint32_t now) {
    const bool hot = static_cast<int32_t>(txPulseUntilMs_ - now) > 0;
    const bool hasResult = txHasResult_;
    const bool ok = txLastOk_;

    if (hot == txHotDrawnHot_ && hasResult == txHotDrawnHasResult_ && ok == txHotDrawnOk_) {
        return;
    }
    txHotDrawnHot_ = hot;
    txHotDrawnHasResult_ = hasResult;
    txHotDrawnOk_ = ok;

    uint16_t logicalColor = kNeutralGray565;
    if (hot) {
        logicalColor = ST77XX_CYAN;
    } else if (hasResult) {
        logicalColor = ok ? ST77XX_GREEN : ST77XX_RED;
    }

    drawActivityDot(txTile_, toPanelColor(logicalColor));
}
