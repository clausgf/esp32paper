/**
 * esp32paper — DisplayRenderer implementation.
 *
 * All enabled panels are compiled in (see src/panels.h); the active one is
 * created at runtime as a GxEPD2_GFX* via the panel factory. Only the selected
 * panel allocates its page buffer (heap). Colour mapping is dispatched at
 * runtime from the panel's ColorMode. Everything else is panel-agnostic.
 */

#include "display_renderer.h"
#include "config.h"
#include "panels.h"

#include <SPI.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <limits.h>
#include <math.h>
#include <pngle.h>

// ***************************************************************************
// Active panel (created lazily in initPanel_); its palette lives below.
// ***************************************************************************
static GxEPD2_GFX *gx = nullptr;

// Palette of the active panel (set in applyPanel_ from the panel's ColorMode).
static const PaletteEntry *s_palette = EPD_PALETTE_BW;
static uint8_t s_paletteCount = 2;

DisplayRenderer displayRenderer;

uint16_t DisplayRenderer::width() const  { return gx ? gx->width() : 0; }
uint16_t DisplayRenderer::height() const { return gx ? gx->height() : 0; }
String   DisplayRenderer::supportedPanels() const { return epdSupportedPanelsCsv(); }

void DisplayRenderer::applyPanel_(const char *id)
{
    const PanelInfo *info = epdPanelInfo(id);
    if (!info) info = epdPanelInfo(epdDefaultPanelId());
    _panelId = info->id;
    _colorModel = info->colorModel;
    s_palette = epdPalette(info->colorMode, &s_paletteCount);
}

bool DisplayRenderer::setPanel(const String &id)
{
    bool known = true;
    if (!id.isEmpty())
    {
        known = epdPanelInfo(id.c_str()) != nullptr;
        if (known)
            applyPanel_(id.c_str());
    }
    if (_panelId.isEmpty()) // never resolved yet -> compiled-in default
        applyPanel_(epdDefaultPanelId());
    return known;
}

// ***************************************************************************
// PNG decode -> panel
//
// The pixels are drawn straight onto the GxEPD2 page via drawPixel(), which
// ignores pixels outside the current page band — so re-decoding per page and
// letting GxEPD2 clip is all it takes for paged rendering. The image is drawn
// unrotated (GxEPD2 rotation 0): image rows are native panel rows.
// A PNG decodes top to bottom, so a page's decode stops once it has passed the
// last image row that can land in that page (see pageStopRow()).
// ***************************************************************************

static bool s_drawEnabled = false;      // false = validation pass (no drawing)
static uint32_t s_stopRow = UINT32_MAX; // image rows >= this are not needed
static bool s_stopped = false;          // decode passed s_stopRow

static uint16_t nearestPalette(const PaletteEntry *pal, int n,
                               uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t best = GxEPD_WHITE;
    long bestDist = LONG_MAX;
    for (int i = 0; i < n; i++)
    {
        long dr = (long)r - pal[i].r, dg = (long)g - pal[i].g, db = (long)b - pal[i].b;
        long dist = dr * dr + dg * dg + db * db;
        if (dist < bestDist) { bestDist = dist; best = pal[i].color; }
    }
    return best;
}

// nicepaper delivers pixels already snapped to the panel's color_model, so a
// nearest-RGB match against that panel's palette maps every mode (b/w, b/w/red,
// 6- and 7-colour) with the same code path — no per-mode special-casing.
static uint16_t colorForPixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a < 128) return GxEPD_WHITE; // treat transparent as background
    return nearestPalette(s_palette, s_paletteCount, r, g, b);
}

static void onPngDraw(pngle_t *pngle, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint8_t rgba[4])
{
    if (!s_drawEnabled) return;
    if (y >= s_stopRow && pngle_get_ihdr(pngle)->interlace == 0)
    {
        s_stopped = true; // rows arrive in order: nothing more for this page
        return;
    }
    uint16_t color = colorForPixel(rgba[0], rgba[1], rgba[2], rgba[3]);
    if (color == GxEPD_WHITE) return; // background already cleared to white
    for (uint32_t dy = 0; dy < h; dy++)
        for (uint32_t dx = 0; dx < w; dx++)
            gx->drawPixel(x + dx, y + dy, color);
}

