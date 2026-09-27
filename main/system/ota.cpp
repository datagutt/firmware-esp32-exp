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
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include "app_state.h"
#include "display.h"
#include "diag_event_ring.h"
#include "event_bus.h"
#include "http_slot.h"
#include "ota_url_utils.h"
#include "raii_utils.hpp"
#include "scheduler.h"
#include "webp_player.h"

namespace {

const char* TAG = "OTA";
std::atomic<bool> s_ota_in_progress{false};
// ota_claim() moved the app state to OTA and ota_release() must move it back.
// Not set when the claim came from the config portal, which has to stay there.
bool s_claim_entered_ota_state = false;

// Post-update health check. A new image that never proves itself is rolled
// back, so a bad release cannot strand a device that updates unattended.
constexpr uint64_t kHealthTimeoutUs = 10ULL * 60 * 1000 * 1000;
std::atomic<bool> s_health_pending{false};
bool s_health_needs_server = false;
esp_timer_handle_t s_confirm_timer = nullptr;
esp_timer_handle_t s_rollback_timer = nullptr;
// Serializes the otadata writes that end a trial: the confirm and rollback
// timers run on the esp_timer task, ota_confirm_for_update() on the task that
// is about to write an update.
SemaphoreHandle_t s_health_lock = nullptr;

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

// Pending verify only on the first boot of a new OTA image. Factory and
// USB-flashed images report valid or not-supported, so they are left alone.
bool running_app_pending_verify() {
  esp_ota_img_states_t state;
  return esp_ota_get_state_partition(esp_ota_get_running_partition(),
                                     &state) == ESP_OK &&
         state == ESP_OTA_IMG_PENDING_VERIFY;
}

// Ends the trial. Caller holds s_health_lock.
esp_err_t confirm_running_app(const char* reason) {
  s_health_pending.store(false);
  if (s_rollback_timer) esp_timer_stop(s_rollback_timer);
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "OTA image confirmed valid (%s); rollback cancelled",
             reason);
    diag_event_log("INFO", "ota_confirmed", 0, reason);
  } else {
    ESP_LOGE(TAG, "Failed to mark OTA app valid: %s", esp_err_to_name(err));
    diag_event_log("ERROR", "ota_confirm_fail", err, "mark_app_valid failed");
  }
  return err;
}

// Both timer callbacks write otadata, so they run on the esp_timer task
// (internal stack) and never on the event bus task, whose stack may be PSRAM.
void confirm_timer_cb(void*) {
  raii::MutexGuard lock(s_health_lock);
  if (!s_health_pending.load()) return;
  confirm_running_app("App marked valid after boot");
}

void rollback_timer_cb(void*) {
  raii::MutexGuard lock(s_health_lock);
  if (!s_health_pending.load()) return;
  ESP_LOGE(TAG, "New firmware not confirmed healthy within %llu s; rolling back",
           kHealthTimeoutUs / 1000000ULL);
  diag_event_log("ERROR", "ota_rollback", 0,
                 "Health check timed out; rolling back");
  esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
  // Only returns on failure. Keep running the new image rather than retrying
  // a rollback that cannot happen.
  ESP_LOGE(TAG, "Rollback failed: %s", esp_err_to_name(err));
  diag_event_log("ERROR", "ota_rollback_fail", err, esp_err_to_name(err));
}

// With a server configured, the new image must reach it: a WebSocket session,
// or any HTTP status from a poll (a 4xx is a server-side problem that a
// rollback would not fix). Without one there is nothing to reach yet, so an IP
// on WiFi is enough; otherwise a device updated through the web UI before it
// was pointed at a server would roll back on every such update. The config
// portal counts too: someone is setting the device up, and it is also the path
// to upload another image.
void on_health_event(const tronbyt_event_t* event, void*) {
  if (!event || !s_health_pending.load()) return;
  bool healthy = false;
  switch (event->type) {
    case TRONBYT_EVENT_WS_CONNECTED:
    case TRONBYT_EVENT_SERVER_RESPONDED:
      healthy = true;
      break;
    case TRONBYT_EVENT_WIFI_CONNECTED:
      healthy = !s_health_needs_server;
      break;
    case TRONBYT_EVENT_STATE_CHANGED:
      healthy = event->payload.i32 == APP_STATE_CONFIG_PORTAL;
      break;
    default:
      break;
  }
  if (healthy && s_confirm_timer) esp_timer_start_once(s_confirm_timer, 0);
}

void show_ota_screen() {
  display_clear();
  display_text("OTA Update", 2, 10, 0, 0, 255, 1);
  display_flip();
}

