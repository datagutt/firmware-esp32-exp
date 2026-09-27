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
#include "event_bus.h"
#include "messages.h"
#include "nvs_settings.h"
#include "ota.h"
#include "psram_alloc.h"
#include "quiet_hours.h"
#include "sdkconfig.h"
#include "syslog.h"
#include "webp_frame.h"
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

// Work for the consumer task, delivered as task-notification bits so repeated
// requests coalesce. One task does all of it because internal RAM is scarce
// and each item is short and rare.
constexpr uint32_t WORK_TEXT = BIT0;         // s_pending_text holds a message
constexpr uint32_t WORK_CLIENT_INFO = BIT1;  // send client_info to the server

// Upper bound for one reassembled text (JSON) message. Server commands are a
// few hundred bytes; the cap bounds what a misbehaving server can make us
// buffer.
constexpr size_t MAX_TEXT_MESSAGE_LEN = 16 * 1024;

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

// Text message reassembly, touched only from the WS task. A message arrives as
// one or more frames (text frame, then continuation frames), and the
// component delivers each frame in buffer_size chunks.
char* s_text_rx = nullptr;
size_t s_text_rx_cap = 0;         // allocated bytes, including the terminator
size_t s_text_rx_len = 0;         // bytes received so far across all frames
size_t s_text_rx_frame_base = 0;  // offset of the current frame in the message
bool s_text_rx_active = false;    // false while no message is being assembled

TaskHandle_t s_consumer_task = nullptr;
SemaphoreHandle_t s_text_mutex = nullptr;
TextMsg s_pending_text = {nullptr, 0};
uint32_t s_text_replace_count = 0;
// Settings from the last processed message, saved by the consumer after
// process_text_message returns so the flash write does not stack on top of the
// JSON handling. Only the consumer task touches these.
system_config_t s_pending_config = {};
bool s_pending_config_valid = false;

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
    // This task applies server settings that persist to flash (the config blob
    // and quiet-hours windows). Flash operations disable the CPU cache, which
    // makes PSRAM inaccessible, so the stack MUST live in internal RAM. Do not
    // move it to a PSRAM stack (xTaskCreateWithCaps + MALLOC_CAP_SPIRAM): any
    // NVS write from here would trip esp_task_stack_is_sane_cache_disabled().
    BaseType_t rc = xTaskCreate(consumer_task, "txt_handler",
                                CONSUMER_STACK_SIZE, nullptr, CONSUMER_PRIORITY,
                                &s_consumer_task);
    if (rc != pdPASS) {
      ESP_LOGE(TAG, "Failed to create text mailbox consumer task");
      return false;
    }
  }

  return true;
}

