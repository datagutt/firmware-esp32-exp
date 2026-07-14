#include "http_slot.h"

#include <cinttypes>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

const char* TAG = "http_slot";

// Depth-1 counting semaphore used as a mutual-exclusion slot. A counting
// semaphore (rather than a FreeRTOS mutex) is deliberate: OTA sets its
// in-progress flag before acquiring and the lower-priority poll path checks
// that flag and yields, so ownership/priority-inheritance semantics are not
// needed here, and the slot never has to be given by a different task than the
// one that took it in practice.
//
// Created via a Meyers singleton so the first caller lazily and thread-safely
// constructs it (GCC guards function-local statics), avoiding any dependency on
// a global init call site or static-init ordering across translation units.
SemaphoreHandle_t slot() {
  static SemaphoreHandle_t s = xSemaphoreCreateCounting(1, 1);
  return s;
}

}  // namespace

extern "C" bool http_slot_acquire(const char* tag, uint32_t timeout_ms) {
  SemaphoreHandle_t s = slot();
  if (!s) {
    ESP_LOGE(TAG, "slot semaphore unavailable (alloc failed)");
    return false;
  }

  // Fast path: take it immediately when uncontended, so the contention log
  // only fires when a caller actually has to wait.
  if (xSemaphoreTake(s, 0) == pdTRUE) {
    return true;
  }

  const char* who = tag ? tag : "?";
  ESP_LOGI(TAG, "slot busy, '%s' waiting up to %" PRIu32 " ms", who,
           timeout_ms);
  int64_t start_us = esp_timer_get_time();

  if (xSemaphoreTake(s, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    ESP_LOGW(TAG, "slot acquire timed out for '%s' after %" PRIu32 " ms", who,
             timeout_ms);
    return false;
  }

  int64_t waited_ms = (esp_timer_get_time() - start_us) / 1000;
  ESP_LOGI(TAG, "slot acquired by '%s' after waiting %" PRId64 " ms", who,
           waited_ms);
  return true;
}

extern "C" void http_slot_release(void) {
  SemaphoreHandle_t s = slot();
  if (s) {
    xSemaphoreGive(s);
  }
}
