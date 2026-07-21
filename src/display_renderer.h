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
    // Panel rotation (GxEPD2 0..3), applied on the next draw.
    void setRotation(int rotation) { _rotation = rotation; }

    // Decode the in-memory PNG and refresh the panel (paged), adding the
    // status overlay. Returns false if the PNG is invalid (panel untouched).
    //
    // The e-paper refresh is slow (seconds) and mostly a BUSY-wait with the CPU
    // idle. `duringRefresh`, if given, is run exactly once during that wait
    // (via GxEPD2's busy callback) so housekeeping/network exchange overlaps the
    // refresh and WiFi can be shut down without adding wall-clock time. If the
    // callback never fires (e.g. no BUSY pin) it is still invoked once before
    // returning. It only runs when rendering actually proceeds (valid PNG).
    bool renderImage(const uint8_t *png, size_t len, const DisplayStatus &status,
                     const std::function<void()> &duringRefresh = {});

    // Full-screen error page (icon + title + word-wrapped message).
    void showError(ErrorIcon icon, const String &title, const String &message,
                   const DisplayStatus &status);

    uint16_t width() const;
    uint16_t height() const;

    // Phase durations of the last renderImage(), in ms:
    //  - decode + transfer of all pages into panel controller memory,
    //  - the physical panel refresh (the BUSY-wait), measured from the first
    //    busy callback until the refresh returns.
    uint32_t lastDecodeTransferMs() const { return _lastDecodeTransferMs; }
    uint32_t lastRefreshMs() const { return _lastRefreshMs; }

private:
    void initPanel_();                           // GxEPD2 init + SPI remap
    void drawOverlay_(const DisplayStatus &st);  // WiFi + battery, top-right
    void drawBattery_(int x, int y, const DisplayStatus &st);
    void drawWifi_(int x, int y, const DisplayStatus &st);

    int _rotation = 0;
    uint32_t _lastDecodeTransferMs = 0;
    uint32_t _lastRefreshMs = 0;
};

extern DisplayRenderer displayRenderer;
