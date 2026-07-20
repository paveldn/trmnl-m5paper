#include "api_helpers.h"

void addSetupHeaders(HTTPClient& http, const String& macAddress, const String& firmwareVersion, const String& model) {
  http.addHeader("ID", macAddress);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("FW-Version", firmwareVersion);
  http.addHeader("Model", model);
}

void addDisplayHeaders(HTTPClient& http,
                       const String& macAddress,
                       const String& apiKey,
                       int refreshRate,
                       float batteryVoltage,
                       uint8_t batteryLevel,
                       int rssi,
                       bool imageCached,
                       int prevWakeTime,
                       const String& firmwareVersion,
                       const String& model,
                       const String& wifiBand,
                       bool batteryCharging,
                       bool usbConnected,
                       const String& updateSource,
                       int displayWidth,
                       int displayHeight,
                       bool specialFunctionActive) {
  http.addHeader("ID", macAddress);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Update-Source", updateSource);
  http.addHeader("Access-Token", apiKey);
  http.addHeader("Refresh-Rate", String(refreshRate));
  http.addHeader("Battery-Voltage", String(batteryVoltage, 2));
  http.addHeader("Percent-Charged", String(batteryLevel));
  http.addHeader("Battery-Charging", batteryCharging ? "1" : "0");
  http.addHeader("USB-Connected", usbConnected ? "true" : "false");
  http.addHeader("FW-Version", firmwareVersion);
  http.addHeader("Model", model);
  http.addHeader("Image-Cached", imageCached ? "true" : "false");
  http.addHeader("Wake-Time", String(prevWakeTime));
  http.addHeader("RSSI", String(rssi));
  if (wifiBand.length() > 0) {
    http.addHeader("WiFi-Band", wifiBand);
  }
  http.addHeader("Temperature-Profile", "true");
  http.addHeader("Width", String(displayWidth));
  http.addHeader("Height", String(displayHeight));
  if (specialFunctionActive) {
    http.addHeader("special_function", "true");
  }
}

void addAuthHeaders(HTTPClient& http, const String& macAddress, const String& apiKey) {
  http.addHeader("ID", macAddress);
  http.addHeader("Access-Token", apiKey);
}

void addLogHeaders(HTTPClient& http, const String& macAddress, const String& apiKey) {
  http.addHeader("ID", macAddress);
  http.addHeader("Accept", "application/json, */*");
  http.addHeader("Access-Token", apiKey);
  http.addHeader("Content-Type", "application/json");
}
