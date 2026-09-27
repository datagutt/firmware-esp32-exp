#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NTP_TIMEZONE_MAX_LEN 64
#define NTP_SERVER_MAX_LEN 64

/// NTP configuration persisted in NVS.
typedef struct {
  bool auto_timezone;
  bool fetch_tz_on_boot;
  char timezone[NTP_TIMEZONE_MAX_LEN];
  char ntp_server[NTP_SERVER_MAX_LEN];
} ntp_config_t;

/// Load NVS config, set initial TZ, register WiFi event handlers.
void ntp_init(void);

/// Check if time has been synchronized via SNTP.
bool ntp_is_synced(void);

/// Force a time re-sync (restarts SNTP).
void ntp_sync(void);

/// Get a copy of the full NTP configuration.
ntp_config_t ntp_get_config(void);

/// Replace the full NTP configuration and persist it.
void ntp_set_config(const ntp_config_t* config);

void ntp_set_auto_timezone(bool enabled);
bool ntp_get_auto_timezone(void);

void ntp_set_fetch_tz_on_boot(bool enabled);
bool ntp_get_fetch_tz_on_boot(void);

/// Set timezone by IANA name (e.g. "America/New_York"). Disables auto_timezone.
void ntp_set_timezone(const char* timezone);
/// Copy the IANA timezone name into out (NUL-terminated, truncated to len).
void ntp_get_timezone(char* out, size_t len);

void ntp_set_server(const char* server);
/// Copy the primary NTP server name into out (NUL-terminated, truncated).
void ntp_get_server(char* out, size_t len);

#ifdef __cplusplus
}
#endif
