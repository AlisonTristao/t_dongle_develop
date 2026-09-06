#pragma once

#include <Arduino.h>
#include <Adafruit_ST7735.h>

#include <DonglePeripherals.h>

/**
 * @brief Paged status dashboard rendered on the ST7735 LCD (160x80).
 *
 * Replaces the old scrolling text terminal (and, later, the old single-grid
 * dashboard): the same text is already available on the serial console and
 * in the database logs, so the small color screen instead shows a
 * quick-glance page of diagnostics, cycled with the board's BOOT button
 * (topico 41 -- the button is otherwise unread at runtime, only used to
 * force the bootloader at flash time).
 *
 * Layout (top to bottom), persistent across every page:
 * - message banner (left) + page indicator "n/N" (right)
 * - STATE strip: last known robot state
 * - one page of content, advanced by a BOOT button press:
 *     1. Activity: RX | TX pulse dots, PEERS (online/total)
 *     2. Errors:   discriminated ESP-NOW drop counters
 *     3. Session:  BTP session state + SD card + database status
 *     4. Peers:    short list of known peers (id, online, last-seen age)
 *
 * Push updates (notifyRx/notifyTx/notifyPeers/notifyErrorCounters/
 * notifySessionStatus/notifyStorageStatus) are cheap and safe to call from
 * any task: they only record the latest value (notifyRobotState/showMessage
 * draw immediately instead, since those are rare discrete events rather than
 * a polled value). All other drawing happens in tick(), which should be
 * called often (e.g. every main loop iteration) but throttles itself to
 * REFRESH_INTERVAL_MS internally, redrawing only what changed -- including
 * the once-per-tick BOOT button sample that drives page navigation.
 */
class LcdDashboard final {
public:
    /** How often tick() is allowed to actually repaint/poll the button. */
    static constexpr uint32_t REFRESH_INTERVAL_MS = 150;
    /** How long an RX/TX pulse stays "hot" after notifyRx()/notifyTx(). */
    static constexpr uint32_t PULSE_HOLD_MS = 400;
    /** How long showMessage() text stays on screen before auto-clearing. */
    static constexpr uint32_t MESSAGE_HOLD_MS = 2000;
    /**
     * @brief Cap on how many peer rows the Peers page lists. One row short of
     * what the content area's line count fits, so a truncated list still has
     * room for a trailing "+N mais" line.
     */
    static constexpr size_t MAX_DISPLAYED_PEERS = 4;

    /** The pages the BOOT button cycles through, in order. */
    enum class Page : uint8_t {
        kActivity = 0,
        kErrors,
        kSession,
        kPeers,
        kCount
    };

    /** One row of the Peers page; also folded into the Activity page's summary. */
    struct PeerRow {
        uint32_t sourceId;
        uint32_t lastSeenAgeMs;
        bool online;
    };

    LcdDashboard();

    /**
     * @brief Attaches to DonglePeripherals' LCD, lays out the chrome and draws it.
     * @param peripherals Owns the ST7735 driver used by every tile.
     */
    bool begin(DonglePeripherals& peripherals);

    /**
     * @brief Redraws chrome from scratch (e.g. after a rotation change).
     * Accumulated values and the current page are preserved -- a clear()
     * shouldn't lose state, just the pixels.
     */
    void clear();

    /**
     * @brief Refreshes time-based tiles, pulse decay and the BOOT button
     * sample. Safe/cheap to call every loop iteration; actual redraws (and
     * the button poll) are throttled to REFRESH_INTERVAL_MS.
     */
    void tick();

    /**
     * @brief Shows one line of free text (from "dongle -lcd" or command
     * feedback) in the message banner. Draws immediately, color as given
     * (no panel-color correction here; callers decide, same as before).
     */
    void showMessage(const String& text, uint16_t color);

    /** Pulses the RX tile; call once per received ESP-NOW message. */
    void notifyRx();

    /** Pulses the TX tile and records the outcome shown once the pulse ends. */
    void notifyTx(bool delivered);

    /**
     * @brief Shows the robot's last known state in the persistent STATE
     * strip. Draws immediately. Color is derived from keywords in the text
     * (erro/error/fault -> red, warn/aviso -> yellow, ok/pronto/ready/idle/
     * run -> green, otherwise neutral), same heuristic spirit as command
     * feedback coloring elsewhere in the shell.
     */
    void notifyRobotState(const String& state);

