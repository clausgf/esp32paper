/**
 * esp32paper — compile-time board/panel configuration and the runtime
 * AppConfig read from the nice4iot-served config.json.
 *
 * Two boards are supported (see platformio.ini envs); the pin mapping below
 * is picked at compile time from the board's Arduino core define:
 *   - waveshare_esp32_driver: Waveshare "ESP32 e-Paper Driver Board"
 *     (ESP32-WROOM-32). All Waveshare SPI HATs use the same header, so only
 *     the GxEPD2 driver class changes between panel sizes.
 *   - seeed_xiao_esp32s3: Seeed XIAO ESP32-S3 (Plus) on the EE04 ePaper
 *     Display Board (50-pin FPC).
 */

#pragma once

#include <Arduino.h>

// ***************************************************************************
// Pin mapping
// ***************************************************************************
#if defined(ARDUINO_XIAO_ESP32S3)
// Seeed XIAO ESP32-S3 (Plus) on the EE04 ePaper Display Board. Mapping from
// Seeed_GFX (User_Setups/EPaper_Board_Pins_Setups.h,
// USE_XIAO_EPAPER_DISPLAY_BOARD_EE04) / wiki.seeedstudio.com/epaper_ee04 —
// these GPIOs are wired directly on the carrier board, not the 11-pin header.
static const int EPD_SCK  = 7;   // D8
static const int EPD_MISO = -1;  // unused by e-paper; EE04 leaves it open
static const int EPD_MOSI = 9;   // D10
static const int EPD_CS   = 44;  // D7
static const int EPD_DC   = 10;
static const int EPD_RST  = 38;
static const int EPD_BUSY = 4;

// EE04 gates the panel's power rail behind this pin; must be driven HIGH
// before use (see display_renderer.cpp beginPanel_()).
#define EPD_ENABLE_PIN 43

// XIAO ESP32-S3's onboard user LED.
static const int STATUS_LED_PIN = 21;

// EE04's battery ADC (A0/GPIO1) sits behind an enable gate (A5/GPIO6) that
// main.cpp drives HIGH at boot. Divider ratio assumed 2:1 (unconfirmed —
// TODO: measure on real hardware and correct BATTERY_FACTOR).
static const int BATTERY_PIN      = 1;
#define BATTERY_ADC_ENABLE_PIN 6
static const int BATTERY_FACTOR   = 2000; // divider ratio * 1000 (assumed 2:1 -> 2000)
static const int BATTERY_DIVIDER  = 1000;
static const int BATTERY_OFFSET_MV = 0;
static const int BATTERY_MIN_MV   = 3300; // undervoltage shutdown threshold
#else
// Waveshare "ESP32 e-Paper Driver Board" (identical to the reference
// epaper-esp32 firmware; do not change unless you wire a different board).
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
#endif

// ***************************************************************************
// Panel support (opt-in list via PANEL_* build_flags in platformio.ini)
// ***************************************************************************
// Define any subset of these; ALL enabled drivers are compiled in and the
// active panel is chosen at runtime (config.json "panel" > NVS > default). Only
// the runtime-selected panel allocates its page buffer, so unused ones cost
// flash but no RAM. See src/panels.h for the registry / colour models.
//
//   flag                   id "panel"         panel                        color_model
//   PANEL_GDEW042T2        GDEW042T2          4.2"  400x300 b/w            bw
//   PANEL_GDEW042Z15       GDEW042Z15         4.2"  400x300 b/w/red        bwr
//   PANEL_GDEW075T7        GDEW075T7          7.5"  800x480 b/w            bw
//   PANEL_GDEH075Z90       GDEH075Z90         7.5"  800x480 b/w/red        bwr
//   PANEL_GDEP073E01       GDEP073E01        7.3"  800x480 Spectra 6      e6
//   PANEL_ACEP730          ACeP730            7.3"  800x480 ACeP 7-colour  c7
//   PANEL_GDEY073D46       GDEY073D46         7.3"  800x480 ACeP 7-colour  c7
#if !defined(PANEL_GDEW042T2) && !defined(PANEL_GDEW042Z15) && \
    !defined(PANEL_GDEW075T7) && \
    !defined(PANEL_GDEH075Z90) && !defined(PANEL_GDEP073E01) && \
    !defined(PANEL_ACEP730) && !defined(PANEL_GDEY073D46)
#define PANEL_GDEW042T2 1
#endif

// Panel id used before config.json is available (first boot, pre-config error
// screens). If unset, the first compiled-in panel is used. Best-effort: on a
// device whose real panel differs, an early error screen may render on the
// wrong geometry until config.json (or NVS) supplies the correct panel.
// #define EPAPER_DEFAULT_PANEL "GDEW042T2"

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
//   "fw_check_s"    int    min seconds between OTA firmware checks (0 = every wake)
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
    int    fwCheck_s    = 24 * 3600;  // min interval between OTA firmware checks
};
