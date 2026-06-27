#include "wifi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <lwip/dns.h>
#include <lwip/err.h>
#include <lwip/ip4_addr.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "ap.h"
#include "app_state.h"
#include "diag_event_ring.h"
#include "event_bus.h"
#include "nvs_settings.h"
#include "sdkconfig.h"

namespace {

const char* TAG = "WIFI";

// Event group bits
constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
constexpr EventBits_t WIFI_FAIL_BIT = BIT1;
constexpr EventBits_t WIFI_CONNECTED_IPV6_BIT = BIT2;

constexpr int MAX_RECONNECT_ATTEMPTS = 10;

EventGroupHandle_t s_wifi_event_group = nullptr;
esp_netif_t* s_sta_netif = nullptr;
int s_reconnect_attempts = 0;
bool s_connection_given_up = false;
int s_wifi_disconnect_counter = 0;
uint32_t s_disconnect_events = 0;
uint32_t s_disconnect_streak = 0;
int64_t s_disconnect_window_start_us = 0;
uint32_t s_health_disconnect_checks = 0;

esp_timer_handle_t s_health_timer = nullptr;
constexpr int64_t HEALTH_CHECK_INTERVAL_US = 30000 * 1000;  // 30 seconds
void health_timer_callback(void*) { wifi_health_check(); }

esp_timer_handle_t s_reconnect_timer = nullptr;
constexpr uint32_t RECONNECT_BASE_MS = 1000;
constexpr uint32_t RECONNECT_MAX_MS = 60000;
void reconnect_timer_cb(void*) { esp_wifi_connect(); }

// --- Multi-network state ---------------------------------------------------
// Only engaged when 2+ networks are stored. With 0 or 1 stored networks the
// code below this point behaves exactly as the original single-credential path.
constexpr int MAX_ATTEMPTS_PER_NET = 3;
wifi_network_t s_candidates[MAX_WIFI_NETS] = {};
int s_candidate_count = 0;  // ranked candidates to try this cycle
int s_candidate_idx = 0;    // candidate currently being attempted
int s_per_net_attempts = 0;  // consecutive disconnects against the current one

// Computes a per-attempt reconnect delay with full jitter in [50%, 100%] of
// BASE * 2^(attempt-1), capped at RECONNECT_MAX_MS. Jitter de-synchronizes
// fleets of devices that all lost the same AP simultaneously.
uint32_t reconnect_delay_us_for(int attempt) {
  uint32_t shift = attempt > 0 ? (uint32_t)(attempt - 1) : 0;
  if (shift > 16) shift = 16;  // guard against overflow before the shift
  uint64_t base = (uint64_t)RECONNECT_BASE_MS << shift;
  if (base > RECONNECT_MAX_MS) base = RECONNECT_MAX_MS;
  uint32_t half = (uint32_t)(base / 2);
  uint32_t jittered = half + (esp_random() % (half + 1));
  return jittered * 1000u;  // ms -> us
}

// Schedule a reconnect of the currently-configured credential after a jittered
// backoff (or connect immediately if the timer is unavailable).
void schedule_reconnect(int attempt) {
  uint32_t delay_us = reconnect_delay_us_for(attempt);
  ESP_LOGI(TAG, "Reconnect in %lu ms", (unsigned long)(delay_us / 1000));
  if (s_reconnect_timer) {
    esp_timer_stop(s_reconnect_timer);  // cancel any pending attempt
    esp_timer_start_once(s_reconnect_timer, delay_us);
  } else {
    esp_wifi_connect();
  }
}

// Apply candidate `idx`'s credentials and start connecting. Resets the per-net
// attempt counter. WIFI_CONNECT_AP_BY_SIGNAL picks the strongest BSSID when an
// SSID is served by multiple access points (mesh/roaming).
void connect_to_candidate(int idx) {
  if (idx < 0 || idx >= s_candidate_count) return;
  s_candidate_idx = idx;
  s_per_net_attempts = 0;

  wifi_config_t sta = {};
  memcpy(sta.sta.ssid, s_candidates[idx].ssid, sizeof(sta.sta.ssid));
  memcpy(sta.sta.password, s_candidates[idx].password,
         sizeof(sta.sta.password));
  sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_config failed for '%s': %s", s_candidates[idx].ssid,
             esp_err_to_name(err));
  }
  ESP_LOGI(TAG, "Connecting to candidate %d/%d: %s", idx + 1, s_candidate_count,
           s_candidates[idx].ssid);
  esp_wifi_connect();
}

