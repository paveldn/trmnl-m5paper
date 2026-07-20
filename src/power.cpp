#include "power.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <M5Unified.h>
#include <algorithm>
#include <cmath>

static const int M5PAPER_WAKE_BUTTON = 39;
static const int M5EPD_MAIN_PWR_PIN = 2;
static const int DISPLAY_WIDTH = 960;
static const int DISPLAY_HEIGHT = 540;
static const float LOW_BATTERY_VOLTAGE = 3.4f;

extern M5Canvas canvas;
extern unsigned long startupMillis;
extern int lastWakeTime;

extern void deviceLog(const char* fmt, ...);
extern void sendLogs();
extern void invalidateImageCache(const char* reason);

static bool detectExternalPower(int16_t* vbusAvgOut = nullptr, int16_t* vbusMaxOut = nullptr) {
  // VBUS ADC can be noisy on wake; use multiple samples and majority logic.
  constexpr int SAMPLE_COUNT = 5;
  constexpr int VBUS_PRESENT_MV = 4000;
  constexpr int BAT_CURRENT_CHARGING_MA = 10;
  constexpr float BAT_FULL_HINT_V = 4.18f;

  int32_t sum = 0;
  int16_t maxV = 0;
  int aboveThreshold = 0;
  int invalidCount = 0;

  for (int i = 0; i < SAMPLE_COUNT; ++i) {
    int16_t v = M5.Power.getVBUSVoltage();
    if (v < 0) {
      invalidCount++;
      v = 0;
    }
    sum += v;
    if (v > maxV) maxV = v;
    if (v >= VBUS_PRESENT_MV) aboveThreshold++;
    if (i < SAMPLE_COUNT - 1) delay(5);
  }

  int16_t avgV = static_cast<int16_t>(sum / SAMPLE_COUNT);
  bool present = (aboveThreshold >= 2) || (avgV >= VBUS_PRESENT_MV);

  // Fallback for boards where VBUS telemetry is unavailable (often returns -1).
  if (!present && invalidCount == SAMPLE_COUNT) {
    bool charging = M5.Power.isCharging();
    int32_t batCurrent = M5.Power.getBatteryCurrent();
    float batV = readBatteryAvg(4, 5);

    // Infer external power from any strong signal when VBUS data is unavailable.
    present = charging || (batCurrent > BAT_CURRENT_CHARGING_MA) || (batV >= BAT_FULL_HINT_V);
    deviceLog("Power(fallback): charging=%d batCurrent=%ldmA batV=%.2fV\n",
              charging ? 1 : 0,
              (long)batCurrent,
              batV);
  }

  if (vbusAvgOut) *vbusAvgOut = avgV;
  if (vbusMaxOut) *vbusMaxOut = maxV;
  return present;
}

float getBatteryVoltage() {
  // Use a trimmed average to reduce ADC spikes and e-ink/WiFi transients.
  float voltage = readBatteryAvg(8, 30);

  // On battery-only operation, report a slightly conservative value.
  // This better reflects in-load behavior and avoids optimistic % on dashboards.
  bool externalPower = isExternalPowerPresent();
  if (!externalPower && voltage > 0.1f) {
    voltage -= 0.06f;
  }

  deviceLog("Bat: %.2fV (external=%d)\n", voltage, externalPower ? 1 : 0);
  return voltage;
}

float readBatteryAvg(int samples, int delayMs) {
  if (samples <= 0) samples = 1;
  if (samples > 32) samples = 32;

  float sum = 0.0;
  float minV = 100.0f;
  float maxV = 0.0f;

  for (int i = 0; i < samples; ++i) {
    float v = M5.Power.getBatteryVoltage() / 1000.0f;
    sum += v;
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
    if (i < samples - 1) delay(delayMs);
  }

  float avg = sum / (float)samples;
  // Trim one min and one max sample when enough samples are available.
  if (samples >= 5) {
    avg = (sum - minV - maxV) / (float)(samples - 2);
  }

  deviceLog("Bat(avg): %.2fV min=%.2f max=%.2f samples=%d\n", avg, minV, maxV, samples);
  return avg;
}

bool isExternalPowerPresent() {
  int16_t avgV = 0;
  int16_t maxV = 0;
  bool present = detectExternalPower(&avgV, &maxV);
  deviceLog("Power: VBUS avg=%dmV max=%dmV external=%d\n", avgV, maxV, present ? 1 : 0);
  return present;
}

bool isBatteryCharging() {
  // Treat stable USB/VBUS presence as charging for upstream telemetry compatibility.
  return detectExternalPower();
}