// Decode the PNG. With draw=false it only validates the whole image (checks
// for a decode error and a valid size); with draw=true it paints onto the
// current GxEPD2 page, stopping after image row stopRow - 1 (non-interlaced
// PNGs only). Fed in small chunks so the task watchdog stays fed.
static bool decodePng(const uint8_t *data, size_t len, bool draw,
                      uint32_t *outW = nullptr, uint32_t *outH = nullptr,
                      String *outError = nullptr, uint32_t stopRow = UINT32_MAX)
{
    s_drawEnabled = draw;
    s_stopRow = draw ? stopRow : UINT32_MAX;
    s_stopped = false;
    pngle_t *pngle = pngle_new();
    if (!pngle)
    {
        if (outError) *outError = "Out of memory\nfor the PNG decoder.";
        return false;
    }
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
            if (outError) *outError = String("PNG decode error:\n") + pngle_error(pngle);
            ok = false;
            break;
        }
        if (used == 0) break; // decoder reached the end of the stream
        fed += (size_t)used;
        if (s_stopped) break; // rest of the image is outside this page
        yield(); // keep the watchdog happy on large images
    }
    uint32_t w = pngle_get_width(pngle), h = pngle_get_height(pngle);
    if (ok && w == 0)
    {
        if (outError) *outError = "The received image\nis not a valid PNG.";
        ok = false;
    }
    pngle_destroy(pngle);
    s_stopRow = UINT32_MAX;
    if (outW) *outW = w;
    if (outH) *outH = h;
    return ok;
}

// Parse only the PNG header (IHDR, the first few dozen bytes) for its size.
static bool pngSize(const uint8_t *data, size_t len, uint32_t *w, uint32_t *h,
                    String *outError)
{
    *w = *h = 0;
    pngle_t *pngle = pngle_new();
    if (!pngle)
    {
        *outError = "Out of memory\nfor the PNG decoder.";
        return false;
    }
    bool ok = true;
    size_t fed = 0;
    while (fed < len && pngle_get_width(pngle) == 0)
    {
        size_t want = (len - fed < 64) ? (len - fed) : 64;
        int used = pngle_feed(pngle, data + fed, want);
        if (used < 0)
        {
            *outError = String("PNG decode error:\n") + pngle_error(pngle);
            ok = false;
            break;
        }
        if (used == 0) break;
        fed += (size_t)used;
    }
    *w = pngle_get_width(pngle);
    *h = pngle_get_height(pngle);
    pngle_destroy(pngle);
    if (ok && *w == 0)
    {
        *outError = "The received image\nis not a valid PNG.";
        ok = false;
    }
    return ok;
}

// First image row that cannot land in page `page` (GxEPD2 pages are bands of
// native rows, and the image is drawn unrotated).
static uint32_t pageStopRow(int page)
{
    const uint32_t h = gx->epd2.HEIGHT, ph = gx->pageHeight();
    return min((uint32_t)(page + 1) * ph, h);
}

// ***************************************************************************
// BUSY-wait: light sleep
//
// GxEPD2 calls the busy callback in place of its delay(1) on every poll of the
// BUSY pin while the panel works (power on, refresh, power off). There is
// nothing to do but wait (housekeeping and WiFi-off happen before rendering),
// so the CPU light-sleeps until BUSY changes level (GPIO wakeup) or at most
// NAP_MAX_US (timer wakeup, feeds the watchdog), instead of polling at full
// clock. millis()/micros() run off esp_timer, which keeps counting across light
// sleep, so the phase timings and GxEPD2's own busy timeout stay correct.
//
// The control pins must keep driving the panel while the CPU sleeps: their
// sleep-mode pad configuration is disabled, so a pin that is switched to its
// sleep configuration (input, floating) cannot glitch RST/CS mid-refresh.
// EPAPER_REFRESH_LIGHTSLEEP=0 falls back to a plain delay(1).
// ***************************************************************************

#ifndef EPAPER_REFRESH_LIGHTSLEEP
#define EPAPER_REFRESH_LIGHTSLEEP 1
#endif
static const uint64_t NAP_MAX_US = 1000000; // timer fallback, well below the watchdog

// Feed the task watchdog if this task is subscribed (a reset otherwise logs
// an error).
static void feedWatchdog()
{
    if (esp_task_wdt_status(NULL) == ESP_OK)
        esp_task_wdt_reset();
}

static void keepPinDuringSleep(int pin)
{
    if (pin >= 0)
        gpio_sleep_sel_dis((gpio_num_t)pin);
}

