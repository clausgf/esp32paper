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
 * Serious failures are shown as a full-screen error page instead of silently
 * sleeping. Before the server is reachable (no WiFi, provisioning failed) the
 * device panics into arduino4iot's escalating backoff (panic_* config); after
 * that (no/invalid image) it retries after ERROR_RETRY_S.
 */

#include <Arduino.h>
#include <esp_heap_caps.h>
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
// {project}/{device} are expanded by arduino4iot; nicepaper resolves {device}
// to a screen via aliases.json.
static IotConfigValue<std::string> cvImagePath(config,
    "ext/epaper/{project}/screens/{device}/image.png", "image_path",
    { .title = "Image path",
      .description = "API path template for the rendered image ({project}/{device} are auto-expanded)." });
static IotConfigValue<int32_t> cvMinSleep(config, 60, "min_sleep_s",
    { .title = "Min sleep (s)",
      .description = "Lower clamp for the Cache-Control max-age derived sleep (seconds).",
      .min = 1 });
static IotConfigValue<int32_t> cvMaxSleep(config, 24 * 3600, "max_sleep_s",
    { .title = "Max sleep (s)",
      .description = "Upper clamp for the Cache-Control max-age derived sleep (seconds).",
      .min = 1 });
static IotConfigValue<int32_t> cvFwCheck(config, 3600, "fw_check_s",
    { .title = "Firmware check (s)",
      .description = "Minimum time between two checks for firmware updates in seconds (0 = every wake).",
      .min = 0 });

// Deep-sleep interval after an error screen once the server was reachable
// (no/invalid image). Earlier failures use the library's panic backoff.
static const int32_t ERROR_RETRY_S = 15 * 60;

// ***************************************************************************
// State preserved across deep sleep (and software/watchdog resets; reset to
// T{} after power loss or a firmware with a different layout).
// ***************************************************************************
struct AppRtcState
{
    // Validators of the image currently on the panel, committed only after it
    // was rendered, so a failed render is fetched again instead of 304'd.
    IotRtcValidators<> image;

    // Wall-clock time of the last OTA firmware check. The display refreshes far
    // more often than the firmware changes, so the check (a full HTTP
    // round-trip) is throttled to fw_check_s; 0 forces a check.
    int64_t lastFwCheckEpoch;

    // Phase durations only known at the very end of a cycle (after the refresh,
    // radio off), posted with the NEXT cycle's telemetry as last_<key>.
    IotRtcTelemetry<5> next;
};
static RTC_NOINIT_ATTR IotRtcSlot<AppRtcState> rtc;

#ifdef DEV_NO_DEEP_SLEEP
// Development only (env seeed_xiao_esp32s3_dev): deep sleep takes the native
// USB console down, so wait instead (capped to DEV_NO_DEEP_SLEEP seconds) and
// restart. RTC_NOINIT state survives the software reset like a deep-sleep
// wakeup, and a software reset does not count as an abnormal start.
class DevPower : public IotEspPower
{
public:
    void deepSleep(int32_t duration_s) override
    {
        const int32_t wait_s = duration_s < DEV_NO_DEEP_SLEEP ? duration_s : DEV_NO_DEEP_SLEEP;
        Serial.printf("[dev] deep sleep %ld s -> wait %ld s + restart\n",
                      (long)duration_s, (long)wait_s);
        Serial.flush();
        iot.stopWatchdog();
        delay((uint32_t)wait_s * 1000);
        restart();
    }
};
#endif

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
        st.battery_mV = iot.battery_mV();
        st.batteryValid = st.battery_mV > 0;
        st.batteryPct = st.batteryValid ? batteryPercent(st.battery_mV) : -1;
    }
    st.firmwareVersion = iot.firmwareVersion().c_str();
    st.timestamp = iot.timeIso().c_str();
    return st;
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

static void setStatusLed(bool on)
{
    if (STATUS_LED_PIN >= 0)
        digitalWrite(STATUS_LED_PIN, on == STATUS_LED_ON ? HIGH : LOW);
}

