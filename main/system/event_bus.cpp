#include "event_bus.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

const char* TAG = "event_bus";

constexpr int EVENT_QUEUE_SIZE = 32;
constexpr int MAX_SUBSCRIBERS = 32;
constexpr uint16_t EVENT_TYPE_CATEGORY_ALL = 0xFFFF;
constexpr size_t DISPATCH_STACK_SIZE = 4096;
constexpr int DISPATCH_TASK_PRIORITY = 5;

struct Subscriber {
  uint16_t event_type;
  uint16_t category;
  tronbyt_event_handler_t handler;
  void* ctx;
};

struct EventBusState {
  QueueHandle_t queue = nullptr;
  Subscriber subscribers[MAX_SUBSCRIBERS] = {};
  size_t subscriber_count = 0;
  TaskHandle_t dispatch_task = nullptr;
  SemaphoreHandle_t mutex = nullptr;
  bool initialized = false;
};

EventBusState s_bus;

uint16_t event_type_to_category(uint16_t type) {
  if (type >= 100 && type < 150) return TRONBYT_EVENT_CATEGORY_SYSTEM;
  if (type >= 150 && type < 200) return TRONBYT_EVENT_CATEGORY_NETWORK;
  if (type >= 200 && type < 250) return TRONBYT_EVENT_CATEGORY_DISPLAY;
  if (type >= 250 && type < 300) return TRONBYT_EVENT_CATEGORY_OTA;
  return TRONBYT_EVENT_CATEGORY_SYSTEM;
}

void dispatch_task(void*) {
  tronbyt_event_t event;

  while (true) {
    if (xQueueReceive(s_bus.queue, &event, portMAX_DELAY) == pdTRUE) {
      xSemaphoreTake(s_bus.mutex, portMAX_DELAY);

      for (size_t i = 0; i < s_bus.subscriber_count; i++) {
        Subscriber& sub = s_bus.subscribers[i];
        bool match = (sub.event_type == event.type) ||
                     (sub.event_type == EVENT_TYPE_CATEGORY_ALL &&
                      sub.category == event.category);
        if (match && sub.handler) {
          sub.handler(&event, sub.ctx);
        }
      }

      xSemaphoreGive(s_bus.mutex);
    }
  }
}

esp_err_t emit_internal(uint16_t event_type, tronbyt_event_t* event) {
  if (!s_bus.initialized || !s_bus.queue || !event) {
    return ESP_ERR_INVALID_STATE;
  }

  event->type = event_type;
  if (event->category == 0) {
    event->category = event_type_to_category(event_type);
  }
  event->timestamp_ms =
      static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);

  return (xQueueSend(s_bus.queue, event, 0) == pdTRUE) ? ESP_OK
                                                        : ESP_ERR_TIMEOUT;
}

}  // namespace

esp_err_t event_bus_init(void) {
  if (s_bus.initialized) {
    return ESP_OK;
  }

  s_bus.queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(tronbyt_event_t));
  if (!s_bus.queue) {
    return ESP_ERR_NO_MEM;
  }

  s_bus.mutex = xSemaphoreCreateMutex();
  if (!s_bus.mutex) {
    vQueueDelete(s_bus.queue);
    s_bus.queue = nullptr;
    return ESP_ERR_NO_MEM;
  }

  // Try SPIRAM-backed stack first, fall back to internal RAM
  BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
      dispatch_task, "event_bus", DISPATCH_STACK_SIZE, nullptr,
      DISPATCH_TASK_PRIORITY, &s_bus.dispatch_task, tskNO_AFFINITY,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (rc != pdPASS) {
    rc = xTaskCreate(dispatch_task, "event_bus", DISPATCH_STACK_SIZE, nullptr,
                     DISPATCH_TASK_PRIORITY, &s_bus.dispatch_task);
  }

  if (rc != pdPASS) {
    vSemaphoreDelete(s_bus.mutex);
    s_bus.mutex = nullptr;
    vQueueDelete(s_bus.queue);
    s_bus.queue = nullptr;
    return ESP_ERR_NO_MEM;
  }

  s_bus.subscriber_count = 0;
  s_bus.initialized = true;
  ESP_LOGI(TAG, "Event bus initialized");
  return ESP_OK;
}

esp_err_t event_bus_subscribe(uint16_t event_type,
                              tronbyt_event_handler_t handler, void* ctx) {
  if (!s_bus.initialized || !handler) {
    return ESP_ERR_INVALID_STATE;
  }

  xSemaphoreTake(s_bus.mutex, portMAX_DELAY);
  if (s_bus.subscriber_count >= MAX_SUBSCRIBERS) {
    xSemaphoreGive(s_bus.mutex);
    ESP_LOGE(TAG, "Max subscribers reached (%d)", MAX_SUBSCRIBERS);
    return ESP_ERR_NO_MEM;
  }

  Subscriber& sub = s_bus.subscribers[s_bus.subscriber_count++];
  sub.event_type = event_type;
  sub.category = event_type_to_category(event_type);
  sub.handler = handler;
  sub.ctx = ctx;

  xSemaphoreGive(s_bus.mutex);
  return ESP_OK;
}

esp_err_t event_bus_subscribe_category(uint16_t category,
                                       tronbyt_event_handler_t handler,
                                       void* ctx) {
  if (!s_bus.initialized || !handler) {
    return ESP_ERR_INVALID_STATE;
  }

  xSemaphoreTake(s_bus.mutex, portMAX_DELAY);
  if (s_bus.subscriber_count >= MAX_SUBSCRIBERS) {
    xSemaphoreGive(s_bus.mutex);
    ESP_LOGE(TAG, "Max subscribers reached (%d)", MAX_SUBSCRIBERS);
    return ESP_ERR_NO_MEM;
  }

  Subscriber& sub = s_bus.subscribers[s_bus.subscriber_count++];
  sub.event_type = EVENT_TYPE_CATEGORY_ALL;
  sub.category = category;
  sub.handler = handler;
  sub.ctx = ctx;

  xSemaphoreGive(s_bus.mutex);
  return ESP_OK;
}

void event_bus_unsubscribe(tronbyt_event_handler_t handler) {
  if (!s_bus.initialized || !handler) {
    return;
  }

  xSemaphoreTake(s_bus.mutex, portMAX_DELAY);
  for (size_t i = 0; i < s_bus.subscriber_count; i++) {
    if (s_bus.subscribers[i].handler == handler) {
      for (size_t j = i; j < s_bus.subscriber_count - 1; j++) {
        s_bus.subscribers[j] = s_bus.subscribers[j + 1];
      }
      s_bus.subscriber_count--;
      break;
    }
  }
  xSemaphoreGive(s_bus.mutex);
}

esp_err_t event_bus_emit(uint16_t event_type, const tronbyt_event_t* event) {
  if (!event) {
    return ESP_ERR_INVALID_ARG;
  }
  tronbyt_event_t copy;
  memcpy(&copy, event, sizeof(copy));
  return emit_internal(event_type, &copy);
}

esp_err_t event_bus_emit_simple(uint16_t event_type) {
  tronbyt_event_t event = {};
  return emit_internal(event_type, &event);
}

esp_err_t event_bus_emit_i32(uint16_t event_type, int32_t value) {
  tronbyt_event_t event = {};
  event.payload.i32 = value;
  return emit_internal(event_type, &event);
}

esp_err_t event_bus_emit_ptr(uint16_t event_type, void* ptr) {
  tronbyt_event_t event = {};
  event.payload.ptr = ptr;
  return emit_internal(event_type, &event);
}
