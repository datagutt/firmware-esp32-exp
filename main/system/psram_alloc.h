#pragma once

#include <stddef.h>

#include <esp_heap_caps.h>

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
