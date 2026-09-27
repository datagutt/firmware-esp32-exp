#include "mdns_service.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <mdns.h>

#include "board_caps.h"
#include "event_bus.h"
#include "nvs_settings.h"
#include "sdkconfig.h"
#include "wifi.h"

namespace {

const char* TAG = "mdns";

bool s_running = false;

void start_mdns() {
  if (s_running) return;

  esp_err_t ret = mdns_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(ret));
    return;
  }

  char hostname[MAX_HOSTNAME_LEN + 1];
  config_get_hostname(hostname, sizeof(hostname));
  mdns_hostname_set(hostname);

  const esp_app_desc_t* app = esp_app_get_description();

  // Stable device id (STA MAC as hex) so the server and tooling can identify
  // devices on the LAN before any connection is made.
  char device_id[13] = "unknown";
  uint8_t mac[6];
  if (wifi_get_mac(mac) == 0) {
    snprintf(device_id, sizeof(device_id), "%02x%02x%02x%02x%02x%02x", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
  }

  mdns_txt_item_t txt[] = {
      {"model", BOARD_MODEL_NAME},
      {"version", app->version},
      {"id", device_id},
  };

  ret = mdns_service_add(nullptr, "_" CONFIG_BRAND_NAME_LOWER, "_tcp", 80, txt,
                         3);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "mdns_service_add failed: %s", esp_err_to_name(ret));
    mdns_free();
    return;
  }

  s_running = true;
  ESP_LOGI(TAG, "mDNS started: %s.local", hostname);
}

void stop_mdns() {
  if (!s_running) return;

  mdns_free();
  s_running = false;
  ESP_LOGI(TAG, "mDNS stopped");
}

void on_wifi_event(const tronbyt_event_t* event, void*) {
  if (event->type == TRONBYT_EVENT_WIFI_CONNECTED) {
    start_mdns();
  } else if (event->type == TRONBYT_EVENT_WIFI_DISCONNECTED) {
    stop_mdns();
  }
}

}  // namespace

void mdns_service_init(void) {
  event_bus_subscribe(TRONBYT_EVENT_WIFI_CONNECTED, on_wifi_event, nullptr);
  event_bus_subscribe(TRONBYT_EVENT_WIFI_DISCONNECTED, on_wifi_event, nullptr);
  ESP_LOGI(TAG, "mDNS event handlers registered");
}
