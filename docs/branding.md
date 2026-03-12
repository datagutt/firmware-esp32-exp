# Branding System

Multi-brand firmware support. Each build can have its own brand identity — name, colors, boot animation, and server URL.

## Brand Configuration

Kconfig menu in `main/Kconfig.projbuild` → "Brand Configuration":

| Config | Default | Purpose |
|--------|---------|---------|
| `CONFIG_BRAND_NAME` | `"Tronbyt"` | Display name (WebUI header, captive portal, page titles) |
| `CONFIG_BRAND_NAME_LOWER` | `"tronbyt"` | Lowercase (hostname prefix, mDNS service, API type field) |
| `CONFIG_BRAND_ACCENT_COLOR` | `"#00d4ff"` | CSS hex color for WebUI accent |
| `CONFIG_DEFAULT_SERVER_URL` | `""` | Pre-configured server URL for branded firmware |
| `CONFIG_LOCK_SERVER_URL` | `n` | Hide server URL field from setup/settings UI |

## Brand Overlay Files

Brand configs live in `brands/*.cfg` and layer on top of board configs:

```ini
# brands/mybrand.cfg
CONFIG_BRAND_NAME="MyBrand"
CONFIG_BRAND_NAME_LOWER="mybrand"
CONFIG_BRAND_ACCENT_COLOR="#ff6b00"
CONFIG_BOOT_WEBP_MYBRAND=y
CONFIG_DEFAULT_SERVER_URL="https://api.mybrand.com/"
CONFIG_LOCK_SERVER_URL=y
```

Config priority: `sdkconfig.defaults` → `sdkconfig.defaults.{board}` → `brands/{brand}.cfg`

## Building

```bash
# Single board with brand overlay
make branded BOARD=tronbyt-s3 BRAND=mybrand CHIP=esp32s3

# All boards for a brand
make brand-all BRAND=mybrand
```

## Where Branding Applies

| Location | Mechanism |
|----------|-----------|
| Boot animation | Kconfig `CONFIG_BOOT_WEBP_*` selects asset at compile time |
| WebUI (header, title, accent) | JS fetches `/api/about` → `brand` object, applies dynamically |
| Captive portal (setup/success) | `snprintf` with `CONFIG_BRAND_NAME` in HTML templates |
| Hostname | `CONFIG_BRAND_NAME_LOWER "-%02x%02x%02x"` in `wifi.cpp` |
| mDNS service | `"_" CONFIG_BRAND_NAME_LOWER` in `mdns_service.cpp` |
| API type field | `CONFIG_BRAND_NAME_LOWER` in `/api/about` response |

## Server URL Locking

When `CONFIG_LOCK_SERVER_URL=y`:
- Captive portal hides the image URL field
- WebUI settings hides the Wi-Fi & Connection card (advanced toggle reveals it)
- NVS always overrides with `CONFIG_DEFAULT_SERVER_URL`
- WebSocket and API config changes to `image_url` are silently ignored

## Assets

Boot animations and error screens are managed by the asset pipeline. See `resources/README.md`.
