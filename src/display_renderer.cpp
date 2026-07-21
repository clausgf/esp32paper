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
    // GxEPD2::init() sets up the default SPI pins, so remap the VSPI bus to the
    // Waveshare driver board's pins *after* init() (mirrors the reference
    // epaper-esp32 ordering — reversing it lets init() clobber the pins).
    gx.init(/*serial_diag_bitrate*/ 0, /*initial*/ true, /*reset_duration*/ 2,
            /*pulldown_rst_mode*/ false);
    SPI.end();
    SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);
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

// Print `text` centered on `y`, word-wrapping to fit `maxWidth`. Returns the
// y just below the printed block.
static int printWrappedCentered(int y, int maxWidth, uint8_t textSize,
                                const String &text)
{
    gx.setTextSize(textSize);
    gx.setTextColor(GxEPD_BLACK);
    int lineH = 8 * textSize + 4;

    String line;
    int start = 0;
    while (start <= (int)text.length())
    {
        int sp = text.indexOf(' ', start);
        String word = (sp < 0) ? text.substring(start) : text.substring(start, sp);
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
        start = sp + 1;
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

static void drawNoWifiIcon(int cx, int cy, int size)
{
    // Three concentric wifi arcs + base dot, then a slash across.
    int base = cy + size / 3;
    for (int i = 1; i <= 3; i++)
    {
        int r = (size / 3) * i / 3 + size / 6;
        for (int t = 0; t < 3; t++) // thicken the arc
            gx.drawCircleHelper(cx, base, r + t, 0x03, GxEPD_BLACK); // top quadrants
    }
    gx.fillCircle(cx, base, max(2, size / 20), GxEPD_BLACK);
    int s = size / 2;
    gx.drawLine(cx - s, cy - s, cx + s, cy + s, GxEPD_BLACK);
    gx.drawLine(cx - s, cy - s + 1, cx + s, cy + s + 1, GxEPD_BLACK);
    gx.drawLine(cx - s + 1, cy - s, cx + s + 1, cy + s, GxEPD_BLACK);
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
