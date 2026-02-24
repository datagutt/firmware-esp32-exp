#include "app_state.h"

#include <cstring>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

#include "event_bus.h"

namespace {

const char* TAG = "app_state";

constexpr int MAX_CALLBACKS = 8;

constexpr EventBits_t BIT_WIFI = (1 << 0);
constexpr EventBits_t BIT_INTERNET = (1 << 1);
constexpr EventBits_t BIT_SERVER = (1 << 2);

struct CallbackEntry {
  app_state_change_cb_t cb;
  void* ctx;
};

struct AppStateContext {
  app_state_t state = APP_STATE_BOOT;
  connectivity_level_t connectivity = CONNECTIVITY_NO_WIFI;
  ota_substate_t ota_sub = OTA_SUBSTATE_IDLE;
  config_substate_t config_sub = CONFIG_SUBSTATE_WAITING;
  char error_reason[64] = {};

  SemaphoreHandle_t mutex = nullptr;
  EventGroupHandle_t event_group = nullptr;

  CallbackEntry callbacks[MAX_CALLBACKS] = {};
  int callback_count = 0;

  bool initialized = false;
};

AppStateContext s_ctx;

// ---------------------------------------------------------------------------
// Entry rules
// ---------------------------------------------------------------------------

bool can_enter_state(app_state_t current, app_state_t target) {
  if (target == APP_STATE_ERROR) return true;  // Always allowed

  switch (target) {
    case APP_STATE_NORMAL:
      return current != APP_STATE_ERROR;
    case APP_STATE_CONFIG_PORTAL:
      return current == APP_STATE_BOOT || current == APP_STATE_NORMAL;
    case APP_STATE_OTA:
      return current == APP_STATE_NORMAL;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void update_event_group_bits() {
  if (!s_ctx.event_group) return;

  EventBits_t set = 0;
  EventBits_t clear = 0;

  if (s_ctx.connectivity >= CONNECTIVITY_NO_INTERNET) {
    set |= BIT_WIFI;
  } else {
    clear |= BIT_WIFI;
  }

  if (s_ctx.connectivity >= CONNECTIVITY_CONNECTED) {
    set |= BIT_INTERNET;
  } else {
    clear |= BIT_INTERNET;
  }

  if (s_ctx.connectivity >= CONNECTIVITY_SERVER_ONLINE) {
    set |= BIT_SERVER;
  } else {
    clear |= BIT_SERVER;
  }

  if (clear) xEventGroupClearBits(s_ctx.event_group, clear);
  if (set) xEventGroupSetBits(s_ctx.event_group, set);
}

void invoke_callbacks(app_state_t old_state, app_state_t new_state) {
  // Copy callback list under mutex then invoke outside mutex
  CallbackEntry entries[MAX_CALLBACKS];
  int count = 0;

  xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
  count = s_ctx.callback_count;
  memcpy(entries, s_ctx.callbacks, sizeof(entries));
  xSemaphoreGive(s_ctx.mutex);

  for (int i = 0; i < count; i++) {
    if (entries[i].cb) {
      entries[i].cb(old_state, new_state, entries[i].ctx);
    }
  }
}

esp_err_t transition_to(app_state_t target) {
  xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

  app_state_t current = s_ctx.state;
  if (current == target) {
    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
  }

  if (!can_enter_state(current, target)) {
    ESP_LOGW(TAG, "Transition %s -> %s denied", app_state_name(current),
             app_state_name(target));
    xSemaphoreGive(s_ctx.mutex);
    return ESP_ERR_INVALID_STATE;
  }

  s_ctx.state = target;

  // Reset sub-states on entry
  if (target == APP_STATE_OTA) {
    s_ctx.ota_sub = OTA_SUBSTATE_IDLE;
  } else if (target == APP_STATE_CONFIG_PORTAL) {
    s_ctx.config_sub = CONFIG_SUBSTATE_WAITING;
  }

  ESP_LOGI(TAG, "State: %s -> %s", app_state_name(current),
           app_state_name(target));

  xSemaphoreGive(s_ctx.mutex);

  // Emit event and invoke callbacks outside mutex
  event_bus_emit_i32(TRONBYT_EVENT_STATE_CHANGED, static_cast<int32_t>(target));
  invoke_callbacks(current, target);

  return ESP_OK;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void app_state_init(void) {
  if (s_ctx.initialized) return;

  s_ctx.mutex = xSemaphoreCreateMutex();
  s_ctx.event_group = xEventGroupCreate();
  s_ctx.state = APP_STATE_BOOT;
  s_ctx.connectivity = CONNECTIVITY_NO_WIFI;
  s_ctx.callback_count = 0;
  s_ctx.initialized = true;

  ESP_LOGI(TAG, "State machine initialized (BOOT)");
}

esp_err_t app_state_enter_normal(void) { return transition_to(APP_STATE_NORMAL); }

esp_err_t app_state_enter_config_portal(void) {
  return transition_to(APP_STATE_CONFIG_PORTAL);
}

esp_err_t app_state_enter_ota(void) { return transition_to(APP_STATE_OTA); }

esp_err_t app_state_enter_error(const char* reason) {
  xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
  if (reason) {
    strncpy(s_ctx.error_reason, reason, sizeof(s_ctx.error_reason) - 1);
    s_ctx.error_reason[sizeof(s_ctx.error_reason) - 1] = '\0';
  }
  xSemaphoreGive(s_ctx.mutex);

  return transition_to(APP_STATE_ERROR);
}

app_state_t app_state_get(void) {
  xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
  app_state_t state = s_ctx.state;
  xSemaphoreGive(s_ctx.mutex);
  return state;
}

connectivity_level_t app_state_get_connectivity(void) {
  xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
  connectivity_level_t level = s_ctx.connectivity;
  xSemaphoreGive(s_ctx.mutex);
  return level;
}

ota_substate_t app_state_get_ota_substate(void) {
  xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
  ota_substate_t sub = s_ctx.ota_sub;
  xSemaphoreGive(s_ctx.mutex);
  return sub;
}

bool app_state_has_wifi(void) {
  return app_state_get_connectivity() >= CONNECTIVITY_NO_INTERNET;
}

bool app_state_has_internet(void) {
  return app_state_get_connectivity() >= CONNECTIVITY_CONNECTED;
}

bool app_state_is_server_online(void) {
  return app_state_get_connectivity() >= CONNECTIVITY_SERVER_ONLINE;
}

void app_state_set_ota_substate(ota_substate_t sub) {
  xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
  s_ctx.ota_sub = sub;
  xSemaphoreGive(s_ctx.mutex);

  event_bus_emit_i32(TRONBYT_EVENT_OTA_SUBSTATE_CHANGED,
                     static_cast<int32_t>(sub));
}

void app_state_set_config_substate(config_substate_t sub) {
  xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
  s_ctx.config_sub = sub;
  xSemaphoreGive(s_ctx.mutex);
}

void app_state_set_connectivity(connectivity_level_t level) {
  xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
  connectivity_level_t old = s_ctx.connectivity;
  s_ctx.connectivity = level;
  update_event_group_bits();
  xSemaphoreGive(s_ctx.mutex);

  if (old != level) {
    ESP_LOGI(TAG, "Connectivity: %s -> %s", connectivity_level_name(old),
             connectivity_level_name(level));
    event_bus_emit_i32(TRONBYT_EVENT_CONNECTIVITY_CHANGED,
                       static_cast<int32_t>(level));
  }
}

esp_err_t app_state_wait_for_wifi(uint32_t timeout_ms) {
  if (!s_ctx.event_group) return ESP_ERR_INVALID_STATE;

  EventBits_t bits = xEventGroupWaitBits(
      s_ctx.event_group, BIT_WIFI, pdFALSE, pdTRUE,
      pdMS_TO_TICKS(timeout_ms));

  return (bits & BIT_WIFI) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_state_wait_for_internet(uint32_t timeout_ms) {
  if (!s_ctx.event_group) return ESP_ERR_INVALID_STATE;

  EventBits_t bits = xEventGroupWaitBits(
      s_ctx.event_group, BIT_INTERNET, pdFALSE, pdTRUE,
      pdMS_TO_TICKS(timeout_ms));

  return (bits & BIT_INTERNET) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_state_wait_for_server(uint32_t timeout_ms) {
  if (!s_ctx.event_group) return ESP_ERR_INVALID_STATE;

  EventBits_t bits = xEventGroupWaitBits(
      s_ctx.event_group, BIT_SERVER, pdFALSE, pdTRUE,
      pdMS_TO_TICKS(timeout_ms));

  return (bits & BIT_SERVER) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_state_register_callback(app_state_change_cb_t cb, void* ctx) {
  if (!s_ctx.initialized || !cb) return ESP_ERR_INVALID_STATE;

  xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
  if (s_ctx.callback_count >= MAX_CALLBACKS) {
    xSemaphoreGive(s_ctx.mutex);
    return ESP_ERR_NO_MEM;
  }
  s_ctx.callbacks[s_ctx.callback_count].cb = cb;
  s_ctx.callbacks[s_ctx.callback_count].ctx = ctx;
  s_ctx.callback_count++;
  xSemaphoreGive(s_ctx.mutex);

  return ESP_OK;
}

void app_state_unregister_callback(app_state_change_cb_t cb) {
  if (!s_ctx.initialized || !cb) return;

  xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
  for (int i = 0; i < s_ctx.callback_count; i++) {
    if (s_ctx.callbacks[i].cb == cb) {
      for (int j = i; j < s_ctx.callback_count - 1; j++) {
        s_ctx.callbacks[j] = s_ctx.callbacks[j + 1];
      }
      s_ctx.callback_count--;
      break;
    }
  }
  xSemaphoreGive(s_ctx.mutex);
}

const char* app_state_name(app_state_t state) {
  switch (state) {
    case APP_STATE_BOOT:
      return "BOOT";
    case APP_STATE_NORMAL:
      return "NORMAL";
    case APP_STATE_CONFIG_PORTAL:
      return "CONFIG_PORTAL";
    case APP_STATE_OTA:
      return "OTA";
    case APP_STATE_ERROR:
      return "ERROR";
  }
  return "UNKNOWN";
}

const char* connectivity_level_name(connectivity_level_t level) {
  switch (level) {
    case CONNECTIVITY_NO_WIFI:
      return "NO_WIFI";
    case CONNECTIVITY_NO_INTERNET:
      return "NO_INTERNET";
    case CONNECTIVITY_CONNECTED:
      return "CONNECTED";
    case CONNECTIVITY_SERVER_ONLINE:
      return "SERVER_ONLINE";
  }
  return "UNKNOWN";
}
