#include "ap.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>

#include "http_server.h"
#include "nvs_settings.h"
#include "ota_http_upload.h"
#include "webp_player.h"
#include "wifi.h"

namespace {

const char* TAG = "AP";

constexpr const char* DEFAULT_AP_SSID = "TRON-CONFIG";

constexpr int DNS_PORT = 53;
constexpr int DNS_MAX_LEN = 512;

TaskHandle_t s_dns_task_handle = nullptr;
TimerHandle_t s_ap_shutdown_timer = nullptr;

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char success_html_start[] asm("_binary_success_html_start");

#if CONFIG_BOARD_TIDBYT_GEN1 || CONFIG_BOARD_MATRIXPORTAL_S3
constexpr const char* SWAP_COLORS_FMT =
    "<div class='form-group'>"
    "<label>"
    "<input type='checkbox' id='swap_colors' name='swap_colors' value='1' %s>"
    " Swap Colors (Gen1/S3 only - requires reboot)"
    "</label>"
    "</div>";
#endif

struct __attribute__((packed)) DnsHeader {
  uint16_t id;
  uint16_t flags;
  uint16_t qdcount;
  uint16_t ancount;
  uint16_t nscount;
  uint16_t arcount;
};

// Forward declarations
esp_err_t root_handler(httpd_req_t* req);
esp_err_t save_handler(httpd_req_t* req);
esp_err_t update_handler(httpd_req_t* req);
esp_err_t captive_portal_handler(httpd_req_t* req);
void url_decode(char* str);
void start_dns_server();
void stop_dns_server();
void ap_shutdown_timer_callback(TimerHandle_t xTimer);

void dns_server_task(void* pvParameters) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Failed to create DNS socket");
    vTaskDelete(nullptr);
    return;
  }

  struct sockaddr_in server_addr = {};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(DNS_PORT);

  if (bind(sock, reinterpret_cast<struct sockaddr*>(&server_addr),
           sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind DNS socket");
    close(sock);
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "DNS server started on port 53");

  char rx_buffer[DNS_MAX_LEN];
  char tx_buffer[DNS_MAX_LEN];
  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  while (true) {
    int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                       reinterpret_cast<struct sockaddr*>(&client_addr),
                       &client_addr_len);

    if (len < 0) {
      ESP_LOGE(TAG, "DNS recvfrom failed");
      break;
    }

    if (len < static_cast<int>(sizeof(DnsHeader))) {
      continue;
    }

    auto* header = reinterpret_cast<DnsHeader*>(rx_buffer);

    if ((ntohs(header->flags) & 0x8000) != 0) {
      continue;
    }

    memcpy(tx_buffer, rx_buffer, len);
    auto* resp_header = reinterpret_cast<DnsHeader*>(tx_buffer);

    resp_header->flags = htons(0x8400);

    int response_len = len;
    int answers_added = 0;

    uint16_t num_questions = ntohs(header->qdcount);
    if (num_questions > 0 && response_len + 16 < DNS_MAX_LEN) {
      uint8_t answer[] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
                          0x00, 0x3C, 0x00, 0x04, 10,   10,   0,    1};

      memcpy(tx_buffer + response_len, answer, sizeof(answer));
      response_len += sizeof(answer);
      answers_added = 1;
    }

    resp_header->ancount = htons(answers_added);

    sendto(sock, tx_buffer, response_len, 0,
           reinterpret_cast<struct sockaddr*>(&client_addr), client_addr_len);
  }

  close(sock);
  vTaskDelete(nullptr);
}

void start_dns_server() {
  if (s_dns_task_handle != nullptr) {
    ESP_LOGW(TAG, "DNS server already running");
    return;
  }
  xTaskCreate(dns_server_task, "dns_server", 4096, nullptr, 5,
              &s_dns_task_handle);
}

void stop_dns_server() {
  if (s_dns_task_handle != nullptr) {
    vTaskDelete(s_dns_task_handle);
    s_dns_task_handle = nullptr;
    ESP_LOGI(TAG, "DNS server stopped");
  }
}

