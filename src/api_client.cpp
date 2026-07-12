#include "api_client.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>

#include "api_helpers.h"
#include "image_pipeline.h"
#include "ota.h"
#include "power.h"
#include "preferences_persistence.h"
#include "trmnl_keys.h"

#ifndef OTA_MIN_BATTERY_VOLTAGE
#define OTA_MIN_BATTERY_VOLTAGE 3.65
#endif

static const int SLEEP_NOT_CONNECTED = 5;
static const float LOW_BATTERY_VOLTAGE = 3.4f;
static const uint32_t OTA_SAFETY_INTERVAL_SEC = 86400UL;
static const char* DEVICE_MODEL = "m5paper";
static const int DISPLAY_WIDTH = 960;
static const int DISPLAY_HEIGHT = 540;

extern Preferences prefs;
extern String apiKey;
extern String apiBaseUrl;
extern String friendlyId;
extern int refreshRate;
extern bool otaBetaMode;
extern bool forceOtaOnThisBoot;
extern int lastWakeTime;
extern const char* FW_VERSION_STR;
extern const char* UPDATE_SOURCE_STR;

#if defined(DEBUG_LOGS) || defined(ENABLE_SERVER_LOGS)
extern String logBuffer;
#endif

extern void deviceLog(const char* fmt, ...);
extern void disableWiFiPS();
extern void enableWiFiPS();
extern void showErrorScreen(const String& message);
extern void showSetupScreen(const String& message);
extern void apiErrorSleep();
extern void checkRuntimeReset();
extern void goToDeepSleep(int seconds);

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

void sendLogs() {
#ifndef ENABLE_SERVER_LOGS
  return;
#else
  // Build a message from debug buffer if present, otherwise synthesize minimal metadata
  String msg = "";
#ifdef DEBUG_LOGS
  Serial.printf("[Log] sendLogs called: bufLen=%d baseUrl='%s' wifi=%d\n",
    logBuffer.length(), apiBaseUrl.c_str(), WiFi.status());
#endif
  if (logBuffer.length() > 0) msg = logBuffer;

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
  int apiRetryCount = prefs.getInt(KEY_API_RETRY_COUNT, 1);
  prefs.putUInt(KEY_LOG_ID, logId + 1);
  prefs.end();

  disableWiFiPS();
  HTTPClient http;
  String url = apiBaseUrl + "/api/log";
  http.begin(url);
  http.setTimeout(5000);
  addLogHeaders(http, WiFi.macAddress(), apiKey);

  JsonDocument doc;
  JsonArray logs = doc["logs"].to<JsonArray>();
  JsonObject entry = logs.add<JsonObject>();

  entry["created_at"] = (uint32_t)time(nullptr);
  entry["id"] = logId;
  entry["level"] = "debug";
  entry["message"] = msg;
  entry["retry"] = apiRetryCount;
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

#ifndef DEBUG_LOGS
  if (code >= 200 && code < 300) {
    logBuffer = "";
  }
#endif
#endif
}

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
  tryInitRtcFromHttpDate(http.header("Date"), false);

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
    if (fid.length() > 0) {
      deviceLog("Friendly ID detected: %s\n", fid.c_str());
    } else {
      deviceLog("No friendly ID detected\n");
    }

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
    goToDeepSleep(60);
  } else {
    deviceLog("Setup: unexpected status %d\n", status);
    showErrorScreen("Setup error\nStatus: " + String(status));
    apiErrorSleep();
  }
}

void fetchAndDisplay(float batteryVoltage) {
  deviceLog("GET /api/display...\n");
  disableWiFiPS();
  uint8_t batteryLevel = 0;
  if (batteryVoltage > 0.5f) {
    constexpr float BATTERY_FULL = 4.10f;
    constexpr float BATTERY_EMPTY = 3.40f;
    constexpr float BATTERY_GAMMA = 1.70f;
    float clamped = std::clamp(batteryVoltage, BATTERY_EMPTY, BATTERY_FULL);
    float normalized = (clamped - BATTERY_EMPTY) / (BATTERY_FULL - BATTERY_EMPTY);
    float capacity = std::pow(normalized, BATTERY_GAMMA);
    batteryLevel = static_cast<uint8_t>(capacity * 100.0f + 0.5f);
  }

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
                    batteryLevel,
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
  tryInitRtcFromHttpDate(http.header("Date"), false);

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

  String otaUrl = "";
  String otaVersion = "";
  bool forceOta = forceOtaOnThisBoot;
  forceOtaOnThisBoot = false;  // Consume one-shot force for this boot

  bool otaCheckAllowed = true;
  bool otaExternalPower = isExternalPowerPresent();
  if (batteryVoltage > 0.5 && batteryVoltage < OTA_MIN_BATTERY_VOLTAGE && !otaExternalPower) {
    otaCheckAllowed = false;
    deviceLog("OTA: skipped check due to low battery %.2fV < %.2fV\n", batteryVoltage, (double)OTA_MIN_BATTERY_VOLTAGE);
  }

  // Update refresh rate from server
  if (newRefreshRate != refreshRate) {
    deviceLog("Refresh rate: %d -> %d\n", refreshRate, newRefreshRate);
    saveRefreshRate(newRefreshRate);
  }

  // ── OTA firmware update ──
  if (otaCheckAllowed) {
    if (updateFirmware && firmwareUrl && strlen(firmwareUrl) > 0) {
      otaUrl = String(firmwareUrl);
      otaVersion = "server";
      if (forceOta) {
        deviceLog("OTA: server OTA selected while force flag active\n");
      }
    } else {
      String githubUrl;
      String githubVersion;
      deviceLog("OTA: checking GitHub channel %s\n", otaBetaMode ? "beta" : "stable");
      if (checkGitHubReleaseForUpdate(githubUrl, githubVersion, forceOta, otaBetaMode)) {
        otaUrl = githubUrl;
        otaVersion = githubVersion;
      }
    }
  }

  if (otaUrl.length() > 0) {
    if (!forceOta && !isIntervalElapsed(KEY_OTA_LAST_ATTEMPT, OTA_SAFETY_INTERVAL_SEC)) {
      deviceLog("OTA: attempt skipped due to 24h safety interval\n");
    } else {
      if (forceOta) {
        deviceLog("OTA: forcing attempt (cooldown bypass)\n");
      }
      float otaVoltage = readBatteryAvg(4, 30);
      bool externalPower = isExternalPowerPresent();
      if (otaVoltage > 0.5 && otaVoltage < LOW_BATTERY_VOLTAGE && !externalPower) {
        deviceLog("OTA: skipped due to low battery %.2fV\n", otaVoltage);
      } else {
        markIntervalNow(KEY_OTA_LAST_ATTEMPT);
        deviceLog("OTA update (%s): %s\n", otaVersion.c_str(), otaUrl.c_str());
        if (performOTA(otaUrl.c_str())) {
          return;  // OTA success — device will restart
        }
        deviceLog("OTA failed, continuing...\n");
      }
    }
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
#ifdef FORCE_IMAGE_REFRESH_ON_WAKE
  deviceLog("Image force-refresh enabled - redrawing on every wake\n");
#else
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
#endif

  if (needsUpdate) {
    displayImage(imageUrl);
  }

  // Allow user to perform runtime resets by holding the wake button now.
  checkRuntimeReset();

  goToDeepSleep(refreshRate);
}