// Build the ranked candidate list for this connection cycle. Every stored
// network is a candidate; with 2+ stored we run an active scan and rank by live
// RSSI (descending), breaking ties by user priority (ascending). Networks not
// seen in the scan sort last but are still attempted. Returns the count.
int build_candidates() {
  wifi_network_t stored[MAX_WIFI_NETS];
  size_t stored_n = wifi_network_list_get(stored, MAX_WIFI_NETS);
  s_candidate_count = 0;
  s_candidate_idx = 0;
  if (stored_n == 0) return 0;

  for (size_t i = 0; i < stored_n; i++) s_candidates[i] = stored[i];
  s_candidate_count = (int)stored_n;

  if (stored_n < 2) return s_candidate_count;  // single network: no scan

  wifi_scan_config_t scan_cfg = {};
  scan_cfg.show_hidden = true;
  if (esp_wifi_scan_start(&scan_cfg, true) == ESP_OK) {
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num > 40) ap_num = 40;  // cap the transient allocation
    wifi_ap_record_t* recs =
        ap_num ? static_cast<wifi_ap_record_t*>(
                     malloc(ap_num * sizeof(wifi_ap_record_t)))
               : nullptr;
    if (recs) {
      uint16_t got = ap_num;
      esp_wifi_scan_get_ap_records(&got, recs);  // also frees the internal list
      for (int c = 0; c < s_candidate_count; c++) {
        int8_t best = -127;
        bool seen = false;
        for (uint16_t r = 0; r < got; r++) {
          if (strncmp(reinterpret_cast<const char*>(recs[r].ssid),
                      s_candidates[c].ssid, MAX_SSID_LEN) == 0) {
            seen = true;
            if (recs[r].rssi > best) best = recs[r].rssi;
          }
        }
        s_candidates[c].last_rssi = seen ? best : -127;  // unseen ranks last
      }
      free(recs);
    } else {
      esp_wifi_clear_ap_list();  // nothing allocated; release internal list
    }
  } else {
    ESP_LOGW(TAG, "Scan failed; ranking by stored RSSI");
  }

  // Insertion-friendly selection sort: RSSI desc, then priority asc.
  for (int a = 0; a < s_candidate_count; a++) {
    for (int b = a + 1; b < s_candidate_count; b++) {
      bool swap = (s_candidates[b].last_rssi != s_candidates[a].last_rssi)
                      ? (s_candidates[b].last_rssi > s_candidates[a].last_rssi)
                      : (s_candidates[b].priority < s_candidates[a].priority);
      if (swap) {
        wifi_network_t t = s_candidates[a];
        s_candidates[a] = s_candidates[b];
        s_candidates[b] = t;
      }
    }
  }
  ESP_LOGI(TAG, "Ranked %d candidate network(s); best: %s (%d dBm)",
           s_candidate_count, s_candidates[0].ssid, s_candidates[0].last_rssi);
  return s_candidate_count;
}

void handle_successful_ip_acquisition() {
  s_reconnect_attempts = 0;
  s_per_net_attempts = 0;
  if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
  s_connection_given_up = false;
  xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
  xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

  // Record the signal strength of the AP we landed on so the next boot can rank
  // this network by a real measurement rather than a stale/unknown value.
  wifi_ap_record_t ap_info;
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    wifi_network_note_rssi(reinterpret_cast<const char*>(ap_info.ssid),
                           ap_info.rssi);
  }

  event_bus_emit_simple(TRONBYT_EVENT_WIFI_CONNECTED);
  app_state_set_connectivity(CONNECTIVITY_CONNECTED);
}

