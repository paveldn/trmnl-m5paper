/**
 * M5Paper TRMNL Firmware
 * 
 * Fetches and displays images from TRMNL API on M5Paper e-ink display.
 * Algorithm aligned with official TRMNL firmware behavior.
 * 
 * Features:
 * - WiFi captive portal for configuration (no hardcoded credentials)
 * - Support for official TRMNL server and custom/local servers
 * - Proper deep sleep with GPIO2 power hold (M5Paper hardware requirement)
 * - Power-optimized sleep cycle with server-controlled refresh rate
 * - NVS-based persistent settings
 * - Device registration via MAC address (TRMNL API compatible)
 * - Battery voltage reporting
 * - Button wake from deep sleep
 * - NTP clock synchronization
 * - OTA firmware updates
 * - Image caching (skip redraw if unchanged)
 * - WiFi/API exponential backoff retries
 * 
 * Hardware: M5Paper (ESP32-D0WDQ5, 4.7" e-paper 960x540)
 * 
 * @version 2.6.0
 * @see https://docs.trmnl.com/go/diy/byod
 */

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <M5Unified.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <algorithm>
#include <cmath>
#include <time.h>
#include <sys/time.h>

#include "captive_portal.h"
#include "api_helpers.h"
#include "api_client.h"
#include "button.h"
#include "display.h"
#include "preferences_persistence.h"
#include "power.h"
#include "wifi_network.h"

// ─────────────────────────── Hardware Defines ───────────────────────────
#define M5PAPER_WAKE_BUTTON     39   // GPIO39 - physical button
#define M5EPD_MAIN_PWR_PIN       2   // GPIO2 - SY7088 enable (main 3.3V rail)
#define DEVICE_MODEL        "m5paper"
#ifndef FW_VERSION
#define FW_VERSION          "2.6.0"
#endif
#define DISPLAY_WIDTH       960
#define DISPLAY_HEIGHT      540

#ifndef OTA_FIRMWARE_ASSET_NAME
#define OTA_FIRMWARE_ASSET_NAME DEVICE_MODEL ".bin"
#endif

// ─────────────────────────── Configuration ───────────────────────────
#define DEFAULT_API_BASE_URL   "https://trmnl.app"
#define DEFAULT_REFRESH_RATE   900   // 15 minutes (in seconds)
#define WIFI_CONNECT_TIMEOUT   20000 // 20 seconds
#define WIFI_AP_TIMEOUT        300   // 5 minutes in AP mode before sleep
#define MAX_IMAGE_SIZE         200000
#define LOG_BUFFER_SIZE        4096  // Max log buffer to send to server

// ─────────────────────────── Timing (aligned with TRMNL firmware) ────────
#define BUTTON_HOLD_TIME       5000  // 5s hold = WiFi credentials clear
#define BUTTON_FACTORY_RESET   15000 // 15s hold = full factory reset

// WiFi retry backoff (seconds)
#define WIFI_RETRY_1           60
#define WIFI_RETRY_2           180
#define WIFI_RETRY_3           300
#define MAX_WIFI_RETRIES       3

// API retry backoff (seconds)
#define API_RETRY_1            15
#define API_RETRY_2            30
#define API_RETRY_3            60
#define MAX_API_RETRIES        3

// Sleep times for special states
#define SLEEP_NOT_CONNECTED    5     // When plugin not attached / 202 status
#define SLEEP_AFTER_SETUP      60    // After registration, retry

// Display refresh
#define FULL_REFRESH_INTERVAL  30    // Full quality refresh every N wakes
#define LOW_BATTERY_VOLTAGE    3.4   // Below this voltage, show warning and shut down

// OTA battery safety threshold (roughly ~20% for typical Li-ion curves)
#ifndef OTA_MIN_BATTERY_VOLTAGE
#define OTA_MIN_BATTERY_VOLTAGE 3.65
#endif

