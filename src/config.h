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
// Panel support (opt-in list via build_flags in platformio.ini)
// ***************************************************************************
// Define any subset of these; ALL enabled drivers are compiled in and the
// active panel is chosen at runtime (config.json "panel" > NVS > default). Only
// the runtime-selected panel allocates its page buffer, so unused ones cost
// flash but no RAM. See src/panels.h for the registry / colour models.
//
//   flag                 id "panel"         panel                        color_model
//   EPAPER_PANEL_42_BW   gxepd2_420         4.2"  400x300 b/w            bw
//   EPAPER_PANEL_75_BW   gxepd2_750_t7      7.5"  800x480 b/w            bw
//   EPAPER_PANEL_75_BWR  gxepd2_750c_z90    7.5"  800x480 b/w/red        bwr
//   EPAPER_PANEL_73_E6   gxepd2_073e01      7.3"  800x480 Spectra 6      e6
//   EPAPER_PANEL_73_7C   gxepd2_acep_730    7.3"  800x480 ACeP 7-colour  c7
#if !defined(EPAPER_PANEL_42_BW) && !defined(EPAPER_PANEL_75_BW) && \
    !defined(EPAPER_PANEL_75_BWR) && !defined(EPAPER_PANEL_73_E6) && \
    !defined(EPAPER_PANEL_73_7C)
#define EPAPER_PANEL_42_BW 1
#endif

// Panel id used before config.json is available (first boot, pre-config error
// screens). If unset, the first compiled-in panel is used. Best-effort: on a
// device whose real panel differs, an early error screen may render on the
// wrong geometry until config.json (or NVS) supplies the correct panel.
// #define EPAPER_DEFAULT_PANEL "gxepd2_420"

// GxEPD2 page-buffer byte budget: the per-panel page height is derived from
// this so every panel fits without PSRAM. Raise it for fewer PNG re-decodes
// when the module has spare internal DRAM.
#ifndef EPAPER_PAGE_BYTES
#define EPAPER_PAGE_BYTES 16384
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
//   "panel"         string panel id (see table above); persisted in NVS. The
//                          nicepaper color_model is derived from it.
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
    String panel     = "";            // "" = keep NVS/default panel
    int    minSleep_s   = 300;        //  5 min
    int    maxSleep_s   = 24 * 3600;  // 24 h
    int    errorRetry_s = 900;        // 15 min retry after a failure/error screen
    int    rotation     = 0;
};
