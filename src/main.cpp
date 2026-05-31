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
 * @version 2.2.0
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

#include "captive_portal.h"

// ─────────────────────────── Hardware Defines ───────────────────────────
#define M5PAPER_WAKE_BUTTON     39   // GPIO39 - physical button
#define M5EPD_MAIN_PWR_PIN       2   // GPIO2 - SY7088 enable (main 3.3V rail)
#define DEVICE_MODEL        "m5paper"
#define FW_VERSION          "2.2.0"
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
// ─────────────────────────── RTC Memory ───────────────────────────
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int partialRefreshCount = 0;
RTC_DATA_ATTR uint8_t savedBSSID[6] = {0};
RTC_DATA_ATTR uint8_t savedChannel = 0;
RTC_DATA_ATTR int wifiFailCount = 0;

// ─────────────────────────── Globals ───────────────────────────
Preferences prefs;
M5Canvas canvas(&M5.Display);
String configuredSSID;
String configuredPass;
String apiKey;
String apiBaseUrl;
String friendlyId;
int refreshRate = DEFAULT_REFRESH_RATE;

// ─────────────────────────── Log Buffer ───────────────────────────
String logBuffer;

void deviceLog(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
  if (logBuffer.length() < LOG_BUFFER_SIZE) {
    logBuffer += buf;
  }
}

// Exported constants for captive_portal.cpp
const char* FW_VERSION_STR = FW_VERSION;
int DEFAULT_REFRESH_RATE_VAL = DEFAULT_REFRESH_RATE;
int WIFI_AP_TIMEOUT_VAL = WIFI_AP_TIMEOUT;
const char* DEFAULT_API_BASE_URL_STR = DEFAULT_API_BASE_URL;

// ─────────────────────────── Forward Declarations ───────────────────────────
void loadSettings();
void saveWiFiSettings(const String& ssid, const String& pass);
void saveServerSettings(const String& key, const String& url);
void clearAllSettings();
bool connectWiFi();
float getBatteryVoltage();
void fetchAndDisplay(float batteryVoltage);
void displayImage(const char* imageUrl);
bool downloadAndDisplayImage(const char* url);
void registerDevice();
void goToDeepSleep(int seconds);
void goToDeepSleepButtonOnly();
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
  Serial.begin(115200);
  bootCount++;
  logBuffer.reserve(LOG_BUFFER_SIZE);

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

  // ── Display init ──
  M5.Display.setRotation(1);  // landscape 960x540
  M5.Display.setEpdMode(epd_mode_t::epd_fast);

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
      M5.Display.setEpdMode(epd_mode_t::epd_quality);
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

  // ── Low battery protection ──
  float bootVoltage = M5.Power.getBatteryVoltage() / 1000.0;
  if (bootVoltage > 0.5 && bootVoltage < LOW_BATTERY_VOLTAGE) {
    deviceLog("LOW BATTERY: %.2fV < %.1fV threshold\n", bootVoltage, LOW_BATTERY_VOLTAGE);
    showLowBatteryAndShutdown();
    return;
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
    showErrorScreen("WiFi connection failed\n" + configuredSSID);
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

  // Fast reconnect: use saved channel + BSSID if available
  bool fastConnect = (savedChannel != 0);
  if (fastConnect) {
    deviceLog("Fast reconnect ch:%d\n", savedChannel);
    WiFi.begin(configuredSSID.c_str(), configuredPass.c_str(), savedChannel, savedBSSID, true);
  } else {
    WiFi.begin(configuredSSID.c_str(), configuredPass.c_str());
  }

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > (fastConnect ? 5000 : WIFI_CONNECT_TIMEOUT)) {
      if (fastConnect) {
        // Fast connect failed — invalidate cache and retry with full scan
        deviceLog("Fast reconnect failed (ch:%d), resetting cache, full scan\n", savedChannel);
        savedChannel = 0;
        memset(savedBSSID, 0, 6);
        WiFi.disconnect(true);
        delay(10);
        WiFi.begin(configuredSSID.c_str(), configuredPass.c_str());
        fastConnect = false;
        start = millis();
        while (WiFi.status() != WL_CONNECTED) {
          if (millis() - start > WIFI_CONNECT_TIMEOUT) {
            deviceLog("WiFi FAILED after full scan (timeout %dms)\n", WIFI_CONNECT_TIMEOUT);
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            return false;
          }
          delay(250);
        }
        break;
      }
      deviceLog("WiFi FAILED (timeout %dms)\n", WIFI_CONNECT_TIMEOUT);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      return false;
    }
    delay(250);
  }

  // Save channel + BSSID for fast reconnect on next wake
  savedChannel = WiFi.channel();
  memcpy(savedBSSID, WiFi.BSSID(), 6);

  deviceLog("OK IP:%s RSSI:%d ch:%d %s\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                savedChannel, fastConnect ? "(fast)" : "(scan)");

  // Report previous WiFi failures to server
  if (wifiFailCount > 0) {
    deviceLog("WiFi: recovered after %d failed attempt(s)\n", wifiFailCount);
    wifiFailCount = 0;
  }

  return true;
}

