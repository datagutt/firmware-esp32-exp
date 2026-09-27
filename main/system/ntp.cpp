#include "ntp.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "embedded_tz_db.h"
#include "event_bus.h"
#include "http_slot.h"
#include "raii_utils.hpp"

namespace {

const char* TAG = "ntp";
constexpr const char* NVS_NAMESPACE = "ntp_cfg";

// Timezone fetch from IP geolocation API
constexpr const char* TZ_FETCH_URL = "http://ip-api.com/json";
constexpr size_t TZ_RESPONSE_BUFFER_SIZE = 512;
// 8192: the fetch runs esp_http_client plus JSON parsing in this task and
// 4096 overflowed in the field (see kd_common 040f31d).
constexpr size_t TZ_FETCH_TASK_STACK = 8192;
constexpr int TZ_FETCH_TASK_PRIORITY = 5;
constexpr int TZ_FETCH_MAX_RETRIES = 2;
constexpr int TZ_FETCH_RETRY_DELAY_MS = 3000;

bool s_initialized = false;
std::atomic<bool> s_synced{false};
std::atomic<bool> s_wifi_up{false};
std::atomic<bool> s_tz_fetch_in_progress{false};
// The geolocated zone does not change while the device stays put, so one
// successful lookup per boot is enough; reconnects reuse it.
std::atomic<bool> s_tz_resolved{false};

// Guards s_config, which the event bus task, the TZ fetch task and the HTTP
// API all touch. Created in ntp_init; before that everything runs on one task
// and MutexGuard treats the null handle as unlocked.
SemaphoreHandle_t s_mutex = nullptr;
ntp_config_t s_config = {
    .auto_timezone = true,
    .fetch_tz_on_boot = true,
    .timezone = "UTC",
    .ntp_server = "pool.ntp.org",
};

// ── NVS persistence ────────────────────────────────────────────────

void load_config_from_nvs() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
  if (err != ESP_OK) {
    ESP_LOGI(TAG, "NVS namespace not found, using defaults");
    return;
  }

  size_t sz = sizeof(ntp_config_t);
  err = nvs_get_blob(h, "config", &s_config, &sz);
  if (err != ESP_OK) {
    ESP_LOGI(TAG, "Config not found in NVS, using defaults");
  } else {
    ESP_LOGI(TAG, "Loaded config: auto_tz=%d, fetch_on_boot=%d, tz=%s, ntp=%s",
             s_config.auto_timezone, s_config.fetch_tz_on_boot,
             s_config.timezone, s_config.ntp_server);
  }

  nvs_close(h);
}

// Caller holds s_mutex, which keeps concurrent saves in order. Callers only
// save after an actual change.
void save_config_locked() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return;
  }

  err = nvs_set_blob(h, "config", &s_config, sizeof(ntp_config_t));
  if (err == ESP_OK) {
    nvs_commit(h);
    ESP_LOGI(TAG, "Config saved to NVS");
  } else {
    ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(err));
  }

  nvs_close(h);
}

// ── Timezone helpers ───────────────────────────────────────────────

// Caller holds s_mutex.
void apply_timezone_locked() {
  const char* posix = tz_db_get_posix_str(s_config.timezone);
  if (!posix) posix = "UTC0";
  setenv("TZ", posix, 1);
  tzset();
}

// Applies a geolocated zone unless the user switched to a manual zone while
// the lookup was running.
void apply_fetched_timezone(const char* name) {
  if (!name || name[0] == '\0') return;

  raii::MutexGuard lock(s_mutex);
  if (!s_config.auto_timezone) return;
  if (strcmp(s_config.timezone, name) == 0) return;

  ESP_LOGI(TAG, "Applying timezone: %s", name);
  snprintf(s_config.timezone, sizeof(s_config.timezone), "%s", name);
  save_config_locked();
  apply_timezone_locked();
}

// ── Timezone fetch from IP geolocation ─────────────────────────────

struct tz_response_buffer_t {
  char data[TZ_RESPONSE_BUFFER_SIZE];
  size_t len;
};

esp_err_t tz_http_event_handler(esp_http_client_event_t* evt) {
  if (!evt || !evt->user_data) return ESP_OK;
  auto* buf = static_cast<tz_response_buffer_t*>(evt->user_data);

  switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
      buf->len = 0;
      buf->data[0] = '\0';
      break;
    case HTTP_EVENT_ON_DATA:
      if (evt->data && evt->data_len > 0) {
        size_t avail = sizeof(buf->data) - buf->len - 1;
        size_t copy = (avail < static_cast<size_t>(evt->data_len))
                          ? avail
                          : static_cast<size_t>(evt->data_len);
        if (copy > 0) {
          memcpy(buf->data + buf->len, evt->data, copy);
          buf->len += copy;
          buf->data[buf->len] = '\0';
        }
      }
      break;
    default:
      break;
  }
  return ESP_OK;
}

