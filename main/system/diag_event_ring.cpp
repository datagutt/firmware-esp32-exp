#include "diag_event_ring.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>

namespace {

const char* TAG = "diag_evt";
constexpr const char* NS = "diag_evt";
constexpr const char* KEY_HEAD = "head";
constexpr const char* KEY_COUNT = "count";
constexpr const char* KEY_SEQ = "seq";
constexpr const char* KEY_ENABLED = "enabled";
constexpr size_t RING_SIZE = 32;

SemaphoreHandle_t s_mutex = nullptr;
bool s_initialized = false;
uint8_t s_head = 0;
uint8_t s_count = 0;
uint32_t s_seq = 0;
bool s_enabled = false;

void sanitize(char* dst, size_t dst_len, const char* src) {
  if (!dst || dst_len == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t j = 0;
  for (size_t i = 0; src[i] != '\0' && j + 1 < dst_len; ++i) {
    char c = src[i];
    if (c == '|' || c == '\n' || c == '\r' || c == '\t') {
      dst[j++] = ' ';
    } else {
      dst[j++] = c;
    }
  }
  dst[j] = '\0';
}

void slot_key(uint8_t idx, char* key, size_t key_len) {
  snprintf(key, key_len, "e%02u", static_cast<unsigned>(idx));
}

bool parse_entry(const char* line, diag_event_t* out) {
  if (!line || !out) return false;

  char buffer[320];
  snprintf(buffer, sizeof(buffer), "%s", line);

  char* save = nullptr;
  char* tok = strtok_r(buffer, "|", &save);
  if (!tok) return false;
  out->seq = static_cast<uint32_t>(strtoul(tok, nullptr, 10));

  tok = strtok_r(nullptr, "|", &save);
  if (!tok) return false;
  out->uptime_ms = static_cast<uint32_t>(strtoul(tok, nullptr, 10));

  tok = strtok_r(nullptr, "|", &save);
  if (!tok) return false;
  out->code = static_cast<int32_t>(strtol(tok, nullptr, 10));

  tok = strtok_r(nullptr, "|", &save);
  if (!tok) return false;
  snprintf(out->level, sizeof(out->level), "%s", tok);

  tok = strtok_r(nullptr, "|", &save);
  if (!tok) return false;
  snprintf(out->type, sizeof(out->type), "%s", tok);

  tok = strtok_r(nullptr, "", &save);
  if (!tok) tok = const_cast<char*>("");
  snprintf(out->message, sizeof(out->message), "%s", tok);

  return true;
}

void persist_meta(nvs_handle_t h) {
  nvs_set_u8(h, KEY_HEAD, s_head);
  nvs_set_u8(h, KEY_COUNT, s_count);
  nvs_set_u32(h, KEY_SEQ, s_seq);
}

void load_entry_by_ring_index(size_t ring_index, diag_event_t* out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));

  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;

  char key[8];
  slot_key(static_cast<uint8_t>(ring_index % RING_SIZE), key, sizeof(key));

  size_t len = 0;
  if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0 ||
      len > 320) {
    nvs_close(h);
    return;
  }

  char buf[320];
  if (nvs_get_str(h, key, buf, &len) == ESP_OK) {
    parse_entry(buf, out);
  }

  nvs_close(h);
}

size_t collect_entries(const char* type_filter, bool prefix_match,
                       diag_event_t* out, size_t max_events) {
  if (!out || max_events == 0) return 0;

  size_t copied = 0;
  for (size_t i = 0; i < s_count && copied < max_events; ++i) {
    size_t idx = (s_head + RING_SIZE - 1 - i) % RING_SIZE;
    diag_event_t entry = {};
    load_entry_by_ring_index(idx, &entry);
    if (entry.seq == 0 && entry.uptime_ms == 0 && entry.type[0] == '\0') {
      continue;
    }
    if (type_filter && type_filter[0] != '\0' &&
        !prefix_match) {
      if (strcmp(type_filter, entry.type) != 0) continue;
    }
    if (type_filter && type_filter[0] != '\0' && prefix_match) {
      if (strncmp(entry.type, type_filter, strlen(type_filter)) != 0) continue;
    }
    out[copied++] = entry;
  }
  return copied;
}

}  // namespace

