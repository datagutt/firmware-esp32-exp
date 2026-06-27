# Design 014: Multi-network WiFi and optional secure provisioning

**Status**: SPIKE complete. Plan A (multi-network) is GO. Plan B (BLE) is DEFER.
**Planned at**: commit `79c0128`, 2026-06-27
**Relates to**: plans/005 (auth), plans/007 (portal hardening)

---

## 1. Current state (drift check confirmed)

`main/config/ap.cpp` `save_handler` stores a single credential set:

```cpp
snprintf(cfg.ssid,     sizeof(cfg.ssid),     "%s", ssid);
snprintf(cfg.password, sizeof(cfg.password), "%s", password);
config_set(&cfg);
```

`main/config/nvs_settings.h` `system_config_t` holds one `ssid[33]` and one
`password[65]`. The config is persisted atomically as a blob under key `"cfg"` in
the `"wifi_config"` NVS namespace, with individual string keys `"ssid"`/`"password"`
also written for backward compatibility.

`main/network/wifi.cpp` connects to the single stored credential. After
`MAX_RECONNECT_ATTEMPTS = 10` consecutive disconnects it sets
`s_connection_given_up = true` and raises the captive portal.

There is no BLE provisioning. The portal is open HTTP on the device's soft-AP.

---

## 2. Part A: Multi-network storage

### 2.1 Proposed NVS schema

Add a second NVS namespace `"wifi_nets"` carrying a single blob key `"nets"` of
type `wifi_network_t[MAX_WIFI_NETS]`. This keeps `system_config_t` and the existing
`"wifi_config"` blob unchanged, so NVS-migration risk for the rest of the settings
is zero.

```c
#define MAX_WIFI_NETS 5
#define MAX_SSID_LEN  32
#define MAX_PASS_LEN  64

typedef struct {
    char    ssid[MAX_SSID_LEN + 1];
    char    password[MAX_PASS_LEN + 1];
    int8_t  last_rssi;   // dBm from most-recent successful connect; 0 = unknown
    uint8_t priority;    // user-set order (0 = highest); secondary to RSSI ranking
    uint8_t _pad[2];     // keep struct size a multiple of 4
} wifi_network_t;        // 102 bytes; 5 slots = 510 bytes blob
```

The `ssid`/`password` fields in `system_config_t` are kept as-is and remain the
"active credential" cache, always mirroring slot 0 of the network list. This
preserves all existing call sites in the codebase without change.

### 2.2 Migration plan

On first boot with new firmware, `"wifi_nets"` blob will not exist. During
`nvs_settings_init()`, after loading `system_config_t`:

1. Attempt `get_blob("wifi_nets", "nets", ...)`. If it succeeds and the blob length
   matches `sizeof(wifi_network_t) * MAX_WIFI_NETS`, use it directly.
2. If it fails with `ESP_ERR_NVS_NOT_FOUND`, read the legacy `"ssid"` and
   `"password"` string keys from `"wifi_config"` (the init code already does this at
   lines 210-216 of `nvs_settings.cpp`). Copy them into `nets[0]`, zero the
   remaining slots, and write the new blob. Log the migration.
3. If `"wifi_nets"` exists but its length doesn't match (future struct change),
   discard and migrate from `"ssid"`/`"password"` as above.

Because the legacy individual string keys are already written on every
`config_set()`, a device that loses its new `"wifi_nets"` blob (power-cycle mid
write, NVS partial corruption) will degrade gracefully to a single-credential
device on the next boot, not a factory-reset device. This is the most important
property for deployed hardware.

The legacy string keys `"ssid"` and `"password"` in `"wifi_config"` should
continue to be written on every save, tracking `nets[0]`, so that any firmware
rollback sees valid credentials.

### 2.3 Ranked connect strategy

The trmnl firmware (Arduino `WiFi.h`) shows the right approach conceptually; the
ESP-IDF equivalents are:

