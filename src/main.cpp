/**
 * esp32paper — nicepaper e-paper display firmware.
 *
 * One wakeup cycle (setup() runs, then the device deep-sleeps):
 *   1. connect WiFi + sync NTP, init subsystems  ........ iot.begin()
 *   2. provision / renew the device token  .............. api.updateProvisioning()
 *   3. config.json, first boot only (else NVS mirror) ... config.updateConfig()
 *   4. conditional GET of the rendered image  ........... api.getCached()
 *   5. decode PNG + refresh the panel + status overlay .. displayRenderer.renderImage()
 *      overlapping the refresh: OTA check, config refresh, schema, telemetry,
 *      then radio off  ................................. housekeeping()
 *   6. deep sleep until nicepaper's next update  ........ iot.deepSleep()
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
#include <iot.h>
#include <iot_esp_ports.h>
#include <iot_rtc_slot.h>

#include "config.h"
#include "display_renderer.h"
#include "panels.h"

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

static const char *LOG_TAG = "epaper";

// ***************************************************************************
// Runtime configuration (config.json, mirrored to NVS by arduino4iot)
// ***************************************************************************
// Declared once here: arduino4iot clamps/validates server values against the
// metadata and publishes the matching config.schema.json (see housekeeping).
// log_level, sleep_s, battery_min_mv and panic_* are the library's own values.
// Constructed at static init, so they are registered before iot.begin().

static const std::string kPanelEnum = epdSupportedPanelsEnum();

static IotConfigValue<std::string> cvPanel(config, epdDefaultPanelId(), "panel",
    { .title = "Panel",
      .description = "Id of the active panel. Only panels compiled into the firmware are valid.",
      .enumValues = kPanelEnum.c_str() });
static IotConfigValue<std::string> cvRotation(config, "0deg", "rotation",
    { .title = "Rotation",
      .description = "GxEPD2 display rotation.",
      .enumValues = "0deg|90deg|180deg|270deg" });
// {project}/{device} are expanded by arduino4iot; nicepaper resolves {device}
// to a screen via aliases.json.
static IotConfigValue<std::string> cvImagePath(config,
    "ext/epaper/{project}/screens/{device}/image.png", "image_path",
    { .title = "Image path",
      .description = "API path template for the rendered image ({project}/{device} are auto-expanded)." });
static IotConfigValue<int32_t> cvMinSleep(config, 300, "min_sleep_s",
    { .title = "Min sleep (s)",
      .description = "Lower clamp for the Cache-Control max-age derived sleep (seconds).",
      .min = 1 });
static IotConfigValue<int32_t> cvMaxSleep(config, 24 * 3600, "max_sleep_s",
    { .title = "Max sleep (s)",
      .description = "Upper clamp for the Cache-Control max-age derived sleep (seconds).",
      .min = 1 });
static IotConfigValue<int32_t> cvErrorRetry(config, 900, "error_retry_s",
    { .title = "Error retry (s)",
      .description = "Deep-sleep interval in seconds after a full-screen error page.",
      .min = 1 });
static IotConfigValue<int32_t> cvFwCheck(config, 3600, "fw_check_s",
    { .title = "Firmware check (s)",
      .description = "Minimum time between two checks for firmware updates in seconds (0 = every wake).",
      .min = 0 });

// GxEPD2 rotation index for the (enum-validated) "rotation" value.
static int rotationIndex(const std::string &rotation)
{
    if (rotation == "90deg")  return 1;
    if (rotation == "180deg") return 2;
    if (rotation == "270deg") return 3;
    return 0;
}

// ***************************************************************************
// State preserved across deep sleep (and software/watchdog resets; reset to
// T{} after power loss or a firmware with a different layout).
// ***************************************************************************
struct AppRtcState
{
    // Validators of the image currently on the panel, committed only after it
    // was rendered, so a failed render is fetched again instead of 304'd.
    char    imageEtag[96];
    char    imageLastModified[40];

    // Wall-clock time of the last OTA firmware check. The display refreshes far
    // more often than the firmware changes, so the check (a full HTTP
    // round-trip) is throttled to fw_check_s; 0 forces a check.
    int64_t lastFwCheckEpoch;

    // Phase durations that are only known at the very end of a cycle (after the
    // refresh) are buffered here and reported as `last_*` in the NEXT cycle's
    // telemetry, so the device can sleep immediately instead of staying awake
    // with WiFi on just to send them.
    bool    hasLast;            // last_* below not yet reported
    int32_t cycle_ms;           // total awake time of the previous cycle
    int32_t refresh_ms;         // previous physical panel refresh
    int32_t decode_transfer_ms; // previous PNG decode + transfer to panel RAM
    int32_t radio_on_ms;        // previous cycle's WiFi-on window (boot -> WiFi off)
    int32_t deferred_cycles;    // 304 wakes since the last telemetry POST
};
static RTC_NOINIT_ATTR IotRtcSlot<AppRtcState> rtc;

// Copy a validator into its RTC field; one that does not fit is dropped, which
// only makes the next GET unconditional.
template <size_t N>
static void storeValidator(char (&dst)[N], const std::string &value)
{
    if (value.size() < N)
        memcpy(dst, value.c_str(), value.size() + 1);
    else
        dst[0] = '\0';
}

static IotCacheValidators imageValidators()
{
    return { rtc->imageEtag, rtc->imageLastModified };
}

static void commitImageValidators(const IotCacheValidators &v)
{
    storeValidator(rtc->imageEtag, v.etag);
    storeValidator(rtc->imageLastModified, v.lastModified);
}

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
    st.wifiConnected = iot.network().isConnected();
    st.rssi = st.wifiConnected ? iot.network().rssi() : 0;
    if (BATTERY_PIN >= 0)
    {
        st.battery_mV = iot.getBatteryVoltage_mV();
        st.batteryValid = st.battery_mV > 0;
        st.batteryPct = st.batteryValid ? batteryPercent(st.battery_mV) : -1;
    }
    return st;
}

// HTTP status, or the (negative) transport error, for logs and telemetry.
static int statusCode(const IotResult &r)
{
    return r.isTransportError() ? r.transportError : r.httpStatus;
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

// Show a full-screen error, then deep-sleep for a shorter retry interval.
[[noreturn]] static void failScreen(ErrorIcon icon, const String &title,
                                     const String &message)
{
    displayRenderer.showError(icon, title, message, buildStatus());
    logger.error(LOG_TAG, "%s — %s", title.c_str(), message.c_str());
    iot.deepSleep(cvErrorRetry);
    while (true) {} // never reached
}

// ***************************************************************************

void setup()
{
    unsigned long tBoot = millis();
    Serial.begin(115200);

    // --- seed the deployment bootstrap config into NVS (once) ---
    // From build -D defines / settings.h - empty values for seecretless build.
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

    // --- status LED (on while the radio is on) ---
    if (STATUS_LED_PIN >= 0)
    {
        pinMode(STATUS_LED_PIN, OUTPUT);
        digitalWrite(STATUS_LED_PIN, HIGH);
    }

    // --- battery + clock adapters (must be set before iot.begin()) ---
    if (BATTERY_PIN >= 0)
    {
        IotAdcBattery::Config bc;
        bc.pin       = BATTERY_PIN;
        bc.factor    = BATTERY_FACTOR;
        bc.divider   = BATTERY_DIVIDER;
        bc.offset_mV = BATTERY_OFFSET_MV;
#ifdef BATTERY_ADC_ENABLE_PIN
        bc.enablePin = BATTERY_ADC_ENABLE_PIN;
#endif
        static IotAdcBattery battery(bc);
        iot.setBattery(battery);
        iot.setBatteryMin_mV(BATTERY_MIN_MV);
    }
    // NTP sync (only when implausible or the resync interval elapsed): device
    // token renewal before expiry, log timestamps and the fw_check_s throttle.
    static IotNtpClock ntpClock(iotRtcState().ntpLastSyncTime);
    iot.setClock(ntpClock);

    // Reuse the cached DHCP lease on a scan-free fast reconnect to skip the DHCP
    // DORA (~0.3–0.5 s/wake); arduino4iot renews via real DHCP every N wakeups and
    // falls back to DHCP on a stale lease. Must be set before iot.begin().
    iot.wifi().setDhcpCache(true);

    // --- connect WiFi (seeded creds from NVS), init subsystems, sync NTP;
    //     panics on undervoltage. The config mirror is loaded from NVS even if
    //     the connect fails, so an error screen already uses the configured
    //     panel/rotation (compiled-in default on a fresh device). ---
    bool connected = iot.begin();
    displayRenderer.setPanel(cvPanel.get().c_str());
    displayRenderer.setRotation(rotationIndex(cvRotation));
    if (!connected)
        failScreen(ErrorIcon::NoWifi, "No WiFi",
                   "Could not join the WiFi network.\n\n"
                   "A new device must be seed-flashed once.\nRetrying shortly.");
    unsigned long connect_ms = millis() - tBoot; // WiFi connect + NTP sync

    // updateProvisioning() returns a typed result so we can show a matching
    // error screen.
    IotResult prov = api.updateProvisioning();
    if (!prov)
    {
        if (prov.isTransportError())
            failScreen(ErrorIcon::NoWifi, "No server connection",
                       "Cannot reach the nice4iot server.\n\n"
                       "Check the API URL, TLS certificate\nand network.");
        else if (prov.httpStatus == IotResult::STATUS_NO_PROVISIONING_TOKEN)
            failScreen(ErrorIcon::Warning, "No provisioning token",
                       "No provisioning token is configured.\n\n"
                       "Set it in settings.h.");
        else if (prov.httpStatus == 403)
            failScreen(ErrorIcon::Warning, "Provisioning rejected",
                       "The server rejected this device.\n\n"
                       "Check the token, and that the device\n"
                       "is approved and active.");
        else if (prov.httpStatus == IotResult::STATUS_MALFORMED_RESPONSE)
            failScreen(ErrorIcon::Warning, "Provisioning failed",
                       "Unexpected response from the server.\n\n"
                       "Is this a nice4iot API URL?");
        else
            failScreen(ErrorIcon::Warning, "Provisioning failed",
                       String("The server returned status ") + prov.httpStatus +
                       ".\n\nCheck the token and device approval.");
    }

    // --- config: if config.json was mirrored to NVS before (stored ETag or
    //     Last-Modified), use the mirror for this cycle and refresh config.json
    //     in the background (housekeeping) for the NEXT cycle. Otherwise (fresh
    //     flash / erased NVS) fetch it now so the first render already uses the
    //     configured panel/rotation.
    bool haveCachedConfig = !config.etag().empty() || !config.lastModified().empty();
    if (!haveCachedConfig)
    {
        config.updateConfig();
        displayRenderer.setPanel(cvPanel.get().c_str());
        displayRenderer.setRotation(rotationIndex(cvRotation));
    }
    logger.verbose(LOG_TAG, "heap: %u free / %u total, PSRAM %u B",
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapSize(),
                    (unsigned)ESP.getPsramSize());

    // --- conditional GET of the image (ETag / Last-Modified from RTC) ---
    unsigned long t0 = millis();
    IotCachedResponse img = api.getCached(cvImagePath.get(), imageValidators(),
                                          { .headers = { { "Accept", "image/png" } } });
    unsigned long net_ms = millis() - (tBoot + connect_ms); // provision + image GET
    logger.info(LOG_TAG, "image GET %s -> %d (%u bytes, max-age=%d)",
                cvImagePath.get().c_str(), statusCode(img.result),
                (unsigned)img.body.size(), (int)img.maxAge_s);

    // Decide the sleep duration now so deep sleep after the refresh is immediate.
    // This is the schedule-aligned estimate at WiFi-off time (reported in
    // telemetry); it is recomputed just before deepSleep() with the full active
    // time once the refresh has finished. nicepaper sends max-age on 304, too.
    int sleep_s = img.maxAge_s > 0
                      ? alignedSleep_s(img.maxAge_s, millis() - t0, cvMinSleep, cvMaxSleep)
                      : -1;
    if (sleep_s > 0)
        iot.setSleepDuration_s(sleep_s);
    // else: keep the arduino4iot default (config "sleep_s")

    bool displayed = false;
    unsigned long radioOnMs = 0; // WiFi-on window (boot -> WiFi off); set below

    // Network housekeeping meant to overlap the (multi-second) e-paper refresh:
    // a throttled OTA check, config refresh for the next cycle, schema publish,
    // telemetry and log flush over WiFi, then the radio off before the refresh
    // even finishes. Runs exactly once, driven by the display's busy callback
    // (see renderImage).
    auto housekeeping = [&]()
    {
        iot.resetWatchdog();
        // OTA check, throttled to fw_check_s (a real update reboots): the display
        // changes far more often than the firmware. lastFwCheckEpoch == 0 (cold
        // boot, RTC state lost) forces a check.
        int64_t now = (int64_t)time(nullptr);
        if (cvFwCheck <= 0 || rtc->lastFwCheckEpoch == 0 ||
            (now - rtc->lastFwCheckEpoch) >= cvFwCheck)
        {
            rtc->lastFwCheckEpoch = now;
            // One image per board (release asset naming, see build.yml);
            // {board} is IOT_BOARD_ID. The library default is firmware.bin.
            iot.ota.update("file/{project}/{device}/firmware-{board}.bin");
        }
        // Refresh config.json for the NEXT cycle (off this cycle's critical path);
        // the first cycle already fetched it synchronously above, so only the
        // steady state (cache present) refreshes here. The updated values land in
        // NVS for the next wake; the result (usually a 304) is not needed here.
        if (haveCachedConfig)
            config.updateConfig();
        // Publishes config.schema.json only when it changed (hash kept in NVS).
        config.publishSchema();
        iot.telemetry.postSystem();
        IotTelemetry t;
        // This cycle's phases known so far (up to WiFi-off):
        t.add("connect_ms", (int)connect_ms);       // WiFi connect + NTP
        t.add("net_ms", (int)net_ms);               // provision + image GET (config deferred)
        t.add("active_ms", (int)(millis() - t0));   // awake since fetch, to WiFi-off
        t.add("image_status", statusCode(img.result));
        t.add("image_bytes", (int)img.body.size());
        t.add("image_maxage_s", (int)img.maxAge_s);
        t.add("displayed", displayed ? 1 : 0);
        t.add("heap_free", (int)ESP.getFreeHeap());
        t.add("sleep_s", sleep_s > 0 ? sleep_s : iot.getLastSleepDuration_s());
        t.add("panel", displayRenderer.activePanel());        // active panel id
        t.add("supported_panels", displayRenderer.supportedPanels());   // compiled-in panels
        // Previous cycle's end-of-cycle phases, buffered in RTC RAM (see below),
        // plus the 304 wakes counted since the last POST.
        if (rtc->hasLast)
        {
            t.add("last_cycle_ms", rtc->cycle_ms);
            t.add("last_refresh_ms", rtc->refresh_ms);
            t.add("last_decode_transfer_ms", rtc->decode_transfer_ms);
            t.add("last_radio_on_ms", rtc->radio_on_ms);
        }
        t.add("deferred_cycles", rtc->deferred_cycles);
        rtc->hasLast = false;           // consumed — don't resend if this errors out
        rtc->deferred_cycles = 0;
        iot.telemetry.post("epaper", t);

        // Radio off (the single biggest saving) and the LED with it. Flushes the
        // log first; later lines are kept in RTC RAM and sent next cycle.
        radioOnMs = millis() - tBoot; // the radio was on from boot until now
        iot.radioOff();
        if (STATUS_LED_PIN >= 0)
            digitalWrite(STATUS_LED_PIN, LOW);
        logger.info(LOG_TAG, "WiFi off — radio on for %lu ms", radioOnMs);
    };

    // The 7-colour full refresh can take tens of seconds; widen the watchdog so
    // the long BUSY-wait does not trip it (housekeeping also feeds it once).
    iot.startWatchdog(90);

    if (img.result.isNotModified())
    {
        // Nothing to refresh. Housekeeping still runs once so a new firmware
        // version can publish its schema even when the image is unchanged.
        logger.info(LOG_TAG, "image unchanged (304), keeping current frame");
        commitImageValidators(img.validators); // a 304 may resend them
        rtc->deferred_cycles += 1; // keep prior last_* intact; just count
        housekeeping();
    }
    else if (img.result.isOk() && !img.body.empty())
    {
        displayed = true; // committed to rendering; the overlap reports this
        bool ok = displayRenderer.renderImage(
            (const uint8_t *)img.body.data(), img.body.size(),
            buildStatus(), housekeeping);
        if (ok)
        {
            commitImageValidators(img.validators);
        }
        else
        {
            // invalid PNG or wrong size: housekeeping did not run, WiFi still up.
            // Refresh the config before giving up: a wrong panel (hence a wrong
            // canvas size) is itself a likely cause here, and the cached value
            // would otherwise never be replaced -- the device would keep failing
            // on the same stale config forever.
            displayed = false;
            config.updateConfig();
            const String &detail = displayRenderer.renderErrorDetail();
            failScreen(ErrorIcon::Warning, "Image error",
                       detail.length() ? detail
                                       : "The received image\nis not a valid PNG.");
        }
    }
    else if (img.result.isTransportError())
    {
        // transport error (TLS/connection/timeout) — same class as provisioning
        failScreen(ErrorIcon::NoWifi, "No server connection",
                   "Cannot reach the nice4iot server.\n\n"
                   "Check the API URL, TLS certificate\nand network.");
    }
    else
    {
        failScreen(ErrorIcon::Warning, "No image",
                   String("Server responded: ") + img.result.httpStatus + ".\n\n"
                   "Is a screen assigned\nto this device?");
    }

    // A rendered cycle's end-of-cycle phase durations are only complete now
    // (after the refresh); buffer them in RTC RAM so the NEXT cycle reports them
    // as last_* instead of us staying awake with WiFi on to send them. (The 304
    // path counted itself above; error paths already deep-slept.)
    if (displayed)
    {
        rtc->hasLast = true;
        rtc->refresh_ms = (int32_t)displayRenderer.lastRefreshMs();
        rtc->decode_transfer_ms = (int32_t)displayRenderer.lastDecodeTransferMs();
        rtc->radio_on_ms = (int32_t)radioOnMs;
        rtc->cycle_ms = (int32_t)(millis() - tBoot);

        // One-line phase summary (kept in RTC RAM, uploaded next cycle; the
        // durations also ship in telemetry as last_*).
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
    if (img.maxAge_s > 0)
        iot.setSleepDuration_s(
            alignedSleep_s(img.maxAge_s, millis() - t0, cvMinSleep, cvMaxSleep));

    // WiFi is already off and housekeeping done during the refresh — sleep now.
    iot.deepSleep();
}

void loop()
{
    // never reached: setup() ends in deep sleep
}
