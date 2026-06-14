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
 * @version 2.4.0
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
#include <time.h>
#include <sys/time.h>

#include "captive_portal.h"
#include "api_helpers.h"

// ─────────────────────────── Hardware Defines ───────────────────────────
#define M5PAPER_WAKE_BUTTON     39   // GPIO39 - physical button
#define M5EPD_MAIN_PWR_PIN       2   // GPIO2 - SY7088 enable (main 3.3V rail)
#define DEVICE_MODEL        "m5paper"
#define FW_VERSION          "2.4.0"
#define DISPLAY_WIDTH       960
#define DISPLAY_HEIGHT      540

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
#define KEY_RTC_SET            "rtc_set"
#define KEY_LOG_ID             "log_id"
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

// ─────────────────────────── Log Buffer ───────────────────────────
#ifdef DEBUG_LOGS
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
static bool tryInitRtcFromHttpDate(const String &dateHeader, bool force = false) {
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

// Check for a runtime long-press on the wake button. Only blocks if the
// button is actually held. 5s = WiFi clear, 15s = factory reset.
static void checkRuntimeReset() {
  pinMode(M5PAPER_WAKE_BUTTON, INPUT);
  if (digitalRead(M5PAPER_WAKE_BUTTON) == LOW) {
    deviceLog("Runtime button press detected\n");
    unsigned long start = millis();
    while (digitalRead(M5PAPER_WAKE_BUTTON) == LOW) {
      unsigned long held = millis() - start;
      if (held >= BUTTON_FACTORY_RESET) {
        deviceLog("Runtime factory reset triggered\n");
        prefs.begin(NVS_NAMESPACE, false);
        prefs.clear();
        prefs.end();
        showErrorScreen("Factory Reset\n\nAll settings cleared\nRestarting...");
        delay(1500);
        ESP.restart();
        return;
      }
      delay(50);
    }
    unsigned long held = millis() - start;
    if (held >= BUTTON_HOLD_TIME) {
      deviceLog("Runtime WiFi credentials clear triggered\n");
      prefs.begin(NVS_NAMESPACE, false);
      prefs.remove(KEY_WIFI_SSID);
      prefs.remove(KEY_WIFI_PASS);
      prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
      prefs.end();
      showSetupScreen("WiFi cleared\n\nRestarting...");
      delay(1000);
      ESP.restart();
      return;
    }
  }
}

// WiFi power-save helpers: disable PS during transfers, enable otherwise
static inline void disableWiFiPS() {
  esp_wifi_set_ps(WIFI_PS_NONE);
}

static inline void enableWiFiPS() {
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

// Exported constants for captive_portal.cpp
const char* FW_VERSION_STR = FW_VERSION;
int DEFAULT_REFRESH_RATE_VAL = DEFAULT_REFRESH_RATE;
int WIFI_AP_TIMEOUT_VAL = WIFI_AP_TIMEOUT;
const char* DEFAULT_API_BASE_URL_STR = DEFAULT_API_BASE_URL;
const char* UPDATE_SOURCE_STR = "m5paper";

// ─────────────────────────── Forward Declarations ───────────────────────────
void loadSettings();
void saveWiFiSettings(const String& ssid, const String& pass);
float readBatteryAvg(int samples = 6, int delayMs = 50);
void saveServerSettings(const String& key, const String& url);
void clearAllSettings();
bool connectWiFi();
float getBatteryVoltage();
bool isExternalPowerPresent();
bool isBatteryCharging();
String getWifiBand();
static String wifiStatusString(wl_status_t status);
static String wakeReasonString(esp_sleep_wakeup_cause_t reason);
void fetchAndDisplay(float batteryVoltage);
void displayImage(const char* imageUrl);
bool downloadAndDisplayImage(const char* url);
void registerDevice();
void goToDeepSleep(int seconds);
void showSetupScreen(const String& message);
void showErrorScreen(const String& message);
void showLoadingScreen();
bool performOTA(const char* firmwareUrl);
void wifiErrorSleep();
void apiErrorSleep();
void sendLogs();
void showLowBatteryAndShutdown();

// ═══════════════════════════════════════════════════════════════════════════════
//  SETUP — Main algorithm (runs on every wake from deep sleep)
// ═══════════════════════════════════════════════════════════════════════════════
void setup() {
#ifdef DEBUG_LOGS
  Serial.begin(115200);
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
  M5.Display.setEpdMode(epd_mode_t::epd_fast); // Full refresh on first boot

  // Power optimization
  setCpuFrequencyMhz(80);
  btStop();

  canvas.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  pinMode(M5PAPER_WAKE_BUTTON, INPUT_PULLUP);

  // ── Check reset button ──
  // Aligned with TRMNL: 5s hold = WiFi clear, 15s = factory reset
  if (digitalRead(M5PAPER_WAKE_BUTTON) == LOW) {
    deviceLog("Button held at boot...\n");
    unsigned long pressStart = millis();
    while (digitalRead(M5PAPER_WAKE_BUTTON) == LOW) {
      unsigned long held = millis() - pressStart;
      if (held > BUTTON_FACTORY_RESET) {
        break;
      }
      delay(50);
    }
    unsigned long holdTime = millis() - pressStart;

    if (holdTime >= BUTTON_FACTORY_RESET) {
      deviceLog("Factory reset (15s hold)\n");
      M5.Display.setEpdMode(epd_mode_t::epd_fast);
      prefs.begin(NVS_NAMESPACE, false);
      prefs.clear();
      prefs.end();
      showErrorScreen("Factory Reset\n\nAll settings cleared\nRestarting...");
      delay(2000);
      ESP.restart();
      return;
    } else if (holdTime >= BUTTON_HOLD_TIME) {
      deviceLog("WiFi credentials clear (5s hold)\n");
      prefs.begin(NVS_NAMESPACE, false);
      prefs.remove(KEY_WIFI_SSID);
      prefs.remove(KEY_WIFI_PASS);
      prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
      prefs.end();
      // Fall through — will start captive portal below
    }
  }

  // ── Low battery protection (averaged read to avoid ADC transients) ──
  float bootVoltage = readBatteryAvg(4, 50);
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
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LOOP (only used during captive portal)
// ═══════════════════════════════════════════════════════════════════════════════
void loop() {
  // Only reached during captive portal operation
}

// ═══════════════════════════════════════════════════════════════════════════════
//  SETTINGS MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════════
void loadSettings() {
  prefs.begin(NVS_NAMESPACE, true);
  configuredSSID = prefs.getString(KEY_WIFI_SSID, "");
  configuredPass = prefs.getString(KEY_WIFI_PASS, "");
  apiKey = prefs.getString(KEY_API_KEY, "");
  apiBaseUrl = prefs.getString(KEY_API_URL, DEFAULT_API_BASE_URL);
  friendlyId = prefs.getString(KEY_FRIENDLY_ID, "");
  refreshRate = prefs.getInt(KEY_REFRESH_RATE, DEFAULT_REFRESH_RATE);
  prefs.end();
}

void saveWiFiSettings(const String& ssid, const String& pass) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(KEY_WIFI_SSID, ssid);
  prefs.putString(KEY_WIFI_PASS, pass);
  prefs.end();
  configuredSSID = ssid;
  configuredPass = pass;
  savedChannel = 0;  // Invalidate fast reconnect cache
}

void saveServerSettings(const String& key, const String& url) {
  prefs.begin(NVS_NAMESPACE, false);
  if (key.length() > 0) prefs.putString(KEY_API_KEY, key);
  if (url.length() > 0) prefs.putString(KEY_API_URL, url);
  prefs.end();
  if (key.length() > 0) apiKey = key;
  if (url.length() > 0) apiBaseUrl = url;
}

void saveRefreshRate(int rate) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putInt(KEY_REFRESH_RATE, rate);
  prefs.end();
  refreshRate = rate;
}

void clearAllSettings() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  configuredSSID = "";
  configuredPass = "";
  apiKey = "";
  apiBaseUrl = DEFAULT_API_BASE_URL;
  friendlyId = "";
  refreshRate = DEFAULT_REFRESH_RATE;
  savedChannel = 0;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  WIFI CONNECTION
// ═══════════════════════════════════════════════════════════════════════════════
bool connectWiFi() {
  deviceLog("WiFi: %s\n", configuredSSID.c_str());

  WiFi.persistent(false);  // Don't write credentials to flash on every boot
  WiFi.mode(WIFI_STA);

  bool fastConnect = (savedChannel != 0 && configuredPass.length() > 0);
  bool connected = false;

  if (fastConnect) {
    deviceLog("Fast reconnect ch:%d\n", savedChannel);
    WiFi.begin(configuredSSID.c_str(), configuredPass.c_str(), savedChannel, savedBSSID, true);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start <= 5000) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
    } else {
      deviceLog("Fast reconnect failed (ch:%d), resetting cache\n", savedChannel);
      savedChannel = 0;
      memset(savedBSSID, 0, 6);
      WiFi.disconnect(true);
      delay(10);
      fastConnect = false;
    }
  }

  if (!connected) {
    deviceLog("WiFi: full scan connect\n");
    if (configuredPass.length() > 0) {
      WiFi.begin(configuredSSID.c_str(), configuredPass.c_str());
    } else {
      WiFi.begin(configuredSSID.c_str());
    }

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
      if (millis() - start > WIFI_CONNECT_TIMEOUT) {
        deviceLog("WiFi FAILED (timeout %dms)\n", WIFI_CONNECT_TIMEOUT);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return false;
      }
      delay(250);
    }
  }

  // Save channel + BSSID for fast reconnect on next wake
  savedChannel = WiFi.channel();
  memcpy(savedBSSID, WiFi.BSSID(), 6);

  deviceLog("OK IP:%s RSSI:%d ch:%d %s\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                savedChannel, fastConnect ? "(fast)" : "(scan)");

  // Enable WiFi power-save mode to reduce idle radio current
  enableWiFiPS();

  // Report previous WiFi failures to server
  if (wifiFailCount > 0) {
    deviceLog("WiFi: recovered after %d failed attempt(s)\n", wifiFailCount);
    wifiFailCount = 0;
  }

  return true;
}