// The failure paths of run_ota(). Leaves the claim and resumes playback.
void fail_update() {
  app_state_set_ota_substate(OTA_SUBSTATE_FAILED);
  app_state_enter_normal();
  display_clear();
  display_text("OTA Fail", 2, 10, 255, 0, 0, 1);
  display_flip();
  vTaskDelay(pdMS_TO_TICKS(2000));
  s_ota_in_progress.store(false);
  scheduler_resume(SCHEDULER_PAUSE_OTA);
}

}  // namespace

bool ota_in_progress(void) { return s_ota_in_progress.load(); }

bool ota_claim(void) {
  bool expected = false;
  if (!s_ota_in_progress.compare_exchange_strong(expected, true)) {
    ESP_LOGW(TAG, "OTA already in progress");
    return false;
  }
  s_claim_entered_ota_state = app_state_enter_ota() == ESP_OK;
  scheduler_pause(SCHEDULER_PAUSE_OTA);
  show_ota_screen();
  return true;
}

void ota_release(void) {
  if (s_claim_entered_ota_state) {
    app_state_enter_normal();
    s_claim_entered_ota_state = false;
  }
  s_ota_in_progress.store(false);
  scheduler_resume(SCHEDULER_PAUSE_OTA);
}

void ota_start_health_check(bool server_configured) {
  if (s_confirm_timer || !running_app_pending_verify()) return;

  s_health_needs_server = server_configured;

  s_health_lock = xSemaphoreCreateMutex();
  if (!s_health_lock) {
    ESP_LOGE(TAG, "Failed to create OTA health check lock");
    return;
  }

  esp_timer_create_args_t confirm_args = {};
  confirm_args.callback = confirm_timer_cb;
  confirm_args.name = "ota_confirm";
  if (esp_timer_create(&confirm_args, &s_confirm_timer) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create OTA confirm timer");
    return;
  }

  // Without another valid image there is nothing to roll back to; still
  // confirm once healthy so the bootloader stops treating this boot as a trial.
  if (esp_ota_check_rollback_is_possible()) {
    esp_timer_create_args_t rollback_args = {};
    rollback_args.callback = rollback_timer_cb;
    rollback_args.name = "ota_rollback";
    if (esp_timer_create(&rollback_args, &s_rollback_timer) == ESP_OK) {
      esp_timer_start_once(s_rollback_timer, kHealthTimeoutUs);
    }
  } else {
    ESP_LOGW(TAG, "No previous image to roll back to");
  }

  s_health_pending.store(true);
  event_bus_subscribe(TRONBYT_EVENT_WS_CONNECTED, on_health_event, nullptr);
  event_bus_subscribe(TRONBYT_EVENT_SERVER_RESPONDED, on_health_event,
                      nullptr);
  event_bus_subscribe(TRONBYT_EVENT_WIFI_CONNECTED, on_health_event, nullptr);
  event_bus_subscribe(TRONBYT_EVENT_STATE_CHANGED, on_health_event, nullptr);

  ESP_LOGW(TAG,
           "First boot of new firmware: confirming once %s, rolling back "
           "after %llu s otherwise",
           server_configured ? "the server is reached" : "WiFi connects",
           kHealthTimeoutUs / 1000000ULL);
}

esp_err_t ota_confirm_for_update(void) {
  // Without a health check there is no lock, and nothing else writes otadata.
  raii::MutexGuard lock(s_health_lock);
  if (!running_app_pending_verify()) return ESP_OK;
  ESP_LOGW(TAG, "Running image is still on trial; confirming it for an update");
  return confirm_running_app("Confirmed to install an update");
}

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

  app_state_enter_ota();
  app_state_set_ota_substate(OTA_SUBSTATE_FLASHING);
  event_bus_emit_simple(TRONBYT_EVENT_OTA_STARTED);

  esp_http_client_config_t http_config = {};
  http_config.url = final_url;
  http_config.crt_bundle_attach = esp_crt_bundle_attach;
  http_config.timeout_ms = 60000;
  http_config.keep_alive_enable = true;
  http_config.save_client_session = true;
  // 6KB: larger reads cut per-chunk overhead across a multi-megabyte image.
  http_config.buffer_size = 6 * 1024;

  esp_https_ota_config_t ota_config = {};
  ota_config.http_config = &http_config;
#if CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD
  ota_config.partial_http_download = true;
