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
 * @version defined by platformio.ini app_version
 * @see https://docs.trmnl.com/go/diy/byod
 */

#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <M5Unified.h>
#include <Preferences.h>

#include "captive_portal.h"
#include "api_client.h"
#include "button.h"
#include "display.h"
#include "image_pipeline.h"
#include "ota.h"
#include "preferences_persistence.h"
#include "power.h"
#include "trmnl_keys.h"
#include "wifi_network.h"

// ─────────────────────────── Hardware Defines ───────────────────────────
#define M5PAPER_WAKE_BUTTON     39   // GPIO39 - physical button
#define M5EPD_MAIN_PWR_PIN       2   // GPIO2 - SY7088 enable (main 3.3V rail)
#ifndef FW_VERSION
#error "FW_VERSION must be defined by build system (platformio.ini custom_app_version)"
#endif
#define DISPLAY_WIDTH       960
#define DISPLAY_HEIGHT      540

// ─────────────────────────── Configuration ───────────────────────────
#define DEFAULT_API_BASE_URL   "https://trmnl.app"
#define DEFAULT_REFRESH_RATE   900   // 15 minutes (in seconds)
#define WIFI_AP_TIMEOUT        300   // 5 minutes in AP mode before sleep
#define LOG_BUFFER_SIZE        4096  // Max log buffer to send to server

// Display refresh
#define LOW_BATTERY_VOLTAGE    3.4   // Below this voltage, show warning and shut down
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
bool otaEnabled = true;
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
const char* UPDATE_SOURCE_STR = "COLD";  // set dynamically in setup() from wake reason

// ─────────────────────────── Forward Declarations ───────────────────────────
void invalidateImageCache(const char* reason = nullptr);
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
  else if (wakeup == ESP_SLEEP_WAKEUP_EXT1) wakeStr = "EXT1";
  UPDATE_SOURCE_STR = wakeStr;  // server uses this to detect button wakes for playlist advance
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
  M5.Display.setEpdMode(DEFAULT_EPD_MODE); // Full refresh on first boot

  // Power optimization
  setCpuFrequencyMhz(80);
  btStop();

  canvas.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);

  // ── Load settings early (needed before button wake detection for SF lookup) ──
  loadSettings();

  // ── Button handling ──
  bool isSpecialFunction = false;
  if (wakeup == ESP_SLEEP_WAKEUP_EXT1) {
    // Button wakeup: classify press type using TRMNL timing
    WakePress press = detectButtonWakePress();
    switch (press) {
      case WakePress::LONGEST:
        deviceLog("Button: longest press (16s) → factory reset\n");
        prefs.begin(NVS_NAMESPACE, false);
        prefs.clear();
        prefs.end();
        showErrorScreen("Factory Reset\n\nAll settings cleared\nRestarting...");
        delay(2000);
        ESP.restart();
        return;
      case WakePress::LONG:
        deviceLog("Button: long press (6s) → WiFi clear\n");
        prefs.begin(NVS_NAMESPACE, false);
        prefs.remove(KEY_WIFI_SSID);
        prefs.remove(KEY_WIFI_PASS);
        prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
        prefs.end();
        showSetupScreen("WiFi cleared\n\nRestarting...");
        delay(1000);
        ESP.restart();
        return;
      case WakePress::MEDIUM:
        deviceLog("Button: medium press → special function: %s\n", specialFunction.c_str());
        if (specialFunction == "add_wifi") {
          showSetupScreen("Opening WiFi Setup...\nM5Paper-TRMNL\n192.168.4.1");
          startCaptivePortal();
          return;
        }
        isSpecialFunction = true;  // Let server execute other SFs via header
        break;
      case WakePress::CLICK:
      default:
        deviceLog("Button: click → advance playlist\n");
        break;
    }
  } else {
    if (handleBootButtonReset()) return;
  }

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
  fetchAndDisplay(batteryVoltage, isSpecialFunction);

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
