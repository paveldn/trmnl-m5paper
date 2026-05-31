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
 * @version 2.1.0
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
#define FW_VERSION          "2.1.0"
#define DISPLAY_WIDTH       960
#define DISPLAY_HEIGHT      540

// ─────────────────────────── Configuration ───────────────────────────
#define DEFAULT_API_BASE_URL   "https://trmnl.app"
#define DEFAULT_REFRESH_RATE   900   // 15 minutes (in seconds)
#define WIFI_CONNECT_TIMEOUT   20000 // 20 seconds
#define WIFI_AP_TIMEOUT        300   // 5 minutes in AP mode before sleep
#define MAX_IMAGE_SIZE         200000

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

// NTP
#define NTP_SERVER_1           "pool.ntp.org"
#define NTP_SERVER_2           "time.google.com"
#define NTP_SYNC_INTERVAL      86400 // Re-sync every 24 hours

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
#define KEY_LAST_NTP_SYNC      "last_sync"

// ─────────────────────────── RTC Memory ───────────────────────────
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int partialRefreshCount = 0;

// ─────────────────────────── Globals ───────────────────────────
Preferences prefs;
M5Canvas canvas(&M5.Display);
String configuredSSID;
String configuredPass;
String apiKey;
String apiBaseUrl;
String friendlyId;
int refreshRate = DEFAULT_REFRESH_RATE;

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
bool syncClock();
bool performOTA(const char* firmwareUrl);
void wifiErrorSleep();
void apiErrorSleep();

// ═══════════════════════════════════════════════════════════════════════════════
//  SETUP — Main algorithm (runs on every wake)
// ═══════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  bootCount++;

  // ── Determine wake reason ──
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bool userWake = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0 ||
                   wakeup_reason == ESP_SLEEP_WAKEUP_EXT1 ||
                   wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED);
  bool timerWake = (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER);

  Serial.printf("[Boot #%d] Wake: %s\n", bootCount,
                userWake ? "USER" : (timerWake ? "TIMER" : "OTHER"));

  // ── Initialize M5Paper ──
  auto cfg = M5.config();
  cfg.output_power = true;
  cfg.internal_rtc = true;
  M5.begin(cfg);

  // CRITICAL: GPIO2 HIGH for SY7088 boost converter
  pinMode(M5EPD_MAIN_PWR_PIN, OUTPUT);
  digitalWrite(M5EPD_MAIN_PWR_PIN, HIGH);
  gpio_hold_dis((gpio_num_t)M5EPD_MAIN_PWR_PIN);

  // ── Check reset button (before display init) ──
  // Aligned with TRMNL: 5s hold = WiFi clear, 15s = factory reset
  if (digitalRead(M5PAPER_WAKE_BUTTON) == LOW) {
    Serial.println("Button held at boot...");
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
      Serial.println("Factory reset (15s hold)");
      M5.Display.setRotation(1);
      M5.Display.setEpdMode(epd_mode_t::epd_quality);
      canvas.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);
      prefs.begin(NVS_NAMESPACE, false);
      prefs.clear();
      prefs.end();
      showErrorScreen("Factory Reset\n\nAll settings cleared\nRestarting...");
      delay(2000);
      ESP.restart();
      return;
    } else if (holdTime >= BUTTON_HOLD_TIME) {
      Serial.println("WiFi credentials clear (5s hold)");
      prefs.begin(NVS_NAMESPACE, false);
      prefs.remove(KEY_WIFI_SSID);
      prefs.remove(KEY_WIFI_PASS);
      prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
      prefs.end();
      // Fall through — will start captive portal below
    }
  }

  // ── Display init ──
  M5.Display.setRotation(1);  // landscape 960x540
  M5.Display.setEpdMode(epd_mode_t::epd_fast);

  // Power optimization
  setCpuFrequencyMhz(80);
  btStop();

  canvas.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  pinMode(M5PAPER_WAKE_BUTTON, INPUT_PULLUP);

  // ── Display clear on user wake ──
  if (userWake) {
    showLoadingScreen();
  }

  // ── Load settings from NVS ──
  loadSettings();
  Serial.printf("API URL: %s, Refresh: %ds\n", apiBaseUrl.c_str(), refreshRate);

  // ── Check if WiFi is configured ──
  if (configuredSSID.length() == 0) {
    Serial.println("No WiFi configured - starting captive portal");
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

  // ── Clock synchronization ──
  syncClock();

  // ── Check if API key and friendly ID exist ──
  if (apiKey.length() == 0) {
    registerDevice();
    // registerDevice handles sleep on failure
    if (apiKey.length() == 0) return;
  }

  // ── Ping server (fetch display) ──
  float batteryVoltage = getBatteryVoltage();
  Serial.printf("Battery: %.2f V\n", batteryVoltage);
  fetchAndDisplay(batteryVoltage);
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
}