// ─────────────────────────── NVS Keys ───────────────────────────
#define NVS_NAMESPACE          "trmnl"
#define KEY_WIFI_SSID          "wifi_ssid"
#define KEY_WIFI_PASS          "wifi_pass"
#define KEY_API_KEY            "api_key"
#define KEY_API_URL            "api_url"
#define KEY_FRIENDLY_ID        "friendly_id"
#define KEY_REFRESH_RATE       "refresh_rate"
#define KEY_WIFI_RETRY_COUNT   "wifi_retry"
#define KEY_API_RETRY_COUNT    "retry_count"
#define KEY_LAST_FILENAME      "last_file"
#define KEY_IMAGE_ETAG         "image_etag"
#define KEY_IMAGE_LASTMOD      "image_lastmod"
#define KEY_OTA_LAST_CHECK     "ota_last_chk"
#define KEY_OTA_LAST_ATTEMPT   "ota_last_try"
#define KEY_OTA_BETA_MODE      "ota_beta"
#define KEY_RTC_SET            "rtc_set"
#define KEY_LOG_ID             "log_id"

// ─────────────────────────── OTA via GitHub Releases ───────────────────────────
#define OTA_SAFETY_INTERVAL_SEC 86400UL
#ifndef OTA_GITHUB_OWNER
#define OTA_GITHUB_OWNER "paveldn"
#endif

#ifndef OTA_GITHUB_REPO
#define OTA_GITHUB_REPO "trmnl-m5paper"
#endif

#define OTA_GITHUB_API_URL "https://api.github.com/repos/" OTA_GITHUB_OWNER "/" OTA_GITHUB_REPO "/releases/latest"
#define OTA_GITHUB_RELEASES_API_URL "https://api.github.com/repos/" OTA_GITHUB_OWNER "/" OTA_GITHUB_REPO "/releases"
// ─────────────────────────── RTC Memory ───────────────────────────
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int partialRefreshCount = 0;
RTC_DATA_ATTR uint8_t savedBSSID[6] = {0};
RTC_DATA_ATTR uint8_t savedChannel = 0;
RTC_DATA_ATTR int wifiFailCount = 0;
RTC_DATA_ATTR int lastWakeTime = 0;

// ─────────────────────────── Globals ───────────────────────────
Preferences prefs;
M5Canvas canvas(&M5.Display);
unsigned long startupMillis = 0;
String configuredSSID;
String configuredPass;
String apiKey;
String apiBaseUrl;
String friendlyId;
int refreshRate = DEFAULT_REFRESH_RATE;
bool forceOtaOnThisBoot = false;
bool otaBetaMode = false;

// ─────────────────────────── Log Buffer ───────────────────────────
#if defined(DEBUG_LOGS) || defined(ENABLE_SERVER_LOGS)
String logBuffer;
#endif

void deviceLog(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
#ifdef DEBUG_LOGS
  Serial.print(buf);
#endif
#ifdef ENABLE_SERVER_LOGS
  if (logBuffer.length() < LOG_BUFFER_SIZE) {
    logBuffer += buf;
  }
#endif
}

// Forward declare UI helpers used by runtime reset handler
void showErrorScreen(const String& message);
void showSetupScreen(const String& message);

// Try to initialize RTC from an HTTP Date header (RFC1123). Only sets time
// if current system time looks uninitialized (before 2020) or if force=true.
bool tryInitRtcFromHttpDate(const String &dateHeader, bool force) {
  if (dateHeader.length() == 0) return false;

  time_t now = time(nullptr);
  // If time already seems valid and not forced, skip
  if (!force && now > 1600000000UL) {
    deviceLog("RTC already set (epoch=%lu) - skipping\n", (unsigned long)now);
    return false; // ~2020-09-13
  }

  char buf[64];
  dateHeader.toCharArray(buf, sizeof(buf));

  // Expected format: "Sat, 13 Jun 2020 12:34:56 GMT"
  char wk[4], mon[4];
  int day, year, hh, mm, ss;
  int rc = sscanf(buf, "%3s, %d %3s %d %d:%d:%d GMT", wk, &day, mon, &year, &hh, &mm, &ss);
  if (rc < 7) return false;

  // map month
  const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int monIdx = -1;
  for (int i = 0; i < 12; ++i) {
    if (strncmp(mon, months + i*3, 3) == 0) { monIdx = i; break; }
  }
  if (monIdx < 0) return false;

  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_mday = day;
  tm.tm_mon = monIdx;
  tm.tm_year = year - 1900;
  tm.tm_hour = hh;
  tm.tm_min = mm;
  tm.tm_sec = ss;

  // Ensure mktime treats tm as UTC by setting TZ to UTC temporarily
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t t = mktime(&tm);
  if (t <= 0) {
    deviceLog("RTC parse produced invalid epoch\n");
    return false;
  }

  struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
  if (settimeofday(&tv, nullptr) == 0) {
    deviceLog("RTC set from server Date: %s -> %lu\n", buf, (unsigned long)t);
    // Persist a flag/timestamp that RTC was set
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putInt(KEY_RTC_SET, (int)t);
    prefs.end();
    return true;
  }
  return false;
}

