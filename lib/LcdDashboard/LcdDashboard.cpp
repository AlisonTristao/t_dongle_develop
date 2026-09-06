#include "LcdDashboard.h"

#include <cstdio>
#include <cstring>

#include "../../include/config.h"

namespace {

constexpr uint16_t kRawWhite = ST77XX_WHITE;
constexpr uint16_t kRawBlack = ST77XX_BLACK;
constexpr uint16_t kRawGridLine = 0xC618; // light gray, structural chrome (not panel-corrected)
constexpr uint16_t kNeutralGray565 = 0x8410;
constexpr int16_t kBarHeight = 16;
constexpr int16_t kPageIndicatorWidth = 30;
constexpr int16_t kStateStripHeight = 12;
constexpr int16_t kTextLineHeight = 10;

} // namespace

LcdDashboard::LcdDashboard()
    : tft_(nullptr),
      ready_(false),
      screenW_(0),
      screenH_(0),
      messageRect_{0, 0, 0, 0},
      pageIndicatorRect_{0, 0, 0, 0},
      stateRect_{0, 0, 0, 0},
      contentRect_{0, 0, 0, 0},
      rxTile_{0, 0, 0, 0},
      txTile_{0, 0, 0, 0},
      peersTile_{0, 0, 0, 0},
      lastRefreshMs_(0),
      currentPage_(Page::kActivity),
      buttonPressedLast_(false),
      messageExpireMs_(0),
      messageActive_(false),
      rxPulseUntilMs_(0),
      rxCacheValid_(false),
      rxHotDrawn_(false),
      txPulseUntilMs_(0),
      txHasResult_(false),
      txLastOk_(false),
      txCacheValid_(false),
      txHotDrawnHot_(false),
      txHotDrawnHasResult_(false),
      txHotDrawnOk_(false),
      peerRows_{},
      peerRowCount_(0),
      peerTotalKnown_(0),
      peerOnlineCount_(0),
      peersTileCacheValid_(false),
      peersTileDrawnOnline_(0),
      peersTileDrawnTotal_(0),
      peersPageCacheValid_(false),
      peersPageDrawnRows_{},
      peersPageDrawnRowCount_(0),
      peersPageDrawnTotalKnown_(0),
      errLive_{0, 0, 0, 0, 0, 0},
      errCacheValid_(false),
      errDrawn_{0, 0, 0, 0, 0, 0},
      sessionLive_{false, false, false, 0, 0, false},
      sessionCacheValid_(false),
      sessionDrawn_{false, false, false, 0, 0, false} {
}

bool LcdDashboard::begin(DonglePeripherals& peripherals) {
    tft_ = peripherals.lcd();
    if (tft_ == nullptr) {
        ready_ = false;
        return false;
    }

    peripherals.setLcdBacklight(true);

    layoutChrome();
    currentPage_ = Page::kActivity;
    // A flash cycle holds BOOT down to enter the bootloader; sample the real
    // level here instead of assuming "released", so that release doesn't
    // read as a spurious first press once the app starts polling.
    buttonPressedLast_ = (digitalRead(BoardConfig::PIN_BOOT_BUTTON) == LOW);
    resetCaches();

    tft_->fillScreen(kRawWhite);
    tft_->drawFastHLine(0, kBarHeight, screenW_, kRawGridLine);
    tft_->drawFastHLine(0, static_cast<int16_t>(stateRect_.y + stateRect_.h), screenW_, kRawGridLine);
    drawDefaultState();
    drawActivityChrome();
    drawPageIndicator();

    ready_ = true;
    lastRefreshMs_ = millis() - REFRESH_INTERVAL_MS;
    return true;
}

void LcdDashboard::clear() {
    if (!ready_ || tft_ == nullptr) {
        return;
    }

    layoutChrome();
    resetCaches();

    tft_->fillScreen(kRawWhite);
    tft_->drawFastHLine(0, kBarHeight, screenW_, kRawGridLine);
    tft_->drawFastHLine(0, static_cast<int16_t>(stateRect_.y + stateRect_.h), screenW_, kRawGridLine);
    drawDefaultState();
    if (currentPage_ == Page::kActivity) {
        drawActivityChrome();
    }
    drawPageIndicator();
}

