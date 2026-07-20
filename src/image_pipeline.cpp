#include "image_pipeline.h"

#include <HTTPClient.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "api_helpers.h"
#include "trmnl_keys.h"

static const int MAX_IMAGE_SIZE = 200000;
static const int FULL_REFRESH_INTERVAL = 30;

extern Preferences prefs;
extern M5Canvas canvas;
extern int partialRefreshCount;
extern String apiBaseUrl;
extern String apiKey;

extern void deviceLog(const char* fmt, ...);
extern void disableWiFiPS();

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

void displayImage(const char* imageUrl) {
  deviceLog("Downloading...\n");

  if (downloadAndDisplayImage(imageUrl)) {
    partialRefreshCount++;

    if (partialRefreshCount >= FULL_REFRESH_INTERVAL) {
      M5.Display.setEpdMode(DEFAULT_EPD_MODE);
      partialRefreshCount = 0;
      deviceLog("Full refresh (ghost clear)\n");
    } else {
      deviceLog("Fast refresh\n");
      M5.Display.setEpdMode(DEFAULT_EPD_MODE);
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

#ifndef FORCE_IMAGE_REFRESH_ON_WAKE
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
#else
  deviceLog("Image force-refresh enabled - skipping conditional GET headers\n");
#endif
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  int code = http.GET();
#ifndef FORCE_IMAGE_REFRESH_ON_WAKE
  if (code == HTTP_CODE_NOT_MODIFIED) {
    deviceLog("Img HTTP 304 Not Modified\n");
    http.end();
    return false;
  }
#endif

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