// WiFi power-save helpers: disable PS during transfers, enable otherwise
void disableWiFiPS() {
  esp_wifi_set_ps(WIFI_PS_NONE);
}

void enableWiFiPS() {
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

// Exported constants for captive_portal.cpp
const char* FW_VERSION_STR = FW_VERSION;
int DEFAULT_REFRESH_RATE_VAL = DEFAULT_REFRESH_RATE;
int WIFI_AP_TIMEOUT_VAL = WIFI_AP_TIMEOUT;
const char* DEFAULT_API_BASE_URL_STR = DEFAULT_API_BASE_URL;
const char* UPDATE_SOURCE_STR = "m5paper";

// ─────────────────────────── Forward Declarations ───────────────────────────
String getWifiBand();
void displayImage(const char* imageUrl);
bool downloadAndDisplayImage(const char* url);
void invalidateImageCache(const char* reason = nullptr);
bool performOTA(const char* firmwareUrl);
bool checkGitHubReleaseForUpdate(String& firmwareUrlOut, String& versionOut, bool force = false, bool includePrereleases = false);
bool getCurrentEpoch(time_t& epochOut);
bool isIntervalElapsed(const char* key, uint32_t intervalSec);
void markIntervalNow(const char* key);
bool parseVersionParts(const String& version, int& major, int& minor, int& patch);
int compareVersions(const String& a, const String& b);
String normalizeVersionTag(const String& version);
String versionBasePart(const String& version);
int prereleaseRankForBetaChannel(const String& version);
bool isSameBaseVersion(const String& a, const String& b);

// ═══════════════════════════════════════════════════════════════════════════════
//  SETUP — Main algorithm (runs on every wake from deep sleep)
// ═══════════════════════════════════════════════════════════════════════════════
void setup() {
#if defined(DEBUG_LOGS) || defined(ENABLE_SERVER_LOGS)
#ifdef DEBUG_LOGS
  Serial.begin(115200);
#endif
  logBuffer.reserve(LOG_BUFFER_SIZE);
#endif
  bootCount++;

  // ── Determine wake reason ──
  // After deep sleep with gpio_deep_sleep_hold_en(), ESP32 wakes and
  // esp_sleep_get_wakeup_cause() returns the actual cause directly.
  esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
  const char* wakeStr = "COLD";
  if (wakeup == ESP_SLEEP_WAKEUP_TIMER) wakeStr = "TIMER";
  else if (wakeup == ESP_SLEEP_WAKEUP_EXT1) wakeStr = "BUTTON";
  bool coldBoot = (wakeup == ESP_SLEEP_WAKEUP_UNDEFINED);

#ifdef FORCE_OTA_ON_NEXT_BOOT
  // Test helper: force OTA logic once on cold boot only.
  forceOtaOnThisBoot = coldBoot;
  if (forceOtaOnThisBoot) {
    deviceLog("OTA: FORCE_OTA_ON_NEXT_BOOT active for this boot\n");
  }
#endif

  // ── Initialize M5Paper ──
  auto cfg = M5.config();
  cfg.output_power = false;  // No external 5V needed (saves ~5mA via GPIO5)
  cfg.internal_rtc = true;
  cfg.internal_imu = false;  // M5Paper has no IMU
  cfg.internal_mic = false;  // Not used
  cfg.internal_spk = false;  // Not used
  cfg.clear_display = false; // We clear right before drawing new image (minimizes white flash)
  M5.begin(cfg);

  // CRITICAL: Release GPIO hold from deep sleep, then ensure GPIO2 HIGH
  gpio_hold_dis((gpio_num_t)M5EPD_MAIN_PWR_PIN);
  gpio_deep_sleep_hold_dis();
  pinMode(M5EPD_MAIN_PWR_PIN, OUTPUT);
  digitalWrite(M5EPD_MAIN_PWR_PIN, HIGH);
  deviceLog("[Boot #%d] Wake: %s\n", bootCount, wakeStr);
  startupMillis = millis();

  // ── Display init ──
  M5.Display.setRotation(1);  // landscape 960x540
  M5.Display.setEpdMode(epd_mode_t::epd_quality); // Full refresh on first boot

  // Power optimization
  setCpuFrequencyMhz(80);
  btStop();

  canvas.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  if (handleBootButtonReset()) return;

  // ── Low battery protection (averaged read to avoid ADC transients) ──
  float bootVoltage = getBatteryVoltage();
  bool externalPower = isExternalPowerPresent();
  if (bootVoltage > 0.5 && bootVoltage < LOW_BATTERY_VOLTAGE && !externalPower) {
    deviceLog("LOW BATTERY: %.2fV < %.1fV threshold\n", bootVoltage, LOW_BATTERY_VOLTAGE);
    showLowBatteryAndShutdown();
    return;
  } else if (bootVoltage > 0.5 && bootVoltage < LOW_BATTERY_VOLTAGE) {
    deviceLog("LOW BATTERY ignored on external power: %.2fV\n", bootVoltage);
  }

  // ── Load settings from NVS ──
  loadSettings();
  deviceLog("Refresh: %ds\n", refreshRate);

  // ── Check if WiFi is configured ──
  if (configuredSSID.length() == 0) {
    deviceLog("No WiFi configured - starting captive portal\n");
    showSetupScreen("Connect to WiFi:\nM5Paper-TRMNL\nThen open: 192.168.4.1");
    startCaptivePortal();
    return;
  }

  // ── Connect to WiFi (with retry logic) ──
  if (!connectWiFi()) {
    wifiErrorSleep();
    return;
  }

  // Reset WiFi retry counter on success
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
  prefs.end();

  // ── Check if API key and friendly ID exist ──
  if (apiKey.length() == 0) {
    registerDevice();
    // registerDevice handles sleep on failure
    if (apiKey.length() == 0) return;
  }

  // ── Ping server (fetch display) ──
  float batteryVoltage = getBatteryVoltage();
  fetchAndDisplay(batteryVoltage);

  // Safety net: setup should never fall through to loop() in normal operation.
  // If it does, go to sleep instead of spinning at full speed.
  deviceLog("Unexpected setup fallthrough, forcing sleep\n");
  goToDeepSleep(refreshRate);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LOOP (only used during captive portal)
// ═══════════════════════════════════════════════════════════════════════════════
void loop() {
  // Defensive fallback: loop() should not run in normal firmware flow.
  deviceLog("Unexpected loop() entry, forcing sleep\n");
  int sleepSeconds = (refreshRate > 0) ? refreshRate : DEFAULT_REFRESH_RATE;
  goToDeepSleep(sleepSeconds);

  // Should be unreachable, but restart if deep sleep setup ever returns.
  delay(1000);
  ESP.restart();
}

bool getCurrentEpoch(time_t& epochOut) {
  time(&epochOut);
  if (epochOut >= 1700000000) {
    return true;
  }

  // Sync time only when needed for OTA intervals.
  configTime(0, 0, "time.google.com", "time.cloudflare.com", "pool.ntp.org");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10000)) {
    time(&epochOut);
    return (epochOut >= 1700000000);
  }
  return false;
}

