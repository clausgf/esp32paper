/**
 * esp32paper — panel registry.
 *
 * All enabled panels (EPAPER_PANEL_* opt-in flags, see config.h) are compiled
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
#include <epd3c/GxEPD2_750c_Z90.h>
#include <epd7c/GxEPD2_730c_GDEP073E01.h>
#include <epd7c/GxEPD2_730c_ACeP_730.h>

// Colour handling per panel (drives colorForPixel() in display_renderer.cpp).
enum class ColorMode : uint8_t { BW, BWR, E6, C7 };

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
#ifdef EPAPER_PANEL_42_BW
#  define EPD_ROW_42_BW(X) X("gxepd2_420", GxEPD2_BW, GxEPD2_420, 8, "bw", ColorMode::BW)
#else
#  define EPD_ROW_42_BW(X)
#endif
#ifdef EPAPER_PANEL_75_BW
#  define EPD_ROW_75_BW(X) X("gxepd2_750_t7", GxEPD2_BW, GxEPD2_750_T7, 8, "bw", ColorMode::BW)
#else
#  define EPD_ROW_75_BW(X)
#endif
#ifdef EPAPER_PANEL_75_BWR
#  define EPD_ROW_75_BWR(X) X("gxepd2_750c_z90", GxEPD2_3C, GxEPD2_750c_Z90, 4, "bwr", ColorMode::BWR)
#else
#  define EPD_ROW_75_BWR(X)
#endif
#ifdef EPAPER_PANEL_73_E6
#  define EPD_ROW_73_E6(X) X("gxepd2_073e01", GxEPD2_7C, GxEPD2_730c_GDEP073E01, 2, "e6", ColorMode::E6)
#else
#  define EPD_ROW_73_E6(X)
#endif
#ifdef EPAPER_PANEL_73_7C
#  define EPD_ROW_73_7C(X) X("gxepd2_acep_730", GxEPD2_7C, GxEPD2_730c_ACeP_730, 2, "c7", ColorMode::C7)
#else
#  define EPD_ROW_73_7C(X)
#endif

#define EPAPER_FOR_EACH_PANEL(X) \
    EPD_ROW_42_BW(X)  \
    EPD_ROW_75_BW(X)  \
    EPD_ROW_75_BWR(X) \
    EPD_ROW_73_E6(X)  \
    EPD_ROW_73_7C(X)

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
