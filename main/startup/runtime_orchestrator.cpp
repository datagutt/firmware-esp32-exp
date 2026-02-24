#include "runtime_orchestrator.h"

#include <cstring>

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "ap.h"
#include "app_state.h"
#include "display.h"
#include "event_bus.h"
#include "heap_monitor.h"
#include "ntp.h"
#include "nvs_settings.h"
#include "scheduler.h"
#include "sockets.h"
#include "sta_api.h"
#include "syslog.h"
#include "webp_player.h"
#include "webui_server.h"
#include "wifi.h"

namespace {

const char* TAG = "startup";
constexpr uint32_t RUNTIME_TASK_STACK_SIZE = 6144;
constexpr int RUNTIME_TASK_PRIORITY = 5;
constexpr EventBits_t CONFIG_CHANGED_BIT = BIT0;

bool s_button_boot = false;
EventGroupHandle_t s_config_event_group = nullptr;

void on_config_changed(const tronbyt_event_t*, void*) {
  if (s_config_event_group) {
    xEventGroupSetBits(s_config_event_group, CONFIG_CHANGED_BIT);
  }
}

void runtime_task(void*) {
  auto cfg = config_get();

  uint8_t mac[6];
  if (!wifi_get_mac(mac)) {
    ESP_LOGI(TAG, "WiFi MAC: %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
  }

  bool sta_connected = wifi_is_connected();
  if (sta_connected) {
    ESP_LOGI(TAG, "WiFi connected successfully!");

    if (config_get().prefer_ipv6) {
      ESP_LOGI(TAG, "IPv6 preference enabled, waiting for global address...");
      if (wifi_wait_for_ipv6(5000)) {
        ESP_LOGI(TAG, "IPv6 Ready!");
      } else {
        ESP_LOGI(TAG,
                 "IPv6 not available or timed out, proceeding with existing "
                 "connection (IPv4)");
      }
    }
  } else {
    ESP_LOGW(TAG, "WiFi not yet connected - continuing with event-driven startup");
  }

  ntp_init();

  auto syslog_cfg = config_get();
  if (strlen(syslog_cfg.syslog_addr) > 0) {
    syslog_init(syslog_cfg.syslog_addr);
  }

  sta_api_start();

  // Determine whether we need the captive-portal setup flow.
  bool need_setup = false;
  if (cfg.ap_mode) {
    bool has_wifi_creds = (strlen(cfg.ssid) > 0);
    if (s_button_boot || (!sta_connected && !has_wifi_creds)) {
      need_setup = true;
      ESP_LOGW(TAG, "Boot button pressed or no WiFi credentials configured");
      app_state_enter_config_portal();
      ESP_LOGI(TAG, "Loading Config WEBP");
      if (gfx_display_asset("config")) {
        ESP_LOGE(TAG, "Failed to display config screen - continuing without it");
      }
    }
  } else {
    if (!sta_connected) {
      ESP_LOGW(TAG,
               "WiFi didn't connect and AP mode is disabled - check secrets");
    } else if (s_button_boot) {
      ESP_LOGW(TAG,
               "Boot button pressed but AP mode disabled; skipping "
               "configuration portal");
    }
  }

  // Register the wildcard catch-all AFTER all API and specific routes so
  // that /* does not shadow /api/* handlers (httpd matches by registration
  // order).  Only ONE wildcard GET handler can exist.  Use the AP
  // captive-portal wildcard during setup (needs redirects to work), and
  // the webui handler for normal operation.
  if (need_setup) {
    ap_register_wildcard();
  } else {
    webui_register_wildcard();
  }

  if (s_button_boot) {
    if (!cfg.ap_mode) {
      ESP_LOGW(TAG,
               "Boot button pressed but AP mode disabled; skipping "
               "configuration wait");
    } else {
      ESP_LOGW(TAG,
               "Boot button pressed - waiting for configuration or timeout...");
      EventBits_t bits = xEventGroupWaitBits(
          s_config_event_group, CONFIG_CHANGED_BIT, pdTRUE, pdTRUE,
          pdMS_TO_TICKS(120000));
      if (bits & CONFIG_CHANGED_BIT) {
        ESP_LOGI(TAG, "Configuration received - proceeding");
      }
    }
  }

  if (cfg.ap_mode) {
    ap_start_shutdown_timer();
  }

  const char* image_url = nullptr;
  while (true) {
    cfg = config_get();
    image_url = (cfg.image_url[0] != '\0') ? cfg.image_url : nullptr;
    if (image_url && strlen(image_url) > 0) {
      break;
    }
    ESP_LOGW(TAG, "Image URL is not set. Waiting for configuration...");
    xEventGroupWaitBits(s_config_event_group, CONFIG_CHANGED_BIT, pdTRUE,
                        pdTRUE, pdMS_TO_TICKS(5000));
  }

  ESP_LOGI(TAG, "Proceeding with image URL: %s", image_url);

  if (cfg.api_key[0] != '\0') {
    size_t key_len = strlen(cfg.api_key);
    ESP_LOGI(TAG, "API key: ...%s",
             cfg.api_key + (key_len > 4 ? key_len - 4 : 0));
  }

  heap_monitor_log_status("pre-connect");

  app_state_enter_normal();
  scheduler_init();
  if (strncmp(image_url, "ws://", 5) == 0 ||
      strncmp(image_url, "wss://", 6) == 0) {
    ESP_LOGI(TAG, "Using websockets with URL: %s", image_url);
    sockets_init(image_url);
    scheduler_start_ws();
  } else {
    ESP_LOGI(TAG, "Using HTTP polling with URL: %s", image_url);
    scheduler_start_http(image_url);
  }

  ESP_LOGI(TAG, "Runtime setup complete — deleting runtime task");
  vTaskDelete(nullptr);
}

}  // namespace

void runtime_orchestrator_start(bool button_boot) {
  s_button_boot = button_boot;

  s_config_event_group = xEventGroupCreate();
  event_bus_subscribe(TRONBYT_EVENT_CONFIG_CHANGED, on_config_changed, nullptr);

  BaseType_t ret = xTaskCreate(runtime_task, "runtime_coord",
                               RUNTIME_TASK_STACK_SIZE, nullptr,
                               RUNTIME_TASK_PRIORITY, nullptr);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create runtime coordinator task");
  }
}
