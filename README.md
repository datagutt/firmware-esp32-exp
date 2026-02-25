# Tronbyt Firmware

[![Discord Server](https://img.shields.io/discord/928484660785336380?style=flat-square)](https://discord.gg/nKDErHGmU7)

This repository contains a community supported firmware for the Tidbyt hardware 🤓.

## Warning

⚠️ Warning! Flashing your Tidbyt with this firmware or derivatives could fatally
damage your device. As such, flashing your Tidbyt with this firmware or
derivatives voids your warranty and comes without support.

## Getting Started

Follow the setup instructions for [tronbyt-server][3] unless you want to build yourself with ESP-IDF.

## Building yourself with ESP-IDF

Only follow these instructions if you want to build the firmware yourself. Otherwise let the [tronbyt-server][3] generate the firmware file for you.
This project uses the native [ESP-IDF][2] framework to build, flash, and monitor firmware.

Additionally, this firmware is designed to work with <https://github.com/tronbyt/server> or
you can point this firmware at any URL that hosts a WebP image that is optimized for the Tidbyt display.

### Configuration

To flash the custom firmware on your device, follow these steps:

1. Copy `secrets.json.example` to `secrets.json`.
2. Edit `secrets.json` with your information. If using tronbyt_manager in Docker, use the Docker host's IP address.

Example `secrets.json`:

```json
{
    "WIFI_SSID": "myssid",
    "WIFI_PASSWORD": "<PASSWORD>",
    "REMOTE_URL": "http://homeServer.local:8000/tronbyt_1/next",
}
```

### Build and Flash

Use the provided `Makefile` for convenience to build for specific hardware:

```bash
# For Tidbyt Gen 1
make tidbyt-gen1

# For Tidbyt Gen 2
make tidbyt-gen2

# For Tronbyt S3
make tronbyt-s3
```

To flash the built firmware to your device:

```bash
idf.py flash
```

## Monitoring Logs

To check the output of your running firmware, run the following:

```bash
idf.py monitor
```

## Advanced Settings

The firmware supports several advanced settings stored in Non-Volatile Storage (NVS). These can be configured via the WebSocket connection or by using `idf.py menuconfig` (which sets the build-time defaults).

| Setting | NVS Key | Description |
| :--- | :--- | :--- |
| **Hostname** | `hostname` | The network hostname of the device. Defaults to `tronbyt-<mac>`. |
| **Syslog Address** | `syslog_addr` | Remote Syslog (RFC 5424) server in `host:port` format (e.g., `192.168.1.10:1517`). |
| **SNTP Server** | `sntp_server` | Custom NTP server for time synchronization. Defaults to DHCP provided servers or `pool.ntp.org`. |
| **API Key** | `api_key` | Bearer token for server authentication (sent via `Authorization` header on HTTP and WebSocket connections). |
| **Swap Colors** | `swap_colors` | Boolean (0/1) to swap RGB color order. Useful for specific panel variants. |
| **AP Mode** | `ap_mode` | Boolean (0/1) to enable/disable the fallback WiFi configuration portal. |
| **WiFi Power Save**| `wifi_ps` | WiFi power management mode (0: None, 1: Min, 2: Max). |
| **Prefer IPv6** | `prefer_ipv6` | Boolean (0/1) to prefer IPv6 connectivity over IPv4. |

## Back to Normal

### Using Web Flasher (Recommended)

The easiest way to restore your Tidbyt to factory firmware is using the web flasher with the pre-built merged binary files:

1. Download the appropriate merged binary file:
   - **Gen 1**: [gen1_merged.bin](https://github.com/tronbyt/firmware-esp32/raw/main/reset/gen1_merged.bin)
   - **Gen 2**: [gen2_merged.bin](https://github.com/tronbyt/firmware-esp32/raw/main/reset/gen2_merged.bin)
2. Visit [https://espressif.github.io/esptool-js/](https://espressif.github.io/esptool-js/) (requires Chrome or Edge browser)
3. Connect your Tidbyt via USB
4. Use the following settings:
   - **Flash Address**: `0x0`
   - **File**: Select the downloaded merged binary file

![Web Flasher Settings](docs/assets/web_flasher_settings.png)

1. Click "Program" to flash the factory firmware

### Using the WiFi config portal

The firmware has a rudimentary wifi config portal page that can be accessed by joining the TRONBYT-CONFIG network and navigating to <http://10.10.0.1>.

[WiFi Config Portal How-To Video](https://www.youtube.com/watch?v=OAWUCG-HRDs)

## API Routes

### HTTP API (STA mode)

Available on the device's IP once connected to WiFi. All endpoints return JSON and support CORS (`OPTIONS /api/*`). Full OpenAPI 3.0.3 spec available in [`api-docs.yaml`](api-docs.yaml).

| Method | Path | Description |
| :--- | :--- | :--- |
| `GET` | `/api/status` | Firmware version, MAC address, free heap/SPIRAM, min free heap, images loaded count, device temperature, diag events status |
| `GET` | `/api/health` | Simple health check. Returns `{"status":"ok"}` (200) or `{"status":"degraded"}` (503) based on WiFi connectivity |
| `GET` | `/api/about` | Board model, device type, firmware version |
| `GET` | `/api/diag` | Diagnostics: reboot reason, WiFi stats (reconnect attempts, disconnect events), heap trend history, recent diagnostic events, OTA history |
| `GET` | `/api/system/config` | Current system config: auto timezone, timezone, NTP server, hostname, diag events enabled, brightness |
| `POST` | `/api/system/config` | Update system config. Accepts JSON with optional keys: `auto_timezone` (bool), `timezone` (string), `ntp_server` (string), `hostname` (string), `diag_events_enabled` (bool), `brightness` (int, 0-100) |
| `GET` | `/api/time/zonedb` | Full IANA timezone database (chunked response). Returns array of `{name, rule}` objects |
| `POST` | `/api/system/reboot` | Trigger device reboot |
| `POST` | `/api/ota/upload` | Upload firmware binary or TBUP bundle (see [OTA Bundle Updates](#ota-bundle-updates)) |

### WebSocket Interface

The device connects to the server via WebSocket. Messages are handled as follows:

**Binary messages** — Raw WebP image data. Supports chunked/fragmented frames. Images are queued for display with the current dwell time.

**Text messages** — JSON commands with the following optional keys:

| Key | Type | Description |
| :--- | :--- | :--- |
| `immediate` | bool | Interrupt current animation and show next queued image |
| `dwell_secs` | int (1–3600) | How long to display each image |
| `brightness` | int | Display brightness level |
| `ota_url` | string | URL to download and flash a firmware update |
| `swap_colors` | bool | Swap RGB color order (persisted to NVS) |
| `wifi_power_save` | int (0–2) | WiFi power save mode (persisted to NVS) |
| `skip_display_version` | bool | Skip version display on boot (persisted to NVS) |
| `ap_mode` | bool | Enable/disable config portal AP (persisted to NVS) |
| `prefer_ipv6` | bool | Prefer IPv6 connectivity (persisted to NVS) |
| `hostname` | string | Device hostname (persisted to NVS) |
| `syslog_addr` | string | Syslog server `host:port` (persisted to NVS) |
| `sntp_server` | string | Custom NTP server (persisted to NVS) |
| `image_url` | string | Remote image URL (persisted to NVS) |
| `reboot` | bool | Reboot the device |

### Captive Portal (AP mode)

Available when the device is in AP configuration mode (SSID: `TRON-CONFIG`, IP: `10.10.0.1`).

| Method | Path | Description |
| :--- | :--- | :--- |
| `GET` | `/` | WiFi setup form (SSID, password, image URL, API key) |
| `POST` | `/save` | Save WiFi credentials and config, then reboot |
| `POST` | `/update` | OTA firmware upload (binary `.bin` file or TBUP bundle) |
| `GET` | `/hotspot-detect.html` | Captive portal redirect (Apple) |
| `GET` | `/generate_204` | Captive portal redirect (Android) |
| `GET` | `/ncsi.txt` | Captive portal redirect (Windows) |
| `GET` | `/*` | Wildcard catch-all redirect to portal |

## Web UI

On boards with an 8MB+ flash and a `webui` partition (e.g. Tronbyt S3), the firmware serves a built-in web dashboard from a LittleFS partition. Access it at `http://<device-ip>/` or `http://tronbyt.local/` (via mDNS).

- WiFi status card showing SSID, IPv4 and IPv6 addresses
- `/setup` route for WiFi and server configuration
- Serves gzip-compressed static assets with SPA routing
- Falls back to the embedded setup page if the LittleFS partition is missing or corrupt (e.g. 4MB boards)

## OTA Bundle Updates

The firmware accepts a **TBUP bundle** that packages the app firmware and WebUI LittleFS image into a single file. This allows updating both partitions over HTTP without a USB connection.

```
Offset  Size  Field
0       4     Magic: "TBUP"
4       4     App size (uint32 LE)
8       4     WebUI size (uint32 LE, 0 = app-only)
12      4     Reserved (0)
16      N     App firmware binary
16+N    M     WebUI LittleFS image (optional)
```

Upload via `POST /api/ota/upload` (STA) or `POST /update` (AP) -- the same endpoints accept both plain `.bin` files and TBUP bundles (auto-detected by magic bytes).

Create bundles with the included tool:

```bash
python tools/create_bundle.py firmware.bin --webui webui.bin -o bundle.bin
python tools/create_bundle.py firmware.bin -o bundle.bin  # app-only
```

Safety: the app is written and boot partition set before the WebUI write begins. If the WebUI write fails or the board has no `webui` partition, the device boots normally with the new app.

## Differences from Original Firmware

This project is a modernized rewrite of the [original Tronbyt firmware](https://github.com/tronbyt/firmware-esp32).

### Display Driver

| | Original Firmware | This Project |
| :--- | :--- | :--- |
| **Library** | [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) v3.0.13 | [esp-hub75](https://github.com/datagutt/esp-hub75) |
| **Driver class** | `MatrixPanel_I2S_DMA` | `Hub75Driver` |
| **Pin config** | Hardcoded `#define` per board | Kconfig-driven (`CONFIG_HUB75_*`) |
| **Frame sync** | None | VSYNC via `CONFIG_DISPLAY_FRAME_SYNC` (eliminates tearing) |
| **Protocol** | I2S DMA only | GDMA (ESP32-S3) and PARLIO support |
| **128x64 panels** | Supported (Tronbyt S3 Wide) | Scaled buffer with PSRAM fallback |

### Architecture

| | Original Firmware | This Project |
| :--- | :--- | :--- |
| **Language** | Mostly C | C++ |
| **Structure** | Flat — all source files in `main/` | Modular — `main/display/`, `main/network/`, `main/system/`, `main/scheduler/`, `main/startup/`, `main/config/` |
| **Startup** | Sequential init in `main()` | Runtime orchestrator with event-driven startup |
| **WebP player** | Integrated into `main.c` / `gfx.c` | Dedicated `webp_player` component with event system (`GFX_PLAYER_EVENTS`) |
| **Networking** | Single `remote.c` | Split into handlers, sockets, HTTP server, STA API, mDNS, API validation |

### Dependencies

| | Original Firmware | This Project |
| :--- | :--- | :--- |
| **Display** | `ESP32-HUB75-MatrixPanel-DMA` | `esp-hub75` (datagutt/esp-hub75) |
| **WebP** | `tronbyt/libwebp` | `datagutt/libwebp` (with Xtensa PIE) |
| **WebSocket** | `esp_websocket_client` 1.6.0 | `esp_websocket_client` 1.6.1 |
| **JSON** | — | `espressif/cjson` |
| **mDNS** | — | `espressif/mdns` |

### Additional Features (not in original)

- **Web UI dashboard** — built-in LittleFS web interface with WiFi status, setup page, and SPA routing (see [Web UI](#web-ui))
- **OTA bundle updates** — single-file updates for app + WebUI via TBUP format (see [OTA Bundle Updates](#ota-bundle-updates))
- **HTTP REST API** — status, health, diagnostics, system config, brightness, reboot, OTA upload, and timezone database endpoints (see [API Routes](#api-routes))
- **API key authentication** — optional Bearer token auth for server communication (HTTP and WebSocket)
- **Event bus** — decoupled publish/subscribe architecture for system, network, display, and OTA events
- **Application state machine** — boot, normal, config portal, OTA, and error states with connectivity tracking
- **Touch control** — single tap (next app), double tap (cycle brightness), long hold (toggle display) on Tidbyt Gen 2
- **Brightness control** — adjustable via REST API, WebSocket, or touch (0-100%)
- **OpenAPI spec** — full API documentation in [`api-docs.yaml`](api-docs.yaml)
- **USB Serial console** — interactive diagnostics via `CONFIG_ENABLE_CONSOLE`
- **Heap monitor** — tracks memory usage with trend history
- **Device temperature** — on-chip temperature sensor reading
- **Diagnostic event ring** — in-memory event log with OTA history
- **mDNS service advertisement** — automatic start/stop on WiFi events
- **API input validation** — strict JSON key/type validation with error reporting
- **Embedded timezone database** — full IANA timezone DB served over HTTP
- **Scheduler FSM** — finite state machine for playback orchestration with HTTP prefetch
- **Runtime orchestrator** — event-driven startup with WiFi credential validation
- **CORS support** — cross-origin requests enabled on all API endpoints
- **Clean display shutdown** — `gfx_safe_restart` for graceful reboot
- **OTA image validation** — checks app descriptor magic to reject merged binaries uploaded via portal
- **Oversize image protection** — rejects WebP images exceeding display dimensions at decode stage
- **WiFi resilience** — escalates repeated socket failures to WiFi reset and device restart

## Troubleshooting

### OTA Update Fails with "Validation Failed" or "Checksum Error"

If you are seeing errors like `ESP_ERR_OTA_VALIDATE_FAILED` or checksum mismatches in the logs, especially on Gen 1 devices previously used with ESPHome or stock firmware:

1. **Partition Table Mismatch:** You likely have an old or incompatible partition table on your device. This happens if you flashed only the `firmware.bin` instead of the full `merged.bin` during the initial install.

2. **Solution:** You must perform a **clean install**.

    - Use the **Web Flasher** method described in "Back to Normal.
    - Ensure you select the **merged binary** (`gen1_merged.bin`).
    - Ideally, use the "Erase Flash" option in the flasher tool before programming to ensure a clean slate.

[2]: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html
[3]: https://github.com/tronbyt/server
