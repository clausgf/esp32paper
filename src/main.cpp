/**
 * esp32paper — nicepaper e-paper display firmware.
 *
 * One wakeup cycle (setup() runs, then the device deep-sleeps):
 *   1. connect WiFi + sync NTP, init subsystems  ........ iot.begin()
 *   2. provision / renew the device token  .............. api.updateProvisioningOk()
 *   3. download config.json (image path, sleep clamps) .. config.updateConfig()
 *   4. OTA firmware update check  ....................... api.updateFirmware()
 *   5. post system telemetry (monitoring)  .............. iot.postSystemTelemetry()
 *   6. GET the rendered image from nicepaper (ETag) ..... api.apiGet()
 *   7. decode PNG + refresh the panel + status overlay .. displayRenderer.renderImage()
 *   8. post app telemetry, then deep sleep  ............. iot.deepSleep()
 *
 * The compressed PNG is fetched whole into RAM (assumed to fit); the panel is
 * refreshed with GxEPD2's paged rendering, so the full uncompressed bitmap
 * never has to fit at once (see README.md memory notes).
 *
 * Serious failures (no WiFi, provisioning failed, no/invalid image) are shown
 * as a full-screen error page instead of silently sleeping, and the device
 * retries after a shorter interval.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <iot.h>

#include "config.h"
#include "display_renderer.h"

// Deployment bootstrap values (WiFi, endpoint, project, provisioning token, TLS
// trust) are seeded into NVS on first boot. A build WITH these -D defines /
// settings.h is flashed once to seed a device; every later build can be
// SECRETLESS (empty defaults below) and reuses the values already in NVS — so
// OTA updates, and a public CI, need no secrets. See README 'Secrets'.
#if __has_include("settings.h")
#include "settings.h"
#endif
#ifndef IOT_WIFI_SSID
#define IOT_WIFI_SSID ""
#endif
#ifndef IOT_WIFI_PASSWORD
#define IOT_WIFI_PASSWORD ""
#endif
#ifndef IOT_API_URL
#define IOT_API_URL ""
#endif
#ifndef IOT_PROJECT
#define IOT_PROJECT ""
#endif
#ifndef IOT_PROVISIONING_TOKEN
#define IOT_PROVISIONING_TOKEN ""
#endif
#ifndef IOT_SEED_GENERATION
#define IOT_SEED_GENERATION 0
#endif

// ***************************************************************************
// State preserved across deep sleep (survives sleep, not power loss).
// ***************************************************************************
RTC_DATA_ATTR static char rtc_image_etag[96] = {0};

// Phase durations that are only known at the very end of a cycle (after the
// refresh) are buffered here and reported as `last_*` in the NEXT cycle's
// telemetry, so the device can sleep immediately instead of staying awake with
// WiFi on just to send them. `magic` marks the buffer valid/unconsumed.
static const uint32_t RTC_TEL_MAGIC = 0x45503154; // "EP1T"
struct RtcTelemetry
{
    uint32_t magic;
    int32_t  cycle_ms;           // total awake time of the previous cycle
    int32_t  refresh_ms;         // previous physical panel refresh
    int32_t  decode_transfer_ms; // previous PNG decode + transfer to panel RAM
    int32_t  radio_on_ms;        // previous cycle's WiFi-on window (boot -> WiFi off)
    int32_t  deferred_cycles;    // 304 wakes since the last telemetry POST
};
RTC_DATA_ATTR static RtcTelemetry rtc_tel = {0, 0, 0, 0, 0, 0};

// Wall-clock time of the last OTA firmware check. The display refreshes far more
// often than the firmware changes, so the check (a full HTTP round-trip) is
// throttled to fw_check_s instead of running every wake — kept out of the
// radio-on window on most cycles.
RTC_DATA_ATTR static int64_t rtc_lastFwCheckEpoch = 0;

static const char *LOG_TAG = "epaper";

// ***************************************************************************
// Helpers
// ***************************************************************************

// Rough LiPo state-of-charge from the resting voltage. Good enough for a
// battery icon; a proper curve/coulomb counter is an open point (see README).
static int batteryPercent(int mV)
{
    static const int lut_mV[]  = {3300, 3600, 3700, 3750, 3790, 3830, 3870,
                                  3920, 3980, 4060, 4200};
    static const int lut_pct[] = {0,    5,    10,   20,   30,   40,   50,
                                  60,   70,   85,   100};
    if (mV <= lut_mV[0]) return 0;
    for (size_t i = 1; i < sizeof(lut_mV) / sizeof(lut_mV[0]); i++)
    {
        if (mV < lut_mV[i])
        {
            int span = lut_mV[i] - lut_mV[i - 1];
            int into = mV - lut_mV[i - 1];
            return lut_pct[i - 1] +
                   (lut_pct[i] - lut_pct[i - 1]) * into / span;
        }
    }
    return 100;
}

static DisplayStatus buildStatus()
{
    DisplayStatus st;
    st.wifiConnected = (WiFi.status() == WL_CONNECTED);
    st.rssi = st.wifiConnected ? WiFi.RSSI() : 0;
    if (BATTERY_PIN >= 0)
    {
        st.battery_mV = iot.getBatteryVoltage_mV();
        st.batteryValid = st.battery_mV > 0;
        st.batteryPct = st.batteryValid ? batteryPercent(st.battery_mV) : -1;
    }
    return st;
}

static AppConfig loadAppConfig()
{
    AppConfig cfg;
    cfg.imagePath    = config.getConfigString("image_path", cfg.imagePath);
    cfg.panel        = config.getConfigString("panel", cfg.panel);
    cfg.minSleep_s   = config.getConfigInt32("min_sleep_s", cfg.minSleep_s);
    cfg.maxSleep_s   = config.getConfigInt32("max_sleep_s", cfg.maxSleep_s);
    cfg.errorRetry_s = config.getConfigInt32("error_retry_s", cfg.errorRetry_s);
    cfg.rotation     = config.getConfigInt32("rotation", cfg.rotation);
    cfg.fwCheck_s    = config.getConfigInt32("fw_check_s", cfg.fwCheck_s);
    return cfg;
}

// Panel id persisted in NVS (namespace "epaper") so pre-config error screens use
// the right geometry after the first successful config on a multi-panel build.
static String nvsGetPanel()
{
    Preferences p;
    p.begin("epaper", /*readonly*/ true);
    String id = p.getString("panel", "");
    p.end();
    return id;
}
static void nvsSetPanel(const String &id)
{
    if (id.isEmpty()) return;
    Preferences p;
    p.begin("epaper", false);
    if (p.getString("panel", "") != id) p.putString("panel", id);
    p.end();
}