// WiFi error sleep with exponential backoff (60s, 180s, 300s then button-only)
void wifiErrorSleep() {
  wifiFailCount++;

  prefs.begin(NVS_NAMESPACE, false);
  int retryCount = prefs.getInt(KEY_WIFI_RETRY_COUNT, 1);

  int sleepTime;
  switch (retryCount) {
    case 1: sleepTime = WIFI_RETRY_1; break;
    case 2: sleepTime = WIFI_RETRY_2; break;
    case 3: sleepTime = WIFI_RETRY_3; break;
    default:
      // Max retries exceeded — sleep until button press only
      prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
      prefs.end();
      deviceLog("WiFi max retries — button-only sleep\n");
      showErrorScreen("WiFi unreachable\n" + configuredSSID + "\n\nPress button to retry\nHold 5s to reconfigure");
      goToDeepSleepButtonOnly();
      return;
  }

  deviceLog("WiFi retry #%d, sleeping %ds\n", retryCount, sleepTime);
  prefs.putInt(KEY_WIFI_RETRY_COUNT, retryCount + 1);
  prefs.end();

  goToDeepSleep(sleepTime);
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

  goToDeepSleep(sleepTime);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LOG SUBMISSION (POST /api/log)
// ═══════════════════════════════════════════════════════════════════════════════
void sendLogs() {
  Serial.printf("[Log] sendLogs called: bufLen=%d baseUrl='%s' wifi=%d\n",
    logBuffer.length(), apiBaseUrl.c_str(), WiFi.status());

  if (logBuffer.length() == 0) { Serial.println("[Log] SKIP: buffer empty"); return; }

  // Only send to non-official servers (local TRMNL)
  if (apiBaseUrl == DEFAULT_API_BASE_URL) { Serial.println("[Log] SKIP: official server"); return; }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Log] SKIP: WiFi not connected, discarding");
    logBuffer = "";
    return;
  }

  HTTPClient http;
  String url = apiBaseUrl + "/api/log";
  http.begin(url);
  http.setTimeout(5000);
  if (apiKey.length() > 0) {
    http.addHeader("Access-Token", apiKey);
  }
  http.addHeader("ID", WiFi.macAddress());
  http.addHeader("Content-Type", "application/json");

  // Server expects: {"level": "info", "message": "..."}
  // Truncate to 5000 chars max as required by server
  String msg = logBuffer;
  if (msg.length() > 5000) {
    msg = msg.substring(0, 5000);
  }

  JsonDocument doc;
  doc["level"] = "info";
  doc["message"] = msg;

  String payload;
  serializeJson(doc, payload);

  Serial.printf("[Log] POST %s (%d bytes payload)\n", url.c_str(), payload.length());

  int code = http.POST(payload);
  String response = http.getString();
  Serial.printf("[Log] Response: %d - %s\n", code, response.c_str());
  http.end();

  logBuffer = "";
}