// Radio off (the single biggest saving) and the LED with it, once per cycle.
// Flushes the log first; later lines are kept in RTC RAM and sent next cycle.
static unsigned long s_radioOnMs = 0; // WiFi-on window (boot -> WiFi off)
static void radioOff()
{
    if (s_radioOnMs)
        return;
    s_radioOnMs = millis(); // the radio was on from boot until now
    iot.radioOff();
    setStatusLed(false);
    logger.info(LOG_TAG, "WiFi off — radio on for %lu ms", s_radioOnMs);
}

// Log a failed network step to nice4iot (no-op on success/304).
static void logIfFailed(const IotResult &r, const char *what)
{
    if (!r.isOkOrNotModified())
        logger.error(LOG_TAG, "%s failed (%d)", what, r.code());
}

// Select the configured panel for the next draw.
static void applyDisplayConfig()
{
    if (!displayRenderer.setPanel(cvPanel.get().c_str()))
        logger.error(LOG_TAG, "panel '%s' is not compiled in, using '%s'",
                     cvPanel.get().c_str(), displayRenderer.activePanel().c_str());
}

// Showstopper: log it to nice4iot, run `beforeRadioOff` (e.g. housekeeping),
// switch the radio off (flushing the log) and show it as a full-screen error.
// The caller decides how long to sleep.
static void failureScreen(ErrorIcon icon, const String &title, const String &message,
                          const std::function<void()> &beforeRadioOff = {},
                          const DisplayStatus *status = nullptr)
{
    // WiFi state before the radio goes off (or as captured by the caller)
    const DisplayStatus st = status ? *status : buildStatus();
    logger.error(LOG_TAG, "%s — %s", title.c_str(), message.c_str()); // logger joins lines
    if (beforeRadioOff)
        beforeRadioOff();
    radioOff();
    displayRenderer.showError(icon, title, message, st);
}

// Showstopper before the server is usable (no WiFi, provisioning): as above,
// then panic into the library's escalating backoff
// (panic_init_s/panic_factor/panic_max_s).
[[noreturn]] static void panicScreen(ErrorIcon icon, const String &title,
                                     const String &message)
{
    failureScreen(icon, title, message);
    iot.panic("%s", title.c_str()); // the details were logged above
    while (true) {} // never reached
}

// ***************************************************************************

// Print the previous cycle's end-of-cycle phases (posted below as last_*) to
// serial. The console is still up here; after the first light sleep in the
// BUSY-wait the USB console of dev builds stays silent until the next reset.
static void printLastPhases()
{
    const auto &n = rtc->next;
    if (n.isEmpty()) return;
    String line = "previous cycle [ms]:";
    for (uint8_t i = 0; i < n.count && i < sizeof(n.entries) / sizeof(n.entries[0]); i++)
        line += String(" last_") + n.entries[i].key + "=" + String((long)n.entries[i].integer);
    Serial.println(line);
}

