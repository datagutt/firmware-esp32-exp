#pragma once

#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Event categories
// ---------------------------------------------------------------------------

typedef enum {
  TRONBYT_EVENT_CATEGORY_SYSTEM = 1,
  TRONBYT_EVENT_CATEGORY_NETWORK = 2,
  TRONBYT_EVENT_CATEGORY_DISPLAY = 3,
  TRONBYT_EVENT_CATEGORY_OTA = 4,
} tronbyt_event_category_t;

// ---------------------------------------------------------------------------
// Event types
// ---------------------------------------------------------------------------

typedef enum {
  // System events (100–149)
  TRONBYT_EVENT_CONFIG_CHANGED = 100,
  TRONBYT_EVENT_STATE_CHANGED,
  TRONBYT_EVENT_CONNECTIVITY_CHANGED,
  TRONBYT_EVENT_OTA_SUBSTATE_CHANGED,

  // Network events (150–199)
  TRONBYT_EVENT_WIFI_CONNECTED = 150,
  TRONBYT_EVENT_WIFI_DISCONNECTED,
  TRONBYT_EVENT_WS_CONNECTED,
  TRONBYT_EVENT_WS_DISCONNECTED,

  // Display events (200–249)
  TRONBYT_EVENT_IMAGE_RECEIVED = 200,
  TRONBYT_EVENT_DISPLAY_ON,
  TRONBYT_EVENT_DISPLAY_OFF,
  TRONBYT_EVENT_BRIGHTNESS_CHANGED,

  // OTA events (250–299)
  TRONBYT_EVENT_OTA_STARTED = 250,
  TRONBYT_EVENT_OTA_PROGRESS,
  TRONBYT_EVENT_OTA_COMPLETE,
} tronbyt_event_type_t;

// ---------------------------------------------------------------------------
// Event struct
// ---------------------------------------------------------------------------

typedef struct {
  uint16_t type;
  uint16_t category;
  uint32_t timestamp_ms;
  union {
    int32_t i32;
    uint32_t u32;
    void* ptr;
  } payload;
} tronbyt_event_t;

// ---------------------------------------------------------------------------
// Handler callback
// ---------------------------------------------------------------------------

typedef void (*tronbyt_event_handler_t)(const tronbyt_event_t* event,
                                        void* ctx);

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/// Initialize the event bus (queue + dispatch task). Safe to call multiple
/// times; subsequent calls are no-ops.
esp_err_t event_bus_init(void);

/// Subscribe to a specific event type.
esp_err_t event_bus_subscribe(uint16_t event_type,
                              tronbyt_event_handler_t handler, void* ctx);

/// Subscribe to all events in a category.
esp_err_t event_bus_subscribe_category(uint16_t category,
                                       tronbyt_event_handler_t handler,
                                       void* ctx);

/// Remove a handler from all subscriptions.
void event_bus_unsubscribe(tronbyt_event_handler_t handler);

/// Emit a fully-constructed event. Non-blocking.
esp_err_t event_bus_emit(uint16_t event_type, const tronbyt_event_t* event);

/// Emit an event with no payload. Non-blocking.
esp_err_t event_bus_emit_simple(uint16_t event_type);

/// Emit an event with an i32 payload. Non-blocking.
esp_err_t event_bus_emit_i32(uint16_t event_type, int32_t value);

/// Emit an event with a pointer payload. Non-blocking.
esp_err_t event_bus_emit_ptr(uint16_t event_type, void* ptr);

#ifdef __cplusplus
}
#endif
