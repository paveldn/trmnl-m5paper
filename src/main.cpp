/**
 * M5Paper TRMNL Firmware
 * 
 * Fetches and displays images from TRMNL API on M5Paper e-ink display.
 * Optimized for low power consumption with deep sleep between refreshes.
 * 
 * Features:
 * - WiFi captive portal for configuration (no hardcoded credentials)
 * - Support for official TRMNL server and custom/local servers
 * - Proper deep sleep with GPIO2 power hold (M5Paper hardware requirement)
 * - Power-optimized sleep cycle
 * - NVS-based persistent settings
 * - Device registration via MAC address (TRMNL API compatible)
 * - Battery voltage reporting
 * - Button wake from deep sleep
 * 
 * Hardware: M5Paper (ESP32-D0WDQ5, 4.7" e-paper 960x540)
 * 
 * @version 2.0.0
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

#include "captive_portal.h"

// ─────────────────────────── Hardware Defines ───────────────────────────
#define M5PAPER_WAKE_BUTTON     39   // GPIO39 - physical button
#define M5EPD_MAIN_PWR_PIN       2   // GPIO2 - SY7088 enable (main 3.3V rail)
#define DEVICE_MODEL        "m5paper"
#define FW_VERSION          "2.0.0"
#define DISPLAY_WIDTH       960
#define DISPLAY_HEIGHT      540

// ─────────────────────────── Default Configuration ───────────────────────────
#define DEFAULT_API_BASE_URL   "https://trmnl.app"
#define DEFAULT_REFRESH_RATE   900   // 15 minutes
#define WIFI_CONNECT_TIMEOUT   20000 // 20 seconds
#define WIFI_AP_TIMEOUT        300   // 5 minutes in AP mode before sleep
#define MAX_IMAGE_SIZE         200000
// ─────────────────────────── NVS Keys ───────────────────────────
#define NVS_NAMESPACE          "trmnl"
#define KEY_WIFI_SSID          "wifi_ssid"
#define KEY_WIFI_PASS          "wifi_pass"
#define KEY_API_KEY            "api_key"
#define KEY_API_URL            "api_url"
#define KEY_FRIENDLY_ID        "friendly_id"
#define KEY_REFRESH_RATE       "refresh_rate"
#define KEY_WIFI_CONFIGURED    "wifi_ok"
#define KEY_WIFI_FAILS         "wifi_fails"
#define KEY_SERVER_FAILS       "srv_fails"

// ─────────────────────────── RTC Memory ───────────────────────────
RTC_DATA_ATTR int lastRefreshRate = DEFAULT_REFRESH_RATE;
RTC_DATA_ATTR int bootCount = 0;

#define MAX_WIFI_FAILURES    3
#define MAX_SERVER_FAILURES  3

// ─────────────────────────── Globals ───────────────────────────
Preferences prefs;
M5Canvas canvas(&M5.Display);
String configuredSSID;
String configuredPass;
String apiKey;
String apiBaseUrl;
String friendlyId;

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
void showSetupScreen(const String& message);
void showErrorScreen(const String& message);
int getFailCount(const char* key);
void incrementFailCount(const char* key);
void resetFailCount(const char* key);
void handleServerFailure(const String& reason);

// ═══════════════════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  bootCount++;

  // ── Log wake cause for debugging ──
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.printf("[Boot #%d] Wake: TIMER\n", bootCount);
      break;
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.printf("[Boot #%d] Wake: BUTTON (ext0)\n", bootCount);
      break;
    default:
      Serial.printf("[Boot #%d] Wake: POWER-ON/RESET (cause=%d)\n", bootCount, wakeup_reason);
      break;
  }

  // ── Initialize M5Paper ──
  auto cfg = M5.config();
  cfg.output_power = true;   // This sets GPIO2 HIGH (main power hold)
  cfg.internal_rtc = true;
  M5.begin(cfg);

  // CRITICAL: Ensure GPIO2 is explicitly HIGH for power rail.
  // M5Paper uses SY7088 boost converter controlled by GPIO2.
  // If GPIO2 goes LOW during deep sleep, the device powers off permanently.
  pinMode(M5EPD_MAIN_PWR_PIN, OUTPUT);
  digitalWrite(M5EPD_MAIN_PWR_PIN, HIGH);

  // Release GPIO hold from previous deep sleep (safe to do after GPIO2 is driven HIGH)
  gpio_hold_dis((gpio_num_t)M5EPD_MAIN_PWR_PIN);

  M5.Display.setRotation(1);  // landscape 960x540
  M5.Display.setEpdMode(epd_mode_t::epd_quality);

  // ── Power optimization: reduce CPU clock ──
  setCpuFrequencyMhz(80);
  btStop();  // Disable Bluetooth

  canvas.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  pinMode(M5PAPER_WAKE_BUTTON, INPUT_PULLUP);

  // ── Load settings from NVS ──
  loadSettings();

  int wifiFailCount = getFailCount(KEY_WIFI_FAILS);
  int serverFailCount = getFailCount(KEY_SERVER_FAILS);
  Serial.printf("[Boot #%d] WiFi fails: %d, Server fails: %d\n", bootCount, wifiFailCount, serverFailCount);
  Serial.printf("API URL: %s\n", apiBaseUrl.c_str());

  // ── Check if WiFi is configured ──
  if (configuredSSID.length() == 0) {
    Serial.println("No WiFi configured - starting captive portal");
    startCaptivePortal();
    return;
  }

  // ── Check for button press on ANY boot ──
  // Hold >3s = enter setup portal, Hold >10s = factory reset
  if (digitalRead(M5PAPER_WAKE_BUTTON) == LOW) {
    Serial.println("Button held at boot...");
    unsigned long pressStart = millis();
    bool enteredPortal = false;
    while (digitalRead(M5PAPER_WAKE_BUTTON) == LOW) {
      unsigned long held = millis() - pressStart;
      if (held > 10000) {
        Serial.println("10s hold - FACTORY RESET");
        clearAllSettings();
        resetFailCount(KEY_WIFI_FAILS);
        resetFailCount(KEY_SERVER_FAILS);
        showErrorScreen("Factory Reset\n\nRestarting...");
        delay(2000);
        ESP.restart();
        return;
      }
      if (held > 3000 && !enteredPortal) {
        Serial.println("3s hold - entering setup mode");
        enteredPortal = true;
        // Keep waiting to see if user holds longer for factory reset
      }
      delay(50);
    }
    if (enteredPortal) {
      startCaptivePortal();
      return;
    }
  }

  // ── Connect to WiFi ──
  if (!connectWiFi()) {
    incrementFailCount(KEY_WIFI_FAILS);
    int fails = getFailCount(KEY_WIFI_FAILS);
    if (fails >= MAX_WIFI_FAILURES) {
      Serial.println("Too many WiFi failures - entering setup mode");
      resetFailCount(KEY_WIFI_FAILS);
      startCaptivePortal();
      return;
    }
    int backoff = 30 * (1 << (fails - 1));  // 30s, 60s, 120s
    Serial.printf("WiFi failed, retry in %ds\n", backoff);
    showErrorScreen("WiFi connection failed\nRetrying in " + String(backoff) + "s\n\n(" + String(MAX_WIFI_FAILURES - fails) + " tries before setup)");
    goToDeepSleep(backoff);
    return;
  }

  resetFailCount(KEY_WIFI_FAILS);  // Reset on successful connection

  float batteryVoltage = getBatteryVoltage();
  Serial.printf("Battery: %.2f V\n", batteryVoltage);

  // ── Register device if no API key ──
  if (apiKey.length() == 0) {
    registerDevice();
  }

  // ── Fetch and display ──
  fetchAndDisplay(batteryVoltage);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  SETTINGS MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════════
void loadSettings() {
  prefs.begin(NVS_NAMESPACE, true);  // read-only
  configuredSSID = prefs.getString(KEY_WIFI_SSID, "");
  configuredPass = prefs.getString(KEY_WIFI_PASS, "");
  apiKey = prefs.getString(KEY_API_KEY, "");
  apiBaseUrl = prefs.getString(KEY_API_URL, DEFAULT_API_BASE_URL);
  friendlyId = prefs.getString(KEY_FRIENDLY_ID, "");
  lastRefreshRate = prefs.getInt(KEY_REFRESH_RATE, DEFAULT_REFRESH_RATE);
  prefs.end();
}

void saveWiFiSettings(const String& ssid, const String& pass) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(KEY_WIFI_SSID, ssid);
  prefs.putString(KEY_WIFI_PASS, pass);
  prefs.putBool(KEY_WIFI_CONFIGURED, true);
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
  lastRefreshRate = rate;
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
}

// ─────────────────────────── Fail Counter (NVS-persisted) ───────────────────
int getFailCount(const char* key) {
  prefs.begin(NVS_NAMESPACE, true);
  int val = prefs.getInt(key, 0);
  prefs.end();
  return val;
}

void incrementFailCount(const char* key) {
  prefs.begin(NVS_NAMESPACE, false);
  int val = prefs.getInt(key, 0);
  prefs.putInt(key, val + 1);
  prefs.end();
}

void resetFailCount(const char* key) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putInt(key, 0);
  prefs.end();
}

void handleServerFailure(const String& reason) {
  incrementFailCount(KEY_SERVER_FAILS);
  int fails = getFailCount(KEY_SERVER_FAILS);
  Serial.printf("Server failure #%d: %s\n", fails, reason.c_str());

  if (fails >= MAX_SERVER_FAILURES) {
    resetFailCount(KEY_SERVER_FAILS);
    showErrorScreen("Server unreachable\n" + apiBaseUrl + "\n\nEntering setup...");
    delay(3000);
    startCaptivePortal();
    return;
  }

  int backoff = 30 * (1 << (fails - 1));  // 30s, 60s, 120s
  showErrorScreen(reason + "\n" + apiBaseUrl + "\n\nRetry in " + String(backoff) + "s\n(" + String(MAX_SERVER_FAILURES - fails) + " tries before setup)");
  goToDeepSleep(backoff);
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



// ═══════════════════════════════════════════════════════════════════════════════
//  DEVICE REGISTRATION
// ═══════════════════════════════════════════════════════════════════════════════
void registerDevice() {
  Serial.println("Registering device with TRMNL server...");

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
    Serial.printf("Registration failed, HTTP %d\n", code);
    http.end();
    handleServerFailure("Registration failed (HTTP " + String(code) + ")");
    return;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("Registration JSON parse error");
    handleServerFailure("Registration: bad response");
    return;
  }

  int status = doc["status"] | 404;
  if (status == 200) {
    String key = doc["api_key"] | "";
    String fid = doc["friendly_id"] | "";

    if (key.length() > 0) {
      resetFailCount(KEY_SERVER_FAILS);
      saveServerSettings(key, "");
      Serial.printf("Registered! API Key: %s, Friendly ID: %s\n", key.c_str(), fid.c_str());

      prefs.begin(NVS_NAMESPACE, false);
      prefs.putString(KEY_FRIENDLY_ID, fid);
      prefs.end();
      friendlyId = fid;
    }
  } else {
    Serial.printf("Device not registered on server (status %d)\n", status);
    Serial.println("Add this MAC to your TRMNL dashboard: " + WiFi.macAddress());
    showSetupScreen("Register device MAC:\n" + WiFi.macAddress() + "\non your TRMNL dashboard");
    goToDeepSleep(60);  // Retry in 1 minute
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
//  API COMMUNICATION
// ═══════════════════════════════════════════════════════════════════════════════
void fetchAndDisplay(float batteryVoltage) {
  Serial.println("Fetching display from TRMNL API...");

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
  http.addHeader("User-Agent", "M5Paper-TRMNL/" FW_VERSION);
  http.addHeader("FW-Version", FW_VERSION);
  http.addHeader("RSSI", String(WiFi.RSSI()));
  http.addHeader("Model", DEVICE_MODEL);
  http.addHeader("Width", String(DISPLAY_WIDTH));
  http.addHeader("Height", String(DISPLAY_HEIGHT));
  http.addHeader("Refresh-Rate", String(lastRefreshRate));

  int code = http.GET();
  if (code < 200 || code >= 300) {
    Serial.printf("API request failed, HTTP %d\n", code);
    http.end();
    handleServerFailure("Server error (HTTP " + String(code) + ")");
    return;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("JSON parse error");
    handleServerFailure("Server: bad response");
    return;
  }

  // Server responded successfully - reset fail counter
  resetFailCount(KEY_SERVER_FAILS);

  int status = doc["status"] | -1;

  // Handle reset command from server
  bool resetFirmware = doc["reset_firmware"] | false;
  if (resetFirmware) {
    Serial.println("Server requested credential reset");
    clearAllSettings();
    ESP.restart();
    return;
  }

  if (status != 0) {
    Serial.printf("API status: %d\n", status);
    if (status == 202) {
      // Device not yet linked to user
      showSetupScreen("Waiting for setup\nID: " + friendlyId + "\nMAC: " + WiFi.macAddress());
      goToDeepSleep(30);
      return;
    }
    goToDeepSleep(300);
    return;
  }

  const char* imageUrl = doc["image_url"];
  if (!imageUrl || strlen(imageUrl) == 0) {
    Serial.println("No image_url in response");
    goToDeepSleep(300);
    return;
  }

  // Update refresh rate from server
  int refreshSec = doc["refresh_rate"] | lastRefreshRate;
  if (refreshSec != lastRefreshRate) {
    saveRefreshRate(refreshSec);
  }

  Serial.printf("Image: %s\nRefresh: %ds\n", imageUrl, refreshSec);

  displayImage(imageUrl);
  goToDeepSleep(refreshSec);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  IMAGE DISPLAY
// ═══════════════════════════════════════════════════════════════════════════════
void displayImage(const char* imageUrl) {
  Serial.println("Downloading image...");

  if (downloadAndDisplayImage(imageUrl)) {
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

  uint8_t* buffer = (uint8_t*)malloc(len);
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
//  DISPLAY HELPERS
// ═══════════════════════════════════════════════════════════════════════════════
void showSetupScreen(const String& message) {
  canvas.fillSprite(TFT_WHITE);
  canvas.setTextColor(TFT_BLACK);
  canvas.setTextDatum(MC_DATUM);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  
  // Draw title
  canvas.drawString("M5Paper TRMNL", DISPLAY_WIDTH / 2, 60);
  
  // Draw message lines
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

  // Draw footer
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.drawString("FW " FW_VERSION, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT - 40);

  canvas.pushSprite(0, 0);
  M5.Display.display();
}

void showErrorScreen(const String& message) {
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

  canvas.pushSprite(0, 0);
  M5.Display.display();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  DEEP SLEEP
// ═══════════════════════════════════════════════════════════════════════════════
// Enters ESP32 deep sleep with timer and button (GPIO39) wake sources.
// M5Paper has no PMIC — it keeps itself powered via GPIO2 driving the SY7088
// boost converter. GPIO2 must be explicitly held HIGH during deep sleep,
// otherwise the device powers off permanently and cannot wake.
void goToDeepSleep(int seconds) {
  Serial.printf("Entering deep sleep for %d seconds...\n", seconds);
  Serial.flush();

  canvas.deleteSprite();

  // WiFi must be stopped before entering deep sleep
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  delay(100);

  M5.Display.sleep();
  M5.Display.waitDisplay();

  // Hold GPIO2 HIGH through deep sleep to keep SY7088 boost converter enabled
  pinMode(M5EPD_MAIN_PWR_PIN, OUTPUT);
  digitalWrite(M5EPD_MAIN_PWR_PIN, HIGH);
  gpio_hold_en((gpio_num_t)M5EPD_MAIN_PWR_PIN);
  gpio_deep_sleep_hold_en();

  // M5.Power.deepSleep sets up timer + ext0 (button) wake sources
  Serial.printf("Timer set for %d seconds. Sleeping now.\n", seconds);
  Serial.flush();
  delay(10);
  M5.Power.deepSleep((uint64_t)seconds * 1000000ULL, true);
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LOOP (not used - ESP32 restarts from setup() on each wake)
// ═══════════════════════════════════════════════════════════════════════════════
void loop() {
  // Only reached during captive portal operation (handled in startCaptivePortal)
}
