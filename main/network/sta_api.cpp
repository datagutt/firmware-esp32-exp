#include "sta_api.h"

#include <cstring>

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "embedded_tz_db.h"
#include "api_validation.h"
#include "device_temperature.h"
#include "diag_event_ring.h"
#include "heap_monitor.h"
#include "http_server.h"
#include "mdns_service.h"
#include "ntp.h"
#include "nvs_settings.h"
#include "version.h"
#include "webp_player.h"
#include "wifi.h"

namespace {

const char* TAG = "sta_api";

const char* reset_reason_to_string(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:
      return "unknown";
    case ESP_RST_POWERON:
      return "poweron";
    case ESP_RST_EXT:
      return "external";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt_wdt";
    case ESP_RST_TASK_WDT:
      return "task_wdt";
    case ESP_RST_WDT:
      return "other_wdt";
    case ESP_RST_DEEPSLEEP:
      return "deepsleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    default:
      return "unmapped";
  }
}

// ── Existing endpoints ─────────────────────────────────────────────

esp_err_t status_handler(httpd_req_t* req) {
  cJSON* root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_FAIL;
  }

  cJSON_AddStringToObject(root, "firmware_version", FIRMWARE_VERSION);

  uint8_t mac[6];
  if (wifi_get_mac(mac) == 0) {
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "mac", mac_str);
  }

  heap_snapshot_t snap;
  heap_monitor_get_snapshot(&snap);
  cJSON_AddNumberToObject(root, "free_heap",
                          static_cast<double>(snap.internal_free));
  cJSON_AddNumberToObject(root, "free_spiram",
                          static_cast<double>(snap.spiram_free));
  cJSON_AddNumberToObject(root, "min_free_heap",
                          static_cast<double>(snap.internal_min));
  cJSON_AddNumberToObject(root, "images_loaded", gfx_get_loaded_counter());
  cJSON_AddBoolToObject(root, "diag_events_enabled",
                        diag_event_ring_is_enabled());

  float temp_c = 0.0f;
  if (device_temperature_get_c(&temp_c)) {
    cJSON_AddNumberToObject(root, "temperature_c", temp_c);
  } else {
    cJSON_AddNullToObject(root, "temperature_c");
  }

  char* json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  free(json);
  return ESP_OK;
}

esp_err_t diag_handler(httpd_req_t* req) {
  cJSON* root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_FAIL;
  }

  cJSON_AddStringToObject(root, "reboot_reason",
                          reset_reason_to_string(esp_reset_reason()));
  cJSON_AddBoolToObject(root, "diag_events_enabled",
                        diag_event_ring_is_enabled());

  float temp_c = 0.0f;
  if (device_temperature_get_c(&temp_c)) {
    cJSON_AddNumberToObject(root, "temperature_c", temp_c);
  } else {
    cJSON_AddNullToObject(root, "temperature_c");
  }

  wifi_diag_stats_t wifi_stats = {};
  wifi_get_diag_stats(&wifi_stats);
  cJSON* wifi_obj = cJSON_CreateObject();
  if (wifi_obj) {
    cJSON_AddBoolToObject(wifi_obj, "connected", wifi_stats.connected);
    cJSON_AddBoolToObject(wifi_obj, "connection_given_up",
                          wifi_stats.connection_given_up);
    cJSON_AddNumberToObject(wifi_obj, "reconnect_attempts",
                            wifi_stats.reconnect_attempts);
    cJSON_AddNumberToObject(wifi_obj, "disconnect_events",
                            wifi_stats.disconnect_events);
    cJSON_AddNumberToObject(wifi_obj, "health_disconnect_checks",
                            wifi_stats.health_disconnect_checks);
    cJSON_AddItemToObject(root, "wifi", wifi_obj);
  }

  // Heap-allocate large arrays to avoid stack overflow in httpd task
  constexpr size_t kTrendMax = 12;
  constexpr size_t kEventsMax = 16;
  constexpr size_t kOtaEventsMax = 8;

  auto* trend = static_cast<heap_trend_point_t*>(
      calloc(kTrendMax, sizeof(heap_trend_point_t)));
  auto* events =
      static_cast<diag_event_t*>(calloc(kEventsMax, sizeof(diag_event_t)));
  auto* ota_events =
      static_cast<diag_event_t*>(calloc(kOtaEventsMax, sizeof(diag_event_t)));

  if (trend) {
    size_t trend_count = heap_monitor_get_trend(trend, kTrendMax);
    cJSON* heap_arr = cJSON_CreateArray();
    if (heap_arr) {
      for (size_t i = 0; i < trend_count; ++i) {
        cJSON* p = cJSON_CreateObject();
        if (!p) continue;
        cJSON_AddNumberToObject(p, "uptime_ms", trend[i].uptime_ms);
        cJSON_AddNumberToObject(p, "internal_free", trend[i].internal_free);
        cJSON_AddNumberToObject(p, "internal_min", trend[i].internal_min);
        cJSON_AddNumberToObject(p, "spiram_free", trend[i].spiram_free);
        cJSON_AddNumberToObject(p, "spiram_min", trend[i].spiram_min);
        cJSON_AddItemToArray(heap_arr, p);
      }
      cJSON_AddItemToObject(root, "heap_trend", heap_arr);
    }
  }

  if (events) {
    size_t ev_count = diag_event_get_recent(events, kEventsMax);
    cJSON* ev_arr = cJSON_CreateArray();
    if (ev_arr) {
      for (size_t i = 0; i < ev_count; ++i) {
        cJSON* e = cJSON_CreateObject();
        if (!e) continue;
        cJSON_AddNumberToObject(e, "seq", events[i].seq);
        cJSON_AddNumberToObject(e, "uptime_ms", events[i].uptime_ms);
        cJSON_AddStringToObject(e, "level", events[i].level);
        cJSON_AddStringToObject(e, "type", events[i].type);
        cJSON_AddNumberToObject(e, "code", events[i].code);
        cJSON_AddStringToObject(e, "message", events[i].message);
        cJSON_AddItemToArray(ev_arr, e);
      }
      cJSON_AddItemToObject(root, "recent_events", ev_arr);
    }
  }

  if (ota_events) {
    size_t ota_count =
        diag_event_get_recent_by_prefix("ota_", ota_events, kOtaEventsMax);
    cJSON* ota_arr = cJSON_CreateArray();
    if (ota_arr) {
      for (size_t i = 0; i < ota_count; ++i) {
        cJSON* e = cJSON_CreateObject();
        if (!e) continue;
        cJSON_AddNumberToObject(e, "seq", ota_events[i].seq);
        cJSON_AddStringToObject(e, "type", ota_events[i].type);
        cJSON_AddNumberToObject(e, "code", ota_events[i].code);
        cJSON_AddStringToObject(e, "message", ota_events[i].message);
        cJSON_AddItemToArray(ota_arr, e);
      }
      cJSON_AddItemToObject(root, "ota_history", ota_arr);
    }
  }

  free(trend);
  free(events);
  free(ota_events);

  char* json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  free(json);
  return ESP_OK;
}