// "Has config.json ever been fetched successfully?" — persisted in NVS so it
// survives power loss. When false (fresh flash / erased NVS) the first cycle
// fetches config synchronously so the very first render already uses the
// configured panel/rotation; afterwards config is refreshed in the background
// (see setup()), off the pre-fetch critical path.
static bool nvsConfigSeen()
{
    Preferences p;
    p.begin("epaper", /*readonly*/ true);
    bool v = p.getBool("cfg_seen", false);
    p.end();
    return v;
}
static void nvsSetConfigSeen()
{
    Preferences p;
    p.begin("epaper", false);
    if (!p.getBool("cfg_seen", false)) p.putBool("cfg_seen", true);
    p.end();
}

static int parseMaxAge(const String &cacheControl)
{
    int idx = cacheControl.indexOf("max-age=");
    if (idx < 0) return -1;
    return cacheControl.substring(idx + 8).toInt();
}

// Schedule-aligned sleep duration.
//
// nicepaper's Cache-Control max-age counts from when the server generated the
// response (≈ our image fetch). Sleeping the raw max-age from cycle *end* would
// push each wake late by the whole active window (WiFi connect + provisioning +
// the multi-second, on 7-colour panels tens-of-seconds, refresh), stretching
// the real update cadence beyond what nicepaper intends — most noticeable at
// short intervals (a 60 s max-age with a 25 s active window updates every 85 s).
//
// Instead we sleep the time remaining until fetch_time + max-age, i.e. max-age
// minus the time already spent this cycle since the fetch, so the next wake
// lands on nicepaper's schedule regardless of how long this cycle took. A small
// margin keeps us from waking a hair early (which would just fetch a 304 and
// re-sleep). If the active time alone exceeds max-age (slow panel, short
// interval) the result clamps to min_sleep_s — the panel simply cannot refresh
// any faster.
static int alignedSleep_s(int maxAge, unsigned long sinceFetchMs,
                          int minSleep, int maxSleep)
{
    const long MARGIN_S = 2;
    long elapsed = (long)(sinceFetchMs / 1000);
    long s = (long)maxAge - elapsed + MARGIN_S;
    if (s < minSleep) s = minSleep;
    if (s > maxSleep) s = maxSleep;
    return (int)s;
}