```
esp_wifi_set_scan_method(WIFI_IF_STA, WIFI_ALL_CHANNEL_SCAN)
esp_wifi_set_sort_method(WIFI_IF_STA, WIFI_CONNECT_AP_BY_SIGNAL)
```

Setting these before `esp_wifi_connect()` handles the intra-SSID multi-AP case
(mesh/roaming) where multiple BSSIDs share an SSID. The device will connect to the
strongest AP for that SSID rather than the first one found.

For the cross-SSID multi-network case (trying different stored SSIDs):

**Boot-time connect sequence** (replaces the current single-call approach):

1. Run a passive scan (`esp_wifi_scan_start()`, blocking or async with event
   notification) after `esp_wifi_start()`.
2. Call `esp_wifi_scan_get_ap_records()` to retrieve visible BSSIDs with RSSI.
3. For each visible BSSID, check if its SSID matches any stored `wifi_network_t`.
4. Build a candidate list sorted by (RSSI descending, priority ascending), deduplicated by SSID.
5. Try each candidate SSID in order: `esp_wifi_set_config()` with SSID+password,
   then `esp_wifi_connect()`. Wait for `WIFI_CONNECTED_BIT` or `WIFI_FAIL_BIT`
   (current 10-attempt retry loop runs per SSID; reduce to 3 attempts per SSID
   since we will try multiple networks).
6. On success: update `nets[slot].last_rssi` from scan data, write blob.
7. On exhaustion of all candidates: fall back to portal as today.

**Reconnect after disconnect** (inside `WIFI_EVENT_STA_DISCONNECTED` handler):

Do not scan on every disconnect (too slow). Retry the same SSID for
`MAX_RECONNECT_ATTEMPTS_PER_NET = 3`. After exhaustion, promote the next candidate
network. If all candidates exhausted, raise the portal.

Scanning adds roughly 1-2 seconds at boot. That is acceptable. The scan should be
skipped if only one network is stored (single-credential path, existing behavior).

### 2.4 Change list with effort and risk

| File | Change | Effort | Risk |
|---|---|---|---|
| `main/config/nvs_settings.h` | Add `wifi_network_t`, add `wifi_network_list_get()` / `wifi_network_list_set()` / `wifi_network_list_add()` / `wifi_network_list_remove()` functions | S | LOW |
| `main/config/nvs_settings.cpp` | New namespace `"wifi_nets"`, blob load/save, migration from legacy keys on first boot | M | MEDIUM (migration must be tested on real hardware with existing data) |
| `main/network/wifi.cpp` | Replace single-connect path with scan + RSSI-ranked multi-connect; reduce per-SSID retry count; refactor disconnect handler to step through network list before raising portal | L | MEDIUM (event-driven state machine becomes more complex; reconnect logic branches) |
| `main/config/ap.cpp` (`save_handler`) | Accept POST, check if SSID already in list, add or update the matching slot, keep `cfg.ssid`/`cfg.password` in sync with `nets[0]` | S | LOW |
| `main/config/ap.cpp` (`root_handler`) | Render saved network list with per-entry delete buttons; show which network is active | S | LOW |
| `main/config/html/setup.html` | Add network list table below the add-network form; delete endpoint (`/network/delete?ssid=...`); distinguish "add new" from "update existing" | M | LOW |

Total effort estimate: M-L (4-7 developer-days including hardware migration testing).

### 2.5 NVS encryption linkage

Storing 5 credentials multiplies the plaintext-secret exposure. The NVS encryption
question (deferred in `plans/README.md`) becomes more pressing when multi-network is
live: one NVS dump exposes up to 5 household or corporate WiFi passwords instead of
one. If BLE provisioning is built later (Part B), the device password also lands in
NVS. This is not a blocker for Plan A, but the risk should be re-evaluated before
shipping multi-network to a retail audience. See the NVS-encryption deferred item in
`plans/README.md` for the eFuse implications.

---

## 3. Part B: BLE provisioning assessment