esp_err_t root_handler(httpd_req_t* req) {
  auto cfg = config_get();
  const char* image_url = cfg.image_url[0] ? cfg.image_url : "";
  const char* api_key = cfg.api_key[0] ? cfg.api_key : "";
  const char* swap_section = "";
#if CONFIG_BOARD_TIDBYT_GEN1 || CONFIG_BOARD_MATRIXPORTAL_S3
  char swap_buf[192];
  snprintf(swap_buf, sizeof(swap_buf), SWAP_COLORS_FMT,
           cfg.swap_colors ? "checked" : "");
  swap_section = swap_buf;
#endif

  ESP_LOGI(TAG, "Serving root page");
  int len =
      snprintf(nullptr, 0, setup_html_start, image_url, api_key, swap_section);
  auto* buf = static_cast<char*>(malloc(len + 1));
  if (!buf) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Out of memory");
  }
  snprintf(buf, len + 1, setup_html_start, image_url, api_key, swap_section);
  httpd_resp_set_type(req, "text/html");
  esp_err_t ret = httpd_resp_send(req, buf, len);
  free(buf);
  return ret;
}

/// Extract the "key" query parameter from a URL and strip it.
/// If found, the value is copied to key_out and removed from url.
void extract_key_from_url(char* url, char* key_out, size_t key_out_size) {
  key_out[0] = '\0';

  char* qmark = strchr(url, '?');
  if (!qmark) return;

  // Scan each parameter for "key="
  char* search = qmark + 1;
  char* key_param = nullptr;
  while (search && *search) {
    if (strncmp(search, "key=", 4) == 0 &&
        (search == qmark + 1 || *(search - 1) == '&')) {
      key_param = search;
      break;
    }
    search = strchr(search, '&');
    if (search) search++;
  }
  if (!key_param) return;

  char* val_start = key_param + 4;
  char* val_end = strchr(val_start, '&');
  size_t val_len = val_end ? static_cast<size_t>(val_end - val_start)
                           : strlen(val_start);
  if (val_len == 0) return;
  if (val_len >= key_out_size) val_len = key_out_size - 1;
  memcpy(key_out, val_start, val_len);
  key_out[val_len] = '\0';

  // Strip the key parameter from the URL
  if (key_param == qmark + 1) {
    if (val_end) {
      // ?key=val&rest -> ?rest
      memmove(qmark + 1, val_end + 1, strlen(val_end + 1) + 1);
    } else {
      // ?key=val (only param) -> remove query string
      *qmark = '\0';
    }
  } else {
    // &key=val -> remove including leading '&'
    char* amp = key_param - 1;
    if (val_end) {
      memmove(amp, val_end, strlen(val_end) + 1);
    } else {
      *amp = '\0';
    }
  }
}

void url_decode(char* str) {
  char* src = str;
  char* dst = str;

  while (*src) {
    if (*src == '%' && src[1] && src[2]) {
      char hex[3] = {src[1], src[2], 0};
      *dst = static_cast<char>(strtol(hex, nullptr, 16));
      src += 3;
    } else if (*src == '+') {
      *dst = ' ';
      src++;
    } else {
      *dst = *src;
      src++;
    }
    dst++;
  }
  *dst = '\0';
}

