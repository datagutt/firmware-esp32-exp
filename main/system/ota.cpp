#include "ota.h"

#include <atomic>
#include <cstring>

#include <arpa/inet.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include "display.h"
#include "diag_event_ring.h"
#include "ota_url_utils.h"
#include "webp_player.h"

namespace {

const char* TAG = "OTA";
std::atomic<bool> s_ota_in_progress{false};

bool is_ip_private(const struct sockaddr* addr) {
  if (addr->sa_family == AF_INET) {
    auto* sin = reinterpret_cast<const struct sockaddr_in*>(addr);
    uint32_t ip = ntohl(sin->sin_addr.s_addr);
    return (ip >> 24 == 10) ||        // 10.0.0.0/8
           ((ip >> 20) == 0xAC1) ||   // 172.16.0.0/12
           ((ip >> 16) == 0xC0A8) ||  // 192.168.0.0/16
           (ip >> 24 == 127);         // 127.0.0.0/8
  } else if (addr->sa_family == AF_INET6) {
    auto* sin6 = reinterpret_cast<const struct sockaddr_in6*>(addr);
    if ((sin6->sin6_addr.s6_addr[0] & 0xFE) == 0xFC) return true;
    if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) return true;
    if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr)) return true;
  }
  return false;
}

bool resolve_and_validate_host(const ota_url_parts_t* parts,
                               char* ip_str, size_t ip_str_len,
                               bool* is_ipv6) {
  if (!parts || !parts->host || parts->host_len == 0) {
    ESP_LOGE(TAG, "URL host missing");
    return false;
  }

  char host[256];
  size_t host_len = parts->host_len;
  if (host_len >= sizeof(host)) {
    ESP_LOGE(TAG, "URL host is too long");
    return false;
  }
  memcpy(host, parts->host, host_len);
  host[host_len] = '\0';

  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* res;
  if (getaddrinfo(host, nullptr, &hints, &res) != 0) {
    ESP_LOGE(TAG, "DNS resolution failed for %s", host);
    return false;
  }

  bool private_ip = false;
  *is_ipv6 = false;

  for (struct addrinfo* p = res; p; p = p->ai_next) {
    if (is_ip_private(p->ai_addr)) {
      void* addr_ptr;
      if (p->ai_family == AF_INET) {
        addr_ptr = &reinterpret_cast<struct sockaddr_in*>(p->ai_addr)
                        ->sin_addr;
        *is_ipv6 = false;
      } else {
        addr_ptr = &reinterpret_cast<struct sockaddr_in6*>(p->ai_addr)
                        ->sin6_addr;
        *is_ipv6 = true;
      }
      if (inet_ntop(p->ai_family, addr_ptr, ip_str, ip_str_len)) {
        private_ip = true;
        break;
      }
    }
  }
  freeaddrinfo(res);

  if (!private_ip) {
    ESP_LOGE(TAG,
             "Security violation: OTA via HTTP allowed only for private "
             "IPs. Host: %s",
             host);
    return false;
  }
  return true;
}

bool validate_and_rewrite_url(const char* url, char* out_url,
                              size_t out_len) {
  ota_url_parts_t parts = {};
  if (!ota_url_parse(url, &parts)) {
    ESP_LOGE(TAG, "Failed to parse OTA URL");
    return false;
  }

  if (parts.https) {
    if (!ota_url_copy_if_https(url, &parts, out_url, out_len)) {
      ESP_LOGE(TAG, "HTTPS URL is too long for output buffer");
      return false;
    }
    return true;
  }

  char ip_str[INET6_ADDRSTRLEN];
  bool is_ipv6;
  if (!resolve_and_validate_host(&parts, ip_str, sizeof(ip_str),
                                 &is_ipv6)) {
    return false;
  }

  if (!ota_url_rewrite_http_with_ip(&parts, ip_str, is_ipv6, out_url,
                                    out_len)) {
    ESP_LOGE(TAG, "Failed to rewrite OTA URL");
    return false;
  }

  ESP_LOGI(TAG, "Rewritten OTA URL: %s", out_url);
  return true;
}

}  // namespace

