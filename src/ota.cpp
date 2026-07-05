#include "ota.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>

#include "display.h"
#include "trmnl_keys.h"

#ifndef FW_VERSION
#define FW_VERSION "2.6.0"
#endif

#ifndef OTA_FIRMWARE_ASSET_NAME
#define OTA_FIRMWARE_ASSET_NAME "m5paper.bin"
#endif

#ifndef OTA_GITHUB_OWNER
#define OTA_GITHUB_OWNER "paveldn"
#endif
#ifndef OTA_GITHUB_REPO
#define OTA_GITHUB_REPO "trmnl-m5paper"
#endif

static const uint32_t OTA_SAFETY_INTERVAL_SEC = 86400UL;
static const char* OTA_GITHUB_API_URL = "https://api.github.com/repos/" OTA_GITHUB_OWNER "/" OTA_GITHUB_REPO "/releases/latest";
static const char* OTA_GITHUB_RELEASES_API_URL = "https://api.github.com/repos/" OTA_GITHUB_OWNER "/" OTA_GITHUB_REPO "/releases";

extern Preferences prefs;
extern bool otaBetaMode;
extern void deviceLog(const char* fmt, ...);
extern void disableWiFiPS();
extern void enableWiFiPS();
extern void invalidateImageCache(const char* reason);

static bool parseVersionParts(const String& version, int& major, int& minor, int& patch) {
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

static int compareVersions(const String& a, const String& b) {
  int aMaj, aMin, aPat;
  int bMaj, bMin, bPat;

  if (!parseVersionParts(a, aMaj, aMin, aPat)) return 0;
  if (!parseVersionParts(b, bMaj, bMin, bPat)) return 0;

  if (aMaj != bMaj) return (aMaj > bMaj) ? 1 : -1;
  if (aMin != bMin) return (aMin > bMin) ? 1 : -1;
  if (aPat != bPat) return (aPat > bPat) ? 1 : -1;
  return 0;
}

static bool getCurrentEpoch(time_t& epochOut) {
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

bool tryInitRtcFromHttpDate(const String &dateHeader, bool force) {
  if (dateHeader.length() == 0) return false;

  time_t now = time(nullptr);
  if (!force && now > 1600000000UL) {
    deviceLog("RTC already set (epoch=%lu) - skipping\n", (unsigned long)now);
    return false;
  }

  char buf[64];
  dateHeader.toCharArray(buf, sizeof(buf));

  char wk[4], mon[4];
  int day, year, hh, mm, ss;
  int rc = sscanf(buf, "%3s, %d %3s %d %d:%d:%d GMT", wk, &day, mon, &year, &hh, &mm, &ss);
  if (rc < 7) return false;

  const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int monIdx = -1;
  for (int i = 0; i < 12; ++i) {
    if (strncmp(mon, months + i * 3, 3) == 0) {
      monIdx = i;
      break;
    }
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

  setenv("TZ", "UTC0", 1);
  tzset();
  time_t t = mktime(&tm);
  if (t <= 0) {
    deviceLog("RTC parse produced invalid epoch\n");
    return false;
  }

  struct timeval tv = {.tv_sec = t, .tv_usec = 0};
  if (settimeofday(&tv, nullptr) == 0) {
    deviceLog("RTC set from server Date: %s -> %lu\n", buf, (unsigned long)t);
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putInt(KEY_RTC_SET, (int)t);
    prefs.end();
    return true;
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

bool checkGitHubReleaseForUpdate(String& firmwareUrlOut, String& versionOut, bool force, bool includePrereleases) {
  firmwareUrlOut = "";
  versionOut = "";

  if (!force && !isIntervalElapsed(KEY_OTA_LAST_CHECK, OTA_SAFETY_INTERVAL_SEC)) {
    return false;
  }

  if (force) {
    deviceLog("OTA: forcing GitHub release check (cooldown bypass)\n");
  }

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
        if (normalizedTag.equalsIgnoreCase(currentTag)) {
          continue;
        }

        if (versionCmp < 0) {
          continue;
        }

        if (versionCmp == 0 && isPrerelease && !currentIsPrerelease) {
          continue;
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
    deviceLog("OTA: wrote %d/%d bytes\n", (int)written, (int)contentLength);
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