esp_err_t health_handler(httpd_req_t* req) {
  bool connected = wifi_is_connected();
  const char* resp =
      connected ? "{\"status\":\"ok\"}" : "{\"status\":\"degraded\"}";
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_status(req, connected ? "200 OK" : "503 Service Unavailable");
  httpd_resp_sendstr(req, resp);
  return ESP_OK;
}

// ── New endpoints (ported from kd_common) ──────────────────────────

esp_err_t about_handler(httpd_req_t* req) {
  const esp_app_desc_t* app = esp_app_get_description();

  cJSON* root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_FAIL;
  }

  cJSON_AddStringToObject(root, "model", mdns_board_model());
  cJSON_AddStringToObject(root, "type", "tronbyt");
  cJSON_AddStringToObject(root, "version", app->version);

  char* json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  free(json);
  return ESP_OK;
}

esp_err_t system_config_get_handler(httpd_req_t* req) {
  auto cfg = config_get();

  cJSON* root = cJSON_CreateObject();
  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_FAIL;
  }

  cJSON_AddBoolToObject(root, "auto_timezone", ntp_get_auto_timezone());
  cJSON_AddStringToObject(root, "timezone", ntp_get_timezone());
  cJSON_AddStringToObject(root, "ntp_server", ntp_get_server());
  cJSON_AddStringToObject(root, "hostname", cfg.hostname);
  cJSON_AddBoolToObject(root, "diag_events_enabled",
                        diag_event_ring_is_enabled());

  char* json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  free(json);
  return ESP_OK;
}