void wifi_event_handler(void* arg, esp_event_base_t event_base,
                        int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT) {
    switch (event_id) {
      case WIFI_EVENT_STA_START:
        s_reconnect_attempts = 0;
        s_connection_given_up = false;
        // The first connect is driven explicitly from wifi_initialize after
        // candidate ranking; reconnects are driven by the disconnect handler
        // and the reconnect/health timers. So no esp_wifi_connect() here.
        break;
      case WIFI_EVENT_STA_CONNECTED:
        // Only create an IPv6 link-local when the user has opted in.
        // Sending an RS triggers an RA containing RDNSS IPv6 addresses;
        // ESP-IDF stores those in dns[0], overwriting the DHCP IPv4 DNS
        // and causing getaddrinfo() to fail at boot.
        if (config_get().prefer_ipv6) {
          ESP_LOGI(TAG, "Connected to AP, creating IPv6 link local address");
          esp_netif_create_ip6_linklocal(s_sta_netif);
        }
        break;
      case WIFI_EVENT_STA_DISCONNECTED: {
        s_reconnect_attempts++;
        s_disconnect_events++;
        xEventGroupClearBits(s_wifi_event_group,
                             WIFI_CONNECTED_BIT | WIFI_CONNECTED_IPV6_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        event_bus_emit_simple(TRONBYT_EVENT_WIFI_DISCONNECTED);
        app_state_set_connectivity(CONNECTIVITY_NO_WIFI);

        int64_t now_us = esp_timer_get_time();
        if ((now_us - s_disconnect_window_start_us) > 60000000LL) {
          s_disconnect_window_start_us = now_us;
          s_disconnect_streak = 0;
        }
        s_disconnect_streak++;

        diag_event_log("WARN", "wifi_disconnect", s_reconnect_attempts,
                       "Station disconnected");
        if (s_disconnect_streak >= 5) {
          diag_event_log("ERROR", "wifi_disconnect_storm",
                         static_cast<int32_t>(s_disconnect_streak),
                         "Repeated disconnects in 60s window");
        }

        if (s_connection_given_up) {
          // Already gave up this cycle; wait for the portal or a reboot.
        } else if (s_candidate_count > 1) {
          // Multi-network: retry the current candidate a few times, then fail
          // over to the next ranked network.
          s_per_net_attempts++;
          if (s_per_net_attempts < MAX_ATTEMPTS_PER_NET) {
            ESP_LOGI(TAG, "Reconnect '%s' (try %d/%d)",
                     s_candidates[s_candidate_idx].ssid, s_per_net_attempts,
                     MAX_ATTEMPTS_PER_NET);
            schedule_reconnect(s_per_net_attempts);
          } else if (s_candidate_idx + 1 < s_candidate_count) {
            ESP_LOGI(TAG, "Network '%s' unreachable, trying '%s'",
                     s_candidates[s_candidate_idx].ssid,
                     s_candidates[s_candidate_idx + 1].ssid);
            connect_to_candidate(s_candidate_idx + 1);
          } else if (config_get().ap_mode) {
            ESP_LOGW(TAG, "All %d networks exhausted, raising portal",
                     s_candidate_count);
            s_connection_given_up = true;
          } else {
            // No portal fallback: restart the cycle from the strongest network.
            // Each attempt blocks on a real connect timeout, so this is not a
            // busy loop; the health check still force-reboots if it never sticks.
            ESP_LOGW(TAG, "All %d networks exhausted, restarting cycle",
                     s_candidate_count);
            connect_to_candidate(0);
          }
        } else if (config_get().ap_mode &&
                   s_reconnect_attempts >= MAX_RECONNECT_ATTEMPTS) {
          ESP_LOGW(TAG, "Maximum reconnection attempts (%d) reached, giving up",
                   MAX_RECONNECT_ATTEMPTS);
          s_connection_given_up = true;
        } else {
          // Single network (or none ranked yet): original backoff behaviour.
          ESP_LOGI(TAG, "WiFi disconnected (attempt %d)", s_reconnect_attempts);
          schedule_reconnect(s_reconnect_attempts);
        }
        break;
      }
      case WIFI_EVENT_AP_STACONNECTED: {
        auto* event =
            static_cast<wifi_event_ap_staconnected_t*>(event_data);
        ESP_LOGI(TAG, "Station joined, AID=%d", event->aid);
      } break;
      case WIFI_EVENT_AP_STADISCONNECTED: {
        auto* event =
            static_cast<wifi_event_ap_stadisconnected_t*>(event_data);
        ESP_LOGI(TAG, "Station left, AID=%d", event->aid);
      } break;
      default:
        break;
    }
  } else if (event_base == IP_EVENT) {
    switch (event_id) {
      case IP_EVENT_STA_GOT_IP: {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "Got IP address: " IPSTR,
                 IP2STR(&event->ip_info.ip));
        handle_successful_ip_acquisition();
      } break;
      case IP_EVENT_GOT_IP6: {
        auto* event = static_cast<ip_event_got_ip6_t*>(event_data);
        auto* addr =
            reinterpret_cast<ip6_addr_t*>(&event->ip6_info.ip);
        ESP_LOGI(TAG, "Got IPv6 address: " IPV6STR,
                 IPV62STR(event->ip6_info.ip));

        if (ip6_addr_isglobal(addr)) {
          ESP_LOGI(TAG, "IPv6 address acquired");
          xEventGroupSetBits(s_wifi_event_group,
                             WIFI_CONNECTED_IPV6_BIT);
          handle_successful_ip_acquisition();
        } else {
          ESP_LOGI(TAG, "IPv6 address is not global, waiting...");
        }
      } break;
      default:
        break;
    }
  }
}

}  // namespace

