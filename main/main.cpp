#include <cstring>

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ap.h"
#include "app_state.h"
#include "console.h"
#include "display.h"
#include "diag_event_ring.h"
#include "event_bus.h"
#include "heap_monitor.h"
#include "http_server.h"
#include "mdns_service.h"
#include "webui_server.h"
#include "nvs_settings.h"
#include "ota.h"
#include "startup/runtime_orchestrator.h"
#include "sdkconfig.h"
#ifdef CONFIG_BOARD_TIDBYT_GEN2
#include "nvs_handle.h"
#include "scheduler.h"
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
// Switched off by touch. "Off" is a scheduler pause rather than brightness 0:
// the panel is blanked and playback stops while the brightness level stays
// whatever the user or server last set. Turning back on restores that level,
// and a server or API brightness change cannot relight a panel switched off
// here. Written by app_main before the touch_poll task starts, then only by
// that task.
bool s_user_off = false;

// Used when turning on a panel whose level is 0, e.g. dimmed to 0 by the
// server, or switched off by older firmware that stored brightness 0 for it.
constexpr uint8_t kTurnOnBrightness = 30;

// Persisted so a device switched off by touch stays off across a reboot.
constexpr const char* kTouchNvsNamespace = "touch";
constexpr const char* kTouchNvsKeyUserOff = "user_off";

bool user_off_load() {
  NvsHandle nvs(kTouchNvsNamespace, NVS_READONLY);
  uint8_t off = 0;
  return nvs && nvs.get_u8(kTouchNvsKeyUserOff, &off) == ESP_OK && off != 0;
}

void set_user_off(bool off) {
  s_user_off = off;
  if (off) {
    scheduler_pause(SCHEDULER_PAUSE_USER_OFF);
  } else {
    scheduler_resume(SCHEDULER_PAUSE_USER_OFF);
  }

  NvsHandle nvs(kTouchNvsNamespace, NVS_READWRITE);
  if (nvs.set_u8(kTouchNvsKeyUserOff, off ? 1 : 0) != ESP_OK ||
      nvs.commit() != ESP_OK) {
    ESP_LOGW(TAG, "Failed to persist display power state");
  }
}

void handle_touch_event(touch_event_t event) {
  ESP_LOGI(TAG, "Touch event: %s", touch_event_to_string(event));

  switch (event) {
    case TOUCH_EVENT_TAP:
      if (!s_user_off) {
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
      // Toggles what the panel looks like: at brightness 0 it looks off, so a
      // HOLD turns it on rather than switching it "off" first.
      if (s_user_off || display_get_brightness() == 0) {
        ESP_LOGI(TAG, "HOLD - Display ON");
        if (display_get_brightness() == 0) {
          display_set_brightness(kTurnOnBrightness);
        }
        if (s_user_off) set_user_off(false);
      } else {
        ESP_LOGI(TAG, "HOLD - Display OFF");
        set_user_off(true);
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
    vTaskDelay(pdMS_TO_TICKS(100));  // 100ms polling, responsive enough for tap/hold
  }
}

// touch_beep can change at runtime (WebSocket, config portal) and takes effect
// without a reboot, unlike disable_touch.
void on_config_changed(const tronbyt_event_t*, void*) {
  touch_control_set_beep(config_get().touch_beep);
}
#endif

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
  ESP_ERROR_CHECK(event_bus_init());
  app_state_init();
  diag_event_ring_init();
  console_init();
  heap_monitor_init();
  // Before WiFi starts, so no event the health check counts can be missed.
  ota_start_health_check(config_get().image_url[0] != '\0');

  ESP_LOGI(TAG, "Initializing WiFi manager...");
  if (wifi_initialize("", "")) {
    ESP_LOGE(TAG, "failed to initialize WiFi");
    return;
  }
  esp_register_shutdown_handler(&wifi_shutdown);
  http_server_init();
  webui_server_init();
  mdns_service_init();

  auto cfg = config_get();
  const char* image_url = (cfg.image_url[0] != '\0') ? cfg.image_url : nullptr;

  if (gfx_initialize(image_url)) {
    ESP_LOGE(TAG, "failed to initialize gfx");
    return;
  }
  esp_register_shutdown_handler(&display_shutdown);

#ifdef CONFIG_BOARD_TIDBYT_GEN2
  // Initialize touch controls (GPIO33 on Tidbyt Gen2). Skipping init entirely
  // when disabled also means the touch_poll task is never spawned, so there is
  // no polling overhead.
  if (!cfg.disable_touch) {
    ESP_LOGI(TAG, "Initializing touch control...");
    esp_err_t touch_ret = touch_control_init();
    if (touch_ret == ESP_OK) {
      ESP_LOGI(TAG, "Touch control ready on GPIO33");
      touch_control_debug_all_pads();
      touch_control_set_beep(cfg.touch_beep);
      event_bus_subscribe(TRONBYT_EVENT_CONFIG_CHANGED, on_config_changed,
                          nullptr);

      // Only restored while touch works: with touch disabled or broken there
      // would be no way to turn the panel back on.
      if (user_off_load()) {
        ESP_LOGI(TAG, "Display was switched off by touch; keeping it off");
        s_user_off = true;
        scheduler_pause(SCHEDULER_PAUSE_USER_OFF);
      }

      // Internal RAM (xTaskCreate), since a HOLD writes NVS. Sized for the
      // deepest path: a HOLD that sets the brightness and persists the power
      // state (NVS write + commit), with log formatting on top.
      xTaskCreate(touch_task, "touch_poll", 3072, nullptr, 2, nullptr);
    } else {
      ESP_LOGW(TAG, "Touch control init failed: %s (continuing without touch)",
               esp_err_to_name(touch_ret));
    }
  } else {
    ESP_LOGI(TAG, "Touch control disabled via NVS");
  }
#endif

  if (cfg.ap_mode) {
    ESP_LOGI(TAG, "Starting AP Web Server...");
    ap_start();
  }

  runtime_orchestrator_start(button_boot);

  // Keep app_main short-lived to free stack early (matrx-fw style handoff).
  ESP_LOGI(TAG, "Core init complete — deleting app_main task");
  vTaskDelete(nullptr);
}