// ═══════════════════════════════════════════════════════════════════════════════
//  DEVICE REGISTRATION (/api/setup)
// ═══════════════════════════════════════════════════════════════════════════════
void registerDevice() {
  deviceLog("GET /api/setup...\n");

  HTTPClient http;
  String url = apiBaseUrl + "/api/setup";
  http.begin(url);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("ID", WiFi.macAddress());
  http.addHeader("Content-Type", "application/json");
  http.addHeader("FW-Version", FW_VERSION);
  http.addHeader("Model", DEVICE_MODEL);

  int code = http.GET();
  if (code < 200 || code >= 300) {
    deviceLog("Setup failed, HTTP %d\n", code);
    http.end();
    showErrorScreen("Setup failed\nHTTP " + String(code) + "\n" + apiBaseUrl);
    apiErrorSleep();
    return;
  }

  String payload = http.getString();
  http.end();

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

// ═══════════════════════════════════════════════════════════════════════════════
//  API COMMUNICATION (/api/display)
// ═══════════════════════════════════════════════════════════════════════════════
void fetchAndDisplay(float batteryVoltage) {
  deviceLog("GET /api/display...\n");

  HTTPClient http;
  String url = apiBaseUrl + "/api/display";
  http.begin(url);
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  // TRMNL API headers
  if (apiKey.length() > 0) {
    http.addHeader("Access-Token", apiKey);
  }
  http.addHeader("ID", WiFi.macAddress());
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Battery-Voltage", String(batteryVoltage, 2));
  http.addHeader("FW-Version", FW_VERSION);
  http.addHeader("RSSI", String(WiFi.RSSI()));
  http.addHeader("Model", DEVICE_MODEL);
  http.addHeader("Width", String(DISPLAY_WIDTH));
  http.addHeader("Height", String(DISPLAY_HEIGHT));
  http.addHeader("Refresh-Rate", String(refreshRate));

  int code = http.GET();
  if (code < 200 || code >= 300) {
    deviceLog("API failed, HTTP %d\n", code);
    http.end();
    showErrorScreen("Server error\nHTTP " + String(code));
    apiErrorSleep();
    return;
  }

  String payload = http.getString();
  http.end();

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
      M5.Display.setEpdMode(epd_mode_t::epd_quality);
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
  HTTPClient http;
  http.begin(url);
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  int code = http.GET();
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

  http.end();

  deviceLog("Downloaded: %d bytes\n", bytesRead);

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
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
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
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
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
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
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
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
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
  if (seconds < 15) seconds = 15;

  deviceLog("Sleep: %d seconds\n", seconds);
  sendLogs();
  Serial.flush();
  delay(10);

  // Shut down WiFi radio before sleep
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  delay(10);

  // Configure wake sources
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_sleep_enable_ext1_wakeup(1ULL << M5PAPER_WAKE_BUTTON, ESP_EXT1_WAKEUP_ALL_LOW);

  // Hold ALL GPIO states through deep sleep (critical for M5Paper on battery)
  // This keeps GPIO2 HIGH (SY7088 on) and SPI bus pins stable (IT8951E safe)
  gpio_hold_en((gpio_num_t)M5EPD_MAIN_PWR_PIN);
  gpio_deep_sleep_hold_en();

  esp_deep_sleep_start();
}

void goToDeepSleepButtonOnly() {
  deviceLog("Sleep: button-only wake\n");
  sendLogs();
  Serial.flush();

  // Shut down WiFi radio before sleep
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  delay(10);

  // Only button can wake
  esp_sleep_enable_ext1_wakeup(1ULL << M5PAPER_WAKE_BUTTON, ESP_EXT1_WAKEUP_ALL_LOW);

  // Hold ALL GPIO states through deep sleep
  gpio_hold_en((gpio_num_t)M5EPD_MAIN_PWR_PIN);
  gpio_deep_sleep_hold_en();

  esp_deep_sleep_start();
}
