/**
 * esp32paper — compile-time board/panel configuration.
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
static const bool STATUS_LED_ON = true;   // pin level that lights the LED

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
static const bool STATUS_LED_ON = false;  // pin level that lights the LED

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
// active panel is chosen at runtime (config.json "panel", mirrored to NVS, else
// the default below). Only the runtime-selected panel allocates its page
// buffer, so unused ones cost flash but no RAM. See src/panels.h for the
// registry / colour models.
//
//   flag                   id "panel"         panel                        color_model
//   PANEL_GDEW042Z15       GDEW042Z15         4.2"  400x300 b/w/red        bwr
//   PANEL_GDEW075T7        GDEW075T7          7.5"  800x480 b/w            bw
//   PANEL_GDEY075Z08       GDEY075Z08         7.5"  800x480 b/w/red        bwr
//   PANEL_GDEP073E01       GDEP073E01        7.3"  800x480 Spectra 6      e6
//   PANEL_ACEP730          ACeP730            7.3"  800x480 ACeP 7-colour  c7
//   PANEL_GDEY073D46       GDEY073D46         7.3"  800x480 ACeP 7-colour  c7
#if !defined(PANEL_GDEW042Z15) && !defined(PANEL_GDEW075T7) && \
    !defined(PANEL_GDEY075Z08) && !defined(PANEL_GDEP073E01) && \
    !defined(PANEL_ACEP730) && !defined(PANEL_GDEY073D46)
#define PANEL_GDEW075T7 1
#endif

// Panel id used before config.json is available (first boot, pre-config error
// screens); also the "panel" default in config.schema.json. If unset, the first
// compiled-in panel is used. Best-effort: on a device whose real panel differs,
// an early error screen may render on the wrong geometry until config.json
// supplies the correct panel.
// #define EPAPER_DEFAULT_PANEL "GDEW075T7"

// GxEPD2 page-buffer byte budget (set per board in platformio.ini): the
// per-panel page height is derived from it, capped at the full frame. Each
// page re-decodes the PNG up to its last row, so larger is faster. Rendering
// runs with WiFi off, so the budget may use most of the free heap as long as
// the PNG decoder (~36 KB) still fits; with PSRAM the full frame fits.
#ifndef EPAPER_PAGE_BYTES
#define EPAPER_PAGE_BYTES 16384
#endif

// ***************************************************************************
// Runtime configuration
// ***************************************************************************
// The config.json keys (panel, image_path, min_sleep_s, max_sleep_s,
// fw_check_s) are declared with defaults, limits and
// descriptions as IotConfigValue in main.cpp; arduino4iot publishes the
// matching config.schema.json. See README 'Runtime config'.
