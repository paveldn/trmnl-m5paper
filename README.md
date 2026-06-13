# M5Paper TRMNL Firmware

Custom firmware for [M5Paper](https://docs.m5stack.com/en/core/m5paper) that turns it into a [TRMNL](https://usetrmnl.com/) BYOD display.

## Features

- WiFi captive portal for setup — no hardcoded credentials
- Works with official TRMNL server and custom/local servers
- Deep sleep with timer and button wake
- NVS-based persistent settings (survives reboots)
- Device registration via MAC address (TRMNL API compatible)
- Battery voltage reporting
- Automatic retry with failure counting (WiFi and server)
 - Initialize RTC from TRMNL server `Date` header when device clock is unset (no NTP required)

## Hardware

- [M5Paper](https://docs.m5stack.com/en/core/m5paper) — ESP32-D0WDQ5, 4.7" e-paper 960×540
- Power architecture: GPIO2 drives SY7088 boost converter (no PMIC)
- Wake sources: RTC timer, button (GPIO39)

## Building

Requires [PlatformIO](https://platformio.org/).

```bash
pio run              # build
pio run -t upload    # flash via USB
```

## Setup

1. Flash the firmware
2. On first boot the device starts a WiFi AP (`TRMNL-XXXX`)
3. Connect to the AP, configure WiFi credentials and TRMNL API key
4. Device reboots, registers with TRMNL, and begins refresh cycle

Hold the button on boot to re-enter setup mode.

Reset UX:
- Hold >5s after wake: clear WiFi credentials (returns to captive portal)
- Hold ~15s after wake: factory reset (clear all settings + reboot)

## Operation

Each wake cycle:
1. Connect to WiFi
2. Fetch current display from TRMNL API
3. Render image on e-paper
4. Enter deep sleep (default 15 min)

The device wakes on timer expiry or button press.

Compatibility / Notes
- The firmware follows official TRMNL header and API behavior: it sends `ID` and `Access-Token` headers and includes a `Wake-Time` header for statistics.
- Image downloads from the same TRMNL server include authentication headers so self-hosted servers should accept the same `Access-Token`.
- Debug logs (when enabled) are posted to `/api/log` in an official-compatible `logs` array payload with fields such as `created_at`, `source_path`, `wake_reason`, `battery_voltage`, and `firmware_version`.
- Log submission is deferred on low battery.
- The device will attempt to initialize its RTC from the HTTP `Date` response header on first successful server contact if the RTC is unset.

## Links

### TRMNL
- [TRMNL BYOD documentation](https://docs.usetrmnl.com/go/diy/byod)
- [TRMNL firmware source (official ESP32)](https://github.com/usetrmnl/firmware)
- [TRMNL API](https://docs.usetrmnl.com/)

### M5Paper
- [M5Paper product page](https://docs.m5stack.com/en/core/m5paper)
- [M5Paper Arduino API](https://docs.m5stack.com/en/arduino/m5paper/system_api)
- [M5Paper deep sleep / wakeup](https://docs.m5stack.com/en/arduino/m5paper/wakeup)
- [M5Unified library](https://github.com/m5stack/M5Unified)
- [Community: M5Paper shutdown & deep sleep](https://community.m5stack.com/topic/2892/m5paper-shutdown-deep-sleep-wakeup)

## License

MIT