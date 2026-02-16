#include "runtime_orchestrator.h"

#include <cstring>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include "ap.h"
#include "display.h"
#include "heap_monitor.h"
#include "ntp.h"
#include "nvs_settings.h"
#include "scheduler.h"
#include "sockets.h"
#include "sta_api.h"
#include "syslog.h"
#include "webp_player.h"
#include "wifi.h"

namespace {

const char* TAG = "startup";
constexpr uint32_t RUNTIME_TASK_STACK_SIZE = 6144;
constexpr int RUNTIME_TASK_PRIORITY = 5;

bool s_button_boot = false;
bool s_config_received = false;

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

  if (cfg.ap_mode) {
    ap_register_wildcard();
  }

  if (cfg.ap_mode) {
    if (s_button_boot || !sta_connected) {
      ESP_LOGW(TAG, "WiFi didn't connect or Boot Button Pressed");
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

  if (s_button_boot) {
    if (!cfg.ap_mode) {
      ESP_LOGW(TAG,
               "Boot button pressed but AP mode disabled; skipping "
               "configuration wait");
    } else {
      ESP_LOGW(TAG,
               "Boot button pressed - waiting for configuration or timeout...");
      int config_wait_counter = 0;
      while (config_wait_counter < 120) {
        if (s_config_received) {
          ESP_LOGI(TAG, "Configuration received - proceeding");
          break;
        }
        config_wait_counter++;
        vTaskDelay(pdMS_TO_TICKS(1000));
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
    vTaskDelay(pdMS_TO_TICKS(5000));
  }

  ESP_LOGI(TAG, "Proceeding with image URL: %s", image_url);
  heap_monitor_log_status("pre-connect");

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

void runtime_orchestrator_on_config_saved() {
  s_config_received = true;
  ESP_LOGI(TAG, "Configuration saved - signaling runtime task");
}

void runtime_orchestrator_start(bool button_boot) {
  s_button_boot = button_boot;
  s_config_received = false;

  BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
      runtime_task, "runtime_coord", RUNTIME_TASK_STACK_SIZE, nullptr,
      RUNTIME_TASK_PRIORITY, nullptr, tskNO_AFFINITY,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ret != pdPASS) {
    ESP_LOGW(TAG, "PSRAM task creation failed, retrying with internal RAM");
    ret = xTaskCreate(runtime_task, "runtime_coord",
                      RUNTIME_TASK_STACK_SIZE, nullptr,
                      RUNTIME_TASK_PRIORITY, nullptr);
  }
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create runtime coordinator task");
  }
}