void diag_event_ring_init(void) {
  if (s_initialized) return;

  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) {
    ESP_LOGE(TAG, "Failed to create mutex");
    return;
  }

  if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;

  nvs_handle_t h;
  esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    xSemaphoreGive(s_mutex);
    ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
    return;
  }

  nvs_get_u8(h, KEY_HEAD, &s_head);
  nvs_get_u8(h, KEY_COUNT, &s_count);
  nvs_get_u32(h, KEY_SEQ, &s_seq);
  uint8_t enabled_u8 = 0;
  if (nvs_get_u8(h, KEY_ENABLED, &enabled_u8) == ESP_OK) {
    s_enabled = (enabled_u8 != 0);
  } else {
    // Default to off to reduce flash wear unless explicitly enabled.
    s_enabled = false;
    nvs_set_u8(h, KEY_ENABLED, 0);
    nvs_commit(h);
  }

  if (s_head >= RING_SIZE) s_head = 0;
  if (s_count > RING_SIZE) s_count = 0;

  nvs_close(h);
  s_initialized = true;
  xSemaphoreGive(s_mutex);

  ESP_LOGI(TAG, "Initialized event ring (enabled=%d head=%u count=%u seq=%lu)",
           s_enabled,
           static_cast<unsigned>(s_head), static_cast<unsigned>(s_count),
           static_cast<unsigned long>(s_seq));
}

void diag_event_log(const char* level, const char* type, int32_t code,
                    const char* message) {
  if (!s_initialized) {
    diag_event_ring_init();
  }
  if (!s_mutex || !s_enabled) return;

  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return;
  }

  nvs_handle_t h;
  esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    xSemaphoreGive(s_mutex);
    return;
  }

  char safe_level[DIAG_EVENT_LEVEL_MAX_LEN + 1];
  char safe_type[DIAG_EVENT_TYPE_MAX_LEN + 1];
  char safe_msg[DIAG_EVENT_MESSAGE_MAX_LEN + 1];
  sanitize(safe_level, sizeof(safe_level), level ? level : "INFO");
  sanitize(safe_type, sizeof(safe_type), type ? type : "event");
  sanitize(safe_msg, sizeof(safe_msg), message ? message : "");

  ++s_seq;
  uint32_t uptime_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);

  char line[320];
  snprintf(line, sizeof(line), "%lu|%lu|%ld|%s|%s|%s",
           static_cast<unsigned long>(s_seq),
           static_cast<unsigned long>(uptime_ms),
           static_cast<long>(code), safe_level, safe_type, safe_msg);

  char key[8];
  slot_key(s_head, key, sizeof(key));
  nvs_set_str(h, key, line);

  s_head = static_cast<uint8_t>((s_head + 1) % RING_SIZE);
  if (s_count < RING_SIZE) {
    s_count++;
  }

  persist_meta(h);
  nvs_commit(h);
  nvs_close(h);

  xSemaphoreGive(s_mutex);
}

void diag_event_ring_set_enabled(bool enabled) {
  if (!s_initialized) {
    diag_event_ring_init();
  }
  if (!s_mutex) return;

  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, KEY_ENABLED, enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
  }
  s_enabled = enabled;
  xSemaphoreGive(s_mutex);
}

bool diag_event_ring_is_enabled(void) {
  if (!s_initialized) {
    diag_event_ring_init();
  }
  return s_enabled;
}

size_t diag_event_get_recent(diag_event_t* out, size_t max_events) {
  if (!s_initialized) {
    diag_event_ring_init();
  }
  if (!s_mutex) return 0;

  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return 0;
  }

  size_t copied = collect_entries(nullptr, false, out, max_events);
  xSemaphoreGive(s_mutex);
  return copied;
}

size_t diag_event_get_recent_by_type(const char* type, diag_event_t* out,
                                     size_t max_events) {
  if (!s_initialized) {
    diag_event_ring_init();
  }
  if (!s_mutex) return 0;

  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return 0;
  }

  size_t copied = collect_entries(type, false, out, max_events);
  xSemaphoreGive(s_mutex);
  return copied;
}

size_t diag_event_get_recent_by_prefix(const char* prefix, diag_event_t* out,
                                       size_t max_events) {
  if (!s_initialized) {
    diag_event_ring_init();
  }
  if (!s_mutex) return 0;

  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return 0;
  }

  size_t copied = collect_entries(prefix, true, out, max_events);
  xSemaphoreGive(s_mutex);
  return copied;
}
