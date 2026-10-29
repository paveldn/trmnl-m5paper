#pragma once

#include <Arduino.h>

String getWifiBand();
bool displayImage(const char* imageUrl);
bool downloadAndDisplayImage(const char* url);