bool isIntervalElapsed(const char* key, uint32_t intervalSec) {
  time_t now;
  if (!getCurrentEpoch(now)) {
    deviceLog("OTA: time unavailable, interval bypass for %s\n", key);
    return true;
  }

  prefs.begin(NVS_NAMESPACE, true);
  uint32_t last = prefs.getULong(key, 0);
  prefs.end();

  if ((last > 0) && ((uint32_t)now > last && ((uint32_t)now - last) < intervalSec)) {
    uint32_t remaining = intervalSec - ((uint32_t)now - last);
    deviceLog("OTA: %s cooldown active (%lus left)\n", key, (unsigned long)remaining);
    return false;
  }

  return true;
}

void markIntervalNow(const char* key) {
  time_t now;
  if (!getCurrentEpoch(now)) {
    return;
  }

  prefs.begin(NVS_NAMESPACE, false);
  prefs.putULong(key, (uint32_t)now);
  prefs.end();
}

void invalidateImageCache(const char* reason) {
  prefs.begin(NVS_NAMESPACE, false);
  bool hadFilename = prefs.isKey(KEY_LAST_FILENAME);
  bool hadEtag = prefs.isKey(KEY_IMAGE_ETAG);
  bool hadLastMod = prefs.isKey(KEY_IMAGE_LASTMOD);
  prefs.remove(KEY_LAST_FILENAME);
  prefs.remove(KEY_IMAGE_ETAG);
  prefs.remove(KEY_IMAGE_LASTMOD);
  prefs.end();

  if (hadFilename || hadEtag || hadLastMod) {
    if (reason && strlen(reason) > 0) {
      deviceLog("Image cache invalidated (%s)\n", reason);
    } else {
      deviceLog("Image cache invalidated\n");
    }
  }
}