bool LcdDashboard::isReady() const {
    return ready_;
}

void LcdDashboard::layoutChrome() {
    screenW_ = tft_->width();
    screenH_ = tft_->height();

    pageIndicatorRect_ = {static_cast<int16_t>(screenW_ - kPageIndicatorWidth), 0, kPageIndicatorWidth, kBarHeight};
    messageRect_ = {0, 0, static_cast<int16_t>(screenW_ - kPageIndicatorWidth), kBarHeight};

    const int16_t stateY = static_cast<int16_t>(kBarHeight + 1);
    stateRect_ = {0, stateY, screenW_, kStateStripHeight};

    const int16_t contentY = static_cast<int16_t>(stateRect_.y + stateRect_.h + 1);
    contentRect_ = {0, contentY, screenW_, static_cast<int16_t>(screenH_ - contentY)};

    const int16_t colW = static_cast<int16_t>(contentRect_.w / 3);
    const int16_t lastColW = static_cast<int16_t>(contentRect_.w - (colW * 2));
    rxTile_ = {contentRect_.x, contentRect_.y, colW, contentRect_.h};
    txTile_ = {static_cast<int16_t>(contentRect_.x + colW), contentRect_.y, colW, contentRect_.h};
    peersTile_ = {static_cast<int16_t>(contentRect_.x + colW * 2), contentRect_.y, lastColW, contentRect_.h};
}