bool ota_in_progress(void) { return s_ota_in_progress.load(); }

void run_ota(const char* url) {
  bool expected = false;
  if (!s_ota_in_progress.compare_exchange_strong(expected, true)) {
    ESP_LOGW(TAG, "OTA already in progress, ignoring request");
    diag_event_log("WARN", "ota_busy", 0,
                   "OTA request dropped because update is already running");
    return;
  }

  char final_url[512] = {0};
  if (!validate_and_rewrite_url(url, final_url, sizeof(final_url))) {
    diag_event_log("ERROR", "ota_validate_fail", -1,
                   "OTA URL validation failed");
    s_ota_in_progress.store(false);
    return;
  }

  ESP_LOGI(TAG, "Starting OTA update from URL: %s", final_url);
  diag_event_log("INFO", "ota_start", 0, final_url);

  esp_http_client_config_t http_config = {};
  http_config.url = final_url;
  http_config.crt_bundle_attach = esp_crt_bundle_attach;
  http_config.timeout_ms = 60000;
  http_config.keep_alive_enable = true;
  http_config.save_client_session = true;

  esp_https_ota_config_t ota_config = {};
  ota_config.http_config = &http_config;
#if CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD
  ota_config.partial_http_download = true;
#endif

  gfx_stop();
  vTaskDelay(pdMS_TO_TICKS(100));

  display_clear();
  display_text("OTA Update", 2, 10, 0, 0, 255, 1);
  display_flip();

  display_clear();
  display_text("OTA Update", 2, 10, 0, 0, 255, 1);

  esp_https_ota_handle_t https_ota_handle = nullptr;
  esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ESP HTTPS OTA Begin failed: %s", esp_err_to_name(err));
    diag_event_log("ERROR", "ota_begin_fail", err, esp_err_to_name(err));
    display_clear();
    display_text("OTA Fail", 2, 10, 255, 0, 0, 1);
    display_flip();
    vTaskDelay(pdMS_TO_TICKS(2000));
    s_ota_in_progress.store(false);
    gfx_start();
    return;
  }

  int bar_x = 2;
  int bar_y = 20;
  int bar_w = 60;
  int bar_h = 4;
  int last_progress_width = -1;

  while (true) {
    err = esp_https_ota_perform(https_ota_handle);
    if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
      break;
    }

    int cur_len = esp_https_ota_get_image_len_read(https_ota_handle);
    int total_len = esp_https_ota_get_image_size(https_ota_handle);

    if (total_len > 0) {
      int progress_width = (cur_len * bar_w) / total_len;

      if (progress_width != last_progress_width) {
        display_fill_rect(bar_x, bar_y, bar_w, bar_h, 10, 10, 10);
        if (progress_width > 0) {
          display_fill_rect(bar_x, bar_y, progress_width, bar_h, 0,
                            255, 0);
        }
        display_flip();
        last_progress_width = progress_width;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OTA Update failed: %s", esp_err_to_name(err));
    diag_event_log("ERROR", "ota_perform_fail", err, esp_err_to_name(err));
    esp_https_ota_finish(https_ota_handle);
    display_clear();
    display_text("OTA Fail", 2, 10, 255, 0, 0, 1);
    display_flip();
    vTaskDelay(pdMS_TO_TICKS(2000));
    s_ota_in_progress.store(false);
    gfx_start();
  } else {
    err = esp_https_ota_finish(https_ota_handle);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "OTA Update successful. Rebooting...");
      diag_event_log("INFO", "ota_success", 0, "OTA update successful");
      gfx_safe_restart();
    } else {
      ESP_LOGE(TAG, "OTA Finish failed: %s", esp_err_to_name(err));
      diag_event_log("ERROR", "ota_finish_fail", err, esp_err_to_name(err));
      display_clear();
      display_text("OTA Fail", 2, 10, 255, 0, 0, 1);
      display_flip();
      vTaskDelay(pdMS_TO_TICKS(2000));
      s_ota_in_progress.store(false);
      gfx_start();
    }
  }
}
