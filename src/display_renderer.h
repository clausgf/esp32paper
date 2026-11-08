/**
 * esp32paper — DisplayRenderer
 *
 * Wraps GxEPD2 and the pngle PNG decoder. The compressed image is fetched
 * whole (via arduino4iot) and kept in RAM; rendering uses GxEPD2's paged
 * refresh, re-decoding the PNG once per page straight into the panel. This
 * avoids allocating a full-screen bitmap, so even large panels fit on an
 * ESP32 without PSRAM (see the memory notes in README.md). On top of the
 * image a phone-like status overlay (WiFi + battery, top-right) is drawn, and
 * serious failures are shown as a full-screen error page.
 *
 * nicepaper server-renders and quantizes the screen to the requested
 * color_model, so the firmware only maps already-quantized pixels to GxEPD
 * colours — no dithering happens on the device.
 */

#pragma once

#include <Arduino.h>
#include <functional>

// Status shown in the top-right overlay and (when known) on error screens.
struct DisplayStatus
{
    bool wifiConnected = false;
    int  rssi          = 0;   // dBm, only meaningful when wifiConnected
    bool batteryValid  = false;
    int  battery_mV    = 0;
    int  batteryPct    = -1;  // 0..100, -1 = unknown
    String firmwareVersion;   // small rotated footer, with the timestamp
    String timestamp;         // when this frame was rendered
};

// Icon shown on a full-screen error page.
enum class ErrorIcon
{
    Warning,  // generic problem (warning triangle)
    NoWifi,   // no network connection (wifi glyph with a slash)
};

class DisplayRenderer
{
public:
    // Select the active panel by id (see src/panels.h). Records the choice;
    // the driver is created lazily on the first draw. An unknown/empty id keeps
    // the previous choice, falling back to the compiled-in default. Must be set
    // before the driver is created (i.e. before the first render/error screen).
    // Returns false if a non-empty id is not compiled in.
    bool setPanel(const String &id);

    // nicepaper color_model of the active panel (bw/bwr/e6/c7).
    String colorModel() const { return _colorModel; }
    // Active panel id, and the comma-separated list of compiled-in panels.
    String activePanel() const { return _panelId; }
    String supportedPanels() const;

    // Cheap pre-check from the PNG header only: is it a PNG, and does its
    // size match the active panel? On false, renderErrorDetail() explains why.
    bool checkImage(const uint8_t *png, size_t len);

    // Decode the in-memory PNG and refresh the panel (paged, or in one pass if
    // EPAPER_PAGE_BYTES holds the full frame), adding the status overlay.
    // Returns false (renderErrorDetail() explains why) if checkImage() fails,
    // the page buffer or the decoder does not fit in memory, or the PNG body
    // is corrupt; the panel is then not refreshed. The CPU light-sleeps
    // through the panel's BUSY-waits (see EPAPER_REFRESH_LIGHTSLEEP), so the
    // caller turns WiFi off before.
    bool renderImage(const uint8_t *png, size_t len, const DisplayStatus &status);

    // Full-screen error page (icon + title + word-wrapped message). The CPU
    // light-sleeps through the refresh, like renderImage(). Shows nothing if
    // the page buffer cannot be allocated.
    void showError(ErrorIcon icon, const String &title, const String &message,
                   const DisplayStatus &status);

    uint16_t width() const;
    uint16_t height() const;

    // Phase durations of the last renderImage(), in ms:
    //  - decode + transfer of all pages into panel controller memory,
    //  - the physical panel refresh incl. power-off, measured from the end of
    //    the last page's drawing until the refresh returns.
    uint32_t lastDecodeTransferMs() const { return _lastDecodeTransferMs; }
    uint32_t lastRefreshMs() const { return _lastRefreshMs; }

    // When checkImage()/renderImage() returned false, why (for the caller's log
    // and error screen): out of memory, a PNG decode error or a size mismatch.
    const String &renderErrorDetail() const { return _renderErrorDetail; }

private:
    bool initPanel_();                           // create driver + SPI + init
    void applyPanel_(const char *id);            // set id/colorModel/colorMode
    void drawOverlay_(const DisplayStatus &st);  // WiFi + battery, top-right
    void drawBattery_(int x, int y, const DisplayStatus &st);
    void drawWifi_(int x, int y, const DisplayStatus &st);

    String _panelId;                 // active panel id ("" until resolved)
    String _colorModel = "bw";       // nicepaper color_model of the active panel
    String _renderErrorDetail;       // reason detail when the last render failed
    uint32_t _lastDecodeTransferMs = 0;
    uint32_t _lastRefreshMs = 0;
};

extern DisplayRenderer displayRenderer;

// One-line memory summary (internal heap free/largest block, PSRAM size/free).
String memorySummary();
