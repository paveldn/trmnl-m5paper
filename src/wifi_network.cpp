#include "wifi_network.h"

#include <Preferences.h>
#include <WiFi.h>

#include "trmnl_keys.h"

static const int WIFI_CONNECT_TIMEOUT = 25000;
static const int WIFI_FAST_CONNECT_TIMEOUT = 8000;
static const int WIFI_DISCONNECT_SETTLE_MS = 300;
static const int WIFI_RETRY_1 = 60;
static const int WIFI_RETRY_2 = 180;
static const int WIFI_RETRY_3 = 300;
static const int API_RETRY_1 = 15;
static const int API_RETRY_2 = 30;
static const int API_RETRY_3 = 60;

extern Preferences prefs;
extern String configuredSSID;
extern String configuredPass;
extern int refreshRate;
extern uint8_t savedBSSID[6];
extern uint8_t savedChannel;
extern int wifiFailCount;

extern void deviceLog(const char* fmt, ...);
extern void enableWiFiPS();
extern void checkRuntimeReset();
extern void showErrorScreen(const String& message);
extern void goToDeepSleep(int seconds);

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
    while (WiFi.status() != WL_CONNECTED && millis() - start <= WIFI_FAST_CONNECT_TIMEOUT) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
    } else {
      deviceLog("Fast reconnect failed (ch:%d), resetting cache\n", savedChannel);
      savedChannel = 0;
      memset(savedBSSID, 0, 6);
      WiFi.disconnect(true);
      delay(WIFI_DISCONNECT_SETTLE_MS);
      fastConnect = false;
    }
  }

  if (!connected) {
    deviceLog("WiFi: full scan connect\n");
    // Scan every channel and pick the strongest matching AP, rather than
    // stopping at the first match — important when the SSID is broadcast
    // by more than one AP (mesh/extenders) or the AP has changed channel.
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
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
      // Max short retries — fall back to normal refresh rate
      prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
      prefs.end();
      deviceLog("WiFi max retries — sleeping normal rate %ds\n", refreshRate);
      showErrorScreen("Can't connect to WiFi\n" + configuredSSID + "\n\nRetrying in " + String(refreshRate) + "s");
      checkRuntimeReset();
      goToDeepSleep(refreshRate);
      return;
  }

  deviceLog("WiFi retry #%d, sleeping %ds\n", retryCount, sleepTime);
  prefs.putInt(KEY_WIFI_RETRY_COUNT, retryCount + 1);
  prefs.end();

  showErrorScreen("Can't connect to WiFi\n" + configuredSSID + "\n\nRetrying in " + String(sleepTime) + "s");
  checkRuntimeReset();
  goToDeepSleep(sleepTime);
}

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