bool parseVersionParts(const String& version, int& major, int& minor, int& patch) {
  String v = version;
  v.trim();

  while (v.length() > 0 && !isDigit(v[0])) {
    v.remove(0, 1);
  }

  int dash = v.indexOf('-');
  if (dash >= 0) v = v.substring(0, dash);
  int plus = v.indexOf('+');
  if (plus >= 0) v = v.substring(0, plus);

  major = 0;
  minor = 0;
  patch = 0;
  int matched = sscanf(v.c_str(), "%d.%d.%d", &major, &minor, &patch);
  return matched >= 2;
}

int compareVersions(const String& a, const String& b) {
  int aMaj, aMin, aPat;
  int bMaj, bMin, bPat;

  if (!parseVersionParts(a, aMaj, aMin, aPat)) return 0;
  if (!parseVersionParts(b, bMaj, bMin, bPat)) return 0;

  if (aMaj != bMaj) return (aMaj > bMaj) ? 1 : -1;
  if (aMin != bMin) return (aMin > bMin) ? 1 : -1;
  if (aPat != bPat) return (aPat > bPat) ? 1 : -1;
  return 0;
}

String normalizeVersionTag(const String& version) {
  String v = version;
  v.trim();
  while (v.length() > 0 && !isDigit(v[0])) {
    v.remove(0, 1);
  }
  return v;
}

String versionBasePart(const String& version) {
  String v = normalizeVersionTag(version);
  int end = v.length();
  int dash = v.indexOf('-');
  int plus = v.indexOf('+');
  if (dash >= 0 && dash < end) end = dash;
  if (plus >= 0 && plus < end) end = plus;
  return v.substring(0, end);
}

int prereleaseRankForBetaChannel(const String& version) {
  String v = normalizeVersionTag(version);
  int dash = v.indexOf('-');
  if (dash < 0) {
    return 0;  // Stable release
  }

  String pre = v.substring(dash + 1);
  pre.toLowerCase();

  int number = 0;
  bool hasNumber = false;
  for (int i = 0; i < pre.length(); ++i) {
    if (isDigit(pre[i])) {
      hasNumber = true;
      number = (number * 10) + (pre[i] - '0');
    }
  }

  if (pre.startsWith("beta")) {
    return 1000 + (hasNumber ? number : 1);
  }
  if (pre.startsWith("rc")) {
    return 2000 + (hasNumber ? number : 1);
  }

  return 500 + (hasNumber ? number : 0);
}

bool isSameBaseVersion(const String& a, const String& b) {
  return versionBasePart(a).equalsIgnoreCase(versionBasePart(b));
}

