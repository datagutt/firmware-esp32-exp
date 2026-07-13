#include "quiet_hours.h"

#include <atomic>
#include <cstring>
#include <ctime>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>

#include "event_bus.h"
#include "ntp.h"

namespace {

const char* TAG = "quiet_hours";

constexpr const char* NVS_NAMESPACE = "quiet_hours";
constexpr const char* NVS_KEY = "windows";

// How often the evaluator re-checks the wall clock against the windows. A
// half-minute cadence keeps the enter/exit edges within one tick of the minute
// boundary the windows are defined on, without meaningful power cost.
constexpr int64_t EVAL_PERIOD_US = 30 * 1000 * 1000;

// bit0 = Sunday .. bit6 = Saturday.
constexpr uint8_t ALL_DAYS_MASK = 0x7F;

// Fixed-layout blob persisted in NVS. Keeping a plain count + array keeps the
// on-flash size stable regardless of how many windows are in use.
struct QuietHoursBlob {
  uint8_t count;
  quiet_window_t windows[QUIET_HOURS_MAX_WINDOWS];
};

quiet_window_t s_windows[QUIET_HOURS_MAX_WINDOWS] = {};
size_t s_count = 0;
std::atomic<bool> s_active{false};
// Server-driven quiet signal (Tronbyt-Quiet header). OR-combined with the local
// window evaluation so either source can blank the display.
std::atomic<bool> s_remote_active{false};
SemaphoreHandle_t s_mutex = nullptr;
esp_timer_handle_t s_timer = nullptr;

void load_from_nvs() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
    ESP_LOGI(TAG, "No stored windows, quiet hours disabled");
    return;
  }

  QuietHoursBlob blob = {};
  size_t sz = sizeof(blob);
  esp_err_t err = nvs_get_blob(h, NVS_KEY, &blob, &sz);
  nvs_close(h);

  if (err != ESP_OK || sz != sizeof(blob)) {
    ESP_LOGI(TAG, "No valid window blob, quiet hours disabled");
    return;
  }

  s_count = blob.count > QUIET_HOURS_MAX_WINDOWS ? QUIET_HOURS_MAX_WINDOWS
                                                 : blob.count;
  memcpy(s_windows, blob.windows, sizeof(s_windows));
  ESP_LOGI(TAG, "Loaded %u quiet window(s)", (unsigned)s_count);
}

// Caller holds s_mutex.
void persist_locked() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS for write");
    return;
  }

  QuietHoursBlob blob = {};
  blob.count = static_cast<uint8_t>(s_count);
  memcpy(blob.windows, s_windows, sizeof(blob.windows));

  if (nvs_set_blob(h, NVS_KEY, &blob, sizeof(blob)) == ESP_OK) {
    nvs_commit(h);
  } else {
    ESP_LOGE(TAG, "Failed to persist windows");
  }
  nvs_close(h);
}

void eval_timer_cb(void*) { quiet_hours_reevaluate(); }

}  // namespace

void quiet_hours_reevaluate(void) {
  bool local_active = false;

  // Fail open until the clock is real. Without a synced clock the local time is
  // meaningless and could blank the display at the wrong moment.
  if (ntp_is_synced()) {
    time_t now = time(nullptr);
    struct tm local = {};
    localtime_r(&now, &local);

    quiet_window_t snapshot[QUIET_HOURS_MAX_WINDOWS];
    size_t count = 0;
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
      count = s_count;
      memcpy(snapshot, s_windows, sizeof(snapshot));
      xSemaphoreGive(s_mutex);
    }
    local_active = quiet_hours_any_active(snapshot, count, &local);
  }

  // The server signal does not depend on the local clock, so it counts even
  // before NTP sync.
  bool active = local_active || s_remote_active.load();

  bool prev = s_active.exchange(active);
  if (prev != active) {
    ESP_LOGI(TAG, "Quiet hours %s", active ? "ENTER" : "EXIT");
    // The display-power events are the single decision point: the scheduler
    // subscribes and pauses/blanks (off) or resumes playback (on).
    event_bus_emit_simple(active ? TRONBYT_EVENT_DISPLAY_OFF
                                 : TRONBYT_EVENT_DISPLAY_ON);
  }
}

void quiet_hours_set_remote_active(bool active) {
  s_remote_active.store(active);
  quiet_hours_reevaluate();
}

bool quiet_hours_is_active(void) { return s_active.load(); }

size_t quiet_hours_get_windows(quiet_window_t* out, size_t max) {
  if (!out || max == 0) return 0;
  size_t n = 0;
  if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
    n = s_count < max ? s_count : max;
    memcpy(out, s_windows, n * sizeof(quiet_window_t));
    xSemaphoreGive(s_mutex);
  }
  return n;
}

void quiet_hours_set_windows(const quiet_window_t* windows, size_t count) {
  if (count > QUIET_HOURS_MAX_WINDOWS) count = QUIET_HOURS_MAX_WINDOWS;
  if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
    memset(s_windows, 0, sizeof(s_windows));
    if (windows && count > 0) {
      memcpy(s_windows, windows, count * sizeof(quiet_window_t));
    }
    s_count = count;
    persist_locked();
    xSemaphoreGive(s_mutex);
  }
  ESP_LOGI(TAG, "Stored %u quiet window(s)", (unsigned)count);
  quiet_hours_reevaluate();
}

