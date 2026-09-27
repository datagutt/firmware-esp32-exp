#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Data buffers (HTTP bodies, WebSocket payloads, form posts) prefer PSRAM so
// internal RAM stays available for TLS sessions, WiFi and task stacks. Boards
// without PSRAM (pixoticker) fall back to internal RAM instead of failing.
// Free the result with free() or heap_caps_free().

static inline void* psram_or_internal_malloc(size_t size) {
  return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static inline void* psram_or_internal_realloc(void* ptr, size_t size) {
  return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Task stacks follow the same preference; the TCB always stays internal. Both
// paths allocate through the WithCaps API, so a task created here must be
// deleted with vTaskDeleteWithCaps(), never vTaskDelete().
//
// Only for tasks that never touch flash themselves: NVS reads and writes
// (config_set, diag_event_log), esp_partition and OTA writes all disable the
// cache, which makes a PSRAM stack unreachable and trips
// esp_task_stack_is_sane_cache_disabled(). Such tasks keep xTaskCreate().
static inline BaseType_t psram_or_internal_task_create(
    TaskFunction_t fn, const char* name, uint32_t stack_bytes, void* arg,
    UBaseType_t priority, TaskHandle_t* out, BaseType_t core) {
  if (xTaskCreatePinnedToCoreWithCaps(fn, name, stack_bytes, arg, priority,
                                      out, core,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) ==
      pdPASS) {
    return pdPASS;
  }
  return xTaskCreatePinnedToCoreWithCaps(fn, name, stack_bytes, arg, priority,
                                         out, core,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