esp_err_t save_handler(httpd_req_t* req) {
  ESP_LOGI(TAG, "Processing form submission");

  auto* buf =
      static_cast<char*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
  if (!buf) {
    ESP_LOGE(TAG, "Failed to allocate memory for form data");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Server Error");
    return ESP_FAIL;
  }

  int ret;
  int remaining = req->content_len;
  int received = 0;

  if (remaining > 4095) {
    ESP_LOGE(TAG, "Form data too large: %d bytes", remaining);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form data too large");
    free(buf);
    return ESP_FAIL;
  }

  while (remaining > 0) {
    ret = httpd_req_recv(req, buf + received, remaining);
    if (ret <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      ESP_LOGE(TAG, "Failed to receive form data");
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                          "Failed to receive form data");
      free(buf);
      return ESP_FAIL;
    }
    received += ret;
    remaining -= ret;
  }

  buf[received] = '\0';
  ESP_LOGI(TAG, "Received form data (%d bytes)", received);

  char ssid[MAX_SSID_LEN + 1] = {0};
  char password[MAX_PASSWORD_LEN + 1] = {0};
  char image_url[MAX_URL_LEN + 1] = {0};
  char api_key[MAX_API_KEY_LEN + 1] = {0};
  char swap_val[2] = {0};
  bool swap_colors = false;

  if (httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
    ESP_LOGD(TAG, "SSID param missing");
  }

  if (httpd_query_key_value(buf, "password", password, sizeof(password)) !=
      ESP_OK) {
    ESP_LOGD(TAG, "Password param missing");
  }

  if (httpd_query_key_value(buf, "image_url", image_url, sizeof(image_url)) !=
      ESP_OK) {
    ESP_LOGD(TAG, "Image URL param missing");
  }

  if (httpd_query_key_value(buf, "api_key", api_key, sizeof(api_key)) !=
      ESP_OK) {
    ESP_LOGD(TAG, "API key param missing");
  }

  if (httpd_query_key_value(buf, "swap_colors", swap_val, sizeof(swap_val)) ==
      ESP_OK) {
    swap_colors = (strcmp(swap_val, "1") == 0);
  }

  url_decode(ssid);
  url_decode(password);
  url_decode(image_url);
  url_decode(api_key);

  // Auto-extract ?key= from URL if no explicit API key was provided
  if (strlen(api_key) == 0) {
    char extracted_key[MAX_API_KEY_LEN + 1] = {0};
    extract_key_from_url(image_url, extracted_key, sizeof(extracted_key));
    if (strlen(extracted_key) > 0) {
      snprintf(api_key, sizeof(api_key), "%s", extracted_key);
      ESP_LOGI(TAG, "Extracted API key from URL");
    }
  } else {
    // User provided an explicit key — still strip ?key= from URL if present
    char discard[MAX_API_KEY_LEN + 1];
    extract_key_from_url(image_url, discard, sizeof(discard));
  }

  ESP_LOGI(TAG, "Received SSID: %s, Image URL: %s, Swap Colors: %s", ssid,
           image_url, swap_colors ? "true" : "false");

  {
    auto cfg = config_get();
    snprintf(cfg.ssid, sizeof(cfg.ssid), "%s", ssid);
    snprintf(cfg.password, sizeof(cfg.password), "%s", password);
    if (strlen(image_url) >= 6) {
      snprintf(cfg.image_url, sizeof(cfg.image_url), "%s", image_url);
    } else {
      cfg.image_url[0] = '\0';
    }
    snprintf(cfg.api_key, sizeof(cfg.api_key), "%s", api_key);
    cfg.swap_colors = swap_colors;
    config_set(&cfg);
  }

  free(buf);

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, success_html_start, HTTPD_RESP_USE_STRLEN);

  ESP_LOGI(TAG, "Configuration saved - rebooting...");
  gfx_safe_restart();

  return ESP_OK;
}

esp_err_t update_handler(httpd_req_t* req) {
  esp_err_t err = ota_http_upload_perform(req);
  if (err != ESP_OK) {
    return err;  // Error response already sent by ota_http_upload_perform
  }

  ESP_LOGI(TAG, "OTA Success! Rebooting...");
  httpd_resp_send(req, "OK", 2);
  gfx_safe_restart();

  return ESP_OK;
}

esp_err_t captive_portal_handler(httpd_req_t* req) {
  char* host_buf = nullptr;
  bool serve_directly = false;

  size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
  if (host_len > 0) {
    host_buf = static_cast<char*>(malloc(host_len + 1));
    if (!host_buf) {
      ESP_LOGE(TAG, "Failed to allocate memory for Host header");
    } else {
      if (httpd_req_get_hdr_value_str(req, "Host", host_buf, host_len + 1) !=
          ESP_OK) {
        free(host_buf);
        host_buf = nullptr;
      }
    }
  }

  if (host_buf && (strcmp(host_buf, "10.10.0.1") == 0 ||
                   strstr(host_buf, "10.10.0.1") != nullptr)) {
    serve_directly = true;
  }

  free(host_buf);

  if (serve_directly) {
    return root_handler(req);
  }

  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://10.10.0.1/setup");
  httpd_resp_send(req, nullptr, 0);

  return ESP_OK;
}

void ap_shutdown_timer_callback(TimerHandle_t xTimer) {
  ESP_LOGI(TAG, "Shutting down config portal");
  ap_stop();
  esp_wifi_set_mode(WIFI_MODE_STA);
}

}  // namespace

