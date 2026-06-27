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
- Initialize RTC from TRMNL server Date header when device clock is unset (no NTP required)
- OTA firmware updates
- Daily GitHub release check (once per 24h)
- OTA safety cooldown (max one OTA attempt per 24h)
- Clone-aware OTA source (uses GitHub origin repo at build time)

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

### Logging Flags

Logging controls are split between server log submission and serial debug output:

- `ENABLE_SERVER_LOGS`: controls sending logs to `/api/log` (enabled by default in `platformio.ini`).
- `DEBUG_LOGS`: controls serial debug output (disabled by default in `platformio.ini`).

Typical combinations:

- Production with server logs only: `ENABLE_SERVER_LOGS` enabled, `DEBUG_LOGS` disabled.
- Local serial debugging + server logs: enable both flags.
- Silent mode: disable both flags.

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

## OTA Updates

The firmware supports two OTA trigger paths:

1. Server-driven OTA (/api/display):
- Uses update_firmware + firmware_url when returned by the TRMNL-compatible API.

2. GitHub releases OTA fallback:
- Checks releases/latest once per 24 hours.
- Downloads the asset matching `OTA_FIRMWARE_ASSET_NAME` when release version is newer than FW_VERSION.

Default asset naming:
- `OTA_FIRMWARE_ASSET_NAME` defaults to `m5paper.bin` (derived from `DEVICE_MODEL ".bin"`).
- This prevents wrong-device updates when multiple firmware assets are published in one release.

Safety rules:
- GitHub check cooldown: 24h.
- OTA attempt cooldown: 24h.
- OTA is skipped on low battery unless external power is present.

### Clone-Aware Release Source

At build time, PlatformIO runs a pre-script that reads git remote origin and injects:
- OTA_GITHUB_OWNER
- OTA_GITHUB_REPO

This means if someone forks/clones the project and builds from their own GitHub remote,
OTA checks their repository releases automatically.

If origin cannot be parsed as a GitHub URL, firmware falls back to default:
- owner: paveldn
- repo: trmnl-m5paper

The pre-build script also reports why fallback was used:
- no remote.origin.url configured,
- non-GitHub origin host detected (for example GitLab or Bitbucket), or
- GitHub origin present but owner/repo could not be parsed.

Release workflow note:
- `.github/workflows/release.yml` publishes `dist/m5paper.bin` so OTA can select a stable, device-specific asset name.

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