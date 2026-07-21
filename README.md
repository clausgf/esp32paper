# esp32paper

Firmware for the **Waveshare ESP32 e-Paper Driver Board** that shows
[nicepaper](https://github.com/clausgf/nicepaper)-rendered images on an
e-paper panel. It is a battery-friendly, deep-sleep client of a
[nice4iot](https://github.com/clausgf/nice4iot) server, built on
[arduino4iot](https://github.com/clausgf/arduino4iot) and
[GxEPD2](https://github.com/ZinggJM/GxEPD2).

The device does almost nothing itself: nice4iot handles provisioning,
configuration, telemetry, logging and OTA; nicepaper (running as the
nice4iot `epaper` extension) renders the screen — including its **update
schedule** — server-side into a PNG. The firmware wakes up, fetches its
image, paints it, and goes back to sleep.

## How it works

One wakeup cycle (`setup()` runs, then the device deep-sleeps). The order is
tuned for energy — as little as possible happens before the panel starts
refreshing, and the WiFi radio is switched off *during* the slow refresh:

1. **Connect** WiFi + sync NTP, init subsystems — `iot.begin()`
2. **Provision / renew** the device token (usually a no-op) — `api.updateProvisioningOk()`
3. **Download `config.json`** (image path, sleep clamps, color model; usually a
   304) — `config.updateConfig()`
4. **GET the rendered image** from nicepaper with `If-None-Match`/ETag —
   `api.apiGet(".../ext/epaper/{project}/screens/{device}/image.png")` (whole
   compressed PNG into RAM)
5. **Refresh** the panel (paged, PNG re-decoded per page) + a **WiFi/battery
   overlay** top-right — `displayRenderer.renderImage()`. **While the panel is
   busy refreshing** (seconds, CPU idle), a busy-callback runs the housekeeping:
   OTA firmware check, system + app telemetry, log flush — **then switches WiFi
   off** so the radio is not powered for the rest of the refresh.
6. **Deep sleep** for `Cache-Control: max-age` seconds (clamped) — `iot.deepSleep()`

The whole schedule lives on the server: nicepaper computes when a screen
expires and answers step 4 with `Cache-Control: max-age`, which this firmware
uses as its next sleep duration. When nothing changed the server returns
`304 Not Modified` and the panel is not redrawn (saving the most expensive
part of the cycle); the housekeeping then runs inline instead of overlapped.

Any serious failure along the way (no WiFi/NTP, provisioning rejected, no image
or an undecodable PNG) is shown as a **full-screen error page** — icon + title +
message — and the device retries after `error_retry_s` instead of leaving a
blank or stale panel.

```
   ESP32 (esp32paper)                nice4iot server
  ┌───────────────────┐   provision ┌────────────────────────────┐
  │ arduino4iot client │◄───────────►│ auth · config · telemetry  │
  │ GxEPD2 + pngle     │   config    │ logging · OTA · forwarding │
  └─────────┬─────────┘   telemetry  │                            │
            │  GET image.png (ETag)  │  ┌──────────────────────┐  │
            └───────────────────────►│  │ nicepaper (extension)│  │
            ◄─── PNG + Cache-Control ─┤  │ screens · schedules  │  │
                                     │  └──────────────────────┘  │
                                     └────────────────────────────┘
```

## Getting started

Requires [PlatformIO](https://platformio.org/) and a running nice4iot server
with the `epaper` extension enabled, a screen created, and that screen
assigned to this device (which writes the `aliases.json` entry keyed by the
device name — see nicepaper's Architecture doc).

```bash
git clone <this-repo> esp32paper
cd esp32paper
cp include/settings.h.example include/settings.h   # fill in WiFi + nice4iot
pio run -t upload
pio device monitor
```

Edit `include/settings.h` (git-ignored) with your WiFi credentials, the
nice4iot API URL, project name and provisioning token — see [Secrets](#secrets)
for the alternatives.

### Selecting a panel

The default is the **4.2" 400×300 black/white** panel (`color_model=bw`).
Switch panels via `build_flags` in `platformio.ini` — define exactly one of:

| build flag             | panel                        | GxEPD2 driver             | color_model |
|------------------------|------------------------------|---------------------------|-------------|
| `EPAPER_PANEL_42_BW`   | 4.2" 400×300 b/w             | `GxEPD2_420`              | `bw`        |
| `EPAPER_PANEL_75_BW`   | 7.5" 800×480 b/w             | `GxEPD2_750_T7`           | `bw`        |
| `EPAPER_PANEL_75_BWR`  | 7.5" 800×480 b/w/red         | `GxEPD2_750c_Z90`         | `bwr`       |
| `EPAPER_PANEL_73_E6`   | 7.3" 800×480 Spectra 6 (E6)  | `GxEPD2_730c_GDEP073E01`  | `e6`        |

Set the matching `EPAPER_COLOR_MODEL` too (e.g. `-DEPAPER_COLOR_MODEL=\"e6\"`).
The pin mapping is the same for all Waveshare HATs (see `src/config.h`). The
7.3" Spectra 6 renders 6 colours (black/white/red/yellow/blue/green); its
192 KB bitmap makes paging mandatory — handled automatically (below).

### Secrets

WiFi credentials and the nice4iot provisioning token are **never committed**.
The provisioning token is only needed for first-time provisioning — arduino4iot
exchanges it for a device token and stores that in NVRAM, so later builds may
blank it. WiFi credentials are needed on every boot. Two ways to provide them:

1. **`include/settings.h`** (default, git-ignored): `cp include/settings.h.example
   include/settings.h` and fill it in. Values use `#ifndef` guards, so build
   flags still win.
2. **`secrets.ini`** (git-ignored, good for CI): `cp secrets.ini.example
   secrets.ini`, fill it in, and uncomment `extra_configs = secrets.ini` in
   `[platformio]` plus `${secrets.build_flags}` in `[env] build_flags`.

Either way the values are compiled **into the firmware `.bin`** — keeping them
out of git protects the source, not the binary (relevant when distributing OTA
images; see open points). `.gitignore` already excludes both files, and the
build fails with a clear message if any secret is missing.

### Runtime configuration (`config.json`)

Served by nice4iot at `file/{project}/{device}/config.json`; all keys optional:

| key           | type   | default                                                | meaning |
|---------------|--------|--------------------------------------------------------|---------|
| `log_level`   | int    | library default                                        | arduino4iot log verbosity |
| `sleep_s`     | int    | library default                                        | fallback sleep if no `max-age` |
| `color_model` | string | `bw`                                                   | nicepaper palette |
| `image_path`  | string | `ext/epaper/{project}/screens/{device}/image.png`      | image API path template |
| `min_sleep_s` | int    | `300`                                                  | lower clamp on `max-age` sleep |
| `max_sleep_s` | int    | `86400`                                                | upper clamp on `max-age` sleep |
| `error_retry_s`| int   | `900`                                                  | sleep after an error screen |
| `rotation`    | int    | `0`                                                    | GxEPD2 rotation 0..3 |

### Monitoring

All monitoring flows through nice4iot:

- **System telemetry** (`iot.postSystemTelemetry()`): battery, WiFi RSSI, boot
  count, cycle durations, firmware version/sha — the arduino4iot standard set.
- **App telemetry** (`kind = "epaper"`), posted during the refresh overlap
  (DD16). Phases known by then are sent live; phases that only complete after the
  refresh are buffered in RTC RAM and sent next cycle as `last_*` (DD17):
  - live: `connect_ms` (WiFi+NTP), `net_ms` (provision+config+image GET),
    `active_ms` (awake since fetch, to WiFi-off), `image_status`, `image_bytes`,
    `image_maxage_s`, `displayed`, `heap_free`, `sleep_s`.
  - from the previous cycle: `last_cycle_ms` (total awake), `last_refresh_ms`
    (physical panel refresh), `last_decode_transfer_ms` (PNG decode + transfer
    into panel controller memory).
- **Remote logging**: buffered `logger.*` messages flushed during the refresh
  overlap (before WiFi off), not at deep sleep.

### Memory & paged rendering

The Waveshare board carries a plain **ESP32-WROOM-32 (4 MB flash, no PSRAM)**.
Free heap once WiFi is up is roughly **~180 KB**. During a refresh the peak is
the compressed PNG (`String`, ≤ ~40 KB, assumed to fit) + pngle's decoder state
(**~36 KB**, transient — it embeds the 32 KB DEFLATE window) + the GxEPD2 **page
buffer**. The *uncompressed* bitmap is the problem, so we never hold it whole —
GxEPD2 renders in pages and the PNG is re-decoded per page.

**Page height is derived at compile time from a byte budget** (`EPAPER_PAGE_BYTES`,
default 16 KB) rather than a hand-picked divisor, so every panel — including the
192 KB 7-colour bitmap — is safe automatically:

| Panel | full bitmap | bytes/row | pages @16 KB | page buffer | render-time peak¹ |
|---|---|---|---|---|---|
| 4.2" b/w 400×300 | 15 KB | 50 | 1 (full) | 15 KB | ~91 KB |
| 7.5" b/w 800×480 | 48 KB | 100 | 4 | 16 KB | ~92 KB |
| 7.5" b/w/red 800×480 | 96 KB (2 planes) | 200 | 6 | 16 KB | ~92 KB |
| 7.3" Spectra 6 800×480 | 192 KB (4 bpp) | 400 | 12 | 16 KB | ~92 KB |

¹ PNG (~40 KB) + pngle (~36 KB) + page buffer, all comfortably under ~180 KB.
A full-screen buffer for the 7-colour panel (192 KB) would not fit at all — the
reason budget-based paging is the default. Raise `EPAPER_PAGE_BYTES` for fewer
re-decodes when you have spare internal DRAM.

**Why not resize pages at runtime?** GxEPD2's page buffer is a *static member
sized by a template parameter* (and lives in internal DRAM — PSRAM can't back
it), so it is fixed at compile time. The firmware therefore auto-scales the
buffer at build time and, at runtime, logs the page layout + free heap and
**refuses to decode below a safe-heap threshold** (showing an error page) rather
than crashing. `heap_free`, `image_bytes` and the page layout are reported via
telemetry so an under-budgeted build is visible in nice4iot.

Measured firmware size (this build, `min_spiffs.csv` partitions): **flash 61.6 %**
of a 1.875 MB OTA slot, **static RAM ~20 %** across all panels (the 16 KB page
budget keeps the static buffer uniform).

---

## Design decisions

- **DD1 — arduino4iot for everything except pixels.** Provisioning, config,
  telemetry, logging and OTA are delegated to arduino4iot/nice4iot rather than
  reimplemented. The firmware only adds the display path. This is the whole
  point of the x4iot ecosystem and keeps the client tiny.
- **DD2 — Server-side rendering (nicepaper), thin client.** The panel receives
  a finished, palette-quantized PNG. No layouting, fonts, weather/calendar
  fetching or dithering on the device. Schedules live on the server; the
  firmware never parses a schedule.
- **DD3 — Sleep duration comes from the image's `Cache-Control: max-age`.**
  nicepaper already encodes "when does this screen next change" in the HTTP
  cache header, so we reuse it as the sleep time (clamped to
  `min_sleep_s`/`max_sleep_s`). One source of truth for the schedule.
- **DD4 — Direct `apiGet` to the extension endpoint (not forwarding).** The
  image is fetched straight from
  `ext/epaper/{project}/screens/{device}/image.png` via arduino4iot's `apiGet`
  with the device bearer token, so the response `ETag` and `Cache-Control`
  headers survive and re-provisioning-on-401 is handled by the library. The
  arduino4iot `apiForward()` path would drop those response headers and thus the
  schedule/caching signal. The path is configurable, so forwarding remains an
  option if a deployment needs it. The compressed PNG is returned whole in an
  Arduino `String` (assumed to fit — nicepaper's quantized PNGs are small); the
  *uncompressed* bitmap is what may not fit, which DD7 handles.
- **DD5 — Device addresses its screen by its own name.** The URL uses the
  `{device}` placeholder; nicepaper maps it to a screen via `aliases.json`
  (written when you assign a screen in the nice4iot device card). Firmware
  never knows a screen id — re-point a device server-side without reflashing.
- **DD6 — ETag cached in RTC RAM.** The last image ETag is kept in
  `RTC_DATA_ATTR` across deep sleep and sent as `If-None-Match`. On `304` the
  panel is not redrawn, saving the most energy-expensive operation. (RTC RAM
  is lost on power loss, which only costs one extra redraw — acceptable.)
- **DD7 — Paged rendering with a compile-time RAM budget (PNG re-decoded per
  page).** The uncompressed bitmap is large (up to 192 KB for the 7-colour 7.3"
  panel) and need not fit at once on an ESP32 without PSRAM. Instead of an own
  framebuffer we use GxEPD2's paged refresh and re-decode the in-RAM PNG once per
  page, drawing each pixel with `gx.drawPixel()` (which ignores pixels outside the
  current page band and is rotation-aware, so no manual coordinate math). The page
  height is **derived from a byte budget** (`EPAPER_PAGE_BYTES`, default 16 KB, via
  a `constexpr`) instead of a hand-tuned divisor, so a new/larger panel is safe
  automatically. GxEPD2's page buffer is a static, template-sized member (internal
  DRAM, not PSRAM), so it cannot be resized at runtime; the runtime lever is a
  free-heap check that refuses to decode below a safe threshold and reports
  `heap_free` via telemetry. Trade-off: more pages = less RAM but N re-decodes
  (cheap for nicepaper's small PNGs); pngle's ~36 KB state is transient. A cheap
  validation decode runs first so a corrupt PNG never reaches the panel. See
  [Memory & paged rendering](#memory--paged-rendering). Vendored decoder under
  `lib/pngle/`.
- **DD11 — Phone-like status overlay, drawn client-side.** WiFi signal bars and
  a battery gauge are drawn top-right over every image (`drawOverlay_`). They
  reflect live device state (RSSI, measured battery) the server cannot know, and
  are redrawn each GxEPD2 page. On a `304` the panel is not refreshed, so the
  overlay is intentionally left as-is (stale by one cycle) to save the redraw.
- **DD12 — Serious failures are visualized full-screen.** No WiFi/NTP, failed
  provisioning, a missing/unassigned screen, or an undecodable PNG each render a
  full-screen page with an icon (warning triangle, or a struck-through WiFi
  glyph for no-connection) plus a German title and word-wrapped message
  (`showError()`), then the device deep-sleeps for the shorter `error_retry_s`
  interval and tries again. A blank or frozen panel no longer hides a fault.
- **DD13 — Battery percentage from a small LiPo lookup table.** `batteryPercent()`
  maps resting voltage to SoC via a piecewise-linear table — enough for the
  gauge; a load-compensated curve is an open point.
- **DD8 — Compile-time panel selection.** The GxEPD2 driver class and geometry
  are chosen by a single `EPAPER_PANEL_*` build flag; `src/config.h` centralises
  pins and the `AppConfig` defaults. Default = 4.2" b/w for a robust first build.
  Supported: 4.2" b/w, 7.5" b/w, 7.5" b/w/red, and 7.3" Spectra 6 (E6, 6-colour).
- **DD9 — pioarduino platform.** arduino4iot needs arduino-esp32 3.x, only
  available through the pioarduino platform fork — pinned in `platformio.ini`.
- **DD10 — Deep-sleep-first.** `setup()` performs the entire cycle and ends in
  `iot.deepSleep()`; `loop()` is never reached. Battery undervoltage triggers
  `iot.panic()` with escalating backoff.
- **DD14 — Secrets out of git, not out of the binary.** WiFi and the provisioning
  token come from a git-ignored `include/settings.h` (default) or git-ignored
  `secrets.ini`/build flags (CI); `settings.h` values are `#ifndef`-guarded so
  flags win, and a missing secret is a clear compile `#error`. They are still
  compiled into the `.bin` (unavoidable without runtime provisioning); the
  provisioning token is only needed once (then it lives in NVRAM). Runtime WiFi
  provisioning (WiFiManager/NVS) is an open point.
- **DD15 — Colour mapping matches the panel.** nicepaper quantizes server-side to
  the requested `color_model`, so the firmware only classifies already-snapped
  pixels: a luma threshold for b/w, a red test for b/w/red, and a nearest-RGB
  match against the fixed 6-colour palette for Spectra 6 (E6) — no dithering on
  the device.
- **DD16 — Energy: overlap housekeeping with the refresh, WiFi off early.** The
  WiFi radio (~80–120 mA) dominates active energy, and the e-paper refresh is the
  longest phase (seconds; tens of seconds for 7-colour) with the CPU idle on a
  BUSY-wait. So (a) only the essentials run before the refresh — provision (a
  no-op while the token is valid), the cached-config check, and the image GET —
  to minimise time-to-refresh; (b) firmware update, telemetry and log flush are
  deferred into GxEPD2's **busy callback** and run *once* during the refresh
  wait, at which point WiFi is switched off; (c) the sleep duration is computed
  before the refresh so deep sleep follows it immediately. Net: the radio is off
  for essentially the whole refresh, and the housekeeping adds no wall-clock time.
  A run-once guard + a fallback (invoke it after the loop if no BUSY wait
  occurred) keep it correct even on a panel without a BUSY pin. Doing an OTA
  *download* in this window would reboot mid-refresh — harmless (re-rendered next
  boot) but noted. The watchdog is widened to 90 s to cover the long refresh.
- **DD17 — End-of-cycle timings buffered in RTC RAM, sent next cycle as `last_*`.**
  Phase durations that are only complete after the refresh (`refresh_ms`,
  `decode_transfer_ms`, total `cycle_ms`) would otherwise force us to stay awake
  with WiFi on just to report them. Instead they are written to an
  `RTC_DATA_ATTR` struct just before deep sleep and reported as `last_refresh_ms`
  etc. in the next cycle's overlapped telemetry — so the device still sleeps
  immediately. The buffer has a validity `magic`, consumed on send so a
  subsequent error cycle doesn't resend stale values (and it's simply absent on
  the first boot after power-up). The refresh start is timestamped from GxEPD2's
  first busy-callback, which cleanly separates decode+transfer from the physical
  refresh.

## TODO / open points

- [ ] **Compiles, not yet flashed/run on hardware.** All four panel
      configurations build with PlatformIO (pioarduino, arduino-esp32 3.x); the
      firmware has **not** been run on a real display yet. First hardware bring-up
      still needs verification (SPI pins, refresh, overlay placement).
- [ ] **Re-decode CPU vs. RAM.** Paged rendering re-decodes the PNG once per page
      (plus one validation decode). Cheap for nicepaper's small images, but a
      large/complex PNG on a high page count adds latency and active-time energy.
      Raise `EPAPER_PAGE_BYTES` (fewer pages) when the module has spare DRAM.
- [ ] **Colour panels not verified on hardware.** `EPAPER_PANEL_75_BWR` (red
      plane) and `EPAPER_PANEL_73_E6` (Spectra 6 nearest-palette match) compile
      and are wired through, but the colour thresholds/mapping and the paged
      `GxEPD2_3C`/`GxEPD2_7C` paths are untested on a real panel. Older ACeP
      7-colour (`c7`) and the 5.65"/other 7-colour panels are not wired up.
- [ ] **Confirm the extension endpoint accepts the device bearer token.** DD4
      assumes the device token authorizes `GET /api/ext/epaper/...`. If nice4iot
      instead gates it only via per-project activation / an `X-Api-Key`
      (roadmap item in nicepaper), switch to `apiForward` (losing the streamed
      ETag/Cache-Control) and derive sleep from `config.json` only. Path is
      configurable via `image_path`.
- [ ] **Battery curve is approximate.** `batteryPercent()` is a resting-voltage
      lookup table (DD13); under load the reading sags. A load-compensated
      estimate or coulomb counting would be more accurate. Also the Waveshare
      driver board has no battery input by default — `BATTERY_*` in `src/config.h`
      assumes an external 2:1 divider on GPIO34; set `BATTERY_PIN = -1` if unused.
- [ ] **Overlay vs. server-rendered indicators.** nicepaper may itself draw
      status; the client overlay could then double up. Decide whether the overlay
      should be config-toggleable (e.g. an `overlay` key) or positioned in a
      screen-reserved corner.
- [ ] **Error-screen refresh cost.** Every failure does a full refresh; a flapping
      connection could refresh often. Consider suppressing a re-draw when the same
      error repeats (track last error kind in RTC RAM), and/or backing off the
      retry interval. Error refreshes also keep WiFi on (to flush the error log at
      deep sleep) — unlike the success path, they are not energy-optimised.
- [ ] **Verify the refresh/housekeeping overlap on hardware (DD16).** The
      busy-callback timing, that WiFi-off mid-refresh doesn't disturb the panel,
      and the widened 90 s watchdog all need confirming on a real device —
      especially the slow 7-colour refresh. Measure `active_ms` / sleep current to
      confirm the WiFi radio is actually off during the refresh.
- [ ] **Only two error icons, German strings hardcoded.** `ErrorIcon` has
      `Warning`/`NoWifi`; messages are German literals in `main.cpp`. Localisation
      and per-error icons could come from config.
- [ ] **Partial refresh.** Always does a full-window refresh. Screens that
      change little could use GxEPD2 partial updates to cut refresh time/power.
- [ ] **HTTP-only OTA note.** OTA firmware update over plain http needs an IDF
      `CONFIG_OTA_ALLOW_HTTP=y` build; https works out of the box (see
      arduino4iot README). Not configured here.
- [ ] **Partition headroom.** Set to `min_spiffs.csv` (two ~1.9 MB OTA slots);
      firmware is ~61 % of a slot. The unused SPIFFS could be reclaimed with a
      custom table if more app space is ever needed.

## Rendering ownership — using or replacing GxEPD2

How much work is it to render into our own memory and use GxEPD2 only as a
driver, or drop it entirely? Three levels:

**A. Today — GxEPD2 owns buffer + GFX (paged).** GxEPD2 provides the framebuffer,
Adafruit_GFX drawing (used for the overlay and error pages), *and* the driver
(per-panel init/LUT/refresh over SPI). We feed it pixels via `drawPixel` and
re-decode the PNG per page. Zero extra code; the cost is N re-decodes and no
runtime-variable banding.

**B. Own framebuffer, GxEPD2 as driver only — moderate (~1–2 days + per-panel
hardware bring-up).** Keep GxEPD2's low-level `epd2` (`writeImage`/`writeNative`/
`refresh`/`powerOff`/BUSY handling) but render into our own buffer via an
`Adafruit_GFX` subclass (so overlay/error text & shapes still work), then push it
in bands. Gains: **decode the PNG once** (not per page) and choose the band
height **at runtime** from `ESP.getFreeHeap()` — the genuinely dynamic paging
that GxEPD2's compile-time template buffer can't do. Costs: pack each colour
format to the controller's native layout (1 bpp b/w, two planes b/w/red, 4 bpp
7-colour) and match `writeImage`/`writeNative` semantics per panel; needs a real
panel of each type to verify. Worth it if the per-page re-decode CPU (active-time
energy) or true dynamic banding becomes important.

**C. Replace GxEPD2 entirely — high effort, not recommended.** GxEPD2 encapsulates
each controller's power sequencing, init registers, waveform **LUTs**, refresh
commands and BUSY timing — effectively a display-driver library. Re-implementing
that per panel is days-to-weeks each plus waveform/ghosting/temperature debugging
on hardware, for no functional gain here. Only justified for a panel GxEPD2 does
not support or a custom partial-refresh waveform we can't get otherwise.

**Recommendation:** stay on **A** for the current energy goals — DD16 already
removes the dominant waste (WiFi during refresh), and the re-decode CPU is small
next to the refresh + radio. Move to **B** only if profiling shows the re-decode
or fixed banding is a real cost; keep **C** off the table.

## Layout

```
esp32paper/
├── platformio.ini              # platform, libs, panel + page-budget build flags
├── secrets.ini.example         # copy to secrets.ini for build-flag secrets (CI)
├── include/
│   └── settings.h.example      # copy to settings.h (WiFi + nice4iot secrets)
├── lib/pngle/                  # vendored streaming PNG decoder
└── src/
    ├── config.h                # pins, panel selection, AppConfig defaults
    ├── display_renderer.{h,cpp}# GxEPD2 (paged) + pngle → panel, overlay, errors
    └── main.cpp                # the wakeup cycle
```

## Related projects

- [arduino4iot](https://github.com/clausgf/arduino4iot) — ESP32 IoT client library
- [nice4iot](https://github.com/clausgf/nice4iot) — self-hosted IoT server
- [nicepaper](https://github.com/clausgf/nicepaper) — screen renderer / `epaper` extension
- [GxEPD2](https://github.com/ZinggJM/GxEPD2) — e-paper display driver