int wifi_initialize(const char* ssid, const char* password) {
  ESP_LOGI(TAG, "Initializing WiFi");

  if (!config_get().ap_mode) {
    ESP_LOGI(TAG, "AP mode disabled via settings");
  }

  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  s_sta_netif = esp_netif_create_default_wifi_sta();
  auto settings = config_get();
  if (settings.ap_mode) {
    ap_init_netif();
  }

  wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

  char hostname[MAX_HOSTNAME_LEN + 1];
  snprintf(hostname, sizeof(hostname), "%s", settings.hostname);
  if (strlen(hostname) == 0) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(hostname, sizeof(hostname), CONFIG_BRAND_NAME_LOWER "-%02x%02x%02x",
             mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Generated default hostname: %s", hostname);
    snprintf(settings.hostname, sizeof(settings.hostname), "%s", hostname);
    config_set(&settings);
  }
  wifi_set_hostname(hostname);

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &wifi_event_handler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6,
                                             &wifi_event_handler, nullptr));

  bool has_credentials = (wifi_network_list_count() > 0);

  if (settings.ap_mode) {
    ap_configure();
  } else {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  }
  // Per-network credentials are applied after the post-start scan/ranking below.

  ESP_ERROR_CHECK(esp_wifi_start());
  wifi_apply_power_save();

  int8_t tx_power;
  ESP_ERROR_CHECK(esp_wifi_get_max_tx_power(&tx_power));
  ESP_LOGI(TAG, "Max TX Power (Current): %.2f dBm", tx_power * 0.25f);

#ifdef CONFIG_IDF_TARGET_ESP32S3
  ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(44));
  ESP_ERROR_CHECK(esp_wifi_get_max_tx_power(&tx_power));
  ESP_LOGI(TAG, "Max TX Power (S3 limit applied): %.2f dBm",
           tx_power * 0.25f);
