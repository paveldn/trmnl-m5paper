#pragma once

#include <Arduino.h>

void sendLogs();
void registerDevice();
void fetchAndDisplay(float batteryVoltage, bool specialFunctionActive = false);