void LcdDashboard::resetCaches() {
    // Forces the next tick()/showMessage() to repaint from current values,
    // without touching running accumulators (peer table, error counters,
    // session/storage status, pulse timers) -- a clear() shouldn't erase
    // history, just the pixels.
    messageActive_ = false;
    rxCacheValid_ = false;
    txCacheValid_ = false;
    peersTileCacheValid_ = false;
    peersPageCacheValid_ = false;
    errCacheValid_ = false;
    sessionCacheValid_ = false;
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

void LcdDashboard::drawActivityDot(const Rect& tile, const char* label, uint16_t color) {
    // Sized off the sub-area below the label (so it never grows into the
    // label text). Centered on the whole tile when the label's footprint
    // (drawLabel: tile.x+3, tile.y+2, ~6px/char, ~8px tall) doesn't reach
    // that far, so it reads as centered in the grid cell; otherwise it
    // falls back to sitting below the label, like before, so the two never
    // overlap (e.g. "PEERS" is wide enough to reach under a fully-centered
    // dot in its narrow tile).
    const Rect area = valueArea(tile);
    const int16_t shorterSide = (area.w < area.h) ? area.w : area.h;
    int16_t radius = static_cast<int16_t>(shorterSide / 2 - 2);
    if (radius < 2) {
        radius = 2;
    }

    const int16_t cx = static_cast<int16_t>(tile.x + tile.w / 2);
    const int16_t labelRight = static_cast<int16_t>(tile.x + 3 + static_cast<int16_t>(std::strlen(label)) * 6);
    const int16_t labelBottom = static_cast<int16_t>(tile.y + 2 + 8);

    int16_t cy = static_cast<int16_t>(tile.y + tile.h / 2);
    if (labelRight > cx - radius) {
        cy = static_cast<int16_t>(labelBottom + radius + 2);
    }

    const int16_t clearSide = static_cast<int16_t>(radius * 2 + 2);
    const int16_t clearX = static_cast<int16_t>(cx - radius - 1);
    const int16_t clearY = static_cast<int16_t>(cy - radius - 1);
    tft_->fillRect(clearX, clearY, clearSide, clearSide, kRawWhite);

    tft_->fillCircle(cx, cy, radius, color);
}

void LcdDashboard::drawTextLine(int16_t x, int16_t y, const String& text, uint16_t color) {
    tft_->setTextSize(1);
    tft_->setCursor(x, y);
    tft_->setTextColor(color, kRawWhite);
    tft_->print(text);
}

void LcdDashboard::drawPageIndicator() {
    tft_->fillRect(pageIndicatorRect_.x, pageIndicatorRect_.y, pageIndicatorRect_.w, pageIndicatorRect_.h, kRawWhite);

    char buf[8] = {0};
    std::snprintf(buf, sizeof(buf), "%u/%u", static_cast<unsigned>(currentPage_) + 1U,
                  static_cast<unsigned>(Page::kCount));

    drawTextLine(static_cast<int16_t>(pageIndicatorRect_.x + 2),
                 static_cast<int16_t>(pageIndicatorRect_.y + (pageIndicatorRect_.h - 8) / 2), buf, kRawBlack);
}

void LcdDashboard::drawDefaultState() {
    tft_->fillRect(stateRect_.x, stateRect_.y, stateRect_.w, stateRect_.h, kRawWhite);
    drawTextLine(static_cast<int16_t>(stateRect_.x + 2),
                 static_cast<int16_t>(stateRect_.y + (stateRect_.h - 8) / 2),
                 "estado: --", toPanelColor(kNeutralGray565));
}

void LcdDashboard::drawActivityChrome() {
    tft_->drawFastVLine(txTile_.x, contentRect_.y, contentRect_.h, kRawGridLine);
    tft_->drawFastVLine(peersTile_.x, contentRect_.y, contentRect_.h, kRawGridLine);

    drawLabel(rxTile_, "RX");
    drawLabel(txTile_, "TX");
    drawLabel(peersTile_, "PEERS");
}

void LcdDashboard::pollButton() {
    const bool pressed = (digitalRead(BoardConfig::PIN_BOOT_BUTTON) == LOW);
    // tick() only samples this once per REFRESH_INTERVAL_MS (150ms), well
    // past any mechanical bounce, so a simple level compare across samples
    // is enough debounce -- no separate timer needed. Advances on the press
    // edge only; holding the button down doesn't keep cycling pages.
    if (pressed && !buttonPressedLast_) {
        const uint8_t next = static_cast<uint8_t>(
            (static_cast<uint8_t>(currentPage_) + 1U) % static_cast<uint8_t>(Page::kCount));
        switchToPage(static_cast<Page>(next));
    }
    buttonPressedLast_ = pressed;
}

void LcdDashboard::switchToPage(Page page) {
    currentPage_ = page;
    tft_->fillRect(contentRect_.x, contentRect_.y, contentRect_.w, contentRect_.h, kRawWhite);

    // Every page's cache is invalidated on every switch: only the page that
    // becomes active runs its refresh*() next tick, so this just guarantees
    // that one does a full repaint onto the blank content area above,
    // regardless of whether its live values happen to match what was drawn
    // the last time it was on screen.
    rxCacheValid_ = false;
    txCacheValid_ = false;
    peersTileCacheValid_ = false;
    peersPageCacheValid_ = false;
    errCacheValid_ = false;
    sessionCacheValid_ = false;

    if (page == Page::kActivity) {
        drawActivityChrome();
    }
    drawPageIndicator();
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

    messageExpireMs_ = millis() + MESSAGE_HOLD_MS;
    messageActive_ = true;
}

void LcdDashboard::notifyRx() {
    rxPulseUntilMs_ = millis() + PULSE_HOLD_MS;
}

void LcdDashboard::notifyTx(bool delivered) {
    txPulseUntilMs_ = millis() + PULSE_HOLD_MS;
    txLastOk_ = delivered;
    txHasResult_ = true;
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

    // The strip is one line tall now (it used to be the biggest tile on the
    // grid) -- try size 1 first and truncate with "..." if the state name is
    // still too wide for it.
    tft_->fillRect(stateRect_.x, stateRect_.y, stateRect_.w, stateRect_.h, kRawWhite);

    const int16_t charWidth = 6;
    const int16_t maxChars = (stateRect_.w > 4) ? static_cast<int16_t>((stateRect_.w - 4) / charWidth) : 0;
    if (maxChars > 3 && text.length() > static_cast<unsigned>(maxChars)) {
        text = text.substring(0, maxChars - 3) + "...";
    } else if (maxChars > 0 && text.length() > static_cast<unsigned>(maxChars)) {
        text = text.substring(0, maxChars);
    }

    drawTextLine(static_cast<int16_t>(stateRect_.x + 2),
                 static_cast<int16_t>(stateRect_.y + (stateRect_.h - 8) / 2), text, color);
}

void LcdDashboard::notifyPeers(const PeerRow* rows, size_t rowCount, size_t totalKnown) {
    const size_t cappedCount = (rowCount > MAX_DISPLAYED_PEERS) ? MAX_DISPLAYED_PEERS : rowCount;
    peerRowCount_ = cappedCount;
    for (size_t i = 0; i < cappedCount; ++i) {
        peerRows_[i] = rows[i];
    }

    peerTotalKnown_ = totalKnown;
    size_t online = 0;
    for (size_t i = 0; i < cappedCount; ++i) {
        if (peerRows_[i].online) {
            ++online;
        }
    }
    peerOnlineCount_ = online;
}

void LcdDashboard::notifyErrorCounters(uint32_t droppedRx, uint32_t droppedDecode, uint32_t droppedCrc,
                                       uint32_t droppedReassembly, uint32_t droppedQueueFull, uint32_t droppedAuth) {
    errLive_.droppedRx = droppedRx;
    errLive_.droppedDecode = droppedDecode;
    errLive_.droppedCrc = droppedCrc;
    errLive_.droppedReassembly = droppedReassembly;
    errLive_.droppedQueueFull = droppedQueueFull;
    errLive_.droppedAuth = droppedAuth;
}

void LcdDashboard::notifySessionStatus(bool protocolled, bool consoleOwned) {
    sessionLive_.protocolled = protocolled;
    sessionLive_.consoleOwned = consoleOwned;
}

void LcdDashboard::notifyStorageStatus(bool sdReady, uint64_t sdUsedMB, uint64_t sdTotalMB, bool dbReady) {
    sessionLive_.sdReady = sdReady;
    sessionLive_.sdUsedMB = sdUsedMB;
    sessionLive_.sdTotalMB = sdTotalMB;
    sessionLive_.dbReady = dbReady;
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

    pollButton();

    switch (currentPage_) {
        case Page::kActivity:
            refreshRxTile(now);
            refreshTxTile(now);
            refreshPeersTile();
            break;
        case Page::kErrors:
            refreshErrorsPage();
            break;
        case Page::kSession:
            refreshSessionPage();
            break;
        case Page::kPeers:
            refreshPeersPage();
            break;
        default:
            break;
    }

    refreshMessageExpiry(now);
}

void LcdDashboard::refreshMessageExpiry(uint32_t now) {
    if (!messageActive_) {
        return;
    }
    if (static_cast<int32_t>(now - messageExpireMs_) < 0) {
        return;
    }
    messageActive_ = false;

    tft_->fillRect(messageRect_.x, messageRect_.y, messageRect_.w, messageRect_.h, kRawWhite);
}

void LcdDashboard::refreshRxTile(uint32_t now) {
    const bool hot = static_cast<int32_t>(rxPulseUntilMs_ - now) > 0;
    if (rxCacheValid_ && hot == rxHotDrawn_) {
        return;
    }
    rxCacheValid_ = true;
    rxHotDrawn_ = hot;

    const uint16_t color = toPanelColor(hot ? ST77XX_CYAN : kNeutralGray565);
    drawActivityDot(rxTile_, "RX", color);
}

void LcdDashboard::refreshTxTile(uint32_t now) {
    const bool hot = static_cast<int32_t>(txPulseUntilMs_ - now) > 0;
    const bool hasResult = txHasResult_;
    const bool ok = txLastOk_;

    if (txCacheValid_ && hot == txHotDrawnHot_ && hasResult == txHotDrawnHasResult_ && ok == txHotDrawnOk_) {
        return;
    }
    txCacheValid_ = true;
    txHotDrawnHot_ = hot;
    txHotDrawnHasResult_ = hasResult;
    txHotDrawnOk_ = ok;

    uint16_t logicalColor = kNeutralGray565;
    if (hot) {
        logicalColor = ST77XX_CYAN;
    } else if (hasResult) {
        logicalColor = ok ? ST77XX_GREEN : ST77XX_RED;
    }

    drawActivityDot(txTile_, "TX", toPanelColor(logicalColor));
}

void LcdDashboard::refreshPeersTile() {
    const size_t online = peerOnlineCount_;
    const size_t total = peerTotalKnown_;
    if (peersTileCacheValid_ && online == peersTileDrawnOnline_ && total == peersTileDrawnTotal_) {
        return;
    }
    peersTileCacheValid_ = true;
    peersTileDrawnOnline_ = online;
    peersTileDrawnTotal_ = total;

    char buf[12] = {0};
    std::snprintf(buf, sizeof(buf), "%u/%u", static_cast<unsigned>(online), static_cast<unsigned>(total));

    uint16_t logicalColor = kNeutralGray565;
    if (total > 0) {
        logicalColor = (online == 0) ? ST77XX_RED : ((online < total) ? ST77XX_YELLOW : ST77XX_GREEN);
    }

    drawCenteredValue(peersTile_, buf, toPanelColor(logicalColor), 2);
}

void LcdDashboard::refreshErrorsPage() {
    const ErrorCounters live = errLive_;
    if (errCacheValid_ && std::memcmp(&live, &errDrawn_, sizeof(ErrorCounters)) == 0) {
        return;
    }
    errCacheValid_ = true;
    errDrawn_ = live;

    tft_->fillRect(contentRect_.x, contentRect_.y, contentRect_.w, contentRect_.h, kRawWhite);

    const uint32_t total = live.droppedRx + live.droppedDecode + live.droppedCrc + live.droppedReassembly +
                            live.droppedQueueFull + live.droppedAuth;
    const uint16_t color = toPanelColor((total > 0) ? ST77XX_RED : kNeutralGray565);

    char line1[24] = {0};
    char line2[24] = {0};
    char line3[24] = {0};
    std::snprintf(line1, sizeof(line1), "RX:%lu  DEC:%lu",
                  static_cast<unsigned long>(live.droppedRx), static_cast<unsigned long>(live.droppedDecode));
    std::snprintf(line2, sizeof(line2), "CRC:%lu  REASM:%lu",
                  static_cast<unsigned long>(live.droppedCrc), static_cast<unsigned long>(live.droppedReassembly));
    std::snprintf(line3, sizeof(line3), "Q:%lu  AUTH:%lu",
                  static_cast<unsigned long>(live.droppedQueueFull), static_cast<unsigned long>(live.droppedAuth));

    const int16_t x = static_cast<int16_t>(contentRect_.x + 2);
    int16_t y = static_cast<int16_t>(contentRect_.y + 2);
    drawTextLine(x, y, line1, color);
    y = static_cast<int16_t>(y + kTextLineHeight);
    drawTextLine(x, y, line2, color);
    y = static_cast<int16_t>(y + kTextLineHeight);
    drawTextLine(x, y, line3, color);
}

void LcdDashboard::refreshSessionPage() {
    const SessionStatus live = sessionLive_;
    // Field-by-field (not memcmp): the bool/uint64_t mix leaves compiler-
    // dependent padding between members, which memcmp would compare too.
    if (sessionCacheValid_ &&
        live.protocolled == sessionDrawn_.protocolled &&
        live.consoleOwned == sessionDrawn_.consoleOwned &&
        live.sdReady == sessionDrawn_.sdReady &&
        live.sdUsedMB == sessionDrawn_.sdUsedMB &&
        live.sdTotalMB == sessionDrawn_.sdTotalMB &&
        live.dbReady == sessionDrawn_.dbReady) {
        return;
    }
    sessionCacheValid_ = true;
    sessionDrawn_ = live;

    tft_->fillRect(contentRect_.x, contentRect_.y, contentRect_.w, contentRect_.h, kRawWhite);

    const int16_t x = static_cast<int16_t>(contentRect_.x + 2);
    int16_t y = static_cast<int16_t>(contentRect_.y + 2);

    const char* btpLabel = live.consoleOwned ? "Console" : (live.protocolled ? "Protocolado" : "Aguardando");
    const uint16_t btpColor = live.consoleOwned ? kNeutralGray565 : (live.protocolled ? ST77XX_GREEN : ST77XX_YELLOW);
    char line1[24] = {0};
    std::snprintf(line1, sizeof(line1), "BTP: %s", btpLabel);
    drawTextLine(x, y, line1, toPanelColor(btpColor));
    y = static_cast<int16_t>(y + kTextLineHeight);

    char line2[24] = {0};
    if (live.sdReady) {
        std::snprintf(line2, sizeof(line2), "SD %lu/%luMB",
                      static_cast<unsigned long>(live.sdUsedMB), static_cast<unsigned long>(live.sdTotalMB));
    } else {
        std::snprintf(line2, sizeof(line2), "SD ausente");
    }
    drawTextLine(x, y, line2, toPanelColor(live.sdReady ? ST77XX_GREEN : kNeutralGray565));
    y = static_cast<int16_t>(y + kTextLineHeight);

    const char* dbLabel = live.dbReady ? "DB pronto" : "DB indisponivel";
    drawTextLine(x, y, dbLabel, toPanelColor(live.dbReady ? ST77XX_GREEN : kNeutralGray565));
}

void LcdDashboard::refreshPeersPage() {
    const size_t rowCount = peerRowCount_;
    const size_t totalKnown = peerTotalKnown_;
    bool sameRows = peersPageCacheValid_ && rowCount == peersPageDrawnRowCount_ &&
                     totalKnown == peersPageDrawnTotalKnown_;
    if (sameRows) {
        // Age is compared in whole seconds (its display resolution): the
        // millisecond value that feeds notifyPeers() changes on essentially
        // every call, and redrawing on every such change would flicker the
        // page for no visible difference.
        for (size_t i = 0; i < rowCount; ++i) {
            if (peersPageDrawnRows_[i].sourceId != peerRows_[i].sourceId ||
                (peersPageDrawnRows_[i].lastSeenAgeMs / 1000U) != (peerRows_[i].lastSeenAgeMs / 1000U) ||
                peersPageDrawnRows_[i].online != peerRows_[i].online) {
                sameRows = false;
                break;
            }
        }
    }
    if (sameRows) {
        return;
    }
    peersPageCacheValid_ = true;
    peersPageDrawnRowCount_ = rowCount;
    peersPageDrawnTotalKnown_ = totalKnown;
    for (size_t i = 0; i < rowCount; ++i) {
        peersPageDrawnRows_[i] = peerRows_[i];
    }

    tft_->fillRect(contentRect_.x, contentRect_.y, contentRect_.w, contentRect_.h, kRawWhite);

    const int16_t x = static_cast<int16_t>(contentRect_.x + 2);
    int16_t y = static_cast<int16_t>(contentRect_.y + 2);

    if (rowCount == 0) {
        drawTextLine(x, y, "sem peers", toPanelColor(kNeutralGray565));
        return;
    }

    for (size_t i = 0; i < rowCount; ++i) {
        char line[24] = {0};
        std::snprintf(line, sizeof(line), "%04lX  %lus",
                      static_cast<unsigned long>(peerRows_[i].sourceId & 0xFFFFUL),
                      static_cast<unsigned long>(peerRows_[i].lastSeenAgeMs / 1000U));
        const uint16_t color = toPanelColor(peerRows_[i].online ? ST77XX_GREEN : kNeutralGray565);
        drawTextLine(x, y, line, color);
        y = static_cast<int16_t>(y + kTextLineHeight);
    }

    if (totalKnown > rowCount) {
        char more[24] = {0};
        std::snprintf(more, sizeof(more), "+%lu mais", static_cast<unsigned long>(totalKnown - rowCount));
        drawTextLine(x, y, more, toPanelColor(kNeutralGray565));
    }
}