// Light-sleep until BUSY leaves its current (busy) level or NAP_MAX_US pass.
static void napWhileBusy()
{
    const gpio_num_t busy = (gpio_num_t)EPD_BUSY;
    const bool busyHigh = digitalRead(EPD_BUSY) == HIGH; // GxEPD2 just saw it busy
    gpio_wakeup_enable(busy, busyHigh ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup(NAP_MAX_US);
    esp_light_sleep_start(); // a level trigger returns at once if BUSY already changed
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    gpio_wakeup_disable(busy);
}

static void busyTrampoline(const void *)
{
#if EPAPER_REFRESH_LIGHTSLEEP
    napWhileBusy();
#else
    delay(1);
#endif
    feedWatchdog();
}

// Route the panel's BUSY-waits through busyTrampoline (light sleep);
// endBusyWaits() restores GxEPD2's plain polling.
static void beginBusyWaits()
{
    gx->epd2.setBusyCallback(busyTrampoline);
}

static void endBusyWaits()
{
    gx->epd2.setBusyCallback(nullptr);
}

// One-line memory summary for the log: internal heap (free / largest block)
// and PSRAM (size / free; 0 if none was found at boot).
String memorySummary()
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "internal free %u B (largest %u B), psram %u B (free %u B)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    return String(buf);
}

bool DisplayRenderer::checkImage(const uint8_t *png, size_t len)
{
    // Header only (the full decode happens while rendering): a non-PNG and
    // a wrong size are found before the caller turns WiFi off, so they are
    // reported right away. A corrupt image body is only found while rendering.
    _renderErrorDetail = "";
    uint32_t imgW = 0, imgH = 0;
    if (!pngSize(png, len, &imgW, &imgH, &_renderErrorDetail))
        return false;

    // The image must exactly fill the panel. GxEPD2 would otherwise silently
    // clip/misplace a valid-but-wrong-sized PNG — garbage on the panel with no
    // error. nicepaper renders the screen at the panel size, so a mismatch is a
    // server-side size problem or a wrong panel in config.json.
    if (_panelId.isEmpty())
        applyPanel_(epdDefaultPanelId());
    uint16_t panelW = 0, panelH = 0;
    if (const PanelInfo *pi = epdPanelInfo(_panelId.c_str()))
    {
        panelW = pi->width;
        panelH = pi->height;
    }
    if (panelW && (imgW != panelW || imgH != panelH))
    {
        log_e("image %ux%u does not match panel '%s' %ux%u",
              (unsigned)imgW, (unsigned)imgH, _panelId.c_str(),
              (unsigned)panelW, (unsigned)panelH);
        _renderErrorDetail = String("Image is ") + imgW + "x" + imgH +
                             ",\npanel is " + panelW + "x" + panelH + ".";
        return false;
    }
    return true;
}

bool DisplayRenderer::renderImage(const uint8_t *png, size_t len,
                                  const DisplayStatus &status)
{
    _renderErrorDetail = "";
    if (!checkImage(png, len))
        return false;

    uint32_t renderStart = millis();

    if (!initPanel_()) // creates the panel driver (page buffer)
    {
        _renderErrorDetail = "Not enough memory\nfor the display buffer.";
        return false;
    }
    log_i("render '%s': %d page(s) x %d rows; %s", _panelId.c_str(),
          gx->pages(), gx->pageHeight(), memorySummary().c_str());

    // pngle_t (~44 KB in one block: 32 KB DEFLATE window + ~11 KB inflator,
    // lib/pngle/pngle.c) is allocated per decode; bail out on a clearly
    // insufficient heap rather than failing halfway through the pages.
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest < 44 * 1024)
    {
        log_e("insufficient heap to decode the PNG: %s", memorySummary().c_str());
        _renderErrorDetail = String("Not enough memory\nto decode the PNG (") +
                             largest + " B free).";
        return false;
    }

    beginBusyWaits();

    // The refresh proper runs inside the last nextPage() (the first BUSY wait
    // is the controller's power-on during the first page), so it is timed from
    // the end of the last page's drawing. A decode error stops before that
    // nextPage(): the panel is not refreshed, the caller shows an error screen.
    uint32_t refreshStart = 0;
    bool decodeOk = true;
    int page = 0;
    const int pages = gx->pages();
    gx->setFullWindow();
    gx->firstPage();
    do
    {
        gx->fillScreen(GxEPD_WHITE);
        // re-decode into this page band, up to the last row it can contain
        if (!decodePng(png, len, /*draw*/ true, nullptr, nullptr,
                       &_renderErrorDetail, pageStopRow(page)))
        {
            decodeOk = false;
            break; // no nextPage(): no refresh with a half-decoded image
        }
        drawOverlay_(status);
        if (++page >= pages && refreshStart == 0)
        {
            refreshStart = millis();
            log_i("panel refresh: start");
        }
    } while (gx->nextPage());
    gx->hibernate();
    endBusyWaits();
    if (!decodeOk)
        return false;

    // Phase timing: decode+transfer of all pages, then the refresh incl. power
    // off and hibernate.
    uint32_t renderEnd = millis();
    if (refreshStart == 0) refreshStart = renderEnd;
    _lastDecodeTransferMs = refreshStart - renderStart;
    _lastRefreshMs = renderEnd - refreshStart;
    log_i("panel refresh: done in %u ms (decode+transfer %u ms)",
          (unsigned)_lastRefreshMs, (unsigned)_lastDecodeTransferMs);
    return true;
}

