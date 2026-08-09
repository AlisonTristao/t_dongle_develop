#pragma once

#include <Arduino.h>
#include <Adafruit_ST7735.h>

#include <DonglePeripherals.h>

/**
 * @brief Grid-based status dashboard rendered on the ST7735 LCD (160x80).
 *
 * Replaces the old scrolling text terminal: the same text is already
 * available on the serial console and in the database logs, so the small
 * color screen instead shows a quick-glance grid of icons/values (heartbeat
 * link health, ESP-NOW RX/TX activity, dropped-packet counter, last known
 * robot state) plus a one-line message banner and a clock.
 *
 * Layout (top to bottom):
 * - message banner + clock (one row)
 * - top grid row : LINK  | RX | TX
 * - bottom grid row: STATE (double-wide) | ERR
 *
 * Push updates (notifyRx/notifyTx/notifyHeartbeat/notifyDropped/
 * notifyRobotState) are cheap and safe to call from any task (e.g. the
 * ESP-NOW RX/heartbeat worker tasks): they only record a timestamp/value
 * (notifyRobotState draws immediately, same as showMessage, since state
 * changes are rare discrete events rather than a polled value). All other
 * drawing happens in tick(), which should be called often (e.g. every main
 * loop iteration) but throttles itself to REFRESH_INTERVAL_MS internally,
 * redrawing a tile only when its value actually changed.
 */
class LcdDashboard final {
public:
    /** How often tick() is allowed to actually repaint tiles. */
    static constexpr uint32_t REFRESH_INTERVAL_MS = 150;
    /** How long an RX/TX pulse stays "hot" after notifyRx()/notifyTx(). */
    static constexpr uint32_t PULSE_HOLD_MS = 400;
    /** How long showMessage() text stays on screen before auto-clearing. */
    static constexpr uint32_t MESSAGE_HOLD_MS = 2000;

    LcdDashboard();

    /**
     * @brief Attaches to DonglePeripherals' LCD, lays out the grid and draws it.
     * @param peripherals Owns the ST7735 driver and SD status used by tiles.
     */
    bool begin(DonglePeripherals& peripherals);

    /**
     * @brief Redraws chrome from scratch (e.g. after a rotation change).
     * Accumulators (dropped count, last TX outcome) are preserved.
     */
    void clear();

    /**
     * @brief Refreshes time-based tiles and pulse decay. Safe/cheap to call
     * every loop iteration; actual redraws are throttled to REFRESH_INTERVAL_MS.
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

    /** Updates the LINK tile with the latest heartbeat probe outcome. */
    void notifyHeartbeat(bool delivered);

    /** Adds to the running dropped/overwritten ESP-NOW packet counter. */
    void notifyDropped(uint32_t additionalCount);

    /**
     * @brief Shows the robot's last known state in the double-wide STATE
     * tile. Draws immediately. Color is derived from keywords in the text
     * (erro/error/fault -> red, warn/aviso -> yellow, ok/pronto/ready/idle/
     * run -> green, otherwise neutral), same heuristic spirit as command
     * feedback coloring elsewhere in the shell.
     */
    void notifyRobotState(const String& state);

    bool isReady() const;

private:
    struct Rect {
        int16_t x;
        int16_t y;
        int16_t w;
        int16_t h;
    };

    Adafruit_ST7735* tft_;
    bool ready_;

    int16_t screenW_;
    int16_t screenH_;
    Rect messageRect_;
    Rect clockRect_;
    Rect linkTile_;
    Rect rxTile_;
    Rect txTile_;
    Rect stateTile_;
    Rect errTile_;

    uint32_t lastRefreshMs_;
    String lastDrawnClock_;

    volatile uint32_t messageExpireMs_;
    volatile bool messageActive_;

    volatile uint32_t rxPulseUntilMs_;
    bool rxHotDrawn_;

    volatile uint32_t txPulseUntilMs_;
    volatile bool txHasResult_;
    volatile bool txLastOk_;
    bool txHotDrawnHot_;
    bool txHotDrawnHasResult_;
    bool txHotDrawnOk_;

    volatile uint32_t droppedTotal_;
    bool errCacheValid_;
    uint32_t errCacheTotal_;

    volatile bool hasHeartbeatResult_;
    volatile bool heartbeatOk_;
    bool linkCacheValid_;
    bool linkCacheHasResult_;
    bool linkCacheOk_;

    void layoutGrid();
    void drawStaticChrome();
    void resetCaches();
    uint16_t toPanelColor(uint16_t color) const;

    void refreshClock();
    void refreshLink();
    void refreshDropped();
    void refreshRxTile(uint32_t now);
    void refreshTxTile(uint32_t now);
    void refreshMessageExpiry(uint32_t now);

    Rect valueArea(const Rect& tile) const;
    void drawLabel(const Rect& tile, const char* label);
    void drawCenteredValue(const Rect& tile, const String& text, uint16_t color, uint8_t textSize);
    void drawActivityDot(const Rect& tile, const char* label, uint16_t color);
};
