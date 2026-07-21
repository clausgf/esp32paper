/**
 * esp32paper — compile-time board/panel configuration and the runtime
 * AppConfig read from the nice4iot-served config.json.
 *
 * The pin mapping matches the Waveshare "ESP32 e-Paper Driver Board"
 * (ESP32-WROOM-32). All Waveshare SPI HATs use the same header, so only
 * the GxEPD2 driver class below changes between panel sizes.
 */

#pragma once

#include <Arduino.h>

// ***************************************************************************
// Pin mapping — Waveshare ESP32 e-Paper Driver Board
// ***************************************************************************
// (Identical to the reference epaper-esp32 firmware; do not change unless
//  you wire a different board.)
static const int EPD_SCK  = 13;  // CLK
static const int EPD_MISO = 16;  // unused by e-paper, kept for SPI bus init
static const int EPD_MOSI = 14;  // DIN
static const int EPD_CS   = 15;  // CS
static const int EPD_DC   = 27;  // DC
static const int EPD_RST  = 26;  // RST
static const int EPD_BUSY = 25;  // BUSY

// On-board status LED (ESP32-WROOM devkit style). Set to -1 if absent.
static const int STATUS_LED_PIN = 2;

// Battery voltage divider on ADC. The Waveshare driver board has no battery
// input by default; these values assume an external 2:1 divider on GPIO34.
// Set BATTERY_PIN to -1 to disable battery monitoring entirely.
static const int BATTERY_PIN      = 34;
static const int BATTERY_FACTOR   = 2000; // divider ratio * 1000 (2:1 -> 2000)
static const int BATTERY_DIVIDER  = 1000;
static const int BATTERY_OFFSET_MV = 0;
static const int BATTERY_MIN_MV   = 3300; // undervoltage shutdown threshold

// ***************************************************************************
// Panel selection (chosen via build_flags in platformio.ini)
// ***************************************************************************
// Exactly one EPAPER_PANEL_* macro must be defined. It selects the GxEPD2
// driver class and the display geometry in display_renderer.cpp:
//   EPAPER_PANEL_42_BW   4.2"  400x300 black/white          (color_model bw)
//   EPAPER_PANEL_75_BW   7.5"  800x480 black/white          (color_model bw)
//   EPAPER_PANEL_75_BWR  7.5"  800x480 black/white/red      (color_model bwr)
//   EPAPER_PANEL_73_E6   7.3"  800x480 Spectra 6 / E6 (6c)  (color_model e6)
#if !defined(EPAPER_PANEL_42_BW) && \
    !defined(EPAPER_PANEL_75_BW) && \
    !defined(EPAPER_PANEL_75_BWR) && \
    !defined(EPAPER_PANEL_73_E6)
#define EPAPER_PANEL_42_BW 1
#endif

// The nicepaper color_model requested via ?color_model=. Should match the
// selected panel (bw for the b/w panels, bwr for the 3-colour panel).
#ifndef EPAPER_COLOR_MODEL
#define EPAPER_COLOR_MODEL "bw"
#endif

// ***************************************************************************
// Runtime configuration (defaults; overridden by nice4iot config.json)
// ***************************************************************************
// config.json is fetched by arduino4iot from
//   file/{project}/{device}/config.json
// Recognised keys (all optional — the defaults below apply when absent):
//
//   "log_level"     int    arduino4iot log level (handled by the library)
//   "sleep_s"       int    fallback deep-sleep duration (handled by library)
//   "color_model"   string nicepaper palette: bw | bwr | gs4 | c7 | e6
//   "image_path"    string API path template for the rendered image
//   "min_sleep_s"   int    lower clamp for the Cache-Control derived sleep
//   "max_sleep_s"   int    upper clamp for the Cache-Control derived sleep
//   "error_retry_s" int    deep-sleep interval after an error screen
//   "rotation"      int    GxEPD2 rotation 0..3
//
struct AppConfig
{
    // API path to the nicepaper image. {project} and {device} are expanded by
    // arduino4iot; nicepaper resolves {device} to a screen via aliases.json.
    String imagePath = "ext/epaper/{project}/screens/{device}/image.png";
    String colorModel = EPAPER_COLOR_MODEL;
    int    minSleep_s   = 300;        //  5 min
    int    maxSleep_s   = 24 * 3600;  // 24 h
    int    errorRetry_s = 900;        // 15 min retry after a failure/error screen
    int    rotation     = 0;
};
