#pragma once

#include <Arduino.h>

bool tryInitRtcFromHttpDate(const String& dateHeader, bool force = false);
bool checkGitHubReleaseForUpdate(String& firmwareUrlOut, String& versionOut, bool force = false, bool includePrereleases = false);
bool isIntervalElapsed(const char* key, uint32_t intervalSec);
void markIntervalNow(const char* key);
bool performOTA(const char* firmwareUrl);