void queue_config_persist(const system_config_t& cfg) {
  s_pending_config = cfg;
  s_pending_config_valid = true;
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
                                      "skip_boot_animation",
                                      "ap_mode",         "prefer_ipv6",
                                      "disable_touch",   "touch_beep",
                                      "hostname",        "syslog_addr",
                                      "sntp_server",     "image_url",
                                      "api_key",         "quiet_hours",
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
  const char* api_key_value = nullptr;
  bool has_api_key = false;
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
  if (!validate_or_abort(api_validate_optional_string(
          root, "api_key", 0, MAX_API_KEY_LEN, &api_key_value, &has_api_key,
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
    event_bus_emit_i32(TRONBYT_EVENT_BRIGHTNESS_CHANGED, brightness_value);
  }

  if (has_ota_url) {
    size_t url_len = strlen(ota_url_value) + 1;
    char* ota_url = static_cast<char*>(psram_or_internal_malloc(url_len));
    if (ota_url) {
      memcpy(ota_url, ota_url_value, url_len);
      ESP_LOGI(TAG, "OTA URL received via WS: %s", ota_url);
      // OTA writes the app partition via esp_ota_write, which disables the
      // flash cache; the task stack must be in internal RAM. A PSRAM stack here
      // trips esp_task_stack_is_sane_cache_disabled() on the first write.
      //
      // Priority 3 matches http_fetch: both are I/O-bound downloads that block
      // on the socket, so they belong in the same class.
      BaseType_t ota_rc =
          xTaskCreate(ota_task_entry, "ota_task", 8192, ota_url, 3, nullptr);
      if (ota_rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task; dropping request");
        free(ota_url);  // no task will run to free it
      }
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

  cJSON* skip_boot_item = cJSON_GetObjectItem(root, "skip_boot_animation");
  if (cJSON_IsBool(skip_boot_item)) {
    bool val = cJSON_IsTrue(skip_boot_item);
    cfg.skip_boot_animation = val;
    ESP_LOGI(TAG, "Updated skip_boot_animation to %d", val);
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
    if (val) {
      wifi_enable_ipv6();
    }
  }

  cJSON* disable_touch_item = cJSON_GetObjectItem(root, "disable_touch");
  if (cJSON_IsBool(disable_touch_item)) {
    bool val = cJSON_IsTrue(disable_touch_item);
    cfg.disable_touch = val;
    ESP_LOGI(TAG, "Updated disable_touch to %d", val);
    settings_changed = true;
  }

  cJSON* touch_beep_item = cJSON_GetObjectItem(root, "touch_beep");
  if (cJSON_IsBool(touch_beep_item)) {
    bool val = cJSON_IsTrue(touch_beep_item);
    cfg.touch_beep = val;
    ESP_LOGI(TAG, "Updated touch_beep to %d", val);
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
#ifdef CONFIG_LOCK_SERVER_URL
    ESP_LOGW(TAG, "image_url change ignored (server URL locked)");
#else
    snprintf(cfg.image_url, sizeof(cfg.image_url), "%s", image_url_value);
    ESP_LOGI(TAG, "Updated image_url to %s", image_url_value);
    settings_changed = true;
#endif
  }

  if (has_api_key) {
    snprintf(cfg.api_key, sizeof(cfg.api_key), "%s", api_key_value);
    ESP_LOGI(TAG, "Updated api_key");
    settings_changed = true;
  }

  // Quiet hours persists to its own NVS namespace (not the config blob), so it
  // is applied directly rather than folded into settings_changed.
  cJSON* quiet_item = cJSON_GetObjectItem(root, "quiet_hours");
  if (quiet_item) {
    char quiet_err[96] = {0};
    if (quiet_hours_apply_json(quiet_item, quiet_err, sizeof(quiet_err))) {
      ESP_LOGI(TAG, "Updated quiet_hours");
    } else {
      ESP_LOGW(TAG, "quiet_hours rejected: %s", quiet_err);
      diag_event_log("WARN", "json_validation_error", -1, quiet_err);
    }
  }

  if (settings_changed) {
    queue_config_persist(cfg);
  }

  cJSON* reboot_item = cJSON_GetObjectItem(root, "reboot");
  if (cJSON_IsBool(reboot_item) && cJSON_IsTrue(reboot_item)) {
    ESP_LOGI(TAG, "Reboot command received via WS");
    cJSON_Delete(root);
    // Settings sent alongside the reboot must reach flash first; the consumer
    // loop that normally saves them never runs again.
    if (s_pending_config_valid) {
      s_pending_config_valid = false;
      config_set(&s_pending_config);
    }
    gfx_safe_restart();
  }

  cJSON_Delete(root);
}

// Handles every queued text message, then saves any settings they changed.
void drain_text_mailbox() {
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

    if (s_pending_config_valid) {
      s_pending_config_valid = false;
      config_set(&s_pending_config);
      // Echo the stored settings so the server sees what was applied.
      msg_send_client_info_now();
    }
  }
}

void consumer_task(void*) {
  while (true) {
    uint32_t work = 0;
    xTaskNotifyWait(0, UINT32_MAX, &work, portMAX_DELAY);
    if (work & WORK_TEXT) drain_text_mailbox();
    if (work & WORK_CLIENT_INFO) msg_send_client_info_now();
  }
}

char* text_rx_alloc(size_t size) {
  return static_cast<char*>(psram_or_internal_malloc(size));
}

void text_rx_reset() {
  free(s_text_rx);
  s_text_rx = nullptr;
  s_text_rx_cap = 0;
  s_text_rx_len = 0;
  s_text_rx_frame_base = 0;
  s_text_rx_active = false;
}

// Makes room for a frame of `frame_len` bytes appended at s_text_rx_len.
bool text_rx_begin_frame(size_t frame_len) {
  s_text_rx_frame_base = s_text_rx_len;
  size_t needed = s_text_rx_frame_base + frame_len;
  if (needed > MAX_TEXT_MESSAGE_LEN) {
    ESP_LOGW(TAG, "Text message exceeds %u bytes, dropping",
             (unsigned)MAX_TEXT_MESSAGE_LEN);
    return false;
  }
  if (needed + 1 <= s_text_rx_cap) return true;

  char* grown = text_rx_alloc(needed + 1);
  if (!grown) {
    ESP_LOGE(TAG, "Failed to allocate text message buffer");
    return false;
  }
  if (s_text_rx_len > 0) memcpy(grown, s_text_rx, s_text_rx_len);
  free(s_text_rx);
  s_text_rx = grown;
  s_text_rx_cap = needed + 1;
  return true;
}

// Takes ownership of buf and hands it to the consumer task.
void text_mailbox_post(char* buf, size_t len) {
  if (!ensure_text_mailbox_initialized()) {
    ESP_LOGW(TAG, "Text mailbox not initialized, dropping text message");
    free(buf);
    return;
  }

  TextMsg msg = {buf, len};
  if (xSemaphoreTake(s_text_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    ESP_LOGW(TAG, "Text mailbox busy, dropping newest message");
    free(buf);
    return;
  }

  if (s_pending_text.data) {
    free(s_pending_text.data);
    s_text_replace_count++;
    if ((s_text_replace_count % 20) == 1) {
      ESP_LOGW(TAG,
               "Text message burst: replaced older pending messages (%" PRIu32
               " replacements)",
               s_text_replace_count);
    }
  }
  s_pending_text = msg;
  xSemaphoreGive(s_text_mutex);
  xTaskNotify(s_consumer_task, WORK_TEXT, eSetBits);
}

}  // namespace

void handlers_init() {
  if (ensure_text_mailbox_initialized()) {
    ESP_LOGI(TAG, "Text message mailbox initialized");
  }
}

void handlers_deinit() {
  // The consumer task is left running: it may be in the middle of a flash
  // write, and deleting it there would leave NVS locks held forever.
  if (s_text_mutex &&
      xSemaphoreTake(s_text_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (s_pending_text.data) {
      free(s_pending_text.data);
      s_pending_text = {nullptr, 0};
    }
    xSemaphoreGive(s_text_mutex);
  }
  s_text_replace_count = 0;
  text_rx_reset();
}

esp_err_t handlers_request_client_info() {
  if (!s_consumer_task) return ESP_ERR_INVALID_STATE;
  xTaskNotify(s_consumer_task, WORK_CLIENT_INFO, eSetBits);
  return ESP_OK;
}

void handle_text_message(esp_websocket_event_data_t* data) {
  if (data->payload_offset < 0 || data->data_len < 0 ||
      data->payload_len < 0) {
    return;
  }
  size_t offset = static_cast<size_t>(data->payload_offset);
  size_t chunk = static_cast<size_t>(data->data_len);
  size_t frame_len = static_cast<size_t>(data->payload_len);

  // A text frame starts a new message and discards any unfinished one; a
  // continuation frame without a message in progress is dropped.
  if (data->op_code == WS_TRANSPORT_OPCODES_TEXT && offset == 0) {
    text_rx_reset();
    s_text_rx_active = true;
  }
  if (!s_text_rx_active) return;

  if (offset == 0 && !text_rx_begin_frame(frame_len)) {
    text_rx_reset();
    return;
  }
  size_t end = s_text_rx_frame_base + offset + chunk;
  if (offset + chunk > frame_len || end >= s_text_rx_cap) {
    ESP_LOGE(TAG, "Invalid text chunk offsets (%u+%u of %u), dropping",
             (unsigned)offset, (unsigned)chunk, (unsigned)frame_len);
    text_rx_reset();
    return;
  }
  if (chunk > 0) {
    memcpy(s_text_rx + s_text_rx_frame_base + offset, data->data_ptr, chunk);
  }
  if (end > s_text_rx_len) s_text_rx_len = end;

  bool frame_done = offset + chunk >= frame_len;
  if (!data->fin || !frame_done) return;

  char* buf = s_text_rx;
  size_t len = s_text_rx_len;
  s_text_rx = nullptr;
  text_rx_reset();
  if (len == 0) {
    free(buf);
    return;
  }
  buf[len] = '\0';
  text_mailbox_post(buf, len);
}

void handle_binary_message(esp_websocket_event_data_t* data) {
  // WebSocket image pushes bypass the scheduler, so they need their own quiet
  // hours gate: drop incoming frames while the panel is intentionally dark.
  if (quiet_hours_is_active()) return;

  if (data->op_code == 2 && data->payload_offset == 0) {
    ESP_LOGI(TAG, "WS binary start: total=%d dwell=%" PRId32
                  " first_image=%d",
             data->payload_len, s_dwell_secs, s_first_image_received);
    if (s_webp) {
      ESP_LOGW(TAG, "Discarding incomplete previous WebP buffer");
      free(s_webp);
      s_webp = nullptr;
    }
    s_ws_accumulated_len = 0;
    s_oversize_detected = false;

    if (webp_frame_check_offsets((uint32_t)data->payload_offset,
                                 (uint32_t)data->data_len,
                                 (uint32_t)data->payload_len,
                                 (size_t)CONFIG_HTTP_BUFFER_SIZE_MAX) !=
        WEBP_FRAME_OK) {
      ESP_LOGE(TAG, "WebP size (%d bytes) exceeds max (%d)", data->payload_len,
               CONFIG_HTTP_BUFFER_SIZE_MAX);
      s_oversize_detected = true;
      if (gfx_display_asset("oversize") != 0) {
        ESP_LOGE(TAG, "Failed to display oversize graphic");
      }
      return;
    }

    if (data->payload_len > 0) {
      s_webp = static_cast<uint8_t*>(
          psram_or_internal_malloc(static_cast<size_t>(data->payload_len)));
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
  webp_frame_check_t frame_chk = webp_frame_check_offsets(
      (uint32_t)data->payload_offset, (uint32_t)data->data_len,
      (uint32_t)data->payload_len, (size_t)CONFIG_HTTP_BUFFER_SIZE_MAX);
  if (frame_chk == WEBP_FRAME_OVERSIZE) {
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
  if (frame_chk == WEBP_FRAME_INVALID_OFFSET) {
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

    int32_t dwell_gfx =
        effective_dwell_for_brightness(display_get_brightness(), s_dwell_secs);
    int counter = gfx_update(s_webp, s_ws_accumulated_len, dwell_gfx);
    if (counter < 0) {
      ESP_LOGE(TAG, "Failed to queue downloaded WebP");
      free(s_webp);
    } else {
      ESP_LOGI(TAG, "Queued WS image counter=%d size=%zu dwell=%" PRId32,
               counter, s_ws_accumulated_len, dwell_gfx);
    }

    if (counter >= 0 && !s_first_image_received) {
      ESP_LOGI(TAG,
               "First WebSocket image received - interrupting boot animation");
      s_first_image_received = true;
    }

    // Ownership transferred to gfx
    s_webp = nullptr;
    s_ws_accumulated_len = 0;
  }
}
