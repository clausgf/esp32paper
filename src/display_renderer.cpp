/**
 * esp32paper — DisplayRenderer implementation.
 *
 * The GxEPD2 driver class, geometry and page count are selected at compile
 * time from the EPAPER_PANEL_* / EPAPER_PAGE_DIV macros (see platformio.ini
 * and README.md's memory notes). Everything else is panel-agnostic.
 */

#include "display_renderer.h"
#include "config.h"

#include <SPI.h>
#include <limits.h>
#include <math.h>

// --- automatic page height from a RAM budget ------------------------------
// GxEPD2 renders in pages: it keeps a page buffer of `page_height` rows and we
// re-decode the in-RAM PNG once per page. The buffer is a *static* member sized
// by a compile-time template parameter, so it cannot be resized at runtime
// (and PSRAM does not help — it lives in internal DRAM). Instead of hand-picking
// a divisor per panel, we derive `page_height` from a byte budget so every panel
// (incl. the 192 KB 7-colour bitmap) is safe by default. Tune per module RAM
// with -DEPAPER_PAGE_BYTES=N; a runtime free-heap check (see renderImage) guards
// against an over-optimistic budget. A larger budget = fewer re-decodes.
#ifndef EPAPER_PAGE_BYTES
#define EPAPER_PAGE_BYTES 16384
#endif

// Rows that fit in the budget, clamped to [1, panel height].
static constexpr int clampPageRows(int budgetBytes, int rowBytes, int height)
{
    int rows = budgetBytes / (rowBytes > 0 ? rowBytes : 1);
    return rows < 1 ? 1 : (rows > height ? height : rows);
}