bool checkGitHubReleaseForUpdate(String& firmwareUrlOut, String& versionOut, bool force, bool includePrereleases) {
  firmwareUrlOut = "";
  versionOut = "";

  if (!force && !isIntervalElapsed(KEY_OTA_LAST_CHECK, OTA_SAFETY_INTERVAL_SEC)) {
    return false;
  }

  if (force) {
    deviceLog("OTA: forcing GitHub release check (cooldown bypass)\n");
  }

  // Mark before attempting network so failed checks are not retried on every wake.
  markIntervalNow(KEY_OTA_LAST_CHECK);

  deviceLog("OTA: checking GitHub releases\n");
  disableWiFiPS();

  HTTPClient http;
  http.begin(includePrereleases ? OTA_GITHUB_RELEASES_API_URL : OTA_GITHUB_API_URL);
  http.setTimeout(15000);
  http.addHeader("User-Agent", "trmnl-m5paper");
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    deviceLog("OTA: GitHub release check HTTP %d\n", code);
    http.end();
    enableWiFiPS();
    return false;
  }

  String payload = http.getString();
  http.end();
  enableWiFiPS();

  String wantedAsset = String(OTA_FIRMWARE_ASSET_NAME);
  String selectedTag = "";
  String selectedUrl = "";
  bool selectedIsPrerelease = false;

  if (!includePrereleases) {
    JsonDocument filter;
    filter["tag_name"] = true;
    JsonObject filterAsset = filter["assets"].to<JsonArray>().add<JsonObject>();
    filterAsset["name"] = true;
    filterAsset["browser_download_url"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
      deviceLog("OTA: GitHub response parse error\n");
      return false;
    }

    String tag = doc["tag_name"] | "";
    if (tag.length() == 0) {
      deviceLog("OTA: GitHub release missing tag_name\n");
      return false;
    }

    int versionCmp = compareVersions(tag, String(FW_VERSION));
    if (!force && versionCmp <= 0) {
      deviceLog("OTA: no newer release (current %s, latest %s)\n", FW_VERSION, tag.c_str());
      return false;
    }
    if (force && versionCmp <= 0) {
      deviceLog("OTA: force mode active, using latest release %s even though current is %s\n", tag.c_str(), FW_VERSION);
    }

    JsonArray assets = doc["assets"].as<JsonArray>();
    for (JsonObject asset : assets) {
      String name = asset["name"] | "";
      String url = asset["browser_download_url"] | "";
      if (name == wantedAsset) {
        selectedTag = tag;
        selectedUrl = url;
        selectedIsPrerelease = false;
        break;
      }
    }
  } else {
    JsonDocument filter;
    JsonObject filterRelease = filter.to<JsonArray>().add<JsonObject>();
    filterRelease["tag_name"] = true;
    filterRelease["prerelease"] = true;
    filterRelease["draft"] = true;
    JsonObject filterAsset = filterRelease["assets"].to<JsonArray>().add<JsonObject>();
    filterAsset["name"] = true;
    filterAsset["browser_download_url"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
      deviceLog("OTA: GitHub releases response parse error\n");
      return false;
    }

    JsonArray releases = doc.as<JsonArray>();
    if (releases.isNull()) {
      deviceLog("OTA: GitHub releases payload missing array\n");
      return false;
    }

    String stableTag = "";
    String stableUrl = "";
    String prereleaseTag = "";
    String prereleaseUrl = "";

    String currentTag = String(FW_VERSION);
    currentTag.trim();
    if (currentTag.startsWith("v") || currentTag.startsWith("V")) {
      currentTag.remove(0, 1);
    }
    bool currentIsPrerelease = (currentTag.indexOf('-') >= 0);

    for (JsonObject release : releases) {
      bool isDraft = release["draft"] | false;
      if (isDraft) continue;

      bool isPrerelease = release["prerelease"] | false;

      String tag = release["tag_name"] | "";
      if (tag.length() == 0) continue;

      String normalizedTag = tag;
      normalizedTag.trim();
      if (normalizedTag.startsWith("v") || normalizedTag.startsWith("V")) {
        normalizedTag.remove(0, 1);
      }

      int versionCmp = compareVersions(tag, String(FW_VERSION));
      if (!force) {
        // Skip exact same tag to avoid redundant OTA attempts.
        if (normalizedTag.equalsIgnoreCase(currentTag)) {
          continue;
        }

        // Never move backwards.
        if (versionCmp < 0) {
          continue;
        }

        if (versionCmp == 0) {
          if (!isSameBaseVersion(normalizedTag, currentTag)) {
            continue;
          }

          int candidatePreRank = prereleaseRankForBetaChannel(normalizedTag);
          int currentPreRank = prereleaseRankForBetaChannel(currentTag);

          // Allow prerelease -> stable on the same base.
          if (!(candidatePreRank == 0 && currentIsPrerelease)) {
            // Otherwise only allow strictly newer prerelease rank.
            if (candidatePreRank <= currentPreRank) {
              continue;
            }
          }
        }
      }

      String assetUrl = "";
      JsonArray assets = release["assets"].as<JsonArray>();
      for (JsonObject asset : assets) {
        String name = asset["name"] | "";
        String url = asset["browser_download_url"] | "";
        if (name == wantedAsset) {
          assetUrl = url;
          break;
        }
      }

      if (assetUrl.length() == 0) {
        continue;
      }

      if (!isPrerelease && stableUrl.length() == 0) {
        stableTag = tag;
        stableUrl = assetUrl;
      }
      if (isPrerelease && prereleaseUrl.length() == 0) {
        prereleaseTag = tag;
        prereleaseUrl = assetUrl;
      }

      if (stableUrl.length() > 0 && prereleaseUrl.length() > 0) {
        break;
      }
    }

    if (stableUrl.length() > 0) {
      selectedTag = stableTag;
      selectedUrl = stableUrl;
      selectedIsPrerelease = false;
    } else if (prereleaseUrl.length() > 0) {
      selectedTag = prereleaseTag;
      selectedUrl = prereleaseUrl;
      selectedIsPrerelease = true;
    }

    if (selectedTag.length() > 0 && force && compareVersions(selectedTag, String(FW_VERSION)) <= 0) {
      deviceLog("OTA: force mode active, using latest %s %s even though current is %s\n",
                selectedIsPrerelease ? "prerelease" : "release",
                selectedTag.c_str(),
                FW_VERSION);
    }
  }

  if (selectedUrl.length() == 0) {
    deviceLog("OTA: no suitable %s found with asset '%s'\n",
              includePrereleases ? "release/prerelease" : "release",
              wantedAsset.c_str());
    return false;
  }

  firmwareUrlOut = selectedUrl;
  versionOut = selectedTag;
  deviceLog("OTA: update available %s%s\n", selectedTag.c_str(), selectedIsPrerelease ? " (prerelease)" : "");
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  WIFI CONNECTION
// ═══════════════════════════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════════════════════════
//  BATTERY
// ═══════════════════════════════════════════════════════════════════════════════
String getWifiBand() {
  wifi_bandwidth_t bandwidth;
  if (esp_wifi_get_bandwidth(WIFI_IF_STA, &bandwidth) == ESP_OK) {
    switch (bandwidth) {
      case WIFI_BW_HT20: return "HT20";
      case WIFI_BW_HT40: return "HT40";
#ifdef WIFI_BW_HT80
      case WIFI_BW_HT80: return "HT80";
#endif
#ifdef WIFI_BW_HT160
      case WIFI_BW_HT160: return "HT160";
#endif
      default: break;
    }
  }
  return "";
}