void showLowBatteryAndShutdown() {
  invalidateImageCache("low_battery_screen");
  M5.Display.setEpdMode(DEFAULT_EPD_MODE);
  canvas.fillSprite(TFT_WHITE);

  // Draw battery icon (centered, large) — Material Design style battery_alert
  int cx = DISPLAY_WIDTH / 2;
  int cy = DISPLAY_HEIGHT / 2 - 30;
  int bw = 120;  // battery body width
  int bh = 200;  // battery body height
  int cap_w = 50; // top cap width
  int cap_h = 20; // top cap height
  int thick = 8;  // outline thickness

  // Battery cap (top nub)
  canvas.fillRect(cx - cap_w/2, cy - bh/2 - cap_h, cap_w, cap_h, TFT_BLACK);

  // Battery body outline
  canvas.fillRect(cx - bw/2, cy - bh/2, bw, bh, TFT_BLACK);
  canvas.fillRect(cx - bw/2 + thick, cy - bh/2 + thick, bw - 2*thick, bh - 2*thick, TFT_WHITE);

  // Exclamation mark inside battery
  int ex_x = cx;
  int ex_top = cy - 50;
  int ex_w = 14;
  // Vertical bar
  canvas.fillRect(ex_x - ex_w/2, ex_top, ex_w, 70, TFT_BLACK);
  // Dot
  canvas.fillRect(ex_x - ex_w/2, ex_top + 85, ex_w, ex_w, TFT_BLACK);

  // Text below
  canvas.setTextColor(TFT_BLACK);
  canvas.setTextDatum(MC_DATUM);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.drawString("LOW BATTERY", cx, cy + bh/2 + 50);

  canvas.setFont(&fonts::FreeSans9pt7b);
  float v = readBatteryAvg(6, 20);
  constexpr float BATTERY_FULL = 4.10f;
  constexpr float BATTERY_EMPTY = 3.40f;
  constexpr float BATTERY_GAMMA = 1.70f;
  float clamped = std::clamp(v, BATTERY_EMPTY, BATTERY_FULL);
  float normalized = (clamped - BATTERY_EMPTY) / (BATTERY_FULL - BATTERY_EMPTY);
  float capacity = std::pow(normalized, BATTERY_GAMMA);
  uint8_t pct = static_cast<uint8_t>(capacity * 100.0f + 0.5f);
  canvas.drawString(String(pct) + "%  " + String(v, 2) + "V - Connect USB to charge", cx, cy + bh/2 + 90);

  canvas.pushSprite(0, 0);
  M5.Display.waitDisplay();

  // Full shutdown — no timer wake, no button wake
  // Device will only restart when USB power is connected (cold boot)
  deviceLog("Shutting down (low battery)\n");
  Serial.flush();
  delay(100);

  M5.Display.sleep();
  M5.Display.waitDisplay();

  // Kill main power — only USB connection will restart
  gpio_hold_dis((gpio_num_t)M5EPD_MAIN_PWR_PIN);
  pinMode(M5EPD_MAIN_PWR_PIN, OUTPUT);
  digitalWrite(M5EPD_MAIN_PWR_PIN, LOW);
  gpio_hold_en((gpio_num_t)M5EPD_MAIN_PWR_PIN);
  gpio_deep_sleep_hold_en();

  // Deep sleep with no wake sources — effectively off
  esp_deep_sleep_start();
}

void goToDeepSleep(int seconds) {
  // Enforce minimum sleep to avoid rapid wake loops
  if (seconds < 15)
    seconds = 16;

  lastWakeTime = (millis() - startupMillis) / 1000;
  deviceLog("Sleep: %d seconds\n", seconds);
  sendLogs();
  Serial.flush();
  delay(10);

  // Shut down WiFi radio before sleep (guard against already-off state)
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    delay(10);
  }

  // Put IT8951E into STANDBY mode (0x0002) to reduce sleep current
  // STANDBY draws ~1-2mA vs ~5-10mA idle. Wakes with any host command
  // (M5.begin() sends SYS_RUN during panel init on next boot)
  M5.Display.waitDisplay();
  M5.Display.powerSave(true);
  delay(100);

  // Configure wake sources
  // Button press (GPIO39, active LOW) or timer.
  // Use EXT1 here because GPIO39 has no internal pull-up.
  esp_sleep_enable_ext1_wakeup((1ULL << M5PAPER_WAKE_BUTTON), ESP_EXT1_WAKEUP_ALL_LOW);

  // Hold ALL GPIO states through deep sleep (critical for M5Paper on battery)
  gpio_hold_en((gpio_num_t)M5EPD_MAIN_PWR_PIN);
  gpio_deep_sleep_hold_en();
  delay(2100);  // Allow hardware time to settle before deep sleep

  esp_deep_sleep((uint64_t)seconds * 1000000ULL);
}