#endif

  if (has_credentials) {
    // Rank stored networks (active scan when 2+ are stored) and connect to the
    // strongest visible candidate. Failover is handled on disconnect.
    if (build_candidates() > 0) {
      connect_to_candidate(0);
    } else {
      ESP_LOGW(TAG, "Stored networks present but none selectable");
    }
  } else {
    if (settings.ap_mode) {
      ESP_LOGI(
          TAG,
          "No valid WiFi credentials available, starting in AP mode only");
    } else {
      ESP_LOGW(
          TAG,
          "No valid WiFi credentials available and AP mode is disabled");
    }
    s_reconnect_attempts = MAX_RECONNECT_ATTEMPTS;
    s_connection_given_up = true;
  }

  // Create health check timer (runs in all modes: WS and HTTP)
  esp_timer_create_args_t health_args = {};
  health_args.callback = health_timer_callback;
  health_args.name = "wifi_health";
  health_args.skip_unhandled_events = true;
  esp_timer_create(&health_args, &s_health_timer);
  esp_timer_start_periodic(s_health_timer, HEALTH_CHECK_INTERVAL_US);

  esp_timer_create_args_t reconnect_args = {};
  reconnect_args.callback = reconnect_timer_cb;
  reconnect_args.name = "wifi_reconnect";
  reconnect_args.skip_unhandled_events = true;
  esp_timer_create(&reconnect_args, &s_reconnect_timer);

  ESP_LOGI(TAG, "WiFi initialized successfully");
  return 0;
}

void wifi_shutdown(void) {
  if (s_health_timer) {
    esp_timer_stop(s_health_timer);
    esp_timer_delete(s_health_timer);
    s_health_timer = nullptr;
  }
  if (s_reconnect_timer) {
    esp_timer_stop(s_reconnect_timer);
    esp_timer_delete(s_reconnect_timer);
    s_reconnect_timer = nullptr;
  }

  ap_stop();
  esp_wifi_stop();
  esp_wifi_deinit();

  esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               &wifi_event_handler);
  esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               &wifi_event_handler);
  esp_event_handler_unregister(IP_EVENT, IP_EVENT_GOT_IP6,
                               &wifi_event_handler);

  if (s_wifi_event_group) {
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = nullptr;
  }
}

int wifi_get_mac(uint8_t mac[6]) {
  esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get MAC address: %s", esp_err_to_name(err));
    return 1;
  }
  return 0;
}

int wifi_get_ssid_str(char* buf, size_t buf_len) {
  if (!buf || buf_len == 0) return 1;
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    snprintf(buf, buf_len, "%s", reinterpret_cast<const char*>(ap.ssid));
    return 0;
  }
  buf[0] = '\0';
  return 1;
}

int wifi_get_ip_str(char* buf, size_t buf_len) {
  if (!s_sta_netif || !buf || buf_len < 16) return 1;
  esp_netif_ip_info_t ip_info;
  esp_err_t err = esp_netif_get_ip_info(s_sta_netif, &ip_info);
  if (err != ESP_OK || ip_info.ip.addr == 0) return 1;
  snprintf(buf, buf_len, IPSTR, IP2STR(&ip_info.ip));
  return 0;
}

int wifi_get_ip6_str(char* buf, size_t buf_len) {
  if (!s_sta_netif || !buf || buf_len < 40) return 1;

  esp_ip6_addr_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
  int count = esp_netif_get_all_ip6(s_sta_netif, addrs);
  if (count <= 0) return 1;

  // Prefer a global address; fall back to link-local
  int best = -1;
  for (int i = 0; i < count; i++) {
    if (ip6_addr_isglobal((ip6_addr_t*)&addrs[i])) {
      best = i;
      break;
    }
    if (best < 0) best = i;
  }
  if (best < 0) return 1;

  snprintf(buf, buf_len, IPV6STR, IPV62STR(addrs[best]));
  return 0;
}

