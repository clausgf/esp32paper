/**
 * esp32paper — panel registry.
 *
 * All enabled panels (PANEL_* opt-in flags, see config.h) are compiled
 * in; the active one is created at runtime via a factory returning the common
 * GxEPD2_GFX base pointer. Because GxEPD2's page buffers are instance members,
 * only the runtime-selected panel allocates RAM (on the heap). The page height
 * per panel is derived from EPAPER_PAGE_BYTES.
 *
 * Adding a panel = one opt-in row below (driver class + colour model + mode).
 */

#pragma once

#include <Arduino.h>
#include "config.h"

#include <GxEPD2_GFX.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <GxEPD2_7C.h>
#include <epd/GxEPD2_420.h>
#include <epd/GxEPD2_750_T7.h>
#include <epd3c/GxEPD2_420c.h>
#include <epd3c/GxEPD2_750c_Z90.h>
#include <epd7c/GxEPD2_730c_GDEP073E01.h>
#include <epd7c/GxEPD2_730c_ACeP_730.h>
#include <epd7c/GxEPD2_730c_GDEY073D46.h>

// Colour handling per panel (drives colorForPixel() in display_renderer.cpp).
enum class ColorMode : uint8_t { BW, BWR, E6, C7 };

// A panel's colour palette. nicepaper pre-quantizes every image to its panel's
// color_model, so mapping a decoded pixel onto the panel is a plain nearest-RGB
// match against that model's palette (see colorForPixel). One table per colour
// model; a panel selects its model via ColorMode. Adding a colour model = add a
// palette here plus a case in epdPalette(); adding a *panel* stays a one-row
// change in the registry above.
struct PaletteEntry { uint8_t r, g, b; uint16_t color; };

static const PaletteEntry EPD_PALETTE_BW[] = {
    {0, 0, 0, GxEPD_BLACK}, {255, 255, 255, GxEPD_WHITE},
};
static const PaletteEntry EPD_PALETTE_BWR[] = {
    {0, 0, 0, GxEPD_BLACK}, {255, 255, 255, GxEPD_WHITE}, {255, 0, 0, GxEPD_RED},
};
// Spectra 6 (E6): 6 colours.
static const PaletteEntry EPD_PALETTE_E6[] = {
    {  0,   0,   0, GxEPD_BLACK }, {255, 255, 255, GxEPD_WHITE },
    {255,   0,   0, GxEPD_RED   }, {255, 255,   0, GxEPD_YELLOW},
    {  0,   0, 255, GxEPD_BLUE  }, {  0, 255,   0, GxEPD_GREEN },
};
// ACeP 7-colour (c7): the six above plus orange.
static const PaletteEntry EPD_PALETTE_C7[] = {
    {  0,   0,   0, GxEPD_BLACK }, {255, 255, 255, GxEPD_WHITE },
    {  0, 255,   0, GxEPD_GREEN }, {  0,   0, 255, GxEPD_BLUE  },
    {255,   0,   0, GxEPD_RED   }, {255, 255,   0, GxEPD_YELLOW},
    {255, 128,   0, GxEPD_ORANGE},
};

// The palette (and its length) for a colour mode.
static const PaletteEntry *epdPalette(ColorMode mode, uint8_t *count)
{
    switch (mode)
    {
    case ColorMode::BWR: *count = 3; return EPD_PALETTE_BWR;
    case ColorMode::E6:  *count = 6; return EPD_PALETTE_E6;
    case ColorMode::C7:  *count = 7; return EPD_PALETTE_C7;
    case ColorMode::BW:
    default:             *count = 2; return EPD_PALETTE_BW;
    }
}

// Page-buffer rows that fit the byte budget, clamped to [1, panel height].
static constexpr uint16_t epdPageRows(int rowBytes, int height)
{
    int rows = EPAPER_PAGE_BYTES / (rowBytes > 0 ? rowBytes : 1);
    if (rows < 1) rows = 1;
    if (rows > height) rows = height;
    return (uint16_t)rows;
}

