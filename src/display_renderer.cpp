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
#include <esp_sleep.h>
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

void DisplayRenderer::setPanel(const String &id)
{
    if (!id.isEmpty())
    {
        if (epdPanelInfo(id.c_str()))
            applyPanel_(id.c_str());
        else
            log_w("unknown panel '%s' — keeping '%s'", id.c_str(),
                  _panelId.isEmpty() ? "default" : _panelId.c_str());
    }
    if (_panelId.isEmpty()) // never resolved yet -> compiled-in default
        applyPanel_(epdDefaultPanelId());
}

// ***************************************************************************
// PNG decode -> panel
//
// The pixels are drawn straight onto the GxEPD2 page via drawPixel(), which
// ignores pixels outside the current page band — so re-decoding per page and
// letting GxEPD2 clip is all it takes for paged rendering. drawPixel is also
// rotation-aware, so a rotated panel needs no special handling here.
// ***************************************************************************

static bool s_drawEnabled = false; // false = validation pass (no drawing)

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

static void onPngDraw(pngle_t *, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint8_t rgba[4])
{
    if (!s_drawEnabled) return;
    uint16_t color = colorForPixel(rgba[0], rgba[1], rgba[2], rgba[3]);
    if (color == GxEPD_WHITE) return; // background already cleared to white
    for (uint32_t dy = 0; dy < h; dy++)
        for (uint32_t dx = 0; dx < w; dx++)
            gx->drawPixel(x + dx, y + dy, color);
}

// Decode the whole PNG once. With draw=false it only validates (checks for a
// decode error and a valid size); with draw=true it paints onto the current
// GxEPD2 page. Fed in small chunks so the task watchdog stays fed.
static bool decodePng(const uint8_t *data, size_t len, bool draw,
                      uint32_t *outW = nullptr, uint32_t *outH = nullptr)
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
    uint32_t w = pngle_get_width(pngle), h = pngle_get_height(pngle);
    if (ok && w == 0) ok = false;
    pngle_destroy(pngle);
    if (outW) *outW = w;
    if (outH) *outH = h;
    return ok;
}

// Trampoline so GxEPD2's C-style busy callback can invoke a std::function
// exactly once during the refresh BUSY-wait. It also timestamps the first busy
// wait, which marks the start of the physical panel refresh.
//
// GxEPD2 calls this in place of its delay(1) on *every* poll iteration of the
// multi-second BUSY-wait. Once the one-shot housekeeping has run (which ends by
// switching the radio off), there is nothing left to do but wait for the panel,
// so we light-sleep the CPU in short slices instead of busy-polling at full
// clock: the MCU drops from ~40 mA to ~0.8 mA for the bulk of the refresh, and on
// each wake GxEPD2 re-reads BUSY and either finishes or naps once more. millis()/
// micros() run off esp_timer, which keeps counting across light sleep, so the
// phase timings and GxEPD2's own busy-timeout stay correct.
static const uint64_t REFRESH_NAP_US = 20000; // 20 ms slices (refresh-done latency ≤ this)
static std::function<void()> s_duringRefresh;
static bool s_duringRefreshDone = false;
static uint32_t s_refreshStartMs = 0;
#ifdef EPAPER_BUSY_PROBE
static uint32_t s_probeMs = 0; // kept out of the phase timings (see renderImage)
#endif
static void busyTrampoline(const void *)
{
    if (s_refreshStartMs == 0)
    {
        s_refreshStartMs = millis(); // first BUSY wait ~= refresh start
        log_i("panel refresh: start");
    }
    if (!s_duringRefreshDone && s_duringRefresh)
    {
        s_duringRefreshDone = true; // set first: re-entrancy safe
        s_duringRefresh();          // ends with the radio off
        return;                     // let GxEPD2 re-check BUSY once before napping
    }
    // Nap the CPU for the rest of the refresh instead of spinning on BUSY.
    esp_sleep_enable_timer_wakeup(REFRESH_NAP_US);
    esp_light_sleep_start();
}