int wifi_set_hostname(const char* hostname) {
  if (s_sta_netif) {
    esp_err_t err = esp_netif_set_hostname(s_sta_netif, hostname);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "Hostname set to: %s", hostname);
      return 0;
    }
    ESP_LOGE(TAG, "Failed to set hostname: %s", esp_err_to_name(err));
  }
  return 1;
}

bool wifi_is_connected(void) {
  return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

void wifi_enable_ipv6(void) {
  if (!s_sta_netif) return;
  if (!wifi_is_connected()) return;
  ESP_LOGI(TAG, "Enabling IPv6 link-local address at runtime");
  esp_netif_create_ip6_linklocal(s_sta_netif);
}

bool wifi_wait_for_connection(uint32_t timeout_ms) {
  ESP_LOGI(TAG, "Waiting for WiFi connection (timeout: %lu ms)",
           static_cast<unsigned long>(timeout_ms));

  if (wifi_is_connected()) {
    ESP_LOGI(TAG, "Already connected to WiFi");
    return true;
  }

  if (wifi_network_list_count() == 0) {
    ESP_LOGI(TAG, "No saved networks, won't connect.");
    return false;
  }

  TickType_t start_ticks = xTaskGetTickCount();
  TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

  while (xTaskGetTickCount() - start_ticks < timeout_ticks) {
    if (wifi_is_connected()) {
      ESP_LOGI(TAG, "Connected to WiFi");
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  ESP_LOGW(TAG, "WiFi connection timeout");
  return false;
}

bool wifi_wait_for_ipv6(uint32_t timeout_ms) {
  if (!s_wifi_event_group) return false;

  if (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_IPV6_BIT) {
    return true;
  }

  ESP_LOGI(TAG, "Waiting for IPv6 address (timeout: %lu ms)",
           static_cast<unsigned long>(timeout_ms));
  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group, WIFI_CONNECTED_IPV6_BIT, pdFALSE, pdTRUE,
      pdMS_TO_TICKS(timeout_ms));

  if (bits & WIFI_CONNECTED_IPV6_BIT) {
    return true;
  }

  ESP_LOGI(TAG, "IPv6 address wait timeout");
  return false;
}

void wifi_health_check(void) {
  if (wifi_is_connected()) {
    if (s_wifi_disconnect_counter > 0) {
      s_wifi_disconnect_counter = 0;
    }
    s_connection_given_up = false;
    s_reconnect_attempts = 0;
    return;
  }

  s_wifi_disconnect_counter++;
  s_health_disconnect_checks++;
  ESP_LOGW(TAG, "WiFi Health check. Disconnect count: %d",
           s_wifi_disconnect_counter);

  if (s_wifi_disconnect_counter >= 10) {
    ESP_LOGE(TAG, "WiFi disconnect count reached %d - rebooting",
             s_wifi_disconnect_counter);
    diag_event_log("ERROR", "wifi_health_reboot", s_wifi_disconnect_counter,
                   "WiFi health check forced reboot");
    esp_restart();
  }

  if (wifi_network_list_count() > 0) {
    ESP_LOGI(TAG, "Reconnecting in Health check...");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "WiFi reconnect attempt failed: %s",
               esp_err_to_name(err));
    }
  } else {
    ESP_LOGW(TAG, "No networks configured, cannot reconnect");
  }
}

void wifi_get_diag_stats(wifi_diag_stats_t* out) {
  if (!out) return;
  out->connected = wifi_is_connected();
  out->connection_given_up = s_connection_given_up;
  out->reconnect_attempts = s_reconnect_attempts;
  out->disconnect_events = s_disconnect_events;
  out->health_disconnect_checks = s_health_disconnect_checks;
}

void wifi_apply_power_save(void) {
  wifi_ps_type_t power_save_mode = config_get().wifi_power_save;
  ESP_LOGI(TAG, "Setting WiFi Power Save Mode to %d...", power_save_mode);
  esp_wifi_set_ps(power_save_mode);
}
