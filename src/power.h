#pragma once

#include <Arduino.h>

float readBatteryAvg(int samples = 6, int delayMs = 50);
float getBatteryVoltage();
bool isExternalPowerPresent();
bool isBatteryCharging();
void initializeHardwarePower();
bool wasHardwareTimerWake();
bool wasHardwareButtonWake();
void showLowBatteryAndShutdown();
void goToDeepSleep(int seconds);