// ═══════════════════════════════════════════════════════════════════════════════
//  WIFI CONNECTION
// ═══════════════════════════════════════════════════════════════════════════════
bool connectWiFi() {
  Serial.printf("Connecting to WiFi: %s\n", configuredSSID.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.begin(configuredSSID.c_str(), configuredPass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT) {
      Serial.println("WiFi connection timeout");
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      return false;
    }
    delay(250);
    Serial.print(".");
  }

  Serial.printf("\nConnected! IP: %s, RSSI: %d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

// WiFi error sleep with exponential backoff (60s, 180s, 300s then button-only)
void wifiErrorSleep() {
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
      Serial.println("WiFi max retries — button-only sleep");
      showErrorScreen("WiFi unreachable\n" + configuredSSID + "\n\nPress button to retry\nHold 5s to reconfigure");
      goToDeepSleepButtonOnly();
      return;
  }

  Serial.printf("WiFi retry #%d, sleeping %ds\n", retryCount, sleepTime);
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
      Serial.println("API max retries — sleeping normal rate");
      goToDeepSleep(refreshRate);
      return;
  }

  Serial.printf("API retry #%d, sleeping %ds\n", retryCount, sleepTime);
  prefs.putInt(KEY_API_RETRY_COUNT, retryCount + 1);
  prefs.end();

  goToDeepSleep(sleepTime);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  CLOCK SYNCHRONIZATION
// ═══════════════════════════════════════════════════════════════════════════════
bool syncClock() {
  prefs.begin(NVS_NAMESPACE, true);
  uint32_t lastSync = prefs.getUInt(KEY_LAST_NTP_SYNC, 0);
  prefs.end();

  // Skip sync if done within last 24 hours
  time_t now;
  time(&now);
  if (lastSync != 0 && now > 1000000000 && ((uint32_t)now - lastSync) < NTP_SYNC_INTERVAL) {
    Serial.println("NTP: skipping (synced recently)");
    return true;
  }

  Serial.println("NTP: syncing...");
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10000)) {
    Serial.printf("NTP: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    time(&now);
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt(KEY_LAST_NTP_SYNC, (uint32_t)now);
    prefs.end();
    return true;
  }

  Serial.println("NTP: sync failed");
  return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  DEVICE REGISTRATION (/api/setup)
// ═══════════════════════════════════════════════════════════════════════════════
void registerDevice() {
  Serial.println("GET /api/setup...");

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
    Serial.printf("Setup failed, HTTP %d\n", code);
    http.end();
    showErrorScreen("Setup failed\nHTTP " + String(code) + "\n" + apiBaseUrl);
    apiErrorSleep();
    return;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("Setup: JSON parse error");
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
      Serial.printf("Registered! API Key: %s, Friendly ID: %s\n", key.c_str(), fid.c_str());
    }
  } else if (status == 404) {
    // MAC not registered on server
    Serial.println("MAC not registered on server");
    showSetupScreen("Register your device\n\nMAC: " + WiFi.macAddress() + "\n\non usetrmnl.com\nor your server dashboard");
    goToDeepSleep(SLEEP_AFTER_SETUP);
  } else {
    Serial.printf("Setup: unexpected status %d\n", status);
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
  Serial.printf("Battery: %d%%, %.2fV\n", level, voltage);
  return voltage;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  API COMMUNICATION (/api/display)
// ═══════════════════════════════════════════════════════════════════════════════
void fetchAndDisplay(float batteryVoltage) {
  Serial.println("GET /api/display...");

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
    Serial.printf("API failed, HTTP %d\n", code);
    http.end();
    showErrorScreen("Server error\nHTTP " + String(code));
    apiErrorSleep();
    return;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("API: JSON parse error");
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
    Serial.println("Server requested device reset");
    clearAllSettings();
    ESP.restart();
    return;
  }

  // ── Handle status codes ──
  if (status == 202) {
    // Device not yet linked to a user / plugin not attached
    Serial.println("Status 202: plugin not attached");
    showSetupScreen("Waiting for setup\n\nID: " + friendlyId + "\nMAC: " + WiFi.macAddress());
    saveRefreshRate(SLEEP_NOT_CONNECTED);
    goToDeepSleep(SLEEP_NOT_CONNECTED);
    return;
  }

  if (status == 500) {
    // Server says device not found for this token
    Serial.println("Status 500: device not found, resetting");
    clearAllSettings();
    ESP.restart();
    return;
  }

  if (status != 0) {
    Serial.printf("API: unexpected status %d\n", status);
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
    Serial.printf("Refresh rate: %d -> %d\n", refreshRate, newRefreshRate);
    saveRefreshRate(newRefreshRate);
  }

  // ── OTA firmware update ──
  if (updateFirmware && firmwareUrl && strlen(firmwareUrl) > 0) {
    Serial.printf("OTA update: %s\n", firmwareUrl);
    if (performOTA(firmwareUrl)) {
      return;  // OTA success — device will restart
    }
    Serial.println("OTA failed, continuing...");
  }

  // ── Check if image needs update ──
  if (!imageUrl || strlen(imageUrl) == 0) {
    Serial.println("No image_url — sleeping");
    goToDeepSleep(refreshRate);
    return;
  }

  // Check if filename changed (image caching)
  bool needsUpdate = true;
  if (filename && strlen(filename) > 0) {
    prefs.begin(NVS_NAMESPACE, false);
    String lastFile = prefs.getString(KEY_LAST_FILENAME, "");
    if (lastFile == String(filename)) {
      Serial.println("Image unchanged (same filename) — skipping display");
      needsUpdate = false;
    } else {
      prefs.putString(KEY_LAST_FILENAME, String(filename));
    }
    prefs.end();
  }

  if (needsUpdate) {
    Serial.printf("Image: %s\n", imageUrl);
    displayImage(imageUrl);
  }

  goToDeepSleep(refreshRate);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  IMAGE DISPLAY
// ═══════════════════════════════════════════════════════════════════════════════
void displayImage(const char* imageUrl) {
  Serial.println("Downloading image...");

  if (downloadAndDisplayImage(imageUrl)) {
    partialRefreshCount++;

    if (partialRefreshCount >= FULL_REFRESH_INTERVAL) {
      M5.Display.setEpdMode(epd_mode_t::epd_quality);
      partialRefreshCount = 0;
      Serial.println("Full quality refresh (ghost clear)");
    } else {
      M5.Display.setEpdMode(epd_mode_t::epd_fast);
      Serial.printf("Fast refresh (%d/%d until full)\n", partialRefreshCount, FULL_REFRESH_INTERVAL);
    }

    canvas.pushSprite(0, 0);
    M5.Display.display();
    Serial.println("Image displayed.");
  } else {
    Serial.println("Image display failed!");
  }
}

bool downloadAndDisplayImage(const char* url) {
  HTTPClient http;
  http.begin(url);
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Image download failed, HTTP %d\n", code);
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len <= 0) len = MAX_IMAGE_SIZE;
  if (len > MAX_IMAGE_SIZE) {
    Serial.printf("Image too large: %d\n", len);
    http.end();
    return false;
  }

  uint8_t* buffer = (uint8_t*)ps_malloc(len);
  if (!buffer) {
    buffer = (uint8_t*)malloc(len);
  }
  if (!buffer) {
    Serial.println("Memory allocation failed");
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
        Serial.println("Download timeout");
        break;
      }
      delay(1);
    }
  }

  http.end();

  // Disconnect WiFi early to save power during image rendering
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  Serial.printf("Downloaded: %d bytes\n", bytesRead);

  if (bytesRead < 100) {
    Serial.println("Data too small");
    free(buffer);
    return false;
  }

  bool success = false;

  // Detect format by magic bytes
  if (bytesRead >= 4 && buffer[0] == 0x89 && buffer[1] == 0x50 &&
      buffer[2] == 0x4E && buffer[3] == 0x47) {
    Serial.println("PNG format");
    success = canvas.drawPng(buffer, bytesRead, 0, 0);
  } else if (bytesRead >= 2 && buffer[0] == 'B' && buffer[1] == 'M') {
    Serial.println("BMP format");
    success = canvas.drawBmp(buffer, bytesRead, 0, 0);
  } else {
    Serial.printf("Unknown format: %02X %02X %02X %02X\n",
                  buffer[0], buffer[1], buffer[2], buffer[3]);
  }

  free(buffer);
  return success;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  OTA FIRMWARE UPDATE