bool DisplayRenderer::renderImage(const uint8_t *png, size_t len,
                                  const DisplayStatus &status,
                                  const std::function<void()> &duringRefresh)
{
    // The transient cost of decoding is pngle's state (~36 KB, embeds the 32 KB
    // DEFLATE window). Bail out early on a clearly insufficient heap rather than
    // crashing inside the decoder.
    if (ESP.getFreeHeap() < 48 * 1024)
    {
        log_e("insufficient heap (%u B) to decode the PNG", (unsigned)ESP.getFreeHeap());
        return false;
    }

    // Validate first (cheap) so a corrupt PNG never reaches the panel — with
    // multi-page rendering earlier pages would already be committed otherwise.
    // (Runs before any housekeeping, so an invalid PNG leaves WiFi untouched for
    // the caller's error screen.)
    _renderErrorDetail = "";
    uint32_t imgW = 0, imgH = 0;
    if (!decodePng(png, len, /*draw*/ false, &imgW, &imgH))
        return false;

    // The image must exactly fill the panel canvas (post-rotation). GxEPD2 would
    // otherwise silently clip/misplace a valid-but-wrong-sized PNG — garbage on
    // the panel with no error. nicepaper renders the screen at the panel size, so
    // a mismatch is a server-side size/orientation problem: surface it as an
    // error screen rather than rendering nonsense.
    uint16_t panelW = 0, panelH = 0;
    if (const PanelInfo *pi = epdPanelInfo(_panelId.c_str()))
    {
        panelW = pi->width;
        panelH = pi->height;
        if (_rotation & 1) { uint16_t t = panelW; panelW = panelH; panelH = t; }
    }
    if (panelW && (imgW != panelW || imgH != panelH))
    {
        log_e("image %ux%u does not match panel '%s' %ux%u (rotation %d)",
              (unsigned)imgW, (unsigned)imgH, _panelId.c_str(),
              (unsigned)panelW, (unsigned)panelH, _rotation);
        _renderErrorDetail = String("Image is ") + imgW + "x" + imgH +
                             ",\npanel is " + panelW + "x" + panelH + ".";
        return false;
    }

    uint32_t renderStart = millis();
    s_refreshStartMs = 0;

    initPanel_(); // creates the panel driver
    log_i("render '%s': free heap %u B, %d page(s) x %d rows",
          _panelId.c_str(), (unsigned)ESP.getFreeHeap(),
          gx->pages(), gx->pageHeight());

    // Overlap the slow physical refresh with the caller's housekeeping, and let
    // the busy callback timestamp the refresh start for phase timing.
    s_duringRefresh = duringRefresh;
    s_duringRefreshDone = false;
    gx->epd2.setBusyCallback(busyTrampoline);

    gx->setFullWindow();
    gx->firstPage();
    do
    {
        gx->fillScreen(GxEPD_WHITE);
        decodePng(png, len, /*draw*/ true); // re-decode into this page band
        drawOverlay_(status);
    } while (gx->nextPage());
    gx->hibernate();

    gx->epd2.setBusyCallback(nullptr);
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
#ifdef EPAPER_BUSY_PROBE
    // The probe runs inside initPanel_(), i.e. inside the decode+transfer
    // window; discount it so the diagnostic build's timings stay comparable.
    if (_lastDecodeTransferMs > s_probeMs) _lastDecodeTransferMs -= s_probeMs;
#endif
    log_i("panel refresh: done in %u ms (decode+transfer %u ms)",
          (unsigned)_lastRefreshMs, (unsigned)_lastDecodeTransferMs);
    return true;
}

// ***************************************************************************
// Panel setup
// ***************************************************************************