// --- opt-in rows: X(id, GfxTemplate, DriverClass, rowDivisor, colorModel, ColorMode)
//     rowDivisor = WIDTH bytes/row: 8 (b/w, 1bpp), 4 (b/w/red, 2 planes), 2 (7C, 4bpp)
#ifdef PANEL_GDEW042T2
#  define EPW_ROW_GDEW042T2(X) X("GDEW042T2", GxEPD2_BW, GxEPD2_420, 8, "bw", ColorMode::BW)
#else
#  define EPW_ROW_GDEW042T2(X)
#endif
#ifdef PANEL_GDEW075T7
#  define EPW_ROW_GDEW075T7(X) X("GDEW075T7", GxEPD2_BW, GxEPD2_750_T7, 8, "bw", ColorMode::BW)
#else
#  define EPW_ROW_GDEW075T7(X)
#endif
#ifdef PANEL_GDEH075Z90
#  define EPW_ROW_GDEH075Z90(X) X("GDEH075Z90", GxEPD2_3C, GxEPD2_750c_Z90, 4, "bwr", ColorMode::BWR)
#else
#  define EPW_ROW_GDEH075Z90(X)
#endif
#ifdef PANEL_GDEP073E01
#  define EPW_ROW_GDEP073E01(X) X("GDEP073E01", GxEPD2_7C, GxEPD2_730c_GDEP073E01, 2, "e6", ColorMode::E6)
#else
#  define EPW_ROW_GDEP073E01(X)
#endif
#ifdef PANEL_ACEP730
#  define EPW_ROW_ACEP730(X) X("ACeP730", GxEPD2_7C, GxEPD2_730c_ACeP_730, 2, "c7", ColorMode::C7)
#else
#  define EPW_ROW_ACEP730(X)
#endif
#ifdef PANEL_GDEY073D46
#  define EPW_ROW_GDEY073D46(X) X("GDEY073D46", GxEPD2_7C, GxEPD2_730c_GDEY073D46, 2, "c7", ColorMode::C7)
#else
#  define EPW_ROW_GDEY073D46(X)
#endif

#define EPAPER_FOR_EACH_PANEL(X) \
    EPW_ROW_GDEW042T2(X)  \
    EPW_ROW_GDEW075T7(X)  \
    EPW_ROW_GDEH075Z90(X) \
    EPW_ROW_GDEP073E01(X)  \
    EPW_ROW_ACEP730(X) \
    EPW_ROW_GDEY073D46(X)

// --- registry metadata + factory -------------------------------------------
struct PanelInfo
{
    const char *id;
    const char *colorModel;
    ColorMode   colorMode;
    uint16_t    width;
    uint16_t    height;
};

#define EPD_PANEL_INFO(id, gfx, drv, rowdiv, cm, mode) { id, cm, mode, drv::WIDTH, drv::HEIGHT },
static const PanelInfo EPAPER_PANELS[] = { EPAPER_FOR_EACH_PANEL(EPD_PANEL_INFO) };
#undef EPD_PANEL_INFO
static const size_t EPAPER_PANEL_COUNT = sizeof(EPAPER_PANELS) / sizeof(EPAPER_PANELS[0]);

// Create a new display object for `id` (heap; nullptr if id is not compiled in).
static GxEPD2_GFX *epdCreatePanel(const char *id)
{
#define EPD_PANEL_CREATE(pid, gfx, drv, rowdiv, cm, mode)                       \
    if (strcmp(id, pid) == 0)                                                   \
        return new gfx<drv, epdPageRows(drv::WIDTH / (rowdiv), drv::HEIGHT)>(   \
            drv(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
    EPAPER_FOR_EACH_PANEL(EPD_PANEL_CREATE)
#undef EPD_PANEL_CREATE
    return nullptr;
}

// Metadata lookup for `id` (nullptr if unknown / not compiled in).
static const PanelInfo *epdPanelInfo(const char *id)
{
    for (size_t i = 0; i < EPAPER_PANEL_COUNT; i++)
        if (strcmp(EPAPER_PANELS[i].id, id) == 0) return &EPAPER_PANELS[i];
    return nullptr;
}

// Default panel id used before config.json/NVS provide one.
static const char *epdDefaultPanelId()
{
#ifdef EPAPER_DEFAULT_PANEL
    if (epdPanelInfo(EPAPER_DEFAULT_PANEL)) return EPAPER_DEFAULT_PANEL;
#endif
    return EPAPER_PANELS[0].id; // first compiled-in panel
}

// Comma-separated list of compiled-in panel ids (for telemetry).
static String epdSupportedPanelsCsv()
{
    String s;
    for (size_t i = 0; i < EPAPER_PANEL_COUNT; i++)
    {
        if (i) s += ",";
        s += EPAPER_PANELS[i].id;
    }
    return s;
}

// JSON string values for the compiled-in panel ids, without surrounding array
// brackets. Panel ids are registry constants and need no additional escaping.
static String epdSupportedPanelsJsonEnum()
{
    String s;
    for (size_t i = 0; i < EPAPER_PANEL_COUNT; i++)
    {
        if (i) s += ", ";
        s += "\"";
        s += EPAPER_PANELS[i].id;
        s += "\"";
    }
    return s;
}