// ═══════════════════════════════════════════════════════════════════════════════
//  IMAGE DISPLAY
// ═══════════════════════════════════════════════════════════════════════════════
void displayImage(const char* imageUrl) {
  deviceLog("Downloading...\n");

  if (downloadAndDisplayImage(imageUrl)) {
    partialRefreshCount++;

    if (partialRefreshCount >= FULL_REFRESH_INTERVAL) {
      M5.Display.setEpdMode(epd_mode_t::epd_quality);
      partialRefreshCount = 0;
      deviceLog("Full refresh (ghost clear)\n");
    } else {
      deviceLog("Fast refresh\n");
      M5.Display.setEpdMode(epd_mode_t::epd_quality);
    }

    // Clear display to initialize IT8951E framebuffer, then immediately push new image
    M5.Display.clear();
    M5.Display.waitDisplay();
    canvas.pushSprite(0, 0);
    M5.Display.waitDisplay();
    deviceLog("Display done\n");
  } else {
    deviceLog("Image display failed!\n");
  }
}

bool downloadAndDisplayImage(const char* url) {
  disableWiFiPS();

  HTTPClient http;
  String sUrl = String(url);
  http.begin(sUrl);
  if (sUrl.startsWith(apiBaseUrl)) {
    addAuthHeaders(http, WiFi.macAddress(), apiKey);
  }

  // Send conditional GET if we have a stored ETag/Last-Modified
  prefs.begin(NVS_NAMESPACE, true);
  String storedEtag = "";
  String storedLast = "";
  if (prefs.isKey(KEY_IMAGE_ETAG)) {
    storedEtag = prefs.getString(KEY_IMAGE_ETAG, "");
  }
  if (prefs.isKey(KEY_IMAGE_LASTMOD)) {
    storedLast = prefs.getString(KEY_IMAGE_LASTMOD, "");
  }
  prefs.end();
  if (storedEtag.length() > 0) {
    http.addHeader("If-None-Match", storedEtag);
  }
  if (storedLast.length() > 0) {
    http.addHeader("If-Modified-Since", storedLast);
  }
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code == HTTP_CODE_NOT_MODIFIED) {
    deviceLog("Img HTTP 304 Not Modified\n");
    http.end();
    return false;
  }

  if (code != HTTP_CODE_OK) {
    deviceLog("Img HTTP %d\n", code);
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len <= 0) len = MAX_IMAGE_SIZE;
  if (len > MAX_IMAGE_SIZE) {
    deviceLog("Image too large: %d\n", len);
    http.end();
    return false;
  }

  uint8_t* buffer = (uint8_t*)ps_malloc(len);
  if (!buffer) {
    buffer = (uint8_t*)malloc(len);
  }
  if (!buffer) {
    deviceLog("Memory allocation failed\n");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t bytesRead = 0;
  unsigned long lastDataTime = millis();

  while (http.connected() && (int)bytesRead < len) {
    size_t available = stream->available();
    if (available) {
      int toRead = min((int)available, len - (int)bytesRead);
      int read = stream->readBytes(buffer + bytesRead, toRead);
      if (read > 0) {
        bytesRead += read;
        lastDataTime = millis();
      }
    } else {
      if (millis() - lastDataTime > 15000) {
        deviceLog("Download timeout\n");
        break;
      }
      delay(1);
    }
  }

  // Try to capture ETag/Last-Modified for future conditional GETs
  String respEtag = http.header("ETag");
  String respLast = http.header("Last-Modified");
  http.end();

  if (respEtag.length() > 0 || respLast.length() > 0) {
    prefs.begin(NVS_NAMESPACE, false);
    if (respEtag.length() > 0) prefs.putString(KEY_IMAGE_ETAG, respEtag);
    if (respLast.length() > 0) prefs.putString(KEY_IMAGE_LASTMOD, respLast);
    prefs.end();
  }

  deviceLog("Downloaded: %d bytes\n", bytesRead);

  // Disconnect WiFi early so decoding/drawing happens with radio off
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    delay(10);
  }
  if (bytesRead < 100) {
    deviceLog("Data too small\n");
    free(buffer);
    return false;
  }

  bool success = false;

  // Detect format by magic bytes
  if (bytesRead >= 4 && buffer[0] == 0x89 && buffer[1] == 0x50 &&
      buffer[2] == 0x4E && buffer[3] == 0x47) {
    deviceLog("PNG format\n");
    success = canvas.drawPng(buffer, bytesRead, 0, 0);
  } else if (bytesRead >= 2 && buffer[0] == 'B' && buffer[1] == 'M') {
    deviceLog("BMP format\n");
    success = canvas.drawBmp(buffer, bytesRead, 0, 0);
  } else {
    deviceLog("Unknown format: %02X %02X %02X %02X\n",
                  buffer[0], buffer[1], buffer[2], buffer[3]);
  }

  free(buffer);
  return success;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  OTA FIRMWARE UPDATE
