# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Community-supported firmware for Tidbyt hardware (and other ESP32-based matrix displays), built with native ESP-IDF. Displays WebP images fetched via HTTP polling or WebSocket. Written in C++ with `extern "C"` public APIs in headers.

## Build Commands

```bash
# Initialize ESP-IDF (required once per terminal session)
. ~/esp/esp-idf/export.sh

# Build for specific hardware targets
make tidbyt-gen1        # ESP32
make tidbyt-gen2        # ESP32
make tronbyt-s3         # ESP32-S3
make tronbyt-s3-wide    # ESP32-S3
make pixoticker         # ESP32
make matrixportal-s3    # ESP32-S3

# Or build directly with idf.py
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.tidbyt-gen1" build

# Flash and monitor
idf.py flash monitor

# Configuration
idf.py menuconfig       # Opens Kconfig UI under "Application Configuration"

# Clean
idf.py fullclean        # Full rebuild (also deletes sdkconfig)
```

Each `make <board>` target deletes sdkconfig, sets the IDF target, builds, and creates a merged binary.

## Configuration System

Two-tier configuration:

1. **Kconfig** (`Kconfig.projbuild` + `sdkconfig.defaults.*`): Structural settings that affect the binary — stack sizes, compiler optimization, log levels, pin assignments (`CONFIG_BUTTON_PIN`), board-specific hardware flags, feature flags (`ENABLE_AP_MODE`), LwIP tuning. Compiled into binary.
2. **secrets.json**: Deployment-specific parameters — WiFi SSID/password, remote URL, brightness, refresh interval. Parsed by `generate_secrets_cmake.py` at build time into compiler defines via `secrets.cmake`.

**Override priority** (highest wins): NVS (captive portal / WebSocket commands) > `secrets.json` (build-time) > Kconfig defaults

Use Kconfig for structural changes that affect code or memory layout. Use secrets.json for parameters that vary between deployments or need runtime configurability via NVS.

Copy `secrets.json.example` to `secrets.json` before building.

## Architecture

### Source Layout (`main/`)

| Directory | Purpose |
|-----------|---------|
| `main.cpp` | Entry point (`app_main`), boot mode detection |
| `startup/` | `runtime_orchestrator` — event-driven init with WiFi validation |
| `scheduler/` | `scheduler_fsm` — playback state machine, HTTP prefetch |
| `webp_player/` | WebP decode + render task with frame timing |
| `display/` | HUB75 LED matrix driver abstraction (uses `esp-hub75` component) |
| `network/` | WiFi STA, WebSocket client (`sockets`), HTTP fetch (`remote`), REST API (`sta_api`), mDNS, message parsing (`handlers`, `messages`), API validation, config contract, WebUI server |
| `config/` | NVS settings, captive portal AP mode, DNS wrapper, HTML templates |
| `system/` | OTA updates, heap monitor, syslog, NTP, app state machine, event bus, diagnostics, device temperature, console |

### Key Patterns

- **Event bus** (`system/event_bus`): Decoupled pub/sub for system, network, display, and OTA events
- **App state machine** (`system/app_state`): Boot → Normal → Config Portal / OTA / Error states
- **RAII** (`raii_utils.hpp`): `MutexGuard` wrapper for FreeRTOS semaphores
- **Anonymous namespaces** for file-scoped statics (C++ idiom, no `static` globals)
- **`extern "C"` guards** in all `.h` files for C/C++ interop
- **No STL containers** — fixed arrays, `heap_caps_malloc` for PSRAM
- **No exceptions / RTTI** (ESP-IDF default)

### Dependencies (managed via `idf_component.yml`)

- `esp-hub75` (datagutt/esp-hub75) — HUB75 LED matrix driver
- `libwebp` (datagutt/libwebp) — WebP decoding with Xtensa PIE
- `esp_websocket_client` — WebSocket connectivity
- `espressif/cjson` — JSON parsing
- `espressif/mdns` — mDNS service advertisement
- `joltwallet/littlefs` — LittleFS for WebUI partition

### Supported Boards

ESP32: tidbyt-gen1, tidbyt-gen2, pixoticker
ESP32-S3: tronbyt-s3, tronbyt-s3-wide, matrixportal-s3, matrixportal-s3-waveshare

## Code Style

- Formatted with clang-format using **Google** style (`BasedOnStyle: Google`)
- C++ internals: anonymous namespaces, `enum class`, `constexpr`, `std::atomic`
- C public APIs: free functions with `extern "C"` linkage in headers
- ESP-IDF logging macros (`ESP_LOGI`, `ESP_LOGE`, `ESP_LOGW`) with per-file `TAG`

## CI

GitHub Actions workflow (`.github/workflows/main.yml`) builds firmware and injects version from git tags. Local builds show "vdev"; tagged builds show the tag version.

## Version Injection

`main/version.h` defaults to `"dev"`. CI overwrites it with the git tag. The version displays on the LED matrix at boot for 1 second.
