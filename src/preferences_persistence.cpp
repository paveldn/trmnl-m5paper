#include "preferences_persistence.h"

#include <Preferences.h>

#include "trmnl_keys.h"

static const char* DEFAULT_API_BASE_URL = "https://trmnl.app";
static const int DEFAULT_REFRESH_RATE = 900;

extern Preferences prefs;
extern String configuredSSID;
extern String configuredPass;
extern String apiKey;
extern String apiBaseUrl;
extern String friendlyId;
extern int refreshRate;
extern bool otaBetaMode;
extern uint8_t savedChannel;

void loadSettings() {
  prefs.begin(NVS_NAMESPACE, true);
  configuredSSID = prefs.getString(KEY_WIFI_SSID, "");
  configuredPass = prefs.getString(KEY_WIFI_PASS, "");
  apiKey = prefs.getString(KEY_API_KEY, "");
  apiBaseUrl = prefs.getString(KEY_API_URL, DEFAULT_API_BASE_URL);
  friendlyId = prefs.getString(KEY_FRIENDLY_ID, "");
  refreshRate = prefs.getInt(KEY_REFRESH_RATE, DEFAULT_REFRESH_RATE);
  otaBetaMode = prefs.getBool(KEY_OTA_BETA_MODE, false);
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

void saveOtaBetaMode(bool enabled) {
  if (enabled == otaBetaMode) {
    return;
  }

  otaBetaMode = enabled;

  prefs.begin(NVS_NAMESPACE, false);
  prefs.putBool(KEY_OTA_BETA_MODE, otaBetaMode);
  prefs.remove(KEY_OTA_LAST_CHECK);
  prefs.remove(KEY_OTA_LAST_ATTEMPT);
  prefs.end();
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
  otaBetaMode = false;
  savedChannel = 0;
}
