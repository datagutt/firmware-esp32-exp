#include "config_contract.h"

#include <stdio.h>
#include <string.h>

namespace {

bool set_error(char* err, unsigned err_len, const char* text) {
  if (err && err_len > 0) {
    snprintf(err, err_len, "%s", text);
  }
  return false;
}

bool copy_checked(char* dst, size_t dst_len, const char* src, size_t min_len,
                  const char* field, char* err, unsigned err_len) {
  if (!src) {
    char msg[80];
    snprintf(msg, sizeof(msg), "%s must be provided", field);
    return set_error(err, err_len, msg);
  }

  size_t len = strlen(src);
  if (len < min_len || len >= dst_len) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s length out of range", field);
    return set_error(err, err_len, msg);
  }

  snprintf(dst, dst_len, "%s", src);
  return true;
}

}  // namespace

bool config_contract_apply_patch(const config_contract_state_t* in_state,
                                 const config_contract_patch_t* patch,
                                 config_contract_state_t* out_state,
                                 char* err, unsigned err_len) {
  if (!in_state || !patch || !out_state) {
    return set_error(err, err_len, "invalid arguments");
  }

  *out_state = *in_state;

  if (patch->has_swap_colors) out_state->swap_colors = patch->swap_colors;
  if (patch->has_skip_display_version)
    out_state->skip_display_version = patch->skip_display_version;
  if (patch->has_ap_mode) out_state->ap_mode = patch->ap_mode;
  if (patch->has_prefer_ipv6) out_state->prefer_ipv6 = patch->prefer_ipv6;

  if (patch->has_wifi_power_save) {
    if (patch->wifi_power_save < 0 || patch->wifi_power_save > 2) {
      return set_error(err, err_len, "wifi_power_save out of range");
    }
    out_state->wifi_power_save = patch->wifi_power_save;
  }

  if (patch->has_hostname &&
      !copy_checked(out_state->hostname, sizeof(out_state->hostname),
                    patch->hostname, 1, "hostname", err, err_len)) {
    return false;
  }
  if (patch->has_syslog_addr &&
      !copy_checked(out_state->syslog_addr, sizeof(out_state->syslog_addr),
                    patch->syslog_addr, 0, "syslog_addr", err, err_len)) {
    return false;
  }
  if (patch->has_sntp_server &&
      !copy_checked(out_state->sntp_server, sizeof(out_state->sntp_server),
                    patch->sntp_server, 0, "sntp_server", err, err_len)) {
    return false;
  }
  if (patch->has_image_url &&
      !copy_checked(out_state->image_url, sizeof(out_state->image_url),
                    patch->image_url, 0, "image_url", err, err_len)) {
    return false;
  }
  if (patch->has_api_key &&
      !copy_checked(out_state->api_key, sizeof(out_state->api_key),
                    patch->api_key, 0, "api_key", err, err_len)) {
    return false;
  }

  return true;
}
