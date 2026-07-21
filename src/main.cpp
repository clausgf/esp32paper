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
#include <iot.h>

#include "config.h"
#include "display_renderer.h"

// Secrets come from include/settings.h (git-ignored) and/or build_flags /
// secrets.ini; settings.h only fills in what build flags did not provide.
#if __has_include("settings.h")
#include "settings.h"
#endif
#if !defined(WIFI_SSID) || !defined(WIFI_PASSWORD) || !defined(IOT_API_URL) || \
    !defined(IOT_PROJECT) || !defined(IOT_PROVISIONING_TOKEN)
#error "Missing secrets. Copy include/settings.h.example to include/settings.h \
(git-ignored) and fill it in, or define WIFI_SSID/WIFI_PASSWORD/IOT_API_URL/\
IOT_PROJECT/IOT_PROVISIONING_TOKEN via build_flags or secrets.ini. See README 'Secrets'."
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
};
RTC_DATA_ATTR static RtcTelemetry rtc_tel = {0, 0, 0, 0};

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
    cfg.colorModel   = config.getConfigString("color_model", cfg.colorModel);
    cfg.minSleep_s   = config.getConfigInt32("min_sleep_s", cfg.minSleep_s);
    cfg.maxSleep_s   = config.getConfigInt32("max_sleep_s", cfg.maxSleep_s);
    cfg.errorRetry_s = config.getConfigInt32("error_retry_s", cfg.errorRetry_s);
    cfg.rotation     = config.getConfigInt32("rotation", cfg.rotation);
    return cfg;
}

static int parseMaxAge(const String &cacheControl)
{
    int idx = cacheControl.indexOf("max-age=");
    if (idx < 0) return -1;
    return cacheControl.substring(idx + 8).toInt();
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
    if (cfg.colorModel.length())
        path += "?color_model=" + cfg.colorModel;

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

    // --- configure API access to the nice4iot server ---
    api.setApiUrl(IOT_API_URL);
    api.setProjectName(IOT_PROJECT);
    api.setProvisioningTokenIfEmpty(IOT_PROVISIONING_TOKEN);
#ifdef IOT_CA_CERT
    api.setCACert(IOT_CA_CERT);
#endif

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

    // Default retry interval until config.json is loaded.
    int retry_s = AppConfig{}.errorRetry_s;

    // --- connect WiFi, init subsystems, sync NTP; panics on undervoltage ---
    if (!iot.begin(WIFI_SSID, WIFI_PASSWORD))
    {
        failScreen(ErrorIcon::NoWifi, "Kein WLAN",
                   "Keine Verbindung zum WLAN oder Zeit-Sync fehlgeschlagen. "
                   "Neuer Versuch in Kürze.", retry_s);
    }
    unsigned long connect_ms = millis() - tBoot; // WiFi connect + NTP sync

    if (!api.updateProvisioningOk())
    {
        failScreen(ErrorIcon::Warning, "Provisionierung fehlgeschlagen",
                   "Der Server hat die Anmeldung abgelehnt. Provisioning-Token "
                   "und Projektstatus prüfen.", retry_s);
    }

    // --- only the essentials before the refresh (minimise time-to-refresh) ---
    // updateProvisioningOk() above is usually a no-op (token still valid), and
    // updateConfig() a quick 304. Firmware update + telemetry are deferred to
    // the housekeeping below so they overlap the slow refresh.
    config.updateConfig();
    AppConfig cfg = loadAppConfig();
    retry_s = cfg.errorRetry_s;
    logger.info(LOG_TAG, "heap: %u free / %u total, PSRAM %u B",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapSize(),
                (unsigned)ESP.getPsramSize());
    displayRenderer.setRotation(cfg.rotation);

    unsigned long t0 = millis();
    ImageResult img = fetchImage(cfg);
    unsigned long net_ms = millis() - (tBoot + connect_ms); // provision+config+GET

    // Decide the sleep duration now so deep sleep after the refresh is immediate.
    int sleep_s = img.maxAge;
    if (sleep_s > 0)
    {
        sleep_s = constrain(sleep_s, cfg.minSleep_s, cfg.maxSleep_s);
        iot.setSleepDuration_s(sleep_s);
    } // else: keep the arduino4iot default (config "sleep_s")

    bool displayed = false;

    // Housekeeping to overlap with the (multi-second) e-paper refresh: firmware
    // update, telemetry and log flush over WiFi, then switch the WiFi radio off
    // so it is not powered during the rest of the refresh. Runs exactly once,
    // driven by the display's busy callback (see renderImage).
    auto housekeeping = [&]()
    {
        iot.resetWatchdog();
        api.updateFirmware();        // normally a quick 304; a real update reboots
        iot.postSystemTelemetry();
        IotTelemetry t;
        // This cycle's phases known so far (up to WiFi-off):
        t.add("connect_ms", (int)connect_ms);       // WiFi connect + NTP
        t.add("net_ms", (int)net_ms);               // provision + config + image GET
        t.add("active_ms", (int)(millis() - t0));   // awake since fetch, to WiFi-off
        t.add("image_status", img.status);
        t.add("image_bytes", (int)img.body.length());
        t.add("image_maxage_s", img.maxAge);
        t.add("displayed", displayed ? 1 : 0);
        t.add("heap_free", (int)ESP.getFreeHeap());
        t.add("sleep_s", sleep_s > 0 ? sleep_s : iot.getLastSleepDuration_s());
        // Previous cycle's end-of-cycle phases, buffered in RTC RAM (see below).
        if (rtc_tel.magic == RTC_TEL_MAGIC)
        {
            t.add("last_cycle_ms", rtc_tel.cycle_ms);
            t.add("last_refresh_ms", rtc_tel.refresh_ms);
            t.add("last_decode_transfer_ms", rtc_tel.decode_transfer_ms);
            rtc_tel.magic = 0; // consumed — don't resend if this cycle errors out
        }
        iot.postTelemetry("epaper", t);
        logger.flush();
        // WiFi radio off — the biggest saving, done before the refresh finishes.
        api.closeConnection();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        if (STATUS_LED_PIN >= 0)
            iot.setLed(false);
    };

    // The 7-colour full refresh can take tens of seconds; widen the watchdog so
    // the long BUSY-wait does not trip it (housekeeping also feeds it once).
    iot.startWatchdog(90);

    if (img.status == 304)
    {
        logger.info(LOG_TAG, "image unchanged (304), keeping current frame");
        housekeeping(); // no refresh to overlap; do it inline, then sleep
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
            // invalid PNG: housekeeping did not run, WiFi still up for the error
            displayed = false;
            failScreen(ErrorIcon::Warning, "Bildfehler",
                       "Das empfangene Bild ist kein gültiges PNG.", retry_s);
        }
    }
    else
    {
        failScreen(ErrorIcon::Warning, "Kein Bild",
                   String("Server-Antwort: ") + img.status +
                   ". Bildschirm zugewiesen?", retry_s);
    }

    // Buffer this cycle's end-of-cycle phase durations in RTC RAM; they are only
    // complete now (after the refresh), so the next cycle reports them as last_*
    // instead of us staying awake with WiFi on to send them.
    rtc_tel.magic = RTC_TEL_MAGIC;
    rtc_tel.refresh_ms = (int32_t)displayRenderer.lastRefreshMs();
    rtc_tel.decode_transfer_ms = (int32_t)displayRenderer.lastDecodeTransferMs();
    rtc_tel.cycle_ms = (int32_t)(millis() - tBoot);

    // WiFi is already off and housekeeping done during the refresh — sleep now.
    iot.deepSleep();
}

void loop()
{
    // never reached: setup() ends in deep sleep
}
