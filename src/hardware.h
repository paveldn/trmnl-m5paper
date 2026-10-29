#pragma once

#if defined(TRMNL_M5PAPER) && defined(TRMNL_PAPERDIY)
#error "Select only one hardware target"
#elif !defined(TRMNL_M5PAPER) && !defined(TRMNL_PAPERDIY)
#error "Select a hardware target: TRMNL_M5PAPER or TRMNL_PAPERDIY"
#endif

#ifndef DEVICE_MODEL
#ifdef TRMNL_PAPERDIY
#define DEVICE_MODEL "paperdiy"
#elif defined(TRMNL_M5PAPER)
#define DEVICE_MODEL "m5paper"
#endif
#endif

static constexpr const char* DEVICE_MODEL_STR = DEVICE_MODEL;

#ifdef TRMNL_PAPERDIY
static constexpr const char* DEVICE_DISPLAY_NAME = "PaperDIY";
static constexpr const char* DEVICE_SETUP_SSID = "PaperDIY-TRMNL";
static constexpr int DEVICE_WAKE_BUTTON = 4;
#elif defined(TRMNL_M5PAPER)
static constexpr const char* DEVICE_DISPLAY_NAME = "M5Paper";
static constexpr const char* DEVICE_SETUP_SSID = "M5Paper-TRMNL";
static constexpr int DEVICE_WAKE_BUTTON = 39;
static constexpr int M5EPD_MAIN_PWR_PIN = 2;
#endif

static constexpr int DISPLAY_WIDTH = 960;
static constexpr int DISPLAY_HEIGHT = 540;

static constexpr int DEFAULT_REFRESH_RATE = 900;
static constexpr int MIN_REFRESH_RATE = 15;
static constexpr int MIN_NORMAL_REFRESH_RATE = 60;
static constexpr int MAX_REFRESH_RATE = 86400;
