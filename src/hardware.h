#pragma once

#ifndef DEVICE_MODEL
#define DEVICE_MODEL "m5paper"
#endif

static constexpr const char* DEVICE_MODEL_STR = DEVICE_MODEL;

static constexpr int DISPLAY_WIDTH = 960;
static constexpr int DISPLAY_HEIGHT = 540;

static constexpr int M5PAPER_WAKE_BUTTON = 39;
static constexpr int M5EPD_MAIN_PWR_PIN = 2;

static constexpr int DEFAULT_REFRESH_RATE = 900;
static constexpr int MIN_REFRESH_RATE = 15;
static constexpr int MIN_NORMAL_REFRESH_RATE = 60;
static constexpr int MAX_REFRESH_RATE = 86400;