bool fetch_timezone_from_api() {
  tz_response_buffer_t response{};

  esp_http_client_config_t cfg{};
  cfg.url = TZ_FETCH_URL;
  cfg.event_handler = tz_http_event_handler;
  cfg.user_data = &response;
  cfg.timeout_ms = 5000;

  // This fetch runs at boot, concurrently with the scheduler's first (TLS)
  // image fetch, exactly when the internal heap is tightest. Serialize it
  // behind the shared slot so it does not add a second concurrent connection
  // during that window. It is plain HTTP today, but the slot also future-proofs
  // a switch to an HTTPS geolocation endpoint. On contention just skip; the
  // caller's retry loop tries again shortly.
  constexpr uint32_t kTzSlotWaitMs = 8000;
  http_slot::Guard slot("ntp_tz", kTzSlotWaitMs);
  if (!slot) {
    ESP_LOGW(TAG, "HTTP slot busy, deferring TZ fetch");
    return false;
  }

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    ESP_LOGW(TAG, "Failed to init HTTP client for TZ fetch");
    return false;
  }

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK || status != 200) {
    ESP_LOGW(TAG, "TZ fetch failed (err=%s, status=%d)", esp_err_to_name(err),
             status);
    return false;
  }

  cJSON* root = cJSON_Parse(response.data);
  if (!root) {
    ESP_LOGW(TAG, "Failed to parse TZ API response");
    return false;
  }

  cJSON* st = cJSON_GetObjectItem(root, "status");
  if (!st || !cJSON_IsString(st) ||
      strcmp(st->valuestring, "success") != 0) {
    ESP_LOGW(TAG, "TZ API returned non-success status");
    cJSON_Delete(root);
    return false;
  }

  cJSON* tz = cJSON_GetObjectItem(root, "timezone");
  if (!tz || !cJSON_IsString(tz) || !tz->valuestring ||
      !tz->valuestring[0]) {
    ESP_LOGW(TAG, "TZ API response missing timezone field");
    cJSON_Delete(root);
    return false;
  }

  ESP_LOGI(TAG, "Fetched timezone from IP geolocation: %s",
           tz->valuestring);
  apply_fetched_timezone(tz->valuestring);
  s_tz_resolved.store(true);

  cJSON_Delete(root);
  return true;
}

void tz_fetch_task(void* /*arg*/) {
  for (int attempt = 0; attempt <= TZ_FETCH_MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      ESP_LOGI(TAG, "TZ fetch retry %d/%d", attempt, TZ_FETCH_MAX_RETRIES);
      vTaskDelay(pdMS_TO_TICKS(TZ_FETCH_RETRY_DELAY_MS));
    }
    if (fetch_timezone_from_api()) break;
  }

  s_tz_fetch_in_progress.store(false);
  vTaskDelete(nullptr);
}

void spawn_tz_fetch_task() {
  bool expected = false;
  if (!s_tz_fetch_in_progress.compare_exchange_strong(expected, true)) {
    ESP_LOGD(TAG, "TZ fetch already in progress");
    return;
  }

  if (xTaskCreate(tz_fetch_task, "tz_fetch", TZ_FETCH_TASK_STACK, nullptr,
                  TZ_FETCH_TASK_PRIORITY, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create TZ fetch task");
    s_tz_fetch_in_progress.store(false);
  }
}

// Starts a geolocation lookup when auto timezone wants one and it has not
// already succeeded this boot.
void maybe_fetch_timezone() {
  bool wanted;
  {
    raii::MutexGuard lock(s_mutex);
    wanted = s_config.auto_timezone && s_config.fetch_tz_on_boot;
  }
  if (wanted && s_wifi_up.load() && !s_tz_resolved.load()) {
    spawn_tz_fetch_task();
  }
}

// ── SNTP ───────────────────────────────────────────────────────────

void time_sync_callback(struct timeval* tv) {
  s_synced = true;

  time_t now = tv->tv_sec;
  struct tm* info = localtime(&now);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", info);
  ESP_LOGI(TAG, "Time synchronized: %s", buf);

  // Let time-dependent subsystems (e.g. quiet hours) re-evaluate against a
  // now-valid wall clock instead of waiting for their own periodic tick.
  event_bus_emit_simple(TRONBYT_EVENT_TIME_SYNCED);
}

// Caller holds s_mutex. SNTP keeps the server name pointer, so it must point
// at s_config, which outlives the SNTP client.
void start_sntp_locked() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  ESP_LOGI(TAG, "Starting SNTP with server: %s", s_config.ntp_server);

  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, s_config.ntp_server);
  esp_sntp_setservername(1, "time.google.com");
  esp_sntp_setservername(2, "time.cloudflare.com");
  esp_sntp_set_time_sync_notification_cb(time_sync_callback);
  esp_sntp_set_sync_interval(3600 * 1000);
  esp_sntp_init();
}

