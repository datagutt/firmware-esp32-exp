#include "handlers.h"

#include <cstdlib>
#include <cstring>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "display.h"
#include "api_validation.h"
#include "diag_event_ring.h"
#include "messages.h"
#include "nvs_settings.h"
#include "ota.h"
#include "sdkconfig.h"
#include "syslog.h"
#include "webp_player.h"
#include "wifi.h"

namespace {

const char* TAG = "handlers";

#ifndef CONFIG_REFRESH_INTERVAL_SECONDS
constexpr int DEFAULT_REFRESH_INTERVAL = 10;
#else
constexpr int DEFAULT_REFRESH_INTERVAL = CONFIG_REFRESH_INTERVAL_SECONDS;
#endif

constexpr int CONSUMER_STACK_SIZE = 6144;
constexpr int CONSUMER_PRIORITY = 4;

struct TextMsg {
  char* data;
  size_t len;
};

void consumer_task(void*);

int32_t s_dwell_secs = DEFAULT_REFRESH_INTERVAL;
uint8_t* s_webp = nullptr;
size_t s_ws_accumulated_len = 0;
bool s_oversize_detected = false;
bool s_first_image_received = false;

TaskHandle_t s_consumer_task = nullptr;
SemaphoreHandle_t s_text_mutex = nullptr;
TextMsg s_pending_text = {nullptr, 0};
uint32_t s_text_replace_count = 0;

bool ensure_text_mailbox_initialized() {
  if (s_text_mutex && s_consumer_task) {
    return true;
  }

  if (!s_text_mutex) {
    s_text_mutex = xSemaphoreCreateMutex();
    if (!s_text_mutex) {
      ESP_LOGE(TAG, "Failed to create text mailbox mutex");
      return false;
    }
  }

  if (!s_consumer_task) {
    BaseType_t rc = pdFAIL;
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    rc = xTaskCreatePinnedToCoreWithCaps(
        consumer_task, "txt_handler", CONSUMER_STACK_SIZE, nullptr,
        CONSUMER_PRIORITY, &s_consumer_task, tskNO_AFFINITY,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rc != pdPASS) {
      ESP_LOGW(TAG,
               "PSRAM stack task creation failed for text mailbox, retrying "
               "with internal RAM");
    }
#endif
    if (rc != pdPASS) {
      rc = xTaskCreate(consumer_task, "txt_handler", CONSUMER_STACK_SIZE,
                       nullptr, CONSUMER_PRIORITY, &s_consumer_task);
    }
    if (rc != pdPASS) {
      ESP_LOGE(TAG, "Failed to create text mailbox consumer task");
      return false;
    }
  }

  return true;
}

void ota_task_entry(void* param) {
  auto* url = static_cast<char*>(param);
  run_ota(url);
  free(url);
  vTaskDelete(nullptr);
}

void process_text_message(const char* json_str) {
  cJSON* root = cJSON_Parse(json_str);

  if (!root) {
    ESP_LOGW(TAG, "Failed to parse WebSocket text message as JSON");
    diag_event_log("WARN", "json_parse_error", -1,
                   "WebSocket text payload is not valid JSON");
    return;
  }

  const char* const kAllowedKeys[] = {"immediate",       "dwell_secs",
                                      "brightness",      "ota_url",
                                      "swap_colors",     "wifi_power_save",
                                      "skip_display_version",
                                      "ap_mode",         "prefer_ipv6",
                                      "hostname",        "syslog_addr",
                                      "sntp_server",     "image_url",
                                      "reboot"};

  char validation_err[128] = {0};
  if (!api_validate_no_unknown_keys(root, kAllowedKeys,
                                    sizeof(kAllowedKeys) /
                                        sizeof(kAllowedKeys[0]),
                                    validation_err,
                                    sizeof(validation_err))) {
    ESP_LOGW(TAG, "Validation failed: %s", validation_err);
    diag_event_log("WARN", "json_validation_error", -1, validation_err);
    cJSON_Delete(root);
    return;
  }

  int dwell_value = 0;
  bool has_dwell = false;
  int brightness_value = 0;
  bool has_brightness = false;
  int wifi_ps_value = 0;
  bool has_wifi_ps = false;
  const char* ota_url_value = nullptr;
  bool has_ota_url = false;
  const char* hostname_value = nullptr;
  bool has_hostname = false;
  const char* syslog_addr_value = nullptr;
  bool has_syslog_addr = false;
  const char* sntp_server_value = nullptr;
  bool has_sntp_server = false;
  const char* image_url_value = nullptr;
  bool has_image_url = false;
  bool bool_val = false;

  auto validate_or_abort = [&](bool ok) {
    if (!ok) {
      ESP_LOGW(TAG, "Validation failed: %s", validation_err);
      diag_event_log("WARN", "json_validation_error", -1, validation_err);
      cJSON_Delete(root);
      return false;
    }
    return true;
  };

  if (!validate_or_abort(api_validate_optional_int(
          root, "dwell_secs", 1, 3600, &dwell_value, &has_dwell,
          validation_err, sizeof(validation_err))))
    return;
  if (!validate_or_abort(api_validate_optional_int(
          root, "brightness", DISPLAY_MIN_BRIGHTNESS, DISPLAY_MAX_BRIGHTNESS,
          &brightness_value, &has_brightness, validation_err,
          sizeof(validation_err))))
    return;
  if (!validate_or_abort(api_validate_optional_int(
          root, "wifi_power_save", WIFI_PS_NONE, WIFI_PS_MAX_MODEM,
          &wifi_ps_value, &has_wifi_ps, validation_err,
          sizeof(validation_err))))
    return;
  if (!validate_or_abort(api_validate_optional_string(
          root, "ota_url", 1, MAX_URL_LEN, &ota_url_value, &has_ota_url,
          validation_err, sizeof(validation_err))))
    return;
  if (!validate_or_abort(api_validate_optional_string(
          root, "hostname", 1, MAX_HOSTNAME_LEN, &hostname_value, &has_hostname,
          validation_err, sizeof(validation_err))))
    return;
  if (!validate_or_abort(api_validate_optional_string(
          root, "syslog_addr", 0, MAX_SYSLOG_ADDR_LEN, &syslog_addr_value,
          &has_syslog_addr, validation_err, sizeof(validation_err))))
    return;
  if (!validate_or_abort(api_validate_optional_string(
          root, "sntp_server", 0, MAX_SNTP_SERVER_LEN, &sntp_server_value,
          &has_sntp_server, validation_err, sizeof(validation_err))))
    return;
  if (!validate_or_abort(api_validate_optional_string(
          root, "image_url", 0, MAX_URL_LEN, &image_url_value, &has_image_url,
          validation_err, sizeof(validation_err))))
    return;

  bool settings_changed = false;
  auto cfg = config_get();

  cJSON* immediate_item = cJSON_GetObjectItem(root, "immediate");
  if (cJSON_IsBool(immediate_item) && cJSON_IsTrue(immediate_item)) {
    ESP_LOGD(TAG, "Interrupting current animation to load queued image");
    gfx_preempt();
  }

  if (has_dwell) {
    s_dwell_secs = dwell_value;
    ESP_LOGD(TAG, "Updated dwell_secs to %" PRId32 " seconds", s_dwell_secs);
  }

  if (has_brightness) {
    display_set_brightness(static_cast<uint8_t>(brightness_value));
    ESP_LOGI(TAG, "Updated brightness to %d", brightness_value);
  }

  if (has_ota_url) {
    char* ota_url = strdup(ota_url_value);
    if (ota_url) {
      ESP_LOGI(TAG, "OTA URL received via WS: %s", ota_url);
      xTaskCreate(ota_task_entry, "ota_task", 8192, ota_url, 5, nullptr);
    }
  }

  cJSON* swap_colors_item = cJSON_GetObjectItem(root, "swap_colors");
  if (cJSON_IsBool(swap_colors_item)) {
    bool val = cJSON_IsTrue(swap_colors_item);
    cfg.swap_colors = val;
    ESP_LOGI(TAG, "Updated swap_colors to %d", val);
    settings_changed = true;
  }

  if (has_wifi_ps) {
    auto val = static_cast<wifi_ps_type_t>(wifi_ps_value);
    cfg.wifi_power_save = val;
    ESP_LOGI(TAG, "Updated wifi_power_save to %d", val);
    settings_changed = true;
    wifi_apply_power_save();
  }

  cJSON* skip_ver_item = cJSON_GetObjectItem(root, "skip_display_version");
  if (cJSON_IsBool(skip_ver_item)) {
    bool val = cJSON_IsTrue(skip_ver_item);
    cfg.skip_display_version = val;
    ESP_LOGI(TAG, "Updated skip_display_version to %d", val);
    settings_changed = true;
  }

  cJSON* ap_mode_item = cJSON_GetObjectItem(root, "ap_mode");
  if (cJSON_IsBool(ap_mode_item)) {
    bool val = cJSON_IsTrue(ap_mode_item);
    cfg.ap_mode = val;
    ESP_LOGI(TAG, "Updated ap_mode to %d", val);
    settings_changed = true;
  }

  cJSON* prefer_ipv6_item = cJSON_GetObjectItem(root, "prefer_ipv6");
  if (cJSON_IsBool(prefer_ipv6_item)) {
    bool val = cJSON_IsTrue(prefer_ipv6_item);
    cfg.prefer_ipv6 = val;
    ESP_LOGI(TAG, "Updated prefer_ipv6 to %d", val);
    settings_changed = true;
  }

  if (has_hostname) {
    snprintf(cfg.hostname, sizeof(cfg.hostname), "%s", hostname_value);
    wifi_set_hostname(hostname_value);
    ESP_LOGI(TAG, "Updated hostname to %s", hostname_value);
    settings_changed = true;
  }

  if (has_syslog_addr) {
    snprintf(cfg.syslog_addr, sizeof(cfg.syslog_addr), "%s", syslog_addr_value);
    syslog_update_config(syslog_addr_value);
    ESP_LOGI(TAG, "Updated syslog_addr to %s", syslog_addr_value);
    settings_changed = true;
  }

  if (has_sntp_server) {
    snprintf(cfg.sntp_server, sizeof(cfg.sntp_server), "%s", sntp_server_value);
    ESP_LOGI(TAG, "Updated sntp_server to %s", sntp_server_value);
    settings_changed = true;
  }

  if (has_image_url) {
    snprintf(cfg.image_url, sizeof(cfg.image_url), "%s", image_url_value);
    ESP_LOGI(TAG, "Updated image_url to %s", image_url_value);
    settings_changed = true;
  }

  if (settings_changed) {
    config_set(&cfg);
    msg_send_client_info();
  }

  cJSON* reboot_item = cJSON_GetObjectItem(root, "reboot");
  if (cJSON_IsBool(reboot_item) && cJSON_IsTrue(reboot_item)) {
    ESP_LOGI(TAG, "Reboot command received via WS");
    cJSON_Delete(root);
    esp_restart();
  }

  cJSON_Delete(root);
}

void consumer_task(void*) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    while (true) {
      TextMsg msg = {nullptr, 0};
      if (!s_text_mutex) break;
      if (xSemaphoreTake(s_text_mutex, portMAX_DELAY) != pdTRUE) break;
      if (s_pending_text.data) {
        msg = s_pending_text;
        s_pending_text = {nullptr, 0};
      }
      xSemaphoreGive(s_text_mutex);

      if (!msg.data) break;
      process_text_message(msg.data);
      free(msg.data);
    }
  }
}

}  // namespace

