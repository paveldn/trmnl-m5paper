#include "button.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>

#include "trmnl_keys.h"

static const int M5PAPER_WAKE_BUTTON = 39;
static const int BUTTON_MEDIUM_TIME    = 1000;  // 1s  — medium press (special function)
static const int BUTTON_HOLD_TIME      = 6000;  // 6s  — long press (WiFi clear)
static const int BUTTON_FACTORY_RESET  = 16000; // 16s — longest press (factory reset / soft reset)

extern Preferences prefs;
extern void deviceLog(const char* fmt, ...);
extern void showErrorScreen(const String& message);
extern void showSetupScreen(const String& message);

bool handleBootButtonReset() {
  // GPIO39 is input-only and has no internal pull-up on ESP32.
  pinMode(M5PAPER_WAKE_BUTTON, INPUT);

  // Aligned with TRMNL: 5s hold = WiFi clear, 15s = factory reset
  if (digitalRead(M5PAPER_WAKE_BUTTON) == LOW) {
    deviceLog("Button held at boot...\n");
    unsigned long pressStart = millis();
    while (digitalRead(M5PAPER_WAKE_BUTTON) == LOW) {
      unsigned long held = millis() - pressStart;
      if (held > BUTTON_FACTORY_RESET) {
        break;
      }
      delay(50);
    }
    unsigned long holdTime = millis() - pressStart;

    if (holdTime >= BUTTON_FACTORY_RESET) {
      deviceLog("Factory reset (15s hold)\n");
      M5.Display.setEpdMode(DEFAULT_EPD_MODE);
      prefs.begin(NVS_NAMESPACE, false);
      prefs.clear();
      prefs.end();
      showErrorScreen("Factory Reset\n\nAll settings cleared\nRestarting...");
      delay(2000);
      ESP.restart();
      return true;
    }

    if (holdTime >= BUTTON_HOLD_TIME) {
      deviceLog("WiFi credentials clear (5s hold)\n");
      prefs.begin(NVS_NAMESPACE, false);
      prefs.remove(KEY_WIFI_SSID);
      prefs.remove(KEY_WIFI_PASS);
      prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
      prefs.end();
      // Fall through — setup() will start captive portal below.
    }
  }

  return false;
}

void checkRuntimeReset() {
  pinMode(M5PAPER_WAKE_BUTTON, INPUT);
  if (digitalRead(M5PAPER_WAKE_BUTTON) == LOW) {
    deviceLog("Runtime button press detected\n");
    unsigned long start = millis();
    while (digitalRead(M5PAPER_WAKE_BUTTON) == LOW) {
      unsigned long held = millis() - start;
      if (held >= BUTTON_FACTORY_RESET) {
        deviceLog("Runtime factory reset triggered\n");
        prefs.begin(NVS_NAMESPACE, false);
        prefs.clear();
        prefs.end();
        showErrorScreen("Factory Reset\n\nAll settings cleared\nRestarting...");
        delay(1500);
        ESP.restart();
        return;
      }
      delay(50);
    }
    unsigned long held = millis() - start;
    if (held >= BUTTON_HOLD_TIME) {
      deviceLog("Runtime WiFi credentials clear triggered\n");
      prefs.begin(NVS_NAMESPACE, false);
      prefs.remove(KEY_WIFI_SSID);
      prefs.remove(KEY_WIFI_PASS);
      prefs.putInt(KEY_WIFI_RETRY_COUNT, 1);
      prefs.end();
      showSetupScreen("WiFi cleared\n\nRestarting...");
      delay(1000);
      ESP.restart();
      return;
    }
  }
}

// Detect the type of button press that woke the device from deep sleep.
// Call this only when wakeup cause is ESP_SLEEP_WAKEUP_EXT1.
//
// If the button is already released: short CLICK (advance playlist).
// If still held, measures duration and classifies as:
//   MEDIUM  (1–2 s)  → special function (server-configured)
//   LONG    (6–7 s)  → WiFi credentials clear
//   LONGEST (16+ s)  → factory reset / soft reset
WakePress detectButtonWakePress() {
  pinMode(M5PAPER_WAKE_BUTTON, INPUT);

  // Button already released before firmware started → was a short click
  if (digitalRead(M5PAPER_WAKE_BUTTON) == HIGH) {
    return WakePress::CLICK;
  }

  // Button still held — measure how long it stays LOW
  unsigned long start = millis();
  while (digitalRead(M5PAPER_WAKE_BUTTON) == LOW) {
    if (millis() - start >= (unsigned long)BUTTON_FACTORY_RESET) break;
    delay(10);
  }
  unsigned long held = millis() - start;

  if (held >= (unsigned long)BUTTON_FACTORY_RESET) return WakePress::LONGEST;
  if (held >= (unsigned long)BUTTON_HOLD_TIME)     return WakePress::LONG;
  if (held >= (unsigned long)BUTTON_MEDIUM_TIME)   return WakePress::MEDIUM;
  return WakePress::CLICK;
}
