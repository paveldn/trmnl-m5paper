#pragma once

#include <Arduino.h>

String getWifiBand();
void displayImage(const char* imageUrl);
bool downloadAndDisplayImage(const char* url);