void handlers_init() {
  if (ensure_text_mailbox_initialized()) {
    ESP_LOGI("handlers", "Text message mailbox initialized");
  }
}

void handlers_deinit() {
  if (s_consumer_task) {
    vTaskDelete(s_consumer_task);
    s_consumer_task = nullptr;
  }

  if (s_text_mutex) {
    if (xSemaphoreTake(s_text_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (s_pending_text.data) {
        free(s_pending_text.data);
        s_pending_text = {nullptr, 0};
      }
      xSemaphoreGive(s_text_mutex);
    }
    vSemaphoreDelete(s_text_mutex);
    s_text_mutex = nullptr;
  }
  s_text_replace_count = 0;
}

void handle_text_message(esp_websocket_event_data_t* data) {
  bool is_complete =
      (data->payload_offset + data->data_len >= data->payload_len);
  if (!is_complete) return;

  if (!ensure_text_mailbox_initialized()) {
    ESP_LOGW("handlers", "Text mailbox not initialized, dropping text message");
    return;
  }

  auto* buf = static_cast<char*>(malloc(data->data_len + 1));
  if (!buf) {
    ESP_LOGE("handlers", "Failed to allocate text message buffer");
    return;
  }
  memcpy(buf, data->data_ptr, data->data_len);
  buf[data->data_len] = '\0';

  TextMsg msg = {buf, static_cast<size_t>(data->data_len)};
  if (xSemaphoreTake(s_text_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    ESP_LOGW("handlers", "Text mailbox busy, dropping newest message");
    free(buf);
    return;
  }

  if (s_pending_text.data) {
    free(s_pending_text.data);
    s_text_replace_count++;
    if ((s_text_replace_count % 20) == 1) {
      ESP_LOGW("handlers",
               "Text message burst: replaced older pending messages (%" PRIu32
               " replacements)",
               s_text_replace_count);
    }
  }
  s_pending_text = msg;
  xSemaphoreGive(s_text_mutex);
  xTaskNotifyGive(s_consumer_task);
}

void handle_binary_message(esp_websocket_event_data_t* data) {
  if (data->op_code == 2 && data->payload_offset == 0) {
    if (s_webp) {
      ESP_LOGW(TAG, "Discarding incomplete previous WebP buffer");
      free(s_webp);
      s_webp = nullptr;
    }
    s_ws_accumulated_len = 0;
    s_oversize_detected = false;

    if (data->payload_len > CONFIG_HTTP_BUFFER_SIZE_MAX) {
      ESP_LOGE(TAG, "WebP size (%d bytes) exceeds max (%d)", data->payload_len,
               CONFIG_HTTP_BUFFER_SIZE_MAX);
      s_oversize_detected = true;
      if (gfx_display_asset("oversize") != 0) {
        ESP_LOGE(TAG, "Failed to display oversize graphic");
      }
      return;
    }

    if (data->payload_len > 0) {
      s_webp = static_cast<uint8_t*>(heap_caps_malloc(
          static_cast<size_t>(data->payload_len),
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!s_webp) {
        ESP_LOGE(TAG, "Failed to allocate WebP buffer (%d bytes)",
                 data->payload_len);
        s_oversize_detected = true;
        return;
      }
    }
  }

  if (s_oversize_detected) return;

  if (data->op_code == 0 && !s_webp) return;

  size_t end_offset = static_cast<size_t>(data->payload_offset) + data->data_len;
  if (end_offset > CONFIG_HTTP_BUFFER_SIZE_MAX) {
    ESP_LOGE(TAG, "WebP size (%zu bytes) exceeds max (%d)", end_offset,
             CONFIG_HTTP_BUFFER_SIZE_MAX);
    s_oversize_detected = true;
    if (gfx_display_asset("oversize") != 0) {
      ESP_LOGE(TAG, "Failed to display oversize graphic");
    }
    free(s_webp);
    s_webp = nullptr;
    s_ws_accumulated_len = 0;
    return;
  }

  if (data->payload_len > 0 &&
      end_offset > static_cast<size_t>(data->payload_len)) {
    ESP_LOGE(TAG,
             "Invalid WebSocket payload offsets (%zu > total %d); dropping",
             end_offset, data->payload_len);
    free(s_webp);
    s_webp = nullptr;
    s_ws_accumulated_len = 0;
    s_oversize_detected = true;
    return;
  }

  if (data->data_len > 0 && s_webp) {
    memcpy(s_webp + data->payload_offset, data->data_ptr, data->data_len);
  }
  if (end_offset > s_ws_accumulated_len) {
    s_ws_accumulated_len = end_offset;
  }

  bool frame_complete = (data->payload_len > 0)
                            ? (s_ws_accumulated_len >=
                               static_cast<size_t>(data->payload_len))
                            : (data->payload_offset + data->data_len >=
                               data->payload_len);

  if (data->fin && frame_complete) {
    ESP_LOGD(TAG, "WebP download complete (%zu bytes)", s_ws_accumulated_len);

    int counter = gfx_update(s_webp, s_ws_accumulated_len, s_dwell_secs);
    if (counter < 0) {
      ESP_LOGE(TAG, "Failed to queue downloaded WebP");
      free(s_webp);
    }

    if (counter >= 0 && !s_first_image_received) {
      ESP_LOGI(TAG,
               "First WebSocket image received - interrupting boot animation");
      gfx_preempt();
      s_first_image_received = true;
    }

    // Ownership transferred to gfx
    s_webp = nullptr;
    s_ws_accumulated_len = 0;
  }
}