void setup()
{
    const unsigned long tBoot = millis(); // time spent in the boot loader/ROM
    Serial.begin(115200);
    printLastPhases();

    // Row layout of nice4iot's config form; the library appends its own rows.
    config.setLayout({ { cvPanel },
                       { cvImagePath },
                       { cvMinSleep, cvMaxSleep },
                       { cvFwCheck } });

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

#ifdef DEV_NO_DEEP_SLEEP
    static DevPower devPower;
    iot.setPower(devPower);
#endif

    // --- status LED (on while the radio is on) ---
    if (STATUS_LED_PIN >= 0)
        pinMode(STATUS_LED_PIN, OUTPUT);
    setStatusLed(true);

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
    //     panel (compiled-in default on a fresh device). ---
    bool connected = iot.begin();
    applyDisplayConfig();
    if (!connected)
        panicScreen(ErrorIcon::NoWifi, "No WiFi",
                    "Could not join the WiFi network.\n\n"
                    "A new device must be seed-flashed once.\nRetrying shortly.");
    const unsigned long tConnect = millis(); // WiFi connect + NTP sync done

    // updateProvisioning() returns a typed result so we can show a matching
    // error screen.
    IotResult prov = api.updateProvisioning();
    if (!prov)
    {
        if (prov.isTransportError())
            panicScreen(ErrorIcon::NoWifi, "No server connection",
                        "Cannot reach the nice4iot server.\n\n"
                        "Check the API URL, TLS certificate\nand network.");
        else if (prov.httpStatus == IotResult::STATUS_NO_PROVISIONING_TOKEN)
            panicScreen(ErrorIcon::Warning, "No provisioning token",
                        "No provisioning token is configured.\n\n"
                        "Set it in settings.h.");
        else if (prov.httpStatus == 403)
            panicScreen(ErrorIcon::Warning, "Provisioning rejected",
                        "The server rejected this device.\n\n"
                        "Check the token, and that the device\n"
                        "is approved and active.");
        else if (prov.httpStatus == IotResult::STATUS_NOT_CONFIGURED)
            panicScreen(ErrorIcon::Warning, "No API URL",
                        "No nice4iot API URL is configured.\n\n"
                        "Set it in settings.h.");
        else if (prov.httpStatus == IotResult::STATUS_MALFORMED_RESPONSE)
            panicScreen(ErrorIcon::Warning, "Provisioning failed",
                        "Unexpected response from the server.\n\n"
                        "Is this a nice4iot API URL?");
        else
            panicScreen(ErrorIcon::Warning, "Provisioning failed",
                        String("The server returned status ") + prov.httpStatus +
                        ".\n\nCheck the token and device approval.");
    }
    const unsigned long tProvisioning = millis();

    // --- config: if config.json was mirrored to NVS before, use the mirror
    //     for this cycle and refresh config.json
    //     in the background (housekeeping) for the NEXT cycle. Otherwise (fresh
    //     flash / erased NVS) fetch it now so the first render already uses the
    //     configured panel.
    bool haveCachedConfig = config.isMirrored();
    if (!haveCachedConfig)
    {
        // not fatal: the compiled-in defaults apply until a later cycle gets it
        logIfFailed(config.updateConfig(), "first config.json download");
        applyDisplayConfig();
    }
    logger.verbose(LOG_TAG, "heap: %u free / %u total, PSRAM %u B",
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapSize(),
                    (unsigned)ESP.getPsramSize());
    const unsigned long tConfig = millis();

    // --- conditional GET of the image (ETag / Last-Modified from RTC) ---
    IotCachedResponse img = api.getCached(cvImagePath.get(), rtc->image.load(),
                                          { .headers = { { "Accept", "image/png" } } });
    const unsigned long tFetch = millis();
    logger.info(LOG_TAG, "image GET %s -> %d (%u bytes, max-age=%d)",
                cvImagePath.get().c_str(), img.result.code(),
                (unsigned)img.body.size(), (int)img.maxAge_s);

    // Decide the sleep duration now so deep sleep after the refresh is immediate.
    // This is the schedule-aligned estimate at WiFi-off time (reported in
    // telemetry); it is recomputed just before deepSleep() with the full active
    // time once the refresh has finished. nicepaper sends max-age on 304, too.
    int sleep_s = img.maxAge_s > 0
                      ? alignedSleep_s(img.maxAge_s, millis() - tConfig, cvMinSleep, cvMaxSleep)
                      : -1;
    if (sleep_s > 0)
        iot.setSleepDuration_s(sleep_s);
    // else: keep the arduino4iot default (config "sleep_s")

    bool displayed = false;

    // Network housekeeping: a throttled OTA check, config refresh for the next
    // cycle, schema publish, telemetry and log flush over WiFi, then the radio
    // off. Runs once per cycle right after the image fetch, before rendering:
    // decoding is CPU work that needs no network, and the refresh light-sleeps,
    // so WiFi is on as briefly as possible (and its RAM is not competing with
    // the page buffer and the decoder).
    // Each step is bounded by its own network timeout, so the watchdog is fed
    // between the steps rather than widened for their sum.
    auto housekeeping = [&]()
    {
        iot.resetWatchdog();
        // OTA check, throttled to fw_check_s (a real update reboots): the display
        // changes far more often than the firmware. lastFwCheckEpoch == 0 (cold
        // boot, RTC state lost) forces a check.
        int64_t now = (int64_t)time(nullptr);
#ifdef DEV_NO_OTA
        if (false) // development build: keep the serially flashed firmware
#else
        if (cvFwCheck <= 0 || rtc->lastFwCheckEpoch == 0 ||
            (now - rtc->lastFwCheckEpoch) >= cvFwCheck)
#endif
        {
            rtc->lastFwCheckEpoch = now;
            // One image per board (release asset naming, see build.yml);
            // {board} is IOT_BOARD_ID. The library default is firmware.bin.
            // Returns only if there was no update or it failed (else reboots);
            // 404 = no firmware published for this board.
            IotResult r = iot.ota.update("file/{project}/{device}/firmware-{board}.bin");
            if (r.httpStatus != 404)
                logIfFailed(r, "firmware update");
            iot.resetWatchdog();
        }
        // Refresh config.json for the NEXT cycle (off this cycle's critical path);
        // the first cycle already fetched it synchronously above, so only the
        // steady state (cache present) refreshes here. The updated values land in
        // NVS for the next wake; the result (usually a 304) is not needed here.
        if (haveCachedConfig)
        {
            logIfFailed(config.updateConfig(), "config.json update");
            iot.resetWatchdog();
        }
        // Publishes config.schema.json only when it changed (hash kept in NVS).
        logIfFailed(config.publishSchema(), "config.schema.json upload");
        iot.resetWatchdog();
        logIfFailed(iot.telemetry.postSystem(), "system telemetry");
        iot.resetWatchdog();
        IotTelemetry t;
        // This cycle's phases known so far (up to WiFi-off):
        t.add("boot_ms", (int)tBoot);                            // ROM + boot loader
        t.add("connect_ms", (int)(tConnect - tBoot));            // WiFi connect + NTP
        t.add("provisioning_ms", (int)(tProvisioning - tConnect));
        t.add("config_ms", (int)(tConfig - tProvisioning));      // first boot only, else ~0
        t.add("fetch_ms", (int)(tFetch - tConfig));              // image GET
        t.add("image_status", img.result.code());
        t.add("image_bytes", (int)img.body.size());
        t.add("image_maxage_s", (int)img.maxAge_s);
        t.add("displayed", displayed ? 1 : 0);
        t.add("heap_free", (int)ESP.getFreeHeap());              // internal
        t.add("heap_max_block", (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        t.add("psram_free", (int)ESP.getFreePsram());            // 0 = no PSRAM
        t.add("sleep_s", sleep_s > 0 ? sleep_s : iot.lastSleepDuration_s());
        t.add("panel", displayRenderer.activePanel());        // active panel id
        t.add("supported_panels", displayRenderer.supportedPanels());   // compiled-in panels
        // Adds the previous cycle's end-of-cycle phases (last_*), see below.
        logIfFailed(iot.telemetry.post("epaper", t, rtc->next), "epaper telemetry");
        iot.resetWatchdog();
        log_i("memory before WiFi off: %s", memorySummary().c_str());
        radioOff();
        log_i("memory after WiFi off: %s", memorySummary().c_str());
    };


    if (img.result.isNotModified())
    {
        // Nothing to refresh. Housekeeping still runs once so a new firmware
        // version can publish its schema even when the image is unchanged.
        logger.info(LOG_TAG, "image unchanged (304), keeping current frame");
        rtc->image.commit(img.validators); // a 304 may resend them
        housekeeping();
    }
    else if (img.result.isOk() && !img.body.empty())
    {
        const uint8_t *png = (const uint8_t *)img.body.data();
        const size_t pngLen = img.body.size();
        if (!displayRenderer.checkImage(png, pngLen))
        {
            // Not a PNG or wrong size, found from the header while WiFi is still
            // up: log it and run housekeeping (its flush sends the error; its
            // config refresh matters, as a wrong panel in a stale cached config
            // is a likely cause and would otherwise never be replaced).
            failureScreen(ErrorIcon::Warning, "Image error",
                          displayRenderer.renderErrorDetail(), housekeeping);
            iot.deepSleep(ERROR_RETRY_S);
        }

        displayed = true; // committed to rendering; telemetry reports this
        const DisplayStatus st = buildStatus(); // WiFi state before it goes off
        housekeeping(); // ends with WiFi off
        if (displayRenderer.renderImage(png, pngLen, st))
        {
            rtc->image.commit(img.validators);
        }
        else
        {
            // Out of memory or a corrupt PNG body: the panel was not refreshed.
            // WiFi is already off, so the error is kept in RTC RAM and sent
            // with the next cycle's log.
            displayed = false;
            failureScreen(ErrorIcon::Warning, "Image error",
                          displayRenderer.renderErrorDetail(), {}, &st);
            iot.deepSleep(ERROR_RETRY_S);
        }
    }
    else if (img.result.isTransportError())
    {
        // transport error (TLS/connection/timeout) — same class as provisioning
        failureScreen(ErrorIcon::NoWifi, "No image server connection",
                      "Cannot reach the nice4iot server for image download.\n\n"
                      "Check the API URL, TLS certificate\nand network.");
        iot.deepSleep(ERROR_RETRY_S);
    }
    else if (img.result.isOk())
    {
        failureScreen(ErrorIcon::Warning, "Empty image",
                      "The server sent an empty image.\n\n"
                      "Check the screen in nicepaper.");
        iot.deepSleep(ERROR_RETRY_S);
    }
    else
    {
        failureScreen(ErrorIcon::Warning, "No image",
                      String("Server responded: ") + img.result.httpStatus + ".\n\n"
                      "Is a screen assigned\nto this device?");
        iot.deepSleep(ERROR_RETRY_S);
    }

    // A rendered cycle's end-of-cycle phase durations are only complete now
    // (after the refresh); buffer them in RTC RAM so the NEXT cycle reports them
    // as last_* instead of us staying awake with WiFi on to send them. (Error
    // paths already deep-slept.)
    if (displayed)
    {
        rtc->next.add("cycle_ms", (int32_t)millis());
        rtc->next.add("display_ms", (int32_t)(millis() - tFetch)); // decode + transfer + refresh
        rtc->next.add("refresh_ms", (int32_t)displayRenderer.lastRefreshMs());
        rtc->next.add("decode_transfer_ms", (int32_t)displayRenderer.lastDecodeTransferMs());
        rtc->next.add("radio_on_ms", (int32_t)s_radioOnMs);

        // One-line phase summary (kept in RTC RAM, uploaded next cycle; the
        // durations also ship in telemetry as last_*).
        logger.info(LOG_TAG,
                    "cycle phases [ms]: boot=%u connect=%u provisioning=%u config=%u "
                    "fetch=%u decode_transfer=%u refresh=%u radio_on=%u cycle=%u",
                    (unsigned)tBoot, (unsigned)(tConnect - tBoot),
                    (unsigned)(tProvisioning - tConnect), (unsigned)(tConfig - tProvisioning),
                    (unsigned)(tFetch - tConfig),
                    (unsigned)displayRenderer.lastDecodeTransferMs(),
                    (unsigned)displayRenderer.lastRefreshMs(),
                    (unsigned)s_radioOnMs, (unsigned)millis());
    }

    // Recompute the schedule-aligned sleep now that the full active window
    // (including the refresh) is known, so the next wake lands on nicepaper's
    // schedule rather than drifting late by the refresh time.
    if (img.maxAge_s > 0)
        iot.setSleepDuration_s(
            alignedSleep_s(img.maxAge_s, millis() - tConfig, cvMinSleep, cvMaxSleep));

    // WiFi is already off and housekeeping done — sleep now.
    iot.deepSleep();
}

void loop()
{
    // never reached: setup() ends in deep sleep; blink if it ever is
    setStatusLed(true);
    delay(100);
    setStatusLed(false);
    delay(900);
    Serial.print(".");
}