// WiFi error sleep after failed connection attempts
void wifiErrorSleep() {
  wifiFailCount++;

  prefs.begin(NVS_NAMESPACE, false);
  prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
  prefs.end();

  deviceLog("WiFi unreachable after fast reconnect and full scan - sleeping %ds\n", refreshRate);
  showErrorScreen("Can't connect to WiFi\n" + configuredSSID + "\n\nRetrying in " + String(refreshRate) + "s");
  checkRuntimeReset();
  goToDeepSleep(refreshRate);
}

// API error sleep with exponential backoff (15s, 30s, 60s then normal rate)
void apiErrorSleep() {
  prefs.begin(NVS_NAMESPACE, false);
  int retryCount = prefs.getInt(KEY_API_RETRY_COUNT, 1);

  int sleepTime;
  switch (retryCount) {
    case 1: sleepTime = API_RETRY_1; break;
    case 2: sleepTime = API_RETRY_2; break;
    case 3: sleepTime = API_RETRY_3; break;
    default:
      // Max retries — fall back to normal refresh rate
      prefs.putInt(KEY_API_RETRY_COUNT, 1);
      prefs.end();
      deviceLog("API max retries — sleeping normal rate\n");
      goToDeepSleep(refreshRate);
      return;
  }

  deviceLog("API retry #%d, sleeping %ds\n", retryCount, sleepTime);
  prefs.putInt(KEY_API_RETRY_COUNT, retryCount + 1);
  prefs.end();
  checkRuntimeReset();
  goToDeepSleep(sleepTime);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LOG SUBMISSION (POST /api/log)
// ═══════════════════════════════════════════════════════════════════════════════
void sendLogs() {
  // Build a message from debug buffer if present, otherwise synthesize minimal metadata
  String msg = "";
#ifdef DEBUG_LOGS
  Serial.printf("[Log] sendLogs called: bufLen=%d baseUrl='%s' wifi=%d\n",
    logBuffer.length(), apiBaseUrl.c_str(), WiFi.status());
  if (logBuffer.length() > 0) msg = logBuffer;
#endif

  // If no debug buffer, synthesize a short message with device metadata
  if (msg.length() == 0) {
    prefs.begin(NVS_NAMESPACE, true);
    int rtc = prefs.getInt(KEY_RTC_SET, 0);
    prefs.end();
    msg = String("auto-log: fw=") + String(FW_VERSION_STR) + ", mac=" + WiFi.macAddress() + ", fid=" + friendlyId + ", rtc=" + String(rtc);
  }

  float batV = getBatteryVoltage();
  if (batV > 0.5 && batV < LOW_BATTERY_VOLTAGE && !isExternalPowerPresent()) {
#ifdef DEBUG_LOGS
    Serial.println("[Log] SKIP: low battery, deferring logs");
#endif
    return;
  }

  prefs.begin(NVS_NAMESPACE, false);
  uint32_t logId = prefs.getUInt(KEY_LOG_ID, 1);
  prefs.putUInt(KEY_LOG_ID, logId + 1);
  prefs.end();

  disableWiFiPS();
  HTTPClient http;
  String url = apiBaseUrl + "/api/log";
  http.begin(url);
  http.setTimeout(5000);
  addLogHeaders(http, WiFi.macAddress(), apiKey);

  StaticJsonDocument<2048> doc;
  JsonArray logs = doc.createNestedArray("logs");
  JsonObject entry = logs.createNestedObject();

  entry["created_at"] = (uint32_t)time(nullptr);
  entry["id"] = logId;
  entry["message"] = msg;
  entry["source_line"] = 0;
  entry["source_path"] = "main.cpp";

  entry["wifi_signal"] = WiFi.RSSI();
  entry["wifi_status"] = wifiStatusString(WiFi.status());
  entry["refresh_rate"] = refreshRate;
  entry["sleep_duration"] = lastWakeTime;
  entry["firmware_version"] = String(FW_VERSION_STR);
  entry["special_function"] = "none";
  entry["battery_voltage"] = batV;
  entry["wake_reason"] = wakeReasonString(esp_sleep_get_wakeup_cause());
  entry["free_heap_size"] = ESP.getFreeHeap();
  entry["max_alloc_size"] = ESP.getMaxAllocHeap();

  String payload;
  serializeJson(doc, payload);

#ifdef DEBUG_LOGS
  Serial.printf("[Log] POST %s (%d bytes payload)\n", url.c_str(), payload.length());
#endif

  int code = http.POST(payload);
  String response = http.getString();
#ifdef DEBUG_LOGS
  Serial.printf("[Log] Response: %d - %s\n", code, response.c_str());
#endif
  http.end();

  // Re-enable power-save after network activity
  enableWiFiPS();

#ifdef DEBUG_LOGS
  // Clear debug buffer only on success
  if (code >= 200 && code < 300) {
    logBuffer = "";
  }
#endif
}

// ═══════════════════════════════════════════════════════════════════════════════
//  DEVICE REGISTRATION (/api/setup)
// ═══════════════════════════════════════════════════════════════════════════════
void registerDevice() {
  deviceLog("GET /api/setup...\n");
  disableWiFiPS();

  HTTPClient http;
  String url = apiBaseUrl + "/api/setup";
  http.begin(url);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  addSetupHeaders(http, WiFi.macAddress(), FW_VERSION_STR, DEVICE_MODEL);

  int code = http.GET();
  if (code < 200 || code >= 300) {
    deviceLog("Setup failed, HTTP %d\n", code);
    http.end();
    showErrorScreen("Setup failed\nHTTP " + String(code) + "\n" + apiBaseUrl);
    enableWiFiPS();
    apiErrorSleep();
    return;
  }

  // Try to initialize RTC from server Date header if clock is unset
  tryInitRtcFromHttpDate(http.header("Date"));

  String payload = http.getString();
  http.end();

  enableWiFiPS();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    deviceLog("Setup: JSON parse error\n");
    showErrorScreen("Setup: bad response");
    apiErrorSleep();
    return;
  }

  int status = doc["status"] | 404;

  if (status == 200) {
    String key = doc["api_key"] | "";
    String fid = doc["friendly_id"] | "";

    if (key.length() > 0) {
      saveServerSettings(key, "");
      prefs.begin(NVS_NAMESPACE, false);
      prefs.putString(KEY_FRIENDLY_ID, fid);
      prefs.putInt(KEY_API_RETRY_COUNT, 1);
      prefs.end();
      friendlyId = fid;
      deviceLog("Registered! API Key: %s, Friendly ID: %s\n", key.c_str(), fid.c_str());
    }
  } else if (status == 404) {
    // MAC not registered on server
    deviceLog("MAC not registered on server\n");
    showSetupScreen("Register your device\n\nMAC: " + WiFi.macAddress() + "\n\non usetrmnl.com\nor your server dashboard");
    goToDeepSleep(SLEEP_AFTER_SETUP);
  } else {
    deviceLog("Setup: unexpected status %d\n", status);
    showErrorScreen("Setup error\nStatus: " + String(status));
    apiErrorSleep();
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  BATTERY
// ═══════════════════════════════════════════════════════════════════════════════
float getBatteryVoltage() {
  float voltage = M5.Power.getBatteryVoltage() / 1000.0;
  int level = M5.Power.getBatteryLevel();
  deviceLog("Bat: %d%% %.2fV\n", level, voltage);
  return voltage;
}

float readBatteryAvg(int samples, int delayMs) {
  if (samples <= 0) samples = 1;
  float sum = 0.0;
  for (int i = 0; i < samples; ++i) {
    sum += M5.Power.getBatteryVoltage() / 1000.0;
    if (i < samples - 1) delay(delayMs);
  }
  float avg = sum / samples;
  deviceLog("Bat(avg): %.2fV samples=%d\n", avg, samples);
  return avg;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  API COMMUNICATION (/api/display)
// ═══════════════════════════════════════════════════════════════════════════════
bool isExternalPowerPresent() {
  int16_t vbus = M5.Power.getVBUSVoltage();
  auto charging = M5.Power.isCharging();
  bool present = vbus > 4000 || charging == m5::Power_Class::is_charging_t::is_charging;
  deviceLog("Power: VBUS=%dmV charging=%d external=%d\n", vbus, (int)charging, present ? 1 : 0);
  return present;
}

bool isBatteryCharging() {
  return M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
}

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

static String wifiStatusString(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

static String wakeReasonString(esp_sleep_wakeup_cause_t reason) {
  switch (reason) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "UNDEFINED";
    case ESP_SLEEP_WAKEUP_ALL: return "ALL";
    case ESP_SLEEP_WAKEUP_TIMER: return "TIMER";
    case ESP_SLEEP_WAKEUP_EXT0: return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1: return "EXT1";
    case ESP_SLEEP_WAKEUP_GPIO: return "GPIO";
    case ESP_SLEEP_WAKEUP_UART: return "UART";
    case ESP_SLEEP_WAKEUP_ULP: return "ULP";
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return "TOUCHPAD";
    default: return "UNKNOWN";
  }
}

void fetchAndDisplay(float batteryVoltage) {
  deviceLog("GET /api/display...\n");
  disableWiFiPS();

  HTTPClient http;
  String url = apiBaseUrl + "/api/display";
  http.begin(url);
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  bool imageCached = false;
  prefs.begin(NVS_NAMESPACE, true);
  if (prefs.isKey(KEY_LAST_FILENAME)) {
    imageCached = true;
  }
  prefs.end();

  addDisplayHeaders(http,
                    WiFi.macAddress(),
                    apiKey,
                    refreshRate,
                    batteryVoltage,
                    WiFi.RSSI(),
                    imageCached,
                    lastWakeTime,
                    FW_VERSION_STR,
                    DEVICE_MODEL,
                    getWifiBand(),
                    isBatteryCharging(),
                    isExternalPowerPresent(),
                    UPDATE_SOURCE_STR,
                    DISPLAY_WIDTH,
                    DISPLAY_HEIGHT);

  int code = http.GET();
  if (code < 200 || code >= 300) {
    deviceLog("API failed, HTTP %d\n", code);
    http.end();
    showErrorScreen("Server error\nHTTP " + String(code));
    enableWiFiPS();
    apiErrorSleep();
    return;
  }

  // Initialize RTC from server Date header if needed
  tryInitRtcFromHttpDate(http.header("Date"));

  String payload = http.getString();
  http.end();

  enableWiFiPS();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    deviceLog("API: JSON parse error\n");
    showErrorScreen("Server: bad response");
    apiErrorSleep();
    return;
  }

  // ── Success — reset API retry counter ──
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putInt(KEY_API_RETRY_COUNT, 1);
  prefs.end();

  int status = doc["status"] | -1;

  // ── Handle reset_firmware ──
  bool resetFirmware = doc["reset_firmware"] | false;
  if (resetFirmware) {
    deviceLog("Server requested device reset\n");
    clearAllSettings();
    ESP.restart();
    return;
  }

  // ── Handle status codes ──
  if (status == 202) {
    // Device not yet linked to a user / plugin not attached
    deviceLog("Status 202: plugin not attached\n");
    showSetupScreen("Waiting for setup\n\nID: " + friendlyId + "\nMAC: " + WiFi.macAddress());
    saveRefreshRate(SLEEP_NOT_CONNECTED);
    checkRuntimeReset();
    goToDeepSleep(SLEEP_NOT_CONNECTED);
    return;
  }

  if (status == 500) {
    // Server says device not found for this token
    deviceLog("Status 500: device not found, resetting\n");
    clearAllSettings();
    ESP.restart();
    return;
  }

  if (status != 0) {
    deviceLog("API: unexpected status %d\n", status);
    checkRuntimeReset();
    goToDeepSleep(refreshRate);
    return;
  }

  // ── Status 0: normal response ──
  const char* imageUrl = doc["image_url"];
  const char* filename = doc["filename"];
  bool updateFirmware = doc["update_firmware"] | false;
  const char* firmwareUrl = doc["firmware_url"];
  int newRefreshRate = doc["refresh_rate"] | refreshRate;

  // Update refresh rate from server
  if (newRefreshRate != refreshRate) {
    deviceLog("Refresh rate: %d -> %d\n", refreshRate, newRefreshRate);
    saveRefreshRate(newRefreshRate);
  }

  // ── OTA firmware update ──
  if (updateFirmware && firmwareUrl && strlen(firmwareUrl) > 0) {
    deviceLog("OTA update: %s\n", firmwareUrl);
    if (performOTA(firmwareUrl)) {
      return;  // OTA success — device will restart
    }
    deviceLog("OTA failed, continuing...\n");
  }

  // ── Check if image needs update ──
  if (!imageUrl || strlen(imageUrl) == 0) {
    deviceLog("No image_url — sleeping\n");
    checkRuntimeReset();
    goToDeepSleep(refreshRate);
    return;
  }

  // Check if filename changed (image caching)
  bool needsUpdate = true;
  if (filename && strlen(filename) > 0) {
    prefs.begin(NVS_NAMESPACE, false);
    String lastFile = prefs.getString(KEY_LAST_FILENAME, "");
    if (lastFile == String(filename)) {
      deviceLog("Image unchanged (same filename) — skipping display\n");
      needsUpdate = false;
    } else {
      prefs.putString(KEY_LAST_FILENAME, String(filename));
    }
    prefs.end();
  }

  if (needsUpdate) {
    displayImage(imageUrl);
  }

  // Allow user to perform runtime resets by holding the wake button now.
  checkRuntimeReset();

  goToDeepSleep(refreshRate);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  IMAGE DISPLAY
// ═══════════════════════════════════════════════════════════════════════════════
void displayImage(const char* imageUrl) {
  deviceLog("Downloading...\n");

  if (downloadAndDisplayImage(imageUrl)) {
    partialRefreshCount++;

    if (partialRefreshCount >= FULL_REFRESH_INTERVAL) {
      M5.Display.setEpdMode(epd_mode_t::epd_fast);
      partialRefreshCount = 0;
      deviceLog("Full refresh (ghost clear)\n");
    } else {
      deviceLog("Fast refresh\n");
      M5.Display.setEpdMode(epd_mode_t::epd_fast);
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
  showSetupScreen("Firmware Updated!\n\nRestarting...");
  delay(1000);
  ESP.restart();
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  DISPLAY HELPERS
// ═══════════════════════════════════════════════════════════════════════════════
void showLoadingScreen() {
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  canvas.fillSprite(TFT_WHITE);
  canvas.setTextColor(TFT_BLACK);
  canvas.setTextDatum(MC_DATUM);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.drawString("TRMNL", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 - 20);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.drawString("Loading...", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 + 30);
  canvas.pushSprite(0, 0);
  M5.Display.display();
  partialRefreshCount = 0;  // Reset since we did a full refresh
}

void showSetupScreen(const String& message) {
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  canvas.fillSprite(TFT_WHITE);
  canvas.setTextColor(TFT_BLACK);
  canvas.setTextDatum(MC_DATUM);
  canvas.setFont(&fonts::FreeSansBold12pt7b);

  canvas.drawString("M5Paper TRMNL", DISPLAY_WIDTH / 2, 60);

  canvas.setFont(&fonts::FreeSans12pt7b);
  int y = 180;
  int start = 0;
  String msg = message;
  while (start < (int)msg.length()) {
    int nl = msg.indexOf('\n', start);
    if (nl < 0) nl = msg.length();
    String line = msg.substring(start, nl);
    canvas.drawString(line, DISPLAY_WIDTH / 2, y);
    y += 36;
    start = nl + 1;
  }

  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.drawString("FW " FW_VERSION, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT - 40);

  canvas.pushSprite(0, 0);
  M5.Display.display();
}

void showErrorScreen(const String& message) {
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  canvas.fillSprite(TFT_WHITE);
  canvas.setTextColor(TFT_BLACK);
  canvas.setTextDatum(MC_DATUM);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.drawString("Error", DISPLAY_WIDTH / 2, 80);

  canvas.setFont(&fonts::FreeSans12pt7b);
  int y = 200;
  int start = 0;
  String msg = message;
  while (start < (int)msg.length()) {
    int nl = msg.indexOf('\n', start);
    if (nl < 0) nl = msg.length();
    String line = msg.substring(start, nl);
    canvas.drawString(line, DISPLAY_WIDTH / 2, y);
    y += 36;
    start = nl + 1;
  }

  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.drawString("FW " FW_VERSION, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT - 40);

  canvas.pushSprite(0, 0);
  M5.Display.display();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LOW BATTERY WARNING
// ═══════════════════════════════════════════════════════════════════════════════
void showLowBatteryAndShutdown() {
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  canvas.fillSprite(TFT_WHITE);

  // Draw battery icon (centered, large) — Material Design style battery_alert
  int cx = DISPLAY_WIDTH / 2;
  int cy = DISPLAY_HEIGHT / 2 - 30;
  int bw = 120;  // battery body width
  int bh = 200;  // battery body height
  int cap_w = 50; // top cap width
  int cap_h = 20; // top cap height
  int thick = 8;  // outline thickness

  // Battery cap (top nub)
  canvas.fillRect(cx - cap_w/2, cy - bh/2 - cap_h, cap_w, cap_h, TFT_BLACK);

  // Battery body outline
  canvas.fillRect(cx - bw/2, cy - bh/2, bw, bh, TFT_BLACK);
  canvas.fillRect(cx - bw/2 + thick, cy - bh/2 + thick, bw - 2*thick, bh - 2*thick, TFT_WHITE);

  // Exclamation mark inside battery
  int ex_x = cx;
  int ex_top = cy - 50;
  int ex_w = 14;
  // Vertical bar
  canvas.fillRect(ex_x - ex_w/2, ex_top, ex_w, 70, TFT_BLACK);
  // Dot
  canvas.fillRect(ex_x - ex_w/2, ex_top + 85, ex_w, ex_w, TFT_BLACK);

  // Text below
  canvas.setTextColor(TFT_BLACK);
  canvas.setTextDatum(MC_DATUM);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.drawString("LOW BATTERY", cx, cy + bh/2 + 50);

  canvas.setFont(&fonts::FreeSans9pt7b);
  float v = M5.Power.getBatteryVoltage() / 1000.0;
  canvas.drawString(String(v, 2) + "V - Connect USB to charge", cx, cy + bh/2 + 90);

  canvas.pushSprite(0, 0);
  M5.Display.waitDisplay();

  // Full shutdown — no timer wake, no button wake
  // Device will only restart when USB power is connected (cold boot)
  deviceLog("Shutting down (low battery)\n");
  Serial.flush();
  delay(100);

  M5.Display.sleep();
  M5.Display.waitDisplay();

  // Kill main power — only USB connection will restart
  gpio_hold_dis((gpio_num_t)M5EPD_MAIN_PWR_PIN);
  pinMode(M5EPD_MAIN_PWR_PIN, OUTPUT);
  digitalWrite(M5EPD_MAIN_PWR_PIN, LOW);
  gpio_hold_en((gpio_num_t)M5EPD_MAIN_PWR_PIN);
  gpio_deep_sleep_hold_en();

  // Deep sleep with no wake sources — effectively off
  esp_deep_sleep_start();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  SLEEP (Deep Sleep with GPIO hold — per cat-in-136 M5Paper power analysis)
// ═══════════════════════════════════════════════════════════════════════════════
// gpio_deep_sleep_hold_en() holds ALL GPIO pin states through deep sleep:
// - GPIO2 stays HIGH → SY7088 boost converter stays on → power maintained
// - SPI bus pins stay stable → IT8951E doesn't see floating lines
// - Timer and EXT1 wake work because ESP32 stays powered via SY7088
// - RTC_DATA_ATTR variables (bootCount) persist through deep sleep
//
// Reference: https://cat-in-136.github.io/2022/05/note-m5paper-power-supply-management.html

void goToDeepSleep(int seconds) {
  // Enforce minimum sleep to avoid rapid wake loops
  if (seconds < 15) 
    seconds = 16;

  lastWakeTime = (millis() - startupMillis) / 1000;
  deviceLog("Sleep: %d seconds\n", seconds);
  sendLogs();
  Serial.flush();
  delay(10);

  // Shut down WiFi radio before sleep
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  delay(10);

  // Put IT8951E into STANDBY mode (0x0002) to reduce sleep current
  // STANDBY draws ~1-2mA vs ~5-10mA idle. Wakes with any host command
  // (M5.begin() sends SYS_RUN during panel init on next boot)
  M5.Display.waitDisplay();
  M5.Display.powerSave(true);
  delay(100);

  // Configure wake sources
  // Button press (GPIO39, active LOW) or timer
  esp_sleep_enable_ext0_wakeup((gpio_num_t)M5PAPER_WAKE_BUTTON, LOW);
 
  // Hold ALL GPIO states through deep sleep (critical for M5Paper on battery)
  gpio_hold_en((gpio_num_t)M5EPD_MAIN_PWR_PIN);
  gpio_deep_sleep_hold_en();
  delay(2100);  // Allow hardware time to settle before deep sleep

  esp_deep_sleep((uint64_t)seconds * 1000000ULL);
}