// ═══════════════════════════════════════════════════════════════════════════════
bool performOTA(const char* firmwareUrl) {
  Serial.println("Starting OTA update...");
  showSetupScreen("Firmware Update\n\nDownloading...\nDo not power off");

  HTTPClient http;
  http.begin(firmwareUrl);
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("OTA download failed, HTTP %d\n", code);
    http.end();
    showErrorScreen("Firmware update failed\nHTTP " + String(code));
    return false;
  }

  size_t contentLength = http.getSize();
  if (contentLength <= 0) {
    Serial.println("OTA: unknown content length");
    http.end();
    showErrorScreen("Firmware update failed\nInvalid size");
    return false;
  }

  if (!Update.begin(contentLength)) {
    Serial.println("OTA: not enough space");
    http.end();
    showErrorScreen("Firmware update failed\nNot enough space");
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  http.end();

  if (written != contentLength) {
    Serial.printf("OTA: wrote %d/%d bytes\n", written, contentLength);
    Update.abort();
    showErrorScreen("Firmware update failed\nIncomplete download");
    return false;
  }

  if (!Update.end(true)) {
    Serial.println("OTA: finalization failed");
    showErrorScreen("Firmware update failed\nVerification error");
    return false;
  }

  Serial.println("OTA success! Restarting...");
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
//  DEEP SLEEP
// ═══════════════════════════════════════════════════════════════════════════════
// M5Paper has no PMIC — GPIO2 drives the SY7088 boost converter.
// GPIO2 must be held HIGH during deep sleep or the device powers off permanently.

void goToDeepSleep(int seconds) {
  Serial.printf("Sleep: %d seconds\n", seconds);
  Serial.flush();

  canvas.deleteSprite();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  delay(100);

  M5.Display.sleep();
  M5.Display.waitDisplay();

  // Hold GPIO2 HIGH through deep sleep
  pinMode(M5EPD_MAIN_PWR_PIN, OUTPUT);
  digitalWrite(M5EPD_MAIN_PWR_PIN, HIGH);
  gpio_hold_en((gpio_num_t)M5EPD_MAIN_PWR_PIN);
  gpio_deep_sleep_hold_en();

  Serial.flush();
  delay(10);
  M5.Power.deepSleep((uint64_t)seconds * 1000000ULL, true);
}

void goToDeepSleepButtonOnly() {
  Serial.println("Sleep: button-only wake");
  Serial.flush();

  canvas.deleteSprite();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  delay(100);

  M5.Display.sleep();
  M5.Display.waitDisplay();

  // Hold GPIO2 HIGH through deep sleep
  pinMode(M5EPD_MAIN_PWR_PIN, OUTPUT);
  digitalWrite(M5EPD_MAIN_PWR_PIN, HIGH);
  gpio_hold_en((gpio_num_t)M5EPD_MAIN_PWR_PIN);
  gpio_deep_sleep_hold_en();

  // Only button wake — no timer
  esp_sleep_enable_ext0_wakeup((gpio_num_t)M5PAPER_WAKE_BUTTON, 0);

  Serial.flush();
  delay(10);
  esp_deep_sleep_start();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LOOP (not used - ESP32 restarts from setup() on each wake)
// ═══════════════════════════════════════════════════════════════════════════════
void loop() {
  // Only reached during captive portal operation
}