### 3.1 kd_common design summary

`inspiration/kd_common/src/provisioning.c` implements BLE provisioning using the
ESP-IDF `network_provisioning` component (the current-SDK rename of
`wifi_provisioning`). Key properties:

- Transport: `network_prov_scheme_ble`. The device advertises over BLE only; no AP.
- Security: `NETWORK_PROV_SECURITY_2` — SRP-6a (Secure Remote Password). The
  companion app and device perform a zero-knowledge handshake using a short PIN
  shown on the device. Without the PIN, an eavesdropper cannot recover the WiFi
  password even if they capture the BLE exchange.
- PIN format: configurable. Options: static string, 4-digit random numeric, 6-digit
  random numeric, 6-digit reduced-entropy (digits 0-5 only). The PIN is generated
  on boot, displayed on screen, and used to derive the SRP verifier in memory
  (`esp_srp_gen_salt_verifier`).
- Custom endpoint: a `BLE_CONSOLE_ENDPOINT_NAME` GATT endpoint is registered on top
  of the provisioning BLE transport, allowing additional device control commands
  over the same BLE connection.
- Memory lifecycle: `NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM` is set. On
  `NETWORK_PROV_END`, the manager calls `esp_bt_mem_release(ESP_BT_MODE_BTDM)`,
  releasing BLE heap back to the FreeRTOS allocator. BT memory is consumed only
  during the provisioning window.
- Flow: WiFi start event → `network_prov_mgr_is_wifi_provisioned()` → if not
  provisioned, start BLE → user pairs via companion app, confirms PIN → credentials
  written by manager → provisioning ends → BT freed → device reconnects as STA.

### 3.2 ESP-IDF wifi_provisioning assessment for this firmware

**Memory cost**: ESP32 BLE-only mode requires ~50-60 KB heap during provisioning.
This is released by the `FREE_BTDM` handler at the end of the provisioning session.
Steady-state memory is unaffected. On ESP32-S3 (the primary board), the BLE stack
memory cost is similar and also freed post-provisioning.

**Companion app requirement**: A companion app is a hard dependency. Options are:
- Espressif's `ESP BLE Prov` (iOS/Android) — generic, available on app stores,
  handles SECURITY_2/SRP, can show custom device names. No branding.
- A custom branded app (web Bluetooth, React Native, or native) — branded
  experience, full control, significant investment.
- `esp_prov.py` CLI (Linux/macOS) — developer-only, not viable for end-users.

This firmware has no companion app today. Shipping BLE provisioning without one
means users must download a generic third-party Espressif app, which is a poor UX
for a product device and an unacceptable dependency for a hobbyist project where
most users can reach the AP captive portal from a phone browser.