bool quiet_hours_apply_json(const cJSON* obj, char* err, size_t err_len) {
  auto fail = [&](const char* msg) {
    if (err && err_len > 0) snprintf(err, err_len, "%s", msg);
    return false;
  };

  if (!obj || !cJSON_IsObject(obj)) {
    return fail("quiet_hours must be an object");
  }

  const cJSON* windows = cJSON_GetObjectItem(obj, "windows");
  if (windows && !cJSON_IsArray(windows)) {
    return fail("quiet_hours.windows must be an array");
  }

  quiet_window_t parsed[QUIET_HOURS_MAX_WINDOWS] = {};
  size_t count = 0;

  if (windows) {
    int n = cJSON_GetArraySize(windows);
    if (n > QUIET_HOURS_MAX_WINDOWS) {
      return fail("too many quiet windows (max 4)");
    }
    for (int i = 0; i < n; ++i) {
      const cJSON* w = cJSON_GetArrayItem(windows, i);
      if (!cJSON_IsObject(w)) return fail("each window must be an object");

      const cJSON* sh = cJSON_GetObjectItem(w, "start_hour");
      const cJSON* sm = cJSON_GetObjectItem(w, "start_min");
      const cJSON* eh = cJSON_GetObjectItem(w, "end_hour");
      const cJSON* em = cJSON_GetObjectItem(w, "end_min");
      if (!cJSON_IsNumber(sh) || !cJSON_IsNumber(sm) || !cJSON_IsNumber(eh) ||
          !cJSON_IsNumber(em)) {
        return fail("window requires start_hour/min and end_hour/min");
      }
      if (sh->valueint < 0 || sh->valueint > 23 || eh->valueint < 0 ||
          eh->valueint > 23) {
        return fail("hour out of range (0-23)");
      }
      if (sm->valueint < 0 || sm->valueint > 59 || em->valueint < 0 ||
          em->valueint > 59) {
        return fail("minute out of range (0-59)");
      }

      quiet_window_t* out = &parsed[count++];
      out->start_hour = static_cast<uint8_t>(sh->valueint);
      out->start_min = static_cast<uint8_t>(sm->valueint);
      out->end_hour = static_cast<uint8_t>(eh->valueint);
      out->end_min = static_cast<uint8_t>(em->valueint);

      const cJSON* enabled = cJSON_GetObjectItem(w, "enabled");
      out->enabled = enabled ? cJSON_IsTrue(enabled) : true;

      const cJSON* days = cJSON_GetObjectItem(w, "days");
      if (days) {
        if (!cJSON_IsNumber(days) || days->valueint < 0 ||
            days->valueint > 0x7F) {
          return fail("days must be a bitmask 0-127");
        }
        out->day_mask = static_cast<uint8_t>(days->valueint);
      } else {
        out->day_mask = ALL_DAYS_MASK;
      }
    }
  }

  quiet_hours_set_windows(parsed, count);
  return true;
}

cJSON* quiet_hours_to_json(void) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return nullptr;

  cJSON_AddBoolToObject(root, "active", quiet_hours_is_active());

  cJSON* arr = cJSON_AddArrayToObject(root, "windows");
  if (!arr) {
    cJSON_Delete(root);
    return nullptr;
  }

  quiet_window_t windows[QUIET_HOURS_MAX_WINDOWS];
  size_t count = quiet_hours_get_windows(windows, QUIET_HOURS_MAX_WINDOWS);
  for (size_t i = 0; i < count; ++i) {
    cJSON* w = cJSON_CreateObject();
    if (!w) continue;
    cJSON_AddBoolToObject(w, "enabled", windows[i].enabled);
    cJSON_AddNumberToObject(w, "start_hour", windows[i].start_hour);
    cJSON_AddNumberToObject(w, "start_min", windows[i].start_min);
    cJSON_AddNumberToObject(w, "end_hour", windows[i].end_hour);
    cJSON_AddNumberToObject(w, "end_min", windows[i].end_min);
    cJSON_AddNumberToObject(w, "days", windows[i].day_mask);
    cJSON_AddItemToArray(arr, w);
  }

  return root;
}

void quiet_hours_init(void) {
  if (s_mutex) return;  // already initialized

  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) {
    ESP_LOGE(TAG, "Failed to create mutex");
    return;
  }

  load_from_nvs();

  // Re-evaluate the moment the clock becomes valid so a device that booted
  // inside a quiet window blanks as soon as time syncs.
  event_bus_subscribe(TRONBYT_EVENT_TIME_SYNCED,
                      [](const tronbyt_event_t*, void*) {
                        quiet_hours_reevaluate();
                      },
                      nullptr);

  esp_timer_create_args_t args = {};
  args.callback = eval_timer_cb;
  args.name = "quiet_eval";
  if (esp_timer_create(&args, &s_timer) == ESP_OK) {
    esp_timer_start_periodic(s_timer, EVAL_PERIOD_US);
  } else {
    ESP_LOGE(TAG, "Failed to create evaluator timer");
  }

  quiet_hours_reevaluate();
  ESP_LOGI(TAG, "Quiet hours initialized");
}
