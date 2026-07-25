#pragma once

#include <Arduino.h>

extern String specialFunction;

void loadSettings();
void saveWiFiSettings(const String& ssid, const String& pass);
void saveServerSettings(const String& key, const String& url);
void saveOtaEnabled(bool enabled);
void saveOtaBetaMode(bool enabled);
void saveRefreshRate(int rate);
void saveSpecialFunction(const String& sf);
void clearAllSettings();
