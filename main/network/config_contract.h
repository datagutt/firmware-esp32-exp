#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool swap_colors;
  int wifi_power_save;
  bool skip_display_version;
  bool ap_mode;
  bool prefer_ipv6;
  char hostname[33];
  char syslog_addr[129];
  char sntp_server[65];
  char image_url[513];
} config_contract_state_t;

typedef struct {
  bool has_swap_colors;
  bool swap_colors;
  bool has_wifi_power_save;
  int wifi_power_save;
  bool has_skip_display_version;
  bool skip_display_version;
  bool has_ap_mode;
  bool ap_mode;
  bool has_prefer_ipv6;
  bool prefer_ipv6;
  bool has_hostname;
  const char* hostname;
  bool has_syslog_addr;
  const char* syslog_addr;
  bool has_sntp_server;
  const char* sntp_server;
  bool has_image_url;
  const char* image_url;
} config_contract_patch_t;

bool config_contract_apply_patch(const config_contract_state_t* in_state,
                                 const config_contract_patch_t* patch,
                                 config_contract_state_t* out_state,
                                 char* err, unsigned err_len);

#ifdef __cplusplus
}
#endif
