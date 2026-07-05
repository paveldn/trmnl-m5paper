#include "button.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>

#include "trmnl_keys.h"

static const int M5PAPER_WAKE_BUTTON = 39;
static const int BUTTON_HOLD_TIME = 5000;
static const int BUTTON_FACTORY_RESET = 15000;

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
      M5.Display.setEpdMode(epd_mode_t::epd_fast);
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
