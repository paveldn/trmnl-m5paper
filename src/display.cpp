#include "display.h"

#include <M5Unified.h>

static const int DISPLAY_WIDTH = 960;
static const int DISPLAY_HEIGHT = 540;

extern M5Canvas canvas;
extern const char* FW_VERSION_STR;
extern int partialRefreshCount;
extern void invalidateImageCache(const char* reason);

void showLoadingScreen() {
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  canvas.fillSprite(TFT_WHITE);
  canvas.setTextColor(TFT_BLACK);
  canvas.setTextDatum(MC_DATUM);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.drawString("TRMNL", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 - 20);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.drawString("Loading...", DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 + 30);
  canvas.pushSprite(0, 0);
  M5.Display.display();
  partialRefreshCount = 0;  // Reset since we did a full refresh
}

void showSetupScreen(const String& message) {
  invalidateImageCache("setup_screen");
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  canvas.fillSprite(TFT_WHITE);
  canvas.setTextColor(TFT_BLACK);
  canvas.setTextDatum(MC_DATUM);
  canvas.setFont(&fonts::FreeSansBold12pt7b);

  canvas.drawString("M5Paper TRMNL", DISPLAY_WIDTH / 2, 60);

  canvas.setFont(&fonts::FreeSans12pt7b);
  int y = 180;
  int start = 0;
  String msg = message;
  while (start < (int)msg.length()) {
    int nl = msg.indexOf('\n', start);
    if (nl < 0) nl = msg.length();
    String line = msg.substring(start, nl);
    canvas.drawString(line, DISPLAY_WIDTH / 2, y);
    y += 36;
    start = nl + 1;
  }

  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.drawString(String("FW ") + FW_VERSION_STR, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT - 40);

  canvas.pushSprite(0, 0);
  M5.Display.display();
}

void showErrorScreen(const String& message) {
  invalidateImageCache("error_screen");
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  canvas.fillSprite(TFT_WHITE);
  canvas.setTextColor(TFT_BLACK);
  canvas.setTextDatum(MC_DATUM);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.drawString("Error", DISPLAY_WIDTH / 2, 80);

  canvas.setFont(&fonts::FreeSans12pt7b);
  int y = 200;
  int start = 0;
  String msg = message;
  while (start < (int)msg.length()) {
    int nl = msg.indexOf('\n', start);
    if (nl < 0) nl = msg.length();
    String line = msg.substring(start, nl);
    canvas.drawString(line, DISPLAY_WIDTH / 2, y);
    y += 36;
    start = nl + 1;
  }

  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.drawString(String("FW ") + FW_VERSION_STR, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT - 40);

  canvas.pushSprite(0, 0);
  M5.Display.display();
}
