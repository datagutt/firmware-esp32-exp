#include <cstring>

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ap.h"
#include "console.h"
#include "display.h"
#include "diag_event_ring.h"
#include "heap_monitor.h"
#include "http_server.h"
#include "mdns_service.h"
#include "nvs_settings.h"
#include "startup/runtime_orchestrator.h"
#include "sdkconfig.h"
#ifdef CONFIG_BOARD_TIDBYT_GEN2
#include "touch_control.h"
#endif
#include "version.h"
#include "webp_player.h"
#include "wifi.h"

#if CONFIG_BUTTON_PIN >= 0
#include <driver/gpio.h>
#endif

namespace {

const char* TAG = "main";
bool button_boot = false;

#ifdef CONFIG_BOARD_TIDBYT_GEN2
// Touch control state
bool display_power_on = true;
uint8_t saved_brightness = 30;

void handle_touch_event(touch_event_t event) {
  ESP_LOGI(TAG, "Touch event: %s", touch_event_to_string(event));

  switch (event) {
    case TOUCH_EVENT_TAP:
      if (display_power_on) {
        ESP_LOGI(TAG, "TAP - skip to next app");
        gfx_interrupt();
      } else {
        ESP_LOGI(TAG, "TAP ignored - display is off (hold to turn on)");
      }
      break;

    case TOUCH_EVENT_DOUBLE_TAP:
      // Reserved for future use
      ESP_LOGI(TAG, "DOUBLE TAP - no action assigned");
      break;

    case TOUCH_EVENT_HOLD:
      display_power_on = !display_power_on;

      if (display_power_on) {
        ESP_LOGI(TAG, "HOLD - Display ON");
        display_set_brightness(saved_brightness);
        gfx_start();
      } else {
        ESP_LOGI(TAG, "HOLD - Display OFF");
        saved_brightness = 30;
        display_set_brightness(0);
        gfx_stop();
      }
      break;

    default:
      break;
  }
}

void touch_task(void*) {
  while (true) {
    touch_event_t event = touch_control_check();
    if (event != TOUCH_EVENT_NONE) {
      handle_touch_event(event);
    }
    vTaskDelay(pdMS_TO_TICKS(50));  // 50ms = responsive touch
  }
}
#endif

}  // namespace

#ifdef CONFIG_BOARD_TIDBYT_GEN2
void touch_on_brightness_set(uint8_t brightness) {
  display_power_on = true;
  saved_brightness = brightness;
}
#endif

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "App Main Start");

#if CONFIG_BUTTON_PIN >= 0
  gpio_config_t button_config = {.pin_bit_mask = (1ULL << CONFIG_BUTTON_PIN),
                                 .mode = GPIO_MODE_INPUT,
                                 .pull_up_en = GPIO_PULLUP_ENABLE,
                                 .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                 .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&button_config);

  button_boot = (gpio_get_level(static_cast<gpio_num_t>(CONFIG_BUTTON_PIN)) == 0);

  if (button_boot) {
    ESP_LOGI(TAG, "Boot button pressed - forcing configuration mode");
  } else {
    ESP_LOGI(TAG, "Boot button not pressed");
  }
#else
  ESP_LOGI(TAG, "No button pin defined - skipping button check");
#endif

  ESP_LOGI(TAG, "Check for button press");

  ESP_ERROR_CHECK(nvs_settings_init());
  diag_event_ring_init();
  console_init();
  heap_monitor_init();

  ESP_LOGI(TAG, "Initializing WiFi manager...");
  if (wifi_initialize("", "")) {
    ESP_LOGE(TAG, "failed to initialize WiFi");
    return;
  }
  esp_register_shutdown_handler(&wifi_shutdown);
  http_server_init();
  mdns_service_init();

  auto cfg = config_get();
  const char* image_url = (cfg.image_url[0] != '\0') ? cfg.image_url : nullptr;

  if (gfx_initialize(image_url)) {
    ESP_LOGE(TAG, "failed to initialize gfx");
    return;
  }
  esp_register_shutdown_handler(&display_shutdown);

#ifdef CONFIG_BOARD_TIDBYT_GEN2
  // Initialize touch controls (GPIO33 on Tidbyt Gen2)
  ESP_LOGI(TAG, "Initializing touch control...");
  esp_err_t touch_ret = touch_control_init();
  if (touch_ret == ESP_OK) {
    ESP_LOGI(TAG, "Touch control ready on GPIO33");
    touch_control_debug_all_pads();

    xTaskCreate(touch_task, "touch_poll", 2048, nullptr, 2, nullptr);
  } else {
    ESP_LOGW(TAG, "Touch control init failed: %s (continuing without touch)",
             esp_err_to_name(touch_ret));
  }
#endif

  if (cfg.ap_mode) {
    ESP_LOGI(TAG, "Starting AP Web Server...");
    ap_start();
  }

  wifi_register_config_callback(runtime_orchestrator_on_config_saved);
  runtime_orchestrator_start(button_boot);

  // Keep app_main short-lived to free stack early (matrx-fw style handoff).
  ESP_LOGI(TAG, "Core init complete — deleting app_main task");
  vTaskDelete(nullptr);
}
