#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <stdint.h>

void addSetupHeaders(HTTPClient& http, const String& macAddress, const String& firmwareVersion, const String& model);
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
                       int displayHeight);
void addAuthHeaders(HTTPClient& http, const String& macAddress, const String& apiKey);
void addLogHeaders(HTTPClient& http, const String& macAddress, const String& apiKey);