esp_err_t system_config_post_handler(httpd_req_t* req) {
  char content[512];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
      httpd_resp_send_408(req);
    } else {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "receive error");
    }
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON* json = cJSON_Parse(content);
  if (!json) {
    diag_event_log("WARN", "json_parse_error", -1,
                   "system/config payload parse failed");
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  const char* const kAllowedKeys[] = {"auto_timezone", "timezone", "ntp_server",
                                      "hostname", "diag_events_enabled"};
  char validation_err[128] = {0};
  if (!api_validate_no_unknown_keys(json, kAllowedKeys,
                                    sizeof(kAllowedKeys) /
                                        sizeof(kAllowedKeys[0]),
                                    validation_err,
                                    sizeof(validation_err))) {
    diag_event_log("WARN", "json_validation_error", -1, validation_err);
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, validation_err);
    return ESP_FAIL;
  }

  bool auto_tz_value = false;
  bool has_auto_tz = false;
  const char* timezone_value = nullptr;
  bool has_timezone = false;
  const char* ntp_value = nullptr;
  bool has_ntp = false;
  const char* hostname_value = nullptr;
  bool has_hostname = false;
  bool diag_enabled = false;
  bool has_diag_enabled = false;

  if (!api_validate_optional_bool(json, "auto_timezone", &auto_tz_value,
                                  &has_auto_tz, validation_err,
                                  sizeof(validation_err)) ||
      !api_validate_optional_string(json, "timezone", 1, 63, &timezone_value,
                                    &has_timezone, validation_err,
                                    sizeof(validation_err)) ||
      !api_validate_optional_string(json, "ntp_server", 1, 63, &ntp_value,
                                    &has_ntp, validation_err,
                                    sizeof(validation_err)) ||
      !api_validate_optional_string(json, "hostname", 1, MAX_HOSTNAME_LEN,
                                    &hostname_value, &has_hostname,
                                    validation_err, sizeof(validation_err)) ||
      !api_validate_optional_bool(json, "diag_events_enabled", &diag_enabled,
                                  &has_diag_enabled, validation_err,
                                  sizeof(validation_err))) {
    diag_event_log("WARN", "json_validation_error", -1, validation_err);
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, validation_err);
    return ESP_FAIL;
  }

  if (has_auto_tz) ntp_set_auto_timezone(auto_tz_value);
  if (has_timezone) ntp_set_timezone(timezone_value);
  if (has_ntp) ntp_set_server(ntp_value);
  if (has_diag_enabled) diag_event_ring_set_enabled(diag_enabled);
  if (has_hostname) {
    auto cfg = config_get();
    strncpy(cfg.hostname, hostname_value, MAX_HOSTNAME_LEN);
    cfg.hostname[MAX_HOSTNAME_LEN] = '\0';
    config_set(&cfg);
    wifi_set_hostname(hostname_value);
  }

  cJSON_Delete(json);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"status\":\"success\"}");
  return ESP_OK;
}

esp_err_t time_zonedb_handler(httpd_req_t* req) {
  const embeddedTz_t* zones = tz_db_get_all_zones();
  int total = TZ_DB_NUM_ZONES;

  httpd_resp_set_type(req, "application/json");

  httpd_resp_send_chunk(req, "[", 1);

  constexpr int CHUNK_SIZE = 20;
  bool first = true;

  for (int start = 0; start < total; start += CHUNK_SIZE) {
    int end = start + CHUNK_SIZE;
    if (end > total) end = total;

    cJSON* arr = cJSON_CreateArray();
    if (!arr) {
      httpd_resp_send_chunk(req, nullptr, 0);
      return ESP_FAIL;
    }

    for (int i = start; i < end; i++) {
      cJSON* obj = cJSON_CreateObject();
      if (!obj) continue;
      cJSON_AddStringToObject(obj, "name", zones[i].name);
      cJSON_AddStringToObject(obj, "rule", zones[i].rule);
      cJSON_AddItemToArray(arr, obj);
    }

    char* chunk_str = cJSON_PrintUnformatted(arr);
    if (!chunk_str) {
      cJSON_Delete(arr);
      httpd_resp_send_chunk(req, nullptr, 0);
      return ESP_FAIL;
    }

    size_t len = strlen(chunk_str);
    if (len > 2) {
      // Strip outer [ ] — send inner content only
      chunk_str[len - 1] = '\0';
      char* inner = chunk_str + 1;

      if (!first) {
        httpd_resp_send_chunk(req, ",", 1);
      }
      httpd_resp_send_chunk(req, inner, strlen(inner));
      first = false;
    }

    free(chunk_str);
    cJSON_Delete(arr);

    vTaskDelay(pdMS_TO_TICKS(10));
  }

  httpd_resp_send_chunk(req, "]", 1);
  httpd_resp_send_chunk(req, nullptr, 0);

  return ESP_OK;
}

}  // namespace

esp_err_t sta_api_start(void) {
  httpd_handle_t server = http_server_handle();
  if (!server) {
    ESP_LOGE(TAG, "HTTP server not running");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Registering API endpoints on central HTTP server");

  const httpd_uri_t status_uri = {
      .uri = "/api/status",
      .method = HTTP_GET,
      .handler = status_handler,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &status_uri);

  const httpd_uri_t health_uri = {
      .uri = "/api/health",
      .method = HTTP_GET,
      .handler = health_handler,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &health_uri);

  const httpd_uri_t diag_uri = {
      .uri = "/api/diag",
      .method = HTTP_GET,
      .handler = diag_handler,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &diag_uri);

  const httpd_uri_t about_uri = {
      .uri = "/api/about",
      .method = HTTP_GET,
      .handler = about_handler,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &about_uri);

  const httpd_uri_t sys_config_get_uri = {
      .uri = "/api/system/config",
      .method = HTTP_GET,
      .handler = system_config_get_handler,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &sys_config_get_uri);

  const httpd_uri_t sys_config_post_uri = {
      .uri = "/api/system/config",
      .method = HTTP_POST,
      .handler = system_config_post_handler,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &sys_config_post_uri);

  const httpd_uri_t zonedb_uri = {
      .uri = "/api/time/zonedb",
      .method = HTTP_GET,
      .handler = time_zonedb_handler,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &zonedb_uri);

  return ESP_OK;
}