// ***************************************************************************
// Panel setup
// ***************************************************************************

bool DisplayRenderer::initPanel_()
{
    // Resolve + create the panel driver on first use (heap; only the selected
    // panel's page buffer is allocated, in PSRAM if available and large).
    // _panelId is always a compiled-in id (applyPanel_ falls back), so a
    // nullptr means the page buffer did not fit.
    if (_panelId.isEmpty())
        applyPanel_(epdDefaultPanelId());
    if (!gx)
    {
        gx = epdCreatePanel(_panelId.c_str());
        if (!gx)
        {
            log_e("cannot allocate panel '%s' (page buffer): %s",
                  _panelId.c_str(), memorySummary().c_str());
            return false;
        }
    }

#ifdef EPD_ENABLE_PIN
    // EE04 gates the panel's power rail behind this pin.
    pinMode(EPD_ENABLE_PIN, OUTPUT);
    digitalWrite(EPD_ENABLE_PIN, HIGH);
#endif

    // Configure the control pins as outputs up front. GxEPD2's init()/_reset()
    // deliberately pre-set them with digitalWrite() *before* their own pinMode()
    // ("less glitch"); arduino-esp32 3.x logs that as an "IO N is not set as
    // GPIO" error and skips the pre-set. Doing pinMode() here first makes those
    // writes valid and silences the (otherwise harmless) errors.
    pinMode(EPD_CS, OUTPUT);
    pinMode(EPD_DC, OUTPUT);
    pinMode(EPD_RST, OUTPUT);

    // Keep the panel's pins in their normal configuration during light sleep
    // (see napWhileBusy).
    keepPinDuringSleep(EPD_CS);
    keepPinDuringSleep(EPD_DC);
    keepPinDuringSleep(EPD_RST);
    keepPinDuringSleep(EPD_SCK);
    keepPinDuringSleep(EPD_MOSI);
    keepPinDuringSleep(EPD_BUSY);
#ifdef EPD_ENABLE_PIN
    keepPinDuringSleep(EPD_ENABLE_PIN);
#endif

    // Remap the VSPI bus to the board's pins *before* init(): GxEPD2's init()
    // calls SPI.begin() internally, which early-returns once the bus is up, so
    // our pins are kept for the data transfers. (SPI.begin attaches SCK/MISO/
    // MOSI only, never the CS pin, so CS stays a plain GPIO for GxEPD2.)
    SPI.end();
    SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);

    // A non-zero diag bitrate sets GxEPD2's _diag_enabled, which makes
    // _waitWhileBusy print "<comment> : <elapsed_us>" for every BUSY wait. Our
    // phase timings only ever showed the sum of _PowerOn and _Update_Full, which
    // cannot distinguish "the panel never asserted BUSY and the wait fell
    // through" from "the panel refreshed but nothing became visible".
    gx->init(/*serial_diag_bitrate*/ 115200, /*initial*/ true, /*reset_duration*/ 2,
             /*pulldown_rst_mode*/ false);
    gx->setRotation(0);
    gx->setTextColor(GxEPD_BLACK);
    return true;
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
    int batX = gx->width() - margin - (batW + 3);
    drawBattery_(batX, y, st);

    int wifiX = batX - 8 - 18; // 18px wide wifi block + gap
    drawWifi_(wifiX, y + 1, st);

    // Timestamp + firmware version in the built-in 6x8 font, turned by 90°
    // (GxEPD2 rotation 3 while printing), along the left edge.
    String msg = st.timestamp + "  " + st.firmwareVersion;
    msg.trim();
    if (msg.length() > 0)
    {
        gx->setRotation(3);
        gx->setTextSize(1);
        int16_t bx, by;
        uint16_t bw, bh;
        gx->getTextBounds(msg, 0, 0, &bx, &by, &bw, &bh);
        gx->setCursor(0, gx->height() - bh);
        gx->print(msg);
        gx->setRotation(0);
    }
}

