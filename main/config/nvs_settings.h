#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SSID_LEN 32
#define MAX_PASSWORD_LEN 64
#define MAX_HOSTNAME_LEN 32
#define MAX_URL_LEN 512
#define MAX_IP_LEN 64
#define MAX_SYSLOG_ADDR_LEN 128
#define MAX_SNTP_SERVER_LEN 64
#define MAX_API_KEY_LEN 128

typedef struct {
  char hostname[MAX_HOSTNAME_LEN + 1];
  char syslog_addr[MAX_SYSLOG_ADDR_LEN + 1];
  char sntp_server[MAX_SNTP_SERVER_LEN + 1];
  char image_url[MAX_URL_LEN + 1];
  char api_key[MAX_API_KEY_LEN + 1];
  bool swap_colors;
  wifi_ps_type_t wifi_power_save;
  bool skip_display_version;
  bool skip_boot_animation;
  bool ap_mode;
  bool prefer_ipv6;
  bool disable_touch;
} system_config_t;

// Multi-network WiFi storage — the sole source of truth for credentials.
// Credentials live only here (not in system_config_t); every consumer reads the
// list directly. Slot order is the user's stored order; the runtime connect path
// re-ranks by live RSSI.
#define MAX_WIFI_NETS 5

typedef struct {
  char ssid[MAX_SSID_LEN + 1];
  char password[MAX_PASSWORD_LEN + 1];
  int8_t last_rssi;   // dBm from the most-recent successful connect; 0 = unknown
  uint8_t priority;   // user-set order (0 = highest); secondary to RSSI ranking
  uint8_t _pad[2];    // reserved
} wifi_network_t;

/// Initialize NVS and load settings into the config struct.
esp_err_t nvs_settings_init(void);

/// Copy stored networks (non-empty SSID only) into out[], up to `max` entries.
/// Returns the number written. Thread-safe.
size_t wifi_network_list_get(wifi_network_t* out, size_t max);

/// Replace the entire stored list with the first `count` entries of `list`
/// (empty-SSID entries skipped, capped at MAX_WIFI_NETS). Persists. Thread-safe.
void wifi_network_list_set(const wifi_network_t* list, size_t count);

/// Add a network, or update the password of an existing one (matched by SSID).
/// Returns false only if the list is full and the SSID is new. Thread-safe.
bool wifi_network_list_add(const char* ssid, const char* password);

/// Remove the network whose SSID matches. Returns true if one was removed and
/// compacts the list. Thread-safe.
bool wifi_network_list_remove(const char* ssid);

/// Number of stored networks with a non-empty SSID. Thread-safe.
size_t wifi_network_list_count(void);

/// Record the signal strength seen for `ssid` on a successful connect. Updates
/// the in-memory list and persists only when the value changes meaningfully (to
/// bound flash wear). No-op if the SSID is not stored. Thread-safe.
void wifi_network_note_rssi(const char* ssid, int8_t rssi);

/// Return a thread-safe copy of the current configuration.
system_config_t config_get(void);

/// Apply a new configuration and persist it to NVS (atomic save).
void config_set(const system_config_t* cfg);

/// Return a monotonically-increasing generation counter that increments
/// on every config_set() call. Useful for change detection (e.g., web UI
/// can poll to know when settings changed).
uint32_t config_generation(void);

#ifdef __cplusplus
}
#endif