// ***************************************************************************
// Image fetch (whole PNG into RAM via arduino4iot)
// ***************************************************************************

struct ImageResult
{
    int    status = -1;    // HTTP status, or negative on transport error
    int    maxAge = -1;    // Cache-Control max-age in seconds, -1 if absent
    String etag;
    String body;           // the compressed PNG (empty on 304 / error)
};

static ImageResult fetchImage(const AppConfig &cfg)
{
    ImageResult r;

    String path = cfg.imagePath;

    std::map<String, String> reqHeaders;
    reqHeaders["Accept"] = "image/png";
    if (rtc_image_etag[0] != '\0')
        reqHeaders["If-None-Match"] = rtc_image_etag;

    std::map<String, String> respHeaders;
    r.status = api.apiGet(r.body, respHeaders, path,
                          /*collect*/ {"ETag", "Cache-Control"},
                          /*body*/ "", reqHeaders);
    r.maxAge = parseMaxAge(respHeaders["Cache-Control"]);
    r.etag   = respHeaders["ETag"];
    logger.info(LOG_TAG, "image GET %s -> %d (%u bytes, max-age=%d)",
                path.c_str(), r.status, (unsigned)r.body.length(), r.maxAge);
    return r;
}

// ***************************************************************************

// Show a full-screen error, then deep-sleep for a shorter retry interval.
[[noreturn]] static void failScreen(ErrorIcon icon, const String &title,
                                     const String &message, int retry_s)
{
    displayRenderer.showError(icon, title, message, buildStatus());
    logger.error(LOG_TAG, "%s — %s", title.c_str(), message.c_str());
    iot.deepSleep(retry_s, /*panic*/ false);
    while (true) {} // never reached
}

// ***************************************************************************