// ── Event bus handler ─────────────────────────────────────────────

void on_wifi_event(const tronbyt_event_t* event, void*) {
  if (event->type == TRONBYT_EVENT_WIFI_CONNECTED) {
    s_wifi_up.store(true);
    {
      raii::MutexGuard lock(s_mutex);
      apply_timezone_locked();
      start_sntp_locked();
    }
    maybe_fetch_timezone();
  } else if (event->type == TRONBYT_EVENT_WIFI_DISCONNECTED) {
    s_wifi_up.store(false);
    s_synced = false;
  }
}

}  // namespace

// ── Public API ─────────────────────────────────────────────────────

void ntp_init() {
  if (s_initialized) return;
  s_initialized = true;

  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) {
    ESP_LOGE(TAG, "Failed to create NTP config mutex");
  }

  load_config_from_nvs();
  {
    raii::MutexGuard lock(s_mutex);
    apply_timezone_locked();
  }

  event_bus_subscribe(TRONBYT_EVENT_WIFI_CONNECTED, on_wifi_event, nullptr);
  event_bus_subscribe(TRONBYT_EVENT_WIFI_DISCONNECTED, on_wifi_event, nullptr);

  ESP_LOGI(TAG, "NTP initialized (tz_db version: %s)", tz_db_get_version());
}

bool ntp_is_synced() { return s_synced; }

void ntp_sync() {
  if (esp_sntp_enabled()) {
    esp_sntp_restart();
  } else {
    raii::MutexGuard lock(s_mutex);
    start_sntp_locked();
  }
}

ntp_config_t ntp_get_config() {
  raii::MutexGuard lock(s_mutex);
  return s_config;
}

void ntp_set_config(const ntp_config_t* config) {
  if (!config) return;

  {
    raii::MutexGuard lock(s_mutex);
    if (memcmp(&s_config, config, sizeof(s_config)) == 0) return;
    s_config = *config;
    save_config_locked();
    apply_timezone_locked();
    if (esp_sntp_enabled()) {
      start_sntp_locked();
    }
  }
  s_tz_resolved.store(false);
  maybe_fetch_timezone();
}

void ntp_set_auto_timezone(bool enabled) {
  {
    raii::MutexGuard lock(s_mutex);
    if (s_config.auto_timezone == enabled) return;
    s_config.auto_timezone = enabled;
    if (s_initialized) save_config_locked();
  }
  if (enabled) {
    s_tz_resolved.store(false);
    maybe_fetch_timezone();
  }
}

bool ntp_get_auto_timezone() {
  raii::MutexGuard lock(s_mutex);
  return s_config.auto_timezone;
}

void ntp_set_fetch_tz_on_boot(bool enabled) {
  raii::MutexGuard lock(s_mutex);
  if (s_config.fetch_tz_on_boot == enabled) return;
  s_config.fetch_tz_on_boot = enabled;
  if (s_initialized) save_config_locked();
}

bool ntp_get_fetch_tz_on_boot() {
  raii::MutexGuard lock(s_mutex);
  return s_config.fetch_tz_on_boot;
}

void ntp_set_timezone(const char* timezone) {
  if (!timezone) return;

  raii::MutexGuard lock(s_mutex);
  if (!s_config.auto_timezone &&
      strncmp(s_config.timezone, timezone, sizeof(s_config.timezone) - 1) ==
          0) {
    return;
  }
  snprintf(s_config.timezone, sizeof(s_config.timezone), "%s", timezone);
  s_config.auto_timezone = false;

  save_config_locked();
  apply_timezone_locked();
}

void ntp_get_timezone(char* out, size_t len) {
  if (!out || len == 0) return;
  raii::MutexGuard lock(s_mutex);
  snprintf(out, len, "%s", s_config.timezone);
}

void ntp_set_server(const char* server) {
  if (!server) return;

  raii::MutexGuard lock(s_mutex);
  if (strncmp(s_config.ntp_server, server, sizeof(s_config.ntp_server) - 1) ==
      0) {
    return;
  }
  snprintf(s_config.ntp_server, sizeof(s_config.ntp_server), "%s", server);

  save_config_locked();

  if (esp_sntp_enabled()) {
    start_sntp_locked();
  }
}

void ntp_get_server(char* out, size_t len) {
  if (!out || len == 0) return;
  raii::MutexGuard lock(s_mutex);
  snprintf(out, len, "%s", s_config.ntp_server);
}
