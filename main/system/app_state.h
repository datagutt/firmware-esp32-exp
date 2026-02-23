#pragma once

#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Global application states
// ---------------------------------------------------------------------------

typedef enum {
  APP_STATE_BOOT = 0,        // Startup sequence
  APP_STATE_NORMAL,           // Normal operation (content playback)
  APP_STATE_CONFIG_PORTAL,    // AP mode captive portal
  APP_STATE_OTA,              // Firmware update in progress
  APP_STATE_ERROR,            // Critical error (requires reboot)
} app_state_t;

// ---------------------------------------------------------------------------
// OTA sub-states
// ---------------------------------------------------------------------------

typedef enum {
  OTA_SUBSTATE_IDLE = 0,
  OTA_SUBSTATE_UPLOADING,
  OTA_SUBSTATE_FLASHING,
  OTA_SUBSTATE_VERIFYING,
  OTA_SUBSTATE_PENDING_REBOOT,
  OTA_SUBSTATE_FAILED,
} ota_substate_t;

// ---------------------------------------------------------------------------
// Config portal sub-states
// ---------------------------------------------------------------------------

typedef enum {
  CONFIG_SUBSTATE_WAITING = 0,
  CONFIG_SUBSTATE_SAVING,
  CONFIG_SUBSTATE_DONE,
} config_substate_t;

// ---------------------------------------------------------------------------
// Connectivity levels (orthogonal to main state)
// ---------------------------------------------------------------------------

typedef enum {
  CONNECTIVITY_NO_WIFI = 0,   // WiFi not connected
  CONNECTIVITY_NO_INTERNET,   // WiFi connected, no internet access
  CONNECTIVITY_CONNECTED,     // Internet available
  CONNECTIVITY_SERVER_ONLINE, // WebSocket connected to Tronbyt server
} connectivity_level_t;

// ---------------------------------------------------------------------------
// State change callback
// ---------------------------------------------------------------------------

typedef void (*app_state_change_cb_t)(app_state_t old_state,
                                      app_state_t new_state, void* ctx);

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/// Initialize the state machine. Starts in APP_STATE_BOOT.
void app_state_init(void);

// State transitions (return ESP_ERR_INVALID_STATE if denied by entry rules)
esp_err_t app_state_enter_normal(void);
esp_err_t app_state_enter_config_portal(void);
esp_err_t app_state_enter_ota(void);
esp_err_t app_state_enter_error(const char* reason);

/// Get current global state.
app_state_t app_state_get(void);

/// Get current connectivity level.
connectivity_level_t app_state_get_connectivity(void);

/// Get current OTA sub-state.
ota_substate_t app_state_get_ota_substate(void);

// Convenience queries
bool app_state_has_wifi(void);
bool app_state_has_internet(void);
bool app_state_is_server_online(void);

/// Update OTA sub-state (called from ota.cpp).
void app_state_set_ota_substate(ota_substate_t sub);

/// Update config portal sub-state (called from ap.cpp).
void app_state_set_config_substate(config_substate_t sub);

/// Update connectivity level (called from wifi.cpp, sockets.cpp).
void app_state_set_connectivity(connectivity_level_t level);

// Blocking waits
esp_err_t app_state_wait_for_wifi(uint32_t timeout_ms);
esp_err_t app_state_wait_for_internet(uint32_t timeout_ms);
esp_err_t app_state_wait_for_server(uint32_t timeout_ms);

// Callbacks
esp_err_t app_state_register_callback(app_state_change_cb_t cb, void* ctx);
void app_state_unregister_callback(app_state_change_cb_t cb);

/// Get a human-readable string for the state.
const char* app_state_name(app_state_t state);

/// Get a human-readable string for connectivity level.
const char* connectivity_level_name(connectivity_level_t level);

#ifdef __cplusplus
}
#endif