esp_err_t ap_start(void) {
  http_server_start();

  httpd_handle_t server = http_server_handle();
  if (!server) {
    ESP_LOGE(TAG, "Failed to get HTTP server handle");
    return ESP_FAIL;
  }

  // Serve the setup page at /setup so it is always reachable regardless of
  // whether the webui wildcard handles /* in normal operation.
  httpd_uri_t setup_uri = {.uri = "/setup",
                            .method = HTTP_GET,
                            .handler = root_handler,
                            .user_ctx = nullptr};
  httpd_register_uri_handler(server, &setup_uri);

  httpd_uri_t save_uri = {.uri = "/save",
                          .method = HTTP_POST,
                          .handler = save_handler,
                          .user_ctx = nullptr};
  httpd_register_uri_handler(server, &save_uri);

  httpd_uri_t update_uri = {.uri = "/update",
                            .method = HTTP_POST,
                            .handler = update_handler,
                            .user_ctx = nullptr};
  httpd_register_uri_handler(server, &update_uri);

  httpd_uri_t hotspot_detect_uri = {.uri = "/hotspot-detect.html",
                                    .method = HTTP_GET,
                                    .handler = captive_portal_handler,
                                    .user_ctx = nullptr};
  httpd_register_uri_handler(server, &hotspot_detect_uri);

  httpd_uri_t generate_204_uri = {.uri = "/generate_204",
                                  .method = HTTP_GET,
                                  .handler = captive_portal_handler,
                                  .user_ctx = nullptr};
  httpd_register_uri_handler(server, &generate_204_uri);

  httpd_uri_t ncsi_uri = {.uri = "/ncsi.txt",
                          .method = HTTP_GET,
                          .handler = captive_portal_handler,
                          .user_ctx = nullptr};
  httpd_register_uri_handler(server, &ncsi_uri);

  // NOTE: The wildcard catch-all is NOT registered here. Call
  // ap_register_wildcard() after all other handlers (e.g. STA API)
  // have been registered, because httpd_find_uri_handler() returns
  // the first array-order match and we need /api/* to win over /*.

  start_dns_server();

  return ESP_OK;
}

void ap_register_wildcard(void) {
  httpd_handle_t server = http_server_handle();
  if (!server) {
    return;
  }
  httpd_uri_t wildcard_uri = {.uri = "/*",
                              .method = HTTP_GET,
                              .handler = captive_portal_handler,
                              .user_ctx = nullptr};
  httpd_register_uri_handler(server, &wildcard_uri);
}

esp_err_t ap_stop(void) {
  stop_dns_server();
  return ESP_OK;
}

void ap_init_netif(void) {
  esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap();

  esp_netif_ip_info_t ip_info;
  IP4_ADDR(&ip_info.ip, 10, 10, 0, 1);
  IP4_ADDR(&ip_info.gw, 10, 10, 0, 1);
  IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

  esp_netif_dhcps_stop(ap_netif);

  esp_err_t ap_err = esp_netif_set_ip_info(ap_netif, &ip_info);
  if (ap_err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set AP IP info: %s", esp_err_to_name(ap_err));
  } else {
    ESP_LOGI(TAG, "AP IP address set to 10.10.0.1");
  }

  esp_netif_dhcps_start(ap_netif);
}

void ap_configure(void) {
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

  wifi_config_t ap_config = {};
  strcpy(reinterpret_cast<char*>(ap_config.ap.ssid), DEFAULT_AP_SSID);
  ap_config.ap.ssid_len = strlen(DEFAULT_AP_SSID);

  uint8_t random_channel = (esp_random() % 11) + 1;
  ap_config.ap.channel = random_channel;

  ap_config.ap.max_connection = 4;
  ap_config.ap.authmode = WIFI_AUTH_OPEN;
  ap_config.ap.beacon_interval = 100;

  ESP_LOGI(TAG, "Setting AP SSID: %s on channel %d", DEFAULT_AP_SSID,
           random_channel);
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
}

void ap_start_shutdown_timer(void) {
  if (s_ap_shutdown_timer) {
    xTimerDelete(s_ap_shutdown_timer, 0);
    s_ap_shutdown_timer = nullptr;
  }

  s_ap_shutdown_timer =
      xTimerCreate("ap_shutdown_timer",
                   pdMS_TO_TICKS(2 * 60 * 1000),  // 2 minutes
                   pdFALSE,                        // One-shot timer
                   nullptr,                        // No timer ID
                   ap_shutdown_timer_callback);

  if (s_ap_shutdown_timer) {
    if (xTimerStart(s_ap_shutdown_timer, 0) == pdPASS) {
      ESP_LOGI(TAG, "AP will automatically shut down in 2 minutes");
    } else {
      ESP_LOGE(TAG, "Failed to start AP shutdown timer");
      xTimerDelete(s_ap_shutdown_timer, 0);
      s_ap_shutdown_timer = nullptr;
    }
  } else {
    ESP_LOGE(TAG, "Failed to create AP shutdown timer");
  }
}
