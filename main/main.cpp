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
#include "version.h"
#include "webp_player.h"
#include "wifi.h"

#if CONFIG_BUTTON_PIN >= 0
#include <driver/gpio.h>
#endif

namespace {

const char* TAG = "main";
bool button_boot = false;

}  // namespace

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