#if defined(EPAPER_PANEL_73_E6)
  #include <GxEPD2_7C.h>
  #include <epd7c/GxEPD2_730c_GDEP073E01.h>  // 7.3" 800x480 Spectra 6 (E6)
  #define EPD_7COLOR 1
  using PanelDriver = GxEPD2_730c_GDEP073E01;
  static constexpr int EPD_ROW_BYTES = PanelDriver::WIDTH / 2; // 4 bits/pixel
  static constexpr int EPD_PAGE_H =
      clampPageRows(EPAPER_PAGE_BYTES, EPD_ROW_BYTES, PanelDriver::HEIGHT);
  static GxEPD2_7C<PanelDriver, EPD_PAGE_H> gx(
      PanelDriver(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
#elif defined(EPAPER_PANEL_75_BWR)
  #include <GxEPD2_3C.h>
  #include <epd3c/GxEPD2_750c_Z90.h>  // 7.5" 800x480 black/white/red
  #define EPD_HAS_RED 1
  using PanelDriver = GxEPD2_750c_Z90;
  static constexpr int EPD_ROW_BYTES = PanelDriver::WIDTH / 4; // 2 planes, 1bpp
  static constexpr int EPD_PAGE_H =
      clampPageRows(EPAPER_PAGE_BYTES, EPD_ROW_BYTES, PanelDriver::HEIGHT);
  static GxEPD2_3C<PanelDriver, EPD_PAGE_H> gx(
      PanelDriver(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
#elif defined(EPAPER_PANEL_75_BW)
  #include <GxEPD2_BW.h>
  #include <epd/GxEPD2_750_T7.h>      // 7.5" 800x480 black/white
  using PanelDriver = GxEPD2_750_T7;
  static constexpr int EPD_ROW_BYTES = PanelDriver::WIDTH / 8; // 1 bit/pixel
  static constexpr int EPD_PAGE_H =
      clampPageRows(EPAPER_PAGE_BYTES, EPD_ROW_BYTES, PanelDriver::HEIGHT);
  static GxEPD2_BW<PanelDriver, EPD_PAGE_H> gx(
      PanelDriver(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
#else // EPAPER_PANEL_42_BW (default)
  #include <GxEPD2_BW.h>
  #include <epd/GxEPD2_420.h>         // 4.2" 400x300 black/white
  using PanelDriver = GxEPD2_420;
  static constexpr int EPD_ROW_BYTES = PanelDriver::WIDTH / 8; // 1 bit/pixel
  static constexpr int EPD_PAGE_H =
      clampPageRows(EPAPER_PAGE_BYTES, EPD_ROW_BYTES, PanelDriver::HEIGHT);
  static GxEPD2_BW<PanelDriver, EPD_PAGE_H> gx(
      PanelDriver(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
#endif

#include <pngle.h>

// ***************************************************************************

DisplayRenderer displayRenderer;

uint16_t DisplayRenderer::width() const  { return gx.width(); }
uint16_t DisplayRenderer::height() const { return gx.height(); }

// ***************************************************************************
// PNG decode -> panel
//
// The pixels are drawn straight onto the GxEPD2 page via drawPixel(), which
// ignores pixels outside the current page band — so re-decoding per page and
// letting GxEPD2 clip is all it takes for paged rendering. drawPixel is also
// rotation-aware, so a rotated panel needs no special handling here.
// ***************************************************************************

static bool s_drawEnabled = false; // false = validation pass (no drawing)

#if defined(EPD_7COLOR)
// Spectra 6 (E6) palette: black, white, red, yellow, blue, green. nicepaper's
// `e6` output is already quantized to these, so nearest-RGB is an exact match.
struct PaletteEntry { uint8_t r, g, b; uint16_t color; };
static const PaletteEntry E6_PALETTE[] = {
    {  0,   0,   0, GxEPD_BLACK },
    {255, 255, 255, GxEPD_WHITE },
    {255,   0,   0, GxEPD_RED    },
    {255, 255,   0, GxEPD_YELLOW },
    {  0,   0, 255, GxEPD_BLUE   },
    {  0, 255,   0, GxEPD_GREEN  },
};
#endif

// nicepaper delivers pixels already snapped to the panel palette, so plain
// thresholds (b/w, b/w/red) or a nearest-palette match (7-colour) suffice.
static uint16_t colorForPixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a < 128) return GxEPD_WHITE;             // treat transparent as background
#if defined(EPD_7COLOR)
    uint16_t best = GxEPD_WHITE;
    long bestDist = LONG_MAX;
    for (const auto &p : E6_PALETTE)
    {
        long dr = (long)r - p.r, dg = (long)g - p.g, db = (long)b - p.b;
        long dist = dr * dr + dg * dg + db * db;
        if (dist < bestDist) { bestDist = dist; best = p.color; }
    }
    return best;
#elif defined(EPD_HAS_RED)
    if (r > 128 && g < 100 && b < 100) return GxEPD_RED;
    uint32_t luma = (77u * r + 150u * g + 29u * b) >> 8; // Rec. 601
    return (luma < 128) ? GxEPD_BLACK : GxEPD_WHITE;
#else
    uint32_t luma = (77u * r + 150u * g + 29u * b) >> 8; // Rec. 601
    return (luma < 128) ? GxEPD_BLACK : GxEPD_WHITE;
#endif
}

static void onPngDraw(pngle_t *, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint8_t rgba[4])
{
    if (!s_drawEnabled) return;
    uint16_t color = colorForPixel(rgba[0], rgba[1], rgba[2], rgba[3]);
    if (color == GxEPD_WHITE) return; // background already cleared to white
    for (uint32_t dy = 0; dy < h; dy++)
        for (uint32_t dx = 0; dx < w; dx++)
            gx.drawPixel(x + dx, y + dy, color);
}

// Decode the whole PNG once. With draw=false it only validates (checks for a
// decode error and a valid size); with draw=true it paints onto the current
// GxEPD2 page. Fed in small chunks so the task watchdog stays fed.
static bool decodePng(const uint8_t *data, size_t len, bool draw)
{
    s_drawEnabled = draw;
    pngle_t *pngle = pngle_new();
    if (!pngle) return false;
    pngle_set_draw_callback(pngle, onPngDraw);

    bool ok = true;
    size_t fed = 0;
    const size_t CHUNK = 1024;
    while (fed < len)
    {
        size_t want = (len - fed < CHUNK) ? (len - fed) : CHUNK;
        int used = pngle_feed(pngle, data + fed, want);
        if (used < 0)
        {
            log_e("pngle decode error: %s", pngle_error(pngle));
            ok = false;
            break;
        }
        if (used == 0) break; // decoder reached the end of the stream
        fed += (size_t)used;
        yield(); // keep the watchdog happy on large images
    }
    if (ok && pngle_get_width(pngle) == 0) ok = false;
    pngle_destroy(pngle);
    return ok;
}

// Trampoline so GxEPD2's C-style busy callback can invoke a std::function
// exactly once during the refresh BUSY-wait. It also timestamps the first busy
// wait, which marks the start of the physical panel refresh.
static std::function<void()> s_duringRefresh;
static bool s_duringRefreshDone = false;
static uint32_t s_refreshStartMs = 0;
static void busyTrampoline(const void *)
{
    if (s_refreshStartMs == 0)
        s_refreshStartMs = millis(); // first BUSY wait ~= refresh start
    if (!s_duringRefreshDone && s_duringRefresh)
    {
        s_duringRefreshDone = true; // set first: re-entrancy safe
        s_duringRefresh();
    }
}

bool DisplayRenderer::renderImage(const uint8_t *png, size_t len,
                                  const DisplayStatus &status,
                                  const std::function<void()> &duringRefresh)
{
    // The GxEPD2 page buffer is already allocated (static); the transient cost
    // now is pngle's decoder state (~36 KB, embeds the 32 KB DEFLATE window).
    // Report the page layout and bail out early on a clearly insufficient heap
    // rather than crashing inside the decoder.
    log_i("render: free heap %u B, %d page(s) x %d rows (page buf ~%d B)",
          (unsigned)ESP.getFreeHeap(), gx.pages(), gx.pageHeight(),
          gx.pageHeight() * EPD_ROW_BYTES);
    if (ESP.getFreeHeap() < 48 * 1024)
    {
        log_e("insufficient heap (%u B) to decode the PNG", (unsigned)ESP.getFreeHeap());
        return false;
    }

    // Validate first (cheap) so a corrupt PNG never reaches the panel — with
    // multi-page rendering earlier pages would already be committed otherwise.
    // (Runs before any housekeeping, so an invalid PNG leaves WiFi untouched for
    // the caller's error screen.)
    if (!decodePng(png, len, /*draw*/ false))
        return false;

    uint32_t renderStart = millis();
    s_refreshStartMs = 0;

    initPanel_();

    // Overlap the slow physical refresh with the caller's housekeeping, and let
    // the busy callback timestamp the refresh start for phase timing.
    s_duringRefresh = duringRefresh;
    s_duringRefreshDone = false;
    gx.epd2.setBusyCallback(busyTrampoline);

    gx.setFullWindow();
    gx.firstPage();
    do
    {
        gx.fillScreen(GxEPD_WHITE);
        decodePng(png, len, /*draw*/ true); // re-decode into this page band
        drawOverlay_(status);
    } while (gx.nextPage());
    gx.hibernate();

    gx.epd2.setBusyCallback(nullptr);
    if (duringRefresh && !s_duringRefreshDone) // panel had no BUSY wait: run now
    {
        s_duringRefreshDone = true;
        duringRefresh();
    }
    s_duringRefresh = nullptr;

    // Phase timing: decode+transfer is everything up to the first BUSY wait; the
    // refresh is from there until the wait returned.
    uint32_t renderEnd = millis();
    uint32_t refreshStart = s_refreshStartMs ? s_refreshStartMs : renderEnd;
    _lastDecodeTransferMs = refreshStart - renderStart;
    _lastRefreshMs = renderEnd - refreshStart;
    return true;
}

// ***************************************************************************
// Panel setup
// ***************************************************************************

void DisplayRenderer::initPanel_()
{
    // Configure the control pins as outputs up front. GxEPD2's init()/_reset()
    // deliberately pre-set them with digitalWrite() *before* their own pinMode()
    // ("less glitch"); arduino-esp32 3.x logs that as an "IO N is not set as
    // GPIO" error and skips the pre-set. Doing pinMode() here first makes those
    // writes valid and silences the (otherwise harmless) errors.
    pinMode(EPD_CS, OUTPUT);
    pinMode(EPD_DC, OUTPUT);
    pinMode(EPD_RST, OUTPUT);

    // Remap the VSPI bus to the board's pins *before* gx.init(): GxEPD2's init()
    // calls SPI.begin() internally, which early-returns once the bus is up, so
    // our pins are kept for the data transfers. (SPI.begin attaches SCK/MISO/
    // MOSI only, never the CS pin, so CS stays a plain GPIO for GxEPD2.)
    SPI.end();
    SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);

    gx.init(/*serial_diag_bitrate*/ 0, /*initial*/ true, /*reset_duration*/ 2,
            /*pulldown_rst_mode*/ false);
    gx.setRotation(_rotation);
    gx.setTextColor(GxEPD_BLACK);
}

// ***************************************************************************
// Status overlay (top-right, phone-like)
// ***************************************************************************

void DisplayRenderer::drawOverlay_(const DisplayStatus &st)
{
    const int margin = 4;
    const int batW = 24; // battery body width (without the +nub)
    int y = margin;

    // Battery sits flush to the right edge; WiFi to its left.
    int batX = gx.width() - margin - (batW + 3);
    drawBattery_(batX, y, st);

    int wifiX = batX - 8 - 18; // 18px wide wifi block + gap
    drawWifi_(wifiX, y + 1, st);
}

void DisplayRenderer::drawBattery_(int x, int y, const DisplayStatus &st)
{
    const int batW = 24, batH = 12;
    // body + terminal nub
    gx.drawRect(x, y, batW, batH, GxEPD_BLACK);
    gx.fillRect(x + batW, y + 3, 3, batH - 6, GxEPD_BLACK);

    if (!st.batteryValid || st.batteryPct < 0)
    {
        // unknown level -> question mark inside the body
        gx.setTextColor(GxEPD_BLACK);
        gx.setTextSize(1);
        gx.setCursor(x + batW / 2 - 3, y + 2);
        gx.print('?');
        return;
    }

    int pct = constrain(st.batteryPct, 0, 100);
    int innerW = batW - 4;
    int fillW = (innerW * pct) / 100;
    if (fillW > 0)
        gx.fillRect(x + 2, y + 2, fillW, batH - 4, GxEPD_BLACK);
}

void DisplayRenderer::drawWifi_(int x, int y, const DisplayStatus &st)
{
    // Four signal bars of increasing height (phone style).
    const int bars = 4, barW = 3, gap = 1, maxH = 10;
    int strength = 0;
    if (st.wifiConnected)
    {
        int r = st.rssi;
        strength = (r >= -55) ? 4 : (r >= -65) ? 3 : (r >= -75) ? 2 : 1;
    }

    int baseY = y + maxH;
    for (int i = 0; i < bars; i++)
    {
        int h = 3 + (i * (maxH - 3)) / (bars - 1);
        int bx = x + i * (barW + gap);
        int by = baseY - h;
        if (i < strength)
            gx.fillRect(bx, by, barW, h, GxEPD_BLACK);   // active bar
        else
            gx.drawRect(bx, by, barW, h, GxEPD_BLACK);   // inactive outline
    }

    if (!st.wifiConnected)
    {
        // strike-through to signal "no connection"
        int w = bars * (barW + gap);
        gx.drawLine(x - 1, y - 1, x + w, baseY + 1, GxEPD_BLACK);
        gx.drawLine(x - 1, y, x + w, baseY + 2, GxEPD_BLACK);
    }
}

// ***************************************************************************
// Full-screen error page
// ***************************************************************************

// Word-wrap one segment (no '\n') to `maxWidth`, printing each line centered.
static int printSegmentCentered(int y, int maxWidth, int lineH, const String &seg)
{
    String line;
    int s = 0, m = seg.length();
    while (s <= m)
    {
        int sp = seg.indexOf(' ', s);
        String word = (sp < 0) ? seg.substring(s) : seg.substring(s, sp);
        String cand = line.length() ? line + " " + word : word;

        int16_t bx, by; uint16_t bw, bh;
        gx.getTextBounds(cand, 0, 0, &bx, &by, &bw, &bh);
        if (bw > (uint16_t)maxWidth && line.length())
        {
            gx.getTextBounds(line, 0, 0, &bx, &by, &bw, &bh);
            gx.setCursor((gx.width() - bw) / 2, y);
            gx.print(line);
            y += lineH;
            line = word;
        }
        else
        {
            line = cand;
        }
        if (sp < 0) break;
        s = sp + 1;
    }
    if (line.length())
    {
        int16_t bx, by; uint16_t bw, bh;
        gx.getTextBounds(line, 0, 0, &bx, &by, &bw, &bh);
        gx.setCursor((gx.width() - bw) / 2, y);
        gx.print(line);
        y += lineH;
    }
    return y;
}

// Print `text` centered on `y`, honouring '\n' as a hard line break (so "\n\n"
// yields a blank line / paragraph gap) and word-wrapping each line to
// `maxWidth`. Returns the y just below the printed block.
static int printWrappedCentered(int y, int maxWidth, uint8_t textSize,
                                const String &text)
{
    gx.setTextSize(textSize);
    gx.setTextColor(GxEPD_BLACK);
    int lineH = 8 * textSize + 4;

    int start = 0, n = text.length();
    while (start <= n)
    {
        int nl = text.indexOf('\n', start);
        int end = (nl < 0) ? n : nl;
        String seg = text.substring(start, end);
        if (seg.length() == 0)
            y += lineH; // blank line for a paragraph gap
        else
            y = printSegmentCentered(y, maxWidth, lineH, seg);
        if (nl < 0) break;
        start = nl + 1;
    }
    return y;
}

static void drawWarningIcon(int cx, int cy, int size)
{
    // Filled triangle with a white exclamation mark punched out.
    int half = size / 2;
    int topY = cy - half, botY = cy + half;
    gx.fillTriangle(cx, topY, cx - half, botY, cx + half, botY, GxEPD_BLACK);
    int barW = max(3, size / 12);
    gx.fillRect(cx - barW / 2, cy - half / 3, barW, half, GxEPD_WHITE);
    gx.fillRect(cx - barW / 2, botY - size / 6, barW, barW, GxEPD_WHITE);
}

// Thick arc band (radii rInner..rOuter) from a0..a1 degrees; 0° = east, angles
// increase clockwise in screen coordinates (y points down).
static void drawArcBand(int cx, int cy, int rInner, int rOuter,
                        int a0deg, int a1deg, uint16_t color)
{
    int steps = (a1deg - a0deg) * 3;
    if (steps < 24) steps = 24;
    for (int s = 0; s <= steps; s++)
    {
        double a = (a0deg + (double)(a1deg - a0deg) * s / steps) * DEG_TO_RAD;
        double ca = cos(a), sa = sin(a);
        for (int r = rInner; r <= rOuter; r++)
            gx.drawPixel(cx + (int)lround(r * ca), cy + (int)lround(r * sa), color);
    }
}

static void drawNoWifiIcon(int cx, int cy, int size)
{
    // WiFi "fan": a base dot with three arcs opening upward (centred on 270°),
    // crossed by a bold diagonal with a white halo so the slash stays crisp
    // where it passes over the black arcs.
    int apexY = cy + size / 3;                 // fan origin (bottom)
    const int A0 = 270 - 52, A1 = 270 + 52;    // ~104° fan pointing up
    int band = max(2, size / 16);              // arc thickness
    int step = max(band + size / 10, size / 6);
    for (int i = 1; i <= 3; i++)
    {
        int r = i * step;
        drawArcBand(cx, apexY, r, r + band, A0, A1, GxEPD_BLACK);
    }
    gx.fillCircle(cx, apexY, max(2, size / 12), GxEPD_BLACK);

    // Diagonal slash (perpendicular offsets give uniform thickness). Draw the
    // white halo first (wider), then the black ink on top (narrower).
    int s = (size * 62) / 100;
    int halo = size / 14 + 2, ink = size / 28 + 1;
    for (int o = -halo; o <= halo; o++)
        gx.drawLine(cx - s - o, cy - s + o, cx + s - o, cy + s + o, GxEPD_WHITE);
    for (int o = -ink; o <= ink; o++)
        gx.drawLine(cx - s - o, cy - s + o, cx + s - o, cy + s + o, GxEPD_BLACK);
}

void DisplayRenderer::showError(ErrorIcon icon, const String &title,
                                const String &message, const DisplayStatus &status)
{
    initPanel_();
    gx.setFullWindow();
    gx.firstPage();
    do
    {
        gx.fillScreen(GxEPD_WHITE);

        int W = gx.width();
        int iconSize = min(W, (int)gx.height()) / 4;
        int iconCy = gx.height() / 3;
        if (icon == ErrorIcon::NoWifi)
            drawNoWifiIcon(W / 2, iconCy, iconSize);
        else
            drawWarningIcon(W / 2, iconCy, iconSize);

        int y = iconCy + iconSize / 2 + 20;
        uint8_t titleSize = (W >= 600) ? 3 : 2;
        uint8_t msgSize   = (W >= 600) ? 2 : 1;
        y = printWrappedCentered(y, W - 20, titleSize, title);
        y += 6;
        printWrappedCentered(y, W - 24, msgSize, message);

        drawOverlay_(status);
    } while (gx.nextPage());
    gx.hibernate();
}