void DisplayRenderer::drawBattery_(int x, int y, const DisplayStatus &st)
{
    const int batW = 24, batH = 12;
    // body + terminal nub
    gx->drawRect(x, y, batW, batH, GxEPD_BLACK);
    gx->fillRect(x + batW, y + 3, 3, batH - 6, GxEPD_BLACK);

    if (!st.batteryValid || st.batteryPct < 0)
    {
        // unknown level -> question mark inside the body
        gx->setTextColor(GxEPD_BLACK);
        gx->setTextSize(1);
        gx->setCursor(x + batW / 2 - 3, y + 2);
        gx->print('?');
        return;
    }

    int pct = constrain(st.batteryPct, 0, 100);
    int innerW = batW - 4;
    int fillW = (innerW * pct) / 100;
    if (fillW > 0)
        gx->fillRect(x + 2, y + 2, fillW, batH - 4, GxEPD_BLACK);
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
            gx->fillRect(bx, by, barW, h, GxEPD_BLACK);  // active bar
        else
            gx->drawRect(bx, by, barW, h, GxEPD_BLACK);  // inactive outline
    }

    if (!st.wifiConnected)
    {
        // strike-through to signal "no connection"
        int w = bars * (barW + gap);
        gx->drawLine(x - 1, y - 1, x + w, baseY + 1, GxEPD_BLACK);
        gx->drawLine(x - 1, y, x + w, baseY + 2, GxEPD_BLACK);
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
        gx->getTextBounds(cand, 0, 0, &bx, &by, &bw, &bh);
        if (bw > (uint16_t)maxWidth && line.length())
        {
            gx->getTextBounds(line, 0, 0, &bx, &by, &bw, &bh);
            gx->setCursor((gx->width() - bw) / 2, y);
            gx->print(line);
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
        gx->getTextBounds(line, 0, 0, &bx, &by, &bw, &bh);
        gx->setCursor((gx->width() - bw) / 2, y);
        gx->print(line);
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
    gx->setTextSize(textSize);
    gx->setTextColor(GxEPD_BLACK);
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
    gx->fillTriangle(cx, topY, cx - half, botY, cx + half, botY, GxEPD_BLACK);
    int barW = max(2, size / 8);
    int dotSize = max(2, size / 6);
    int gap = max(1, size / 12);
    int startY = topY + dotSize + 2 * gap;
    int totalH = botY - startY - gap;
    gx->fillRect(cx - barW / 2, startY, barW, totalH - dotSize - gap / 2, GxEPD_WHITE);
    gx->fillRect(cx - dotSize / 2, botY - gap - dotSize, dotSize, dotSize, GxEPD_WHITE);
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
            gx->drawPixel(cx + (int)lround(r * ca), cy + (int)lround(r * sa), color);
    }
}

static void drawNoWifiIcon(int cx, int cy, int size)
{
    // WiFi "fan": a base dot with three arcs opening upward (centred on 270°),
    // crossed by a bold diagonal with a white halo so the slash stays crisp
    // where it passes over the black arcs.
    int apexY = cy + size / 3;              // fan origin (bottom)
    const int A0 = 270 - 52, A1 = 270 + 52; // ~104° fan pointing up
    int band = max(2, size / 16);           // arc thickness
    int step = max(band + size / 10, size / 6);
    for (int i = 1; i <= 3; i++)
    {
        int r = i * step;
        drawArcBand(cx, apexY, r, r + band, A0, A1, GxEPD_BLACK);
    }
    gx->fillCircle(cx, apexY, max(2, size / 12), GxEPD_BLACK);

    // Diagonal slash (perpendicular offsets give uniform thickness). Draw the
    // white halo first (wider), then the black ink on top (narrower).
    int s = (size * 62) / 100;
    int halo = size / 14 + 2, ink = size / 28 + 1;
    for (int o = -halo; o <= halo; o++)
        gx->drawLine(cx - s - o, cy - s + o, cx + s - o, cy + s + o, GxEPD_WHITE);
    for (int o = -ink; o <= ink; o++)
        gx->drawLine(cx - s - o, cy - s + o, cx + s - o, cy + s + o, GxEPD_BLACK);
}

void DisplayRenderer::showError(ErrorIcon icon, const String &title,
                                const String &message, const DisplayStatus &status)
{
    if (!initPanel_())
        return; // no memory for the page buffer: nothing can be shown
    beginBusyWaits(); // light-sleep through the refresh
    gx->setFullWindow();
    gx->firstPage();
    do
    {
        gx->fillScreen(GxEPD_WHITE);

        int W = gx->width();
        int iconSize = min(W, (int)gx->height()) / 4;
        int iconCy = gx->height() / 3;
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
    } while (gx->nextPage());
    endBusyWaits();
    //gx->hibernate();
}