**Coexistence with the captive portal**: BLE provisioning and the AP captive portal
are mutually exclusive as the primary provisioning path. Running both simultaneously
is possible (BLE + STA mode) but adds complexity for little gain. The preferred
model if BLE is adopted is: BLE is the primary path, AP portal is the fallback for
environments where BLE is unavailable. Both paths write credentials through the same
`wifi_network_list_set()` API (Plan A's addition).

**API note**: ESP-IDF v5.x renamed `wifi_provisioning` to `network_provisioning`.
The `kd_common` code already uses the new names. The firmware's `sdkconfig` would
need `CONFIG_NETWORK_PROV_MANAGER_ENABLE=y` and `CONFIG_BT_ENABLED=y`.

### 3.3 Threat model open question (GATES go/no-go)

BLE provisioning's value is entirely dependent on the deployment threat model. The
tradeoff is:

**Hobbyist / DIY / single-owner LAN**:
The owner is within Bluetooth range of their own device. The captive portal's open
HTTP AP is a short-lived surface (minutes, at known location). An attacker needs
physical proximity and to be present in that specific time window. The marginal
security gain of SRP over open AP is low, and the companion-app friction is high.
BLE provisioning is **not worth the investment** in this scenario.

**Retail / field deployment / shared spaces**:
The device is provisioned by an installer or end-user in a semi-public space
(office, retail store, event venue). The AP captive portal broadcasts an open WiFi
network that any nearby device can join, allowing credential interception or
evil-twin attacks. BLE provisioning with SRP eliminates this surface: proximity +
PIN confirmation is required. If the product ships to retail or is deployed in
shared environments, BLE provisioning is **worth the investment**.

This is the OPEN QUESTION: which scenario describes this product? The answer
determines whether Plan B is worth the cost of a companion app, the BLE stack
complexity, and the provisioning UX redesign. **The operator must decide this before
Plan B is built.**

---

## 4. Phased recommendation

### Phase A: Multi-network WiFi (GO)

Build now. The utility is high regardless of deployment model (multi-AP home
networks, office rotation, IoT relocations). The NVS migration is safe because the
legacy fallback keys already exist. The implementation is ESP-IDF-native and does
not require any new hardware feature, companion app, or protocol.

**Prerequisites**: merge Plans 005 (auth) and 007 (portal hardening) first, as they
touch `ap.cpp`, `setup.html`, and `nvs_settings.*` — files that Plan A also
modifies. Building on top of already-merged 005/007 avoids conflicts.

**Go/no-go**: GO.

### Phase B: BLE provisioning (DEFER)

Defer until:
1. The product threat model is resolved in favour of retail/field deployment, AND
2. A companion app (or web-Bluetooth portal, or custom CLI) exists or is planned.

Neither condition is met today. Building BLE provisioning without a companion app
ships the hardware component but leaves users without the software side, resulting in
an un-provisionable device for non-developers. That is worse than the current portal.

If the threat model resolves to hobbyist-only, this plan is a NO-GO entirely.

**Go/no-go**: DEFER (pending threat-model decision and companion-app commitment).

### Phase ordering summary

```
[Plans 005 + 007] → [Plan 014-A (multi-network)] → [Plan 014-B (BLE)] ← gated on threat model
```

---

## 5. Open questions

1. **Threat model** (blocks BLE go/no-go): Is the device deployed by a single
   owner at home (hobbyist) or by installers/end-users in shared spaces (retail)?
   This is the single biggest question. BLE provisioning is a large effort that only
   pays off under the retail/field scenario.

2. **Companion app**: If BLE is pursued, which companion-app path? Generic Espressif
   `ESP BLE Prov` app (fast to ship, poor branding), custom web-Bluetooth page
   (no app-store friction, limited BLE API coverage), or native app (full control,
   most cost)?

3. **Network list UI on display**: When the device raises the portal, should the
   LED matrix show a "trying network 2 of 5" indicator, or is the serial log
   sufficient? This is a UX polish question for Plan A.

4. **Priority vs. RSSI**: The design ranks primarily by RSSI (strongest signal
   wins). Some users might prefer a fixed priority order (primary home, secondary
   office, tertiary mobile hotspot) that doesn't change based on current signal.
   The `priority` field in `wifi_network_t` supports this, but the UI to set
   priorities needs a decision before implementation.

5. **NVS encryption**: Multi-network stores up to 5 WiFi passwords in plaintext.
   The deferred NVS-encryption decision (referenced in `plans/README.md`) should be
   revisited before Plan A reaches retail.

---

## 6. Relationship to Plans 005 and 007

- Plan 005 adds Bearer-token auth to the HTTP portal's API endpoints. Plan A adds
  `/network/delete` and modifies `save_handler`. These new handlers must follow the
  same auth pattern as 005.
- Plan 007 hardens the portal against XSS (escaping in `setup.html`, CSP headers).
  The network list rendered by Plan A (network SSIDs in table cells) must escape
  SSID strings that contain `<>` or quote characters. Build Plan A after 007 is
  merged so the escaping helpers are already present.
- Both 005 and 007 are on advisor-approved branches not yet merged to main. Merge
  order: 005 → 007 → 014-A.