// ═══════════════════════════════════════════════════════════════════════════════
bool performOTA(const char* firmwareUrl) {
  deviceLog("Starting OTA update...\n");
  showSetupScreen("Firmware Update\n\nDownloading...\nDo not power off");
  disableWiFiPS();

  HTTPClient http;
  http.begin(firmwareUrl);
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    deviceLog("OTA download failed, HTTP %d\n", code);
    http.end();
    showErrorScreen("Firmware update failed\nHTTP " + String(code));
    return false;
  }

  size_t contentLength = http.getSize();
  if (contentLength <= 0) {
    deviceLog("OTA: unknown content length\n");
    http.end();
    showErrorScreen("Firmware update failed\nInvalid size");
    return false;
  }

  if (!Update.begin(contentLength)) {
    deviceLog("OTA: not enough space\n");
    http.end();
    showErrorScreen("Firmware update failed\nNot enough space");
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  http.end();

  if (written != contentLength) {
    deviceLog("OTA: wrote %d/%d bytes\n", written, contentLength);
    Update.abort();
    showErrorScreen("Firmware update failed\nIncomplete download");
    return false;
  }

  if (!Update.end(true)) {
    deviceLog("OTA: finalization failed\n");
    showErrorScreen("Firmware update failed\nVerification error");
    return false;
  }

  deviceLog("OTA success! Restarting...\n");
  invalidateImageCache("ota");
  showSetupScreen("Firmware Updated!\n\nRestarting...");
  delay(1000);
  ESP.restart();
  return true;
}