#endif

  // Through the scheduler rather than gfx_stop() alone, so nothing queues
  // images or restarts the player (quiet hours ending) under the update. It
  // also waits for the player to go idle before the OTA screen is drawn.
  scheduler_pause(SCHEDULER_PAUSE_OTA);
  show_ota_screen();

  // The other buffer needs the text too: each progress update flips.
  display_clear();
  display_text("OTA Update", 2, 10, 0, 0, 255, 1);

  // Hold the shared TLS slot for the whole download so the update's handshake
  // (and its retries) never collide with another client's. s_ota_in_progress
  // is already set above, so the poll path yields the slot to us on sight; the
  // only wait we can incur is a single in-flight image fetch draining, hence
  // the generous timeout (longer than remote_get's per-attempt HTTP timeout).
  constexpr uint32_t kOtaSlotWaitMs = 30000;
  http_slot::Guard slot("ota", kOtaSlotWaitMs);
  if (!slot) {
    ESP_LOGE(TAG, "Could not acquire HTTP slot for OTA; aborting update");
    diag_event_log("ERROR", "ota_slot_busy", -1,
                   "OTA aborted: shared HTTP slot stayed busy");
    fail_update();
    return;
  }

  // The URL came from the server, so the running image has reached it. The
  // health check's confirm event can still be queued behind this task, and
  // esp_https_ota_begin() would refuse until it lands.
  if (ota_confirm_for_update() != ESP_OK) {
    fail_update();
    return;
  }

  // A transfer can drop mid-stream (transient TLS/transport errors, a proxy
  // closing the connection). Retry the download in place so a brief glitch
  // does not fail the whole update until the next push. Validation failures
  // are deterministic (wrong artifact), so retrying those only burns a flash
  // erase cycle. esp_https_ota_begin re-erases the target partition, so each
  // retry starts from a clean slate.
  constexpr int kOtaDownloadAttempts = 3;
  constexpr int kOtaDownloadRetryMs = 15000;

  int bar_x = 2;
  int bar_y = 20;
  int bar_w = 60;
  int bar_h = 4;

  esp_https_ota_handle_t https_ota_handle = nullptr;
  esp_err_t err = ESP_FAIL;
  for (int attempt = 1; attempt <= kOtaDownloadAttempts; attempt++) {
    https_ota_handle = nullptr;
    err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err == ESP_OK) {
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

      // esp_https_ota_finish() only switches the boot partition when the
      // whole image was received, but returns ESP_OK either way. With
      // partial_http_download that means Content-Length must match the bytes
      // read: a reverse proxy that compresses the response (chunked, no
      // length) would otherwise end in "successful", a reboot and the old
      // firmware. Retrying cannot fix a server that does this.
      if (err == ESP_OK &&
          !esp_https_ota_is_complete_data_received(https_ota_handle)) {
        ESP_LOGE(TAG,
                 "Incomplete OTA image: read %d bytes, server announced %d. "
                 "The server must send Content-Length and honour Range "
                 "requests (check for compression in a reverse proxy)",
                 esp_https_ota_get_image_len_read(https_ota_handle),
                 esp_https_ota_get_image_size(https_ota_handle));
        esp_https_ota_abort(https_ota_handle);
        https_ota_handle = nullptr;
        err = ESP_ERR_INVALID_SIZE;
        break;
      }
      if (err == ESP_OK) {
        break;
      }
      esp_https_ota_abort(https_ota_handle);
      https_ota_handle = nullptr;
    } else {
      ESP_LOGE(TAG, "ESP HTTPS OTA Begin failed: %s", esp_err_to_name(err));
    }

    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
      break;
    }
    if (attempt < kOtaDownloadAttempts) {
      ESP_LOGW(TAG, "OTA download attempt %d/%d failed (%s), retrying in %d ms",
               attempt, kOtaDownloadAttempts, esp_err_to_name(err),
               kOtaDownloadRetryMs);
      diag_event_log("WARN", "ota_retry", err, esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(kOtaDownloadRetryMs));
    }
  }

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OTA Update failed: %s", esp_err_to_name(err));
    diag_event_log("ERROR", "ota_perform_fail", err, esp_err_to_name(err));
    fail_update();
  } else {
    app_state_set_ota_substate(OTA_SUBSTATE_VERIFYING);
    err = esp_https_ota_finish(https_ota_handle);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "OTA Update successful. Rebooting...");
      diag_event_log("INFO", "ota_success", 0, "OTA update successful");
      app_state_set_ota_substate(OTA_SUBSTATE_PENDING_REBOOT);
      event_bus_emit_simple(TRONBYT_EVENT_OTA_COMPLETE);
      gfx_safe_restart();
    } else {
      ESP_LOGE(TAG, "OTA Finish failed: %s", esp_err_to_name(err));
      diag_event_log("ERROR", "ota_finish_fail", err, esp_err_to_name(err));
      fail_update();
    }
  }
}