// Trace the BUSY line across a reset pulse.
//
// A misbehaving BUSY line breaks every driver, but in opposite and equally
// misleading ways: a busy-active-LOW driver (GxEPD2_420) waits out its full
// 10 s timeout per wait and prints "Busy Timeout!", while an active-HIGH one
// (GxEPD2_420_GDEY042T81) sees "not busy" immediately, skips the wait and lets
// hibernate() cut the refresh short — a sub-second "refresh" that reads as a
// success in the phase timings. Neither symptom distinguishes a wrong driver
// from a flaky panel, so record what the pin actually does.
//
// A reset makes a healthy panel assert BUSY and release it again on its own, so
// the trace answers both open questions at once: whether the line moves at all
// (wiring), and which level means "busy" — the level it holds *during* the
// reset is the busy one, which in turn identifies the right driver. Repeat runs
// matter as much as one: identical firmware producing different traces means
// the fault is electrical (seating, supply), not in the driver choice.
#ifdef EPAPER_BUSY_PROBE
static void probeBusyLine_()
{
    const uint32_t windowMs = 5000, stepMs = 2;
    const uint32_t probeStart = millis();
    pinMode(EPD_BUSY, INPUT);
    pinMode(EPD_RST, OUTPUT);

    int before = digitalRead(EPD_BUSY);
    digitalWrite(EPD_RST, LOW); // same pulse shape as GxEPD2's _reset(2)
    delay(2);
    digitalWrite(EPD_RST, HIGH);

    String trace;
    int last = digitalRead(EPD_BUSY), changes = 0;
    const int afterReset = last; // level the panel takes right after RST rises
    uint32_t t0 = millis();
    while (millis() - t0 < windowMs)
    {
        int v = digitalRead(EPD_BUSY);
        if (v != last)
        {
            if (changes < 12) // cap the line length; the count still tells all
                trace += " " + String(millis() - t0) + "ms->" + v;
            last = v;
            changes++;
        }
        delay(stepMs);
    }
    log_i("BUSY probe (pin %d): pre-reset=%d, post-reset=%d, settled=%d, "
          "%d change(s) in %u ms%s",
          EPD_BUSY, before, afterReset, last, changes, (unsigned)windowMs,
          changes ? trace.c_str() : " -- never toggled");
    s_probeMs = millis() - probeStart;
}
#endif

void DisplayRenderer::initPanel_()
{
    // Resolve + create the panel driver on first use (heap; only the selected
    // panel's page buffer is allocated).
    if (_panelId.isEmpty())
        applyPanel_(epdDefaultPanelId());
    if (!gx)
    {
        gx = epdCreatePanel(_panelId.c_str());
        if (!gx) // id not compiled in -> fall back to the default
        {
            applyPanel_(epdDefaultPanelId());
            gx = epdCreatePanel(_panelId.c_str());
        }
    }

    // Configure the control pins as outputs up front. GxEPD2's init()/_reset()
    // deliberately pre-set them with digitalWrite() *before* their own pinMode()
    // ("less glitch"); arduino-esp32 3.x logs that as an "IO N is not set as
    // GPIO" error and skips the pre-set. Doing pinMode() here first makes those
    // writes valid and silences the (otherwise harmless) errors.
    pinMode(EPD_CS, OUTPUT);
    pinMode(EPD_DC, OUTPUT);
    pinMode(EPD_RST, OUTPUT);

#ifdef EPAPER_BUSY_PROBE
    probeBusyLine_(); // diagnostic only; GxEPD2's init() resets the panel again
#endif

    // Remap the VSPI bus to the board's pins *before* init(): GxEPD2's init()
    // calls SPI.begin() internally, which early-returns once the bus is up, so
    // our pins are kept for the data transfers. (SPI.begin attaches SCK/MISO/
    // MOSI only, never the CS pin, so CS stays a plain GPIO for GxEPD2.)
    SPI.end();
    SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);

    gx->init(/*serial_diag_bitrate*/ 0, /*initial*/ true, /*reset_duration*/ 2,
             /*pulldown_rst_mode*/ false);
    gx->setRotation(_rotation);
    gx->setTextColor(GxEPD_BLACK);
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
    initPanel_();
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
    gx->hibernate();
}