void setup()
{
    unsigned long tBoot = millis();
    Serial.begin(115200);

    // Register the app config keys with arduino4iot's IotConfig, else
    // updateConfig() ignores them (only registered keys are stored in NVRAM) and
    // getConfig*() would always return the default. Types must match the JSON;
    // `static` so each registration (a pointer to the value) outlives setup().
    // log_level and sleep_s are registered by the library itself.
    {
        const AppConfig d;
        static IotConfigValue<String>  cvPanel(config, d.panel, "panel");
        static IotConfigValue<String>  cvImagePath(config, d.imagePath, "image_path");
        static IotConfigValue<int32_t> cvRotation(config, d.rotation, "rotation");
        static IotConfigValue<int32_t> cvMinSleep(config, d.minSleep_s, "min_sleep_s");
        static IotConfigValue<int32_t> cvMaxSleep(config, d.maxSleep_s, "max_sleep_s");
        static IotConfigValue<int32_t> cvErrorRetry(config, d.errorRetry_s, "error_retry_s");
        static IotConfigValue<int32_t> cvFwCheck(config, d.fwCheck_s, "fw_check_s");
    }

    // --- seed the deployment bootstrap config into NVS (once) ---
    // On a build carrying the -D defines / settings.h this writes WiFi, endpoint,
    // project, token and TLS trust to NVS (seed-if-absent; bump IOT_SEED_GENERATION
    // to force-update changed values). A secretless build passes empty values — a
    // no-op that reuses the NVS already on the device — so OTA images carry no
    // secrets. iot.begin() / api.begin() below read this config back from NVS.
    {
        IotSeedConfig seed;
        seed.wifiSsid          = IOT_WIFI_SSID;
        seed.wifiPassword      = IOT_WIFI_PASSWORD;
        seed.apiUrl            = IOT_API_URL;
        seed.projectName       = IOT_PROJECT;
        seed.provisioningToken = IOT_PROVISIONING_TOKEN;
        seed.seedGeneration    = IOT_SEED_GENERATION;
        // TLS server trust for an https:// API URL (no-op for http://):
#if defined(IOT_CA_CERT)
        seed.tlsMode   = IotTlsMode::CaPin;    // pin a self-hosted/self-signed CA
        seed.caCertPem = IOT_CA_CERT;
#elif defined(IOT_TLS_CA_BUNDLE)
        seed.tlsMode = IotTlsMode::Bundle;     // built-in public root CA bundle
#elif defined(IOT_TLS_INSECURE)
        seed.tlsMode = IotTlsMode::Insecure;   // unverified — home lab only
#endif
        iot.seedCredentials(seed);
    }

    // --- battery + status LED (must be set before iot.begin()) ---
    // Guards are runtime `if`s because the pins are const ints, not macros;
    // the compiler folds them away for the compile-time constant.
    if (STATUS_LED_PIN >= 0)
    {
        iot.setLedPin(STATUS_LED_PIN);
        iot.setLed(true);
    }
    if (BATTERY_PIN >= 0)
    {
        iot.setBattery(BATTERY_PIN, BATTERY_FACTOR, BATTERY_DIVIDER, BATTERY_OFFSET_MV);
        iot.setBatteryMin_mV(BATTERY_MIN_MV);
    }

    // Reuse the cached DHCP lease on a scan-free fast reconnect to skip the DHCP
    // DORA (~0.3–0.5 s/wake); arduino4iot renews via real DHCP every N wakeups and
    // falls back to DHCP on a stale lease. Must be set before iot.begin().
    iot.setDhcpCache(true);

    // --- panel: for a multi-panel build, use the NVS-persisted panel so a
    //     pre-config error screen renders on the right geometry ("" keeps the
    //     compiled-in default). config.json refines this below (best effort:
    //     the very first boot before any config uses the default).
    displayRenderer.setPanel(nvsGetPanel());

    // Default retry interval until config.json is loaded.
    int retry_s = AppConfig{}.errorRetry_s;

    // --- connect WiFi (seeded creds from NVS), init subsystems, sync NTP;
    //     panics on undervoltage ---
    if (!iot.begin())
    {
        if (WiFi.status() != WL_CONNECTED)
            failScreen(ErrorIcon::NoWifi, "No WiFi",
                       "Could not join the WiFi network.\n\n"
                       "A new device must be seed-flashed once.\nRetrying shortly.", retry_s);
        else
            failScreen(ErrorIcon::Warning, "No time sync",
                       "WiFi is up but NTP time sync failed.\n\nRetrying shortly.", retry_s);
    }
    unsigned long connect_ms = millis() - tBoot; // WiFi connect + NTP sync

    // updateProvisioning() returns a typed result so we can show a matching
    // error screen (arduino4iot >= the updateProvisioning() API).
    IotResult prov = api.updateProvisioning();
    if (!prov)
    {
        if (prov.isTransportError())
            failScreen(ErrorIcon::NoWifi, "No server connection",
                       "Cannot reach the nice4iot server.\n\n"
                       "Check the API URL, TLS certificate\nand network.", retry_s);
        else if (prov.httpStatus == IotResult::STATUS_NO_PROVISIONING_TOKEN)
            failScreen(ErrorIcon::Warning, "No provisioning token",
                       "No provisioning token is configured.\n\n"
                       "Set it in settings.h.", retry_s);
        else if (prov.httpStatus == 403)
            failScreen(ErrorIcon::Warning, "Provisioning rejected",
                       "The server rejected this device.\n\n"
                       "Check the token, and that the device\n"
                       "is approved and active.", retry_s);
        else if (prov.httpStatus == IotResult::STATUS_MALFORMED_RESPONSE)
            failScreen(ErrorIcon::Warning, "Provisioning failed",
                       "Unexpected response from the server.\n\n"
                       "Is this a nice4iot API URL?", retry_s);
        else
            failScreen(ErrorIcon::Warning, "Provisioning failed",
                       String("The server returned status ") + prov.httpStatus +
                       ".\n\nCheck the token and device approval.", retry_s);
    }

    // --- config: use the NVS-cached config from the previous cycle for this
    //     fetch and refresh config.json in the background (housekeeping) for the
    //     NEXT cycle, keeping its HTTP round-trip off the pre-refresh critical
    //     path. Provisioning above is usually a no-op (token still valid) and
    //     costs no round-trip; firmware update + telemetry are likewise deferred
    //     into the housekeeping so they overlap the slow refresh.
    //     Exception: the very first cycle (no cached config yet) fetches config
    //     synchronously so the first render already uses the configured panel.
    bool haveCachedConfig = nvsConfigSeen();
    if (!haveCachedConfig && config.updateConfig().isOkOrNotModified())
        nvsSetConfigSeen(); // bootstrapped; next cycle uses the cache + bg refresh
    AppConfig cfg = loadAppConfig();
    retry_s = cfg.errorRetry_s;
    logger.info(LOG_TAG, "heap: %u free / %u total, PSRAM %u B",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapSize(),
                (unsigned)ESP.getPsramSize());
    displayRenderer.setRotation(cfg.rotation);
    if (cfg.panel.length())
    {
        displayRenderer.setPanel(cfg.panel);
        nvsSetPanel(displayRenderer.activePanel()); // remember for next boot
    }

    unsigned long t0 = millis();
    ImageResult img = fetchImage(cfg);
    unsigned long net_ms = millis() - (tBoot + connect_ms); // provision + image GET

    // Decide the sleep duration now so deep sleep after the refresh is immediate.
    // This is the schedule-aligned estimate at WiFi-off time (reported in
    // telemetry); it is recomputed just before deepSleep() with the full active
    // time once the refresh has finished.
    int sleep_s = img.maxAge > 0
                      ? alignedSleep_s(img.maxAge, millis() - t0,
                                       cfg.minSleep_s, cfg.maxSleep_s)
                      : -1;
    if (sleep_s > 0)
        iot.setSleepDuration_s(sleep_s);
    // else: keep the arduino4iot default (config "sleep_s")

    bool displayed = false;
    unsigned long radioOnMs = 0; // WiFi-on window (boot -> WiFi off); set below

    // Switch the WiFi radio off (the single biggest saving) and the LED with it.
    auto radioOff = [&]()
    {
        radioOnMs = millis() - tBoot; // the radio was on from boot until now
        api.closeConnection();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        if (STATUS_LED_PIN >= 0)
            iot.setLed(false);
        logger.info(LOG_TAG, "WiFi off — radio on for %lu ms", radioOnMs);
    };

    // Network housekeeping meant to overlap the (multi-second) e-paper refresh:
    // a throttled OTA check, config refresh for the next cycle, telemetry and log
    // flush over WiFi, then the radio off before the refresh even finishes. Runs
    // exactly once, driven by the display's busy callback (see renderImage).
    auto housekeeping = [&]()
    {
        iot.resetWatchdog();
        // OTA check, throttled to fw_check_s (a real update reboots): the display
        // changes far more often than the firmware. time() is valid here (NTP
        // synced in iot.begin); rtc_lastFwCheckEpoch==0 forces a check on cold boot.
        int64_t now = (int64_t)time(nullptr);
        if (cfg.fwCheck_s <= 0 || rtc_lastFwCheckEpoch == 0 ||
            (now - rtc_lastFwCheckEpoch) >= cfg.fwCheck_s)
        {
            rtc_lastFwCheckEpoch = now;
            api.updateFirmware();
        }
        // Refresh config.json for the NEXT cycle (off this cycle's critical path);
        // the first cycle already fetched it synchronously above, so only the
        // steady state (cache present) refreshes here. The updated values land in
        // NVS for the next wake; the result (usually a 304) is not needed here.
        if (haveCachedConfig)
            config.updateConfig();
        iot.postSystemTelemetry();
        IotTelemetry t;
        // This cycle's phases known so far (up to WiFi-off):
        t.add("connect_ms", (int)connect_ms);       // WiFi connect + NTP
        t.add("net_ms", (int)net_ms);               // provision + image GET (config deferred)
        t.add("active_ms", (int)(millis() - t0));   // awake since fetch, to WiFi-off
        t.add("image_status", img.status);
        t.add("image_bytes", (int)img.body.length());
        t.add("image_maxage_s", img.maxAge);
        t.add("displayed", displayed ? 1 : 0);
        t.add("heap_free", (int)ESP.getFreeHeap());
        t.add("sleep_s", sleep_s > 0 ? sleep_s : iot.getLastSleepDuration_s());
        t.add("panel", displayRenderer.activePanel());        // active panel id
        t.add("panels", displayRenderer.supportedPanels());   // compiled-in panels
        // Previous cycle's end-of-cycle phases, buffered in RTC RAM (see below),
        // plus any silent 304 wakes skipped since the last POST.
        if (rtc_tel.magic == RTC_TEL_MAGIC)
        {
            t.add("last_cycle_ms", rtc_tel.cycle_ms);
            t.add("last_refresh_ms", rtc_tel.refresh_ms);
            t.add("last_decode_transfer_ms", rtc_tel.decode_transfer_ms);
            t.add("last_radio_on_ms", rtc_tel.radio_on_ms);
            t.add("deferred_cycles", rtc_tel.deferred_cycles);
            rtc_tel.magic = 0;          // consumed — don't resend if this errors out
            rtc_tel.deferred_cycles = 0;
        }
        iot.postTelemetry("epaper", t);
        logger.flush();
        radioOff();
    };

    // The 7-colour full refresh can take tens of seconds; widen the watchdog so
    // the long BUSY-wait does not trip it (housekeeping also feeds it once).
    iot.startWatchdog(90);

    if (img.status == 304)
    {
        // Nothing to refresh -> no slow phase to hide the network housekeeping
        // behind, so skip it (OTA check, telemetry, log flush) to keep the
        // radio-on window minimal. Just count this silent wake so the next
        // telemetry POST (on a cycle that does refresh) reports it; config/OTA
        // catch up then.
        logger.info(LOG_TAG, "image unchanged (304), keeping current frame");
        if (rtc_tel.magic != RTC_TEL_MAGIC) // nothing buffered yet -> init
        {
            rtc_tel.magic = RTC_TEL_MAGIC;
            rtc_tel.cycle_ms = rtc_tel.refresh_ms = rtc_tel.decode_transfer_ms = 0;
            rtc_tel.deferred_cycles = 0;
        }
        rtc_tel.deferred_cycles += 1; // keep prior last_* intact; just count
        radioOff();
    }
    else if (img.status == 200 && img.body.length() > 0)
    {
        displayed = true; // committed to rendering; the overlap reports this
        bool ok = displayRenderer.renderImage(
            (const uint8_t *)img.body.c_str(), img.body.length(),
            buildStatus(), housekeeping);
        if (ok)
        {
            strncpy(rtc_image_etag, img.etag.c_str(), sizeof(rtc_image_etag) - 1);
            rtc_image_etag[sizeof(rtc_image_etag) - 1] = '\0';
        }
        else
        {
            // invalid PNG or wrong size: housekeeping did not run, WiFi still up
            displayed = false;
            const String &detail = displayRenderer.renderErrorDetail();
            failScreen(ErrorIcon::Warning, "Image error",
                       detail.length() ? detail
                                       : "The received image\nis not a valid PNG.",
                       retry_s);
        }
    }
    else if (img.status < 0)
    {
        // transport error (TLS/connection/timeout) — same class as provisioning
        failScreen(ErrorIcon::NoWifi, "No server connection",
                   "Cannot reach the nice4iot server.\n\n"
                   "Check the API URL, TLS certificate\nand network.", retry_s);
    }
    else
    {
        failScreen(ErrorIcon::Warning, "No image",
                   String("Server responded: ") + img.status + ".\n\n"
                   "Is a screen assigned\nto this device?", retry_s);
    }

    // A rendered cycle's end-of-cycle phase durations are only complete now
    // (after the refresh); buffer them in RTC RAM so the NEXT cycle reports them
    // as last_* instead of us staying awake with WiFi on to send them. Reset the
    // deferred-wake counter since housekeeping just posted it. (The 304 path did
    // its own RTC bookkeeping above; error paths already deep-slept.)
    if (displayed)
    {
        rtc_tel.magic = RTC_TEL_MAGIC;
        rtc_tel.refresh_ms = (int32_t)displayRenderer.lastRefreshMs();
        rtc_tel.decode_transfer_ms = (int32_t)displayRenderer.lastDecodeTransferMs();
        rtc_tel.radio_on_ms = (int32_t)radioOnMs;
        rtc_tel.cycle_ms = (int32_t)(millis() - tBoot);
        rtc_tel.deferred_cycles = 0;

        // One-line phase summary on serial (durations also ship in telemetry as
        // last_* next cycle, so the phases are reconstructable without the log).
        logger.info(LOG_TAG,
                    "cycle phases [ms]: connect=%u net=%u decode_transfer=%u "
                    "refresh=%u radio_on=%u active=%u",
                    (unsigned)connect_ms, (unsigned)net_ms,
                    (unsigned)displayRenderer.lastDecodeTransferMs(),
                    (unsigned)displayRenderer.lastRefreshMs(),
                    (unsigned)radioOnMs, (unsigned)(millis() - tBoot));
    }

    // Recompute the schedule-aligned sleep now that the full active window
    // (including the refresh) is known, so the next wake lands on nicepaper's
    // schedule rather than drifting late by the refresh time.
    if (img.maxAge > 0)
        iot.setSleepDuration_s(
            alignedSleep_s(img.maxAge, millis() - t0, cfg.minSleep_s, cfg.maxSleep_s));

    // WiFi is already off and housekeeping done during the refresh — sleep now.
    iot.deepSleep();
}

void loop()
{
    // never reached: setup() ends in deep sleep
}