    /**
     * @brief Latest peer table snapshot: up to MAX_DISPLAYED_PEERS rows for
     * the Peers page, plus the online/total counts folded into the
     * Activity page's PEERS tile. `rowCount` may be less than `totalKnown`
     * when there are more known peers than the page can list.
     */
    void notifyPeers(const PeerRow* rows, size_t rowCount, size_t totalKnown);

    /** Discriminated ESP-NOW drop counters (cumulative totals), for the Errors page. */
    void notifyErrorCounters(uint32_t droppedRx, uint32_t droppedDecode, uint32_t droppedCrc,
                              uint32_t droppedReassembly, uint32_t droppedQueueFull, uint32_t droppedAuth);

    /** BTP session ownership of the USB port, for the Session page. */
    void notifySessionStatus(bool protocolled, bool consoleOwned);

    /** SD card and database readiness, for the Session page. */
    void notifyStorageStatus(bool sdReady, uint64_t sdUsedMB, uint64_t sdTotalMB, bool dbReady);

    bool isReady() const;

private:
    struct Rect {
        int16_t x;
        int16_t y;
        int16_t w;
        int16_t h;
    };

    struct ErrorCounters {
        uint32_t droppedRx;
        uint32_t droppedDecode;
        uint32_t droppedCrc;
        uint32_t droppedReassembly;
        uint32_t droppedQueueFull;
        uint32_t droppedAuth;
    };

    struct SessionStatus {
        bool protocolled;
        bool consoleOwned;
        bool sdReady;
        uint64_t sdUsedMB;
        uint64_t sdTotalMB;
        bool dbReady;
    };

    Adafruit_ST7735* tft_;
    bool ready_;

    int16_t screenW_;
    int16_t screenH_;
    Rect messageRect_;
    Rect pageIndicatorRect_;
    Rect stateRect_;
    Rect contentRect_;

    // Activity page sub-tiles, carved out of contentRect_.
    Rect rxTile_;
    Rect txTile_;
    Rect peersTile_;

    uint32_t lastRefreshMs_;

    Page currentPage_;
    bool buttonPressedLast_;

    volatile uint32_t messageExpireMs_;
    volatile bool messageActive_;

    volatile uint32_t rxPulseUntilMs_;
    bool rxCacheValid_;
    bool rxHotDrawn_;

    volatile uint32_t txPulseUntilMs_;
    volatile bool txHasResult_;
    volatile bool txLastOk_;
    bool txCacheValid_;
    bool txHotDrawnHot_;
    bool txHotDrawnHasResult_;
    bool txHotDrawnOk_;

    // Peers: live values (written by notifyPeers) vs. what's currently on screen.
    // Two independent caches -- the Activity page's PEERS tile (online/total)
    // and the Peers page's row list -- since only one of the two is ever the
    // active page, but both read the same live fields.
    PeerRow peerRows_[MAX_DISPLAYED_PEERS];
    size_t peerRowCount_;
    size_t peerTotalKnown_;
    size_t peerOnlineCount_;

    bool peersTileCacheValid_;
    size_t peersTileDrawnOnline_;
    size_t peersTileDrawnTotal_;

    bool peersPageCacheValid_;
    PeerRow peersPageDrawnRows_[MAX_DISPLAYED_PEERS];
    size_t peersPageDrawnRowCount_;
    size_t peersPageDrawnTotalKnown_;

    ErrorCounters errLive_;
    bool errCacheValid_;
    ErrorCounters errDrawn_;

    SessionStatus sessionLive_;
    bool sessionCacheValid_;
    SessionStatus sessionDrawn_;

    void layoutChrome();
    void resetCaches();
    uint16_t toPanelColor(uint16_t color) const;

    void pollButton();
    void switchToPage(Page page);
    void drawPageIndicator();
    void drawActivityChrome();

    void refreshMessageExpiry(uint32_t now);
    void refreshRxTile(uint32_t now);
    void refreshTxTile(uint32_t now);
    void refreshPeersTile();
    void refreshErrorsPage();
    void refreshSessionPage();
    void refreshPeersPage();

    Rect valueArea(const Rect& tile) const;
    void drawLabel(const Rect& tile, const char* label);
    void drawCenteredValue(const Rect& tile, const String& text, uint16_t color, uint8_t textSize);
    void drawActivityDot(const Rect& tile, const char* label, uint16_t color);
    void drawTextLine(int16_t x, int16_t y, const String& text, uint16_t color);
};
