#include "ap.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <lwip/sockets.h>

#include "board_caps.h"
#include "event_bus.h"
#include "http_server.h"
#include "nvs_settings.h"
#include "ota_http_upload.h"
#include "psram_alloc.h"
#include "raii_utils.hpp"
#include "sdkconfig.h"
#include "webp_player.h"
#include "wifi.h"

namespace {

const char* TAG = "AP";

constexpr const char* DEFAULT_AP_SSID = CONFIG_BRAND_NAME_LOWER "-CONFIG";

constexpr uint8_t AP_IP[4] = {10, 10, 0, 1};
constexpr uint32_t PORTAL_SHUTDOWN_DELAY_MS = 2 * 60 * 1000;

constexpr int DNS_PORT = 53;
constexpr int DNS_MAX_LEN = 512;
constexpr int DNS_RECV_TIMEOUT_MS = 500;
constexpr uint16_t DNS_FLAG_QR = 0x8000;
constexpr uint16_t DNS_FLAG_AA = 0x0400;
constexpr uint16_t DNS_FLAG_RD = 0x0100;
constexpr uint8_t DNS_TYPE_A = 1;
constexpr uint8_t DNS_CLASS_IN = 1;

// DNS task lifetime. The task owns its socket and clears the handle itself on
// exit; stopping only raises the flag so the socket is always closed.
SemaphoreHandle_t s_dns_mutex = nullptr;
TaskHandle_t s_dns_task_handle = nullptr;
std::atomic<bool> s_dns_stop_requested{false};

// Portal lifetime. The portal is open while the soft AP and captive DNS run;
// it closes PORTAL_SHUTDOWN_DELAY_MS after the STA gets an IP (once the setup
// flow allows it) and reopens when the STA gives up connecting.
SemaphoreHandle_t s_portal_mutex = nullptr;
TimerHandle_t s_ap_shutdown_timer = nullptr;
wifi_config_t s_ap_config = {};
bool s_portal_configured = false;
bool s_auto_shutdown_enabled = false;
std::atomic<bool> s_portal_active{false};

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char success_html_start[] asm("_binary_success_html_start");

#if BOARD_HAS_SWAP_COLORS
constexpr const char* SWAP_COLORS_FMT =
    "<div class='form-group'>"
    "<label>"
    "<input type='checkbox' id='swap_colors' name='swap_colors' value='1' %s>"
    " Swap Colors (Gen1/S3 only - requires reboot)"
    "</label>"
    "</div>";
#endif

#if BOARD_HAS_TOUCH
constexpr const char* TOUCH_SETTINGS_FMT =
    "<div class='form-group'>"
    "<label>"
    "<input type='checkbox' id='disable_touch' name='disable_touch' value='1' "
    "%s>"
    " Disable Touch Button (Gen2 only - requires reboot)"
    "</label>"
    "</div>"
    "<div class='form-group'>"
    "<label>"
    "<input type='checkbox' id='touch_beep' name='touch_beep' value='1' %s>"
    " Beep On Touch (Gen2 only)"
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
esp_err_t network_delete_handler(httpd_req_t* req);
char* build_networks_section();
esp_err_t update_handler(httpd_req_t* req);
esp_err_t captive_portal_handler(httpd_req_t* req);
void url_decode(char* str);

// Escape a string for safe inclusion in an HTML attribute or text node.
// Writes a NUL-terminated result; truncates safely if out_size is too small.
void html_escape(const char* in, char* out, size_t out_size) {
  size_t o = 0;
  for (size_t i = 0; in[i] != '\0'; ++i) {
    const char* rep = nullptr;
    switch (in[i]) {
      case '&':  rep = "&amp;";  break;
      case '<':  rep = "&lt;";   break;
      case '>':  rep = "&gt;";   break;
      case '"':  rep = "&quot;"; break;
      case '\'': rep = "&#39;";  break;
      default:   break;
    }
    if (rep) {
      size_t rl = strlen(rep);
      if (o + rl >= out_size) break;
      memcpy(out + o, rep, rl);
      o += rl;
    } else {
      if (o + 1 >= out_size) break;
      out[o++] = in[i];
    }
  }
  out[o < out_size ? o : out_size - 1] = '\0';
}

// Build the "Saved Networks" card for the setup page. Returns a malloc'd,
// NUL-terminated string the caller must free (empty string on OOM). Each row is
// a tiny POST form so the browser handles SSID encoding; SSIDs are HTML-escaped.
char* build_networks_section() {
  wifi_network_t nets[MAX_WIFI_NETS];
  size_t n = wifi_network_list_get(nets, MAX_WIFI_NETS);

  const size_t cap = 512 + n * 768;
  char* out = static_cast<char*>(malloc(cap));
  if (!out) return strdup("");

  size_t o = 0;
  // Advance the write cursor by an snprintf result, clamped so o stays < cap.
  auto adv = [&](int w) {
    if (w < 0) return;
    size_t rem = cap - o;
    o += (static_cast<size_t>(w) < rem) ? static_cast<size_t>(w) : (rem - 1);
  };

  adv(snprintf(out + o, cap - o, "<div class='card'><h2>Saved Networks</h2>"));
  if (n == 0) {
    adv(snprintf(out + o, cap - o,
                 "<div class='hint'>No networks saved yet.</div>"));
  }
  for (size_t i = 0; i < n && o < cap - 1; i++) {
    char ssid_esc[MAX_SSID_LEN * 6 + 1];
    html_escape(nets[i].ssid, ssid_esc, sizeof(ssid_esc));
    adv(snprintf(
        out + o, cap - o,
        "<form action='/network/delete' method='post' style='display:flex;"
        "align-items:center;justify-content:space-between;gap:.5rem;"
        "margin-bottom:.5rem'>"
        "<input type='hidden' name='ssid' value='%s'>"
        "<span style='font-family:var(--mono);font-size:.8125rem;overflow:hidden;"
        "text-overflow:ellipsis'>%s</span>"
        "<button type='submit'>Delete</button></form>",
        ssid_esc, ssid_esc));
  }
  adv(snprintf(out + o, cap - o, "</div>"));
  return out;
}

// Returns the offset just past the first question (QNAME, QTYPE, QCLASS), or
// -1 if the question is malformed or truncated.
int dns_question_end(const uint8_t* msg, int len, uint16_t* qtype,
                     uint16_t* qclass) {
  int pos = sizeof(DnsHeader);
  while (pos < len && msg[pos] != 0) {
    // Compression pointers (0xC0) and extended label types are not valid in
    // the question of a query.
    if ((msg[pos] & 0xC0) != 0) return -1;
    pos += 1 + msg[pos];
  }
  pos++;  // root label
  if (pos + 4 > len) return -1;
  *qtype = static_cast<uint16_t>((msg[pos] << 8) | msg[pos + 1]);
  *qclass = static_cast<uint16_t>((msg[pos + 2] << 8) | msg[pos + 3]);
  return pos + 4;
}

// Builds the captive-portal reply for `query`: an A record pointing at the AP
// for A/IN questions, NOERROR with no answers for every other type (AAAA etc.)
// so clients fall back to IPv4. Only the header and first question are echoed:
// the rest of the query (typically an EDNS0 OPT record) would otherwise sit in
// front of the answer. Returns the reply length, or -1 to drop the packet.
int build_dns_response(const uint8_t* query, int len, uint8_t* resp,
                       int resp_size) {
  if (len < static_cast<int>(sizeof(DnsHeader))) return -1;
  DnsHeader hdr;
  memcpy(&hdr, query, sizeof(hdr));
  uint16_t flags = ntohs(hdr.flags);
  if ((flags & DNS_FLAG_QR) != 0) return -1;
  if (((flags >> 11) & 0xF) != 0) return -1;  // only standard queries
  if (ntohs(hdr.qdcount) == 0) return -1;

  uint16_t qtype = 0;
  uint16_t qclass = 0;
  int q_end = dns_question_end(query, len, &qtype, &qclass);
  if (q_end < 0) return -1;

  const uint8_t answer[] = {
      0xC0, 0x0C,              // name: pointer to the question
      0x00, DNS_TYPE_A,        // type
      0x00, DNS_CLASS_IN,      // class
      0x00, 0x00, 0x00, 0x3C,  // TTL 60 s
      0x00, 0x04,              // rdlength
      AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3]};
  bool answered = (qtype == DNS_TYPE_A && qclass == DNS_CLASS_IN);
  int total = q_end + (answered ? static_cast<int>(sizeof(answer)) : 0);
  if (total > resp_size) return -1;

  memcpy(resp, query, q_end);
  hdr.flags = htons(DNS_FLAG_QR | DNS_FLAG_AA | (flags & DNS_FLAG_RD));
  hdr.qdcount = htons(1);
  hdr.ancount = htons(answered ? 1 : 0);
  hdr.nscount = 0;
  hdr.arcount = 0;
  memcpy(resp, &hdr, sizeof(hdr));
  if (answered) memcpy(resp + q_end, answer, sizeof(answer));
  return total;
}

void dns_server_task(void*) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Failed to create DNS socket");
  } else {
    struct sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(DNS_PORT);

    // The receive timeout bounds how long a stop request waits for the loop.
    struct timeval tv = {};
    tv.tv_usec = DNS_RECV_TIMEOUT_MS * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&server_addr),
             sizeof(server_addr)) < 0) {
      ESP_LOGE(TAG, "Failed to bind DNS socket: errno %d", errno);
      close(sock);
      sock = -1;
    } else {
      ESP_LOGI(TAG, "DNS server started on port %d", DNS_PORT);
    }
  }

  uint8_t rx_buffer[DNS_MAX_LEN];
  uint8_t tx_buffer[DNS_MAX_LEN];
  while (sock >= 0 && !s_dns_stop_requested.load()) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                       reinterpret_cast<struct sockaddr*>(&client_addr),
                       &client_addr_len);
    if (len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      ESP_LOGE(TAG, "DNS recvfrom failed: errno %d", errno);
      break;
    }

    int resp_len = build_dns_response(rx_buffer, len, tx_buffer,
                                      sizeof(tx_buffer));
    if (resp_len > 0) {
      sendto(sock, tx_buffer, resp_len, 0,
             reinterpret_cast<struct sockaddr*>(&client_addr),
             client_addr_len);
    }
  }

  // Close before clearing the handle so a restart can bind port 53 again.
  if (sock >= 0) close(sock);
  {
    raii::MutexGuard lock(s_dns_mutex);
    s_dns_task_handle = nullptr;
  }
  ESP_LOGI(TAG, "DNS server stopped");
  vTaskDeleteWithCaps(nullptr);
}

void start_dns_server() {
  // A previous server may still be draining its receive timeout after a stop.
  for (int waited_ms = 0; waited_ms <= 2 * DNS_RECV_TIMEOUT_MS;
       waited_ms += 50) {
    {
      raii::MutexGuard lock(s_dns_mutex);
      if (s_dns_task_handle == nullptr) {
        s_dns_stop_requested.store(false);
        if (psram_or_internal_task_create(dns_server_task, "dns_server", 4096,
                                          nullptr, 5, &s_dns_task_handle,
                                          tskNO_AFFINITY) != pdPASS) {
          s_dns_task_handle = nullptr;
          ESP_LOGE(TAG, "Failed to create DNS server task");
        }
        return;
      }
      if (!s_dns_stop_requested.load()) {
        ESP_LOGW(TAG, "DNS server already running");
        return;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  ESP_LOGE(TAG, "Previous DNS server did not exit; not restarting it");
}

void stop_dns_server() {
  raii::MutexGuard lock(s_dns_mutex);
  if (s_dns_task_handle != nullptr) {
    s_dns_stop_requested.store(true);
  }
}

esp_err_t root_handler(httpd_req_t* req) {
  auto cfg = config_get();
  char image_url_esc[128 * 6 + 1];
  char api_key_esc[128 * 6 + 1];
  html_escape(cfg.image_url[0] ? cfg.image_url : "", image_url_esc, sizeof(image_url_esc));
  html_escape(cfg.api_key[0]   ? cfg.api_key   : "", api_key_esc,   sizeof(api_key_esc));
  const char* swap_section = "";
#if BOARD_HAS_SWAP_COLORS
  char swap_buf[192];
  snprintf(swap_buf, sizeof(swap_buf), SWAP_COLORS_FMT,
           cfg.swap_colors ? "checked" : "");
  swap_section = swap_buf;
#endif

  const char* touch_section = "";
#if BOARD_HAS_TOUCH
  char touch_buf[512];
  snprintf(touch_buf, sizeof(touch_buf), TOUCH_SETTINGS_FMT,
           cfg.disable_touch ? "checked" : "",
           cfg.touch_beep ? "checked" : "");
  touch_section = touch_buf;
#endif

  const char* brand_name = CONFIG_BRAND_NAME;
  const char* url_hide = "";
#ifdef CONFIG_LOCK_SERVER_URL
  url_hide = "style='display:none'";
#endif

  char* nets_section = build_networks_section();

  ESP_LOGI(TAG, "Serving root page");
  const char* accent = CONFIG_BRAND_ACCENT_COLOR;
  int len =
      snprintf(nullptr, 0, setup_html_start, brand_name, accent, brand_name,
               url_hide, image_url_esc, api_key_esc, swap_section, touch_section,
               nets_section);
  auto* buf = static_cast<char*>(malloc(len + 1));
  if (!buf) {
    free(nets_section);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Out of memory");
  }
  snprintf(buf, len + 1, setup_html_start, brand_name, accent, brand_name,
           url_hide, image_url_esc, api_key_esc, swap_section, touch_section,
           nets_section);
  free(nets_section);
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

  auto* buf = static_cast<char*>(psram_or_internal_malloc(4096));
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
  char touch_val[4] = {0};
  bool disable_touch = false;
  char touch_beep_val[4] = {0};
  bool touch_beep = false;

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

  if (httpd_query_key_value(buf, "disable_touch", touch_val,
                            sizeof(touch_val)) == ESP_OK) {
    disable_touch = (strcmp(touch_val, "1") == 0);
  }

  if (httpd_query_key_value(buf, "touch_beep", touch_beep_val,
                            sizeof(touch_beep_val)) == ESP_OK) {
    touch_beep = (strcmp(touch_beep_val, "1") == 0);
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

  ESP_LOGI(TAG,
           "Received SSID: %s, Image URL: %s, Swap Colors: %s, Disable Touch: "
           "%s, Touch Beep: %s",
           ssid, image_url, swap_colors ? "true" : "false",
           disable_touch ? "true" : "false", touch_beep ? "true" : "false");

  {
    auto cfg = config_get();
#ifdef CONFIG_LOCK_SERVER_URL
    // When server URL is locked, always use the Kconfig default
    snprintf(cfg.image_url, sizeof(cfg.image_url), "%s",
             CONFIG_DEFAULT_SERVER_URL);
    ESP_LOGI(TAG, "Server URL locked to: %s", CONFIG_DEFAULT_SERVER_URL);
#else
    if (strlen(image_url) >= 6) {
      snprintf(cfg.image_url, sizeof(cfg.image_url), "%s", image_url);
    } else {
      cfg.image_url[0] = '\0';
    }
#endif
    snprintf(cfg.api_key, sizeof(cfg.api_key), "%s", api_key);
    cfg.swap_colors = swap_colors;
    cfg.disable_touch = disable_touch;
    cfg.touch_beep = touch_beep;
    config_set(&cfg);

    // Credentials live in the multi-network list (the sole credential store),
    // not the config blob. Add the new network, or update its password.
    if (strlen(ssid) > 0) {
      if (!wifi_network_list_add(ssid, password)) {
        ESP_LOGW(TAG, "Network list full; '%s' not added", ssid);
      }
    }
  }

  free(buf);

  // Render branded success page
  int success_len =
      snprintf(nullptr, 0, success_html_start, CONFIG_BRAND_NAME,
               CONFIG_BRAND_ACCENT_COLOR);
  auto* success_buf = static_cast<char*>(malloc(success_len + 1));
  if (success_buf) {
    snprintf(success_buf, success_len + 1, success_html_start,
             CONFIG_BRAND_NAME, CONFIG_BRAND_ACCENT_COLOR);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, success_buf, success_len);
    free(success_buf);
  } else {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, success_html_start, HTTPD_RESP_USE_STRLEN);
  }

  ESP_LOGI(TAG, "Configuration saved - rebooting...");
  gfx_safe_restart();

  return ESP_OK;
}

esp_err_t network_delete_handler(httpd_req_t* req) {
  char buf[256];
  int total = req->content_len;
  if (total <= 0 || total >= static_cast<int>(sizeof(buf))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
    return ESP_FAIL;
  }
  int received = 0;
  while (received < total) {
    int r = httpd_req_recv(req, buf + received, total - received);
    if (r <= 0) {
      if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive");
      return ESP_FAIL;
    }
    received += r;
  }
  buf[received] = '\0';

  char ssid[MAX_SSID_LEN + 1] = {0};
  if (httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid)) == ESP_OK) {
    url_decode(ssid);
    if (wifi_network_list_remove(ssid)) {
      ESP_LOGI(TAG, "Removed network: %s", ssid);
    } else {
      ESP_LOGW(TAG, "Delete requested for unknown network: %s", ssid);
    }
  }

  // Redirect back to the setup page so the refreshed list is shown. The change
  // takes effect on the next reconnect/reboot; we do not tear down a live link.
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/setup");
  httpd_resp_send(req, nullptr, 0);
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

void register_portal_handlers(httpd_handle_t server) {
  // Serve the setup page at /setup so it is always reachable regardless of
  // whether the webui wildcard handles /* in normal operation.
  const httpd_uri_t setup_uri = {.uri = "/setup",
                                 .method = HTTP_GET,
                                 .handler = root_handler,
                                 .user_ctx = nullptr};
  httpd_register_uri_handler(server, &setup_uri);

  const httpd_uri_t save_uri = {.uri = "/save",
                                .method = HTTP_POST,
                                .handler = save_handler,
                                .user_ctx = nullptr};
  httpd_register_uri_handler(server, &save_uri);

  const httpd_uri_t network_delete_uri = {.uri = "/network/delete",
                                          .method = HTTP_POST,
                                          .handler = network_delete_handler,
                                          .user_ctx = nullptr};
  httpd_register_uri_handler(server, &network_delete_uri);

  const httpd_uri_t update_uri = {.uri = "/update",
                                  .method = HTTP_POST,
                                  .handler = update_handler,
                                  .user_ctx = nullptr};
  httpd_register_uri_handler(server, &update_uri);

  const httpd_uri_t hotspot_detect_uri = {.uri = "/hotspot-detect.html",
                                          .method = HTTP_GET,
                                          .handler = captive_portal_handler,
                                          .user_ctx = nullptr};
  httpd_register_uri_handler(server, &hotspot_detect_uri);

  const httpd_uri_t generate_204_uri = {.uri = "/generate_204",
                                        .method = HTTP_GET,
                                        .handler = captive_portal_handler,
                                        .user_ctx = nullptr};
  httpd_register_uri_handler(server, &generate_204_uri);

  const httpd_uri_t ncsi_uri = {.uri = "/ncsi.txt",
                                .method = HTTP_GET,
                                .handler = captive_portal_handler,
                                .user_ctx = nullptr};
  httpd_register_uri_handler(server, &ncsi_uri);
}

void register_portal_wildcard(httpd_handle_t server) {
  const httpd_uri_t wildcard_uri = {.uri = "/*",
                                    .method = HTTP_GET,
                                    .handler = captive_portal_handler,
                                    .user_ctx = nullptr};
  httpd_register_uri_handler(server, &wildcard_uri);
}

// Caller holds s_portal_mutex.
void arm_shutdown_timer_locked() {
  if (!s_portal_active.load() || !s_auto_shutdown_enabled ||
      !s_ap_shutdown_timer || xTimerIsTimerActive(s_ap_shutdown_timer)) {
    return;
  }
  if (xTimerReset(s_ap_shutdown_timer, 0) == pdPASS) {
    ESP_LOGI(TAG, "Config portal will shut down in %lu s",
             static_cast<unsigned long>(PORTAL_SHUTDOWN_DELAY_MS / 1000));
  } else {
    ESP_LOGE(TAG, "Failed to arm config portal shutdown timer");
  }
}

void ap_shutdown_timer_callback(TimerHandle_t) {
  raii::MutexGuard lock(s_portal_mutex);
  if (!s_portal_active.load()) return;
  // The disconnect notification may still be queued behind this callback.
  if (!wifi_is_connected()) {
    ESP_LOGI(TAG, "WiFi not connected; keeping config portal open");
    return;
  }
  ESP_LOGI(TAG, "Shutting down config portal");
  stop_dns_server();
  esp_wifi_set_mode(WIFI_MODE_STA);
  s_portal_active.store(false);
}

void on_sta_connected(const tronbyt_event_t*, void*) {
  raii::MutexGuard lock(s_portal_mutex);
  arm_shutdown_timer_locked();
}

void on_sta_disconnected(const tronbyt_event_t*, void*) {
  raii::MutexGuard lock(s_portal_mutex);
  if (s_ap_shutdown_timer && xTimerIsTimerActive(s_ap_shutdown_timer)) {
    xTimerStop(s_ap_shutdown_timer, 0);
    ESP_LOGI(TAG, "WiFi lost; config portal stays open");
  }
}

}  // namespace

esp_err_t ap_start(void) {
  http_server_register_handlers(register_portal_handlers);
  http_server_start();
  if (!http_server_handle()) {
    ESP_LOGE(TAG, "Failed to get HTTP server handle");
    return ESP_FAIL;
  }

  // NOTE: The wildcard catch-all is NOT registered here. Call
  // ap_register_wildcard() after all other handlers (e.g. STA API)
  // have been registered, because httpd_find_uri_handler() returns
  // the first array-order match and we need /api/* to win over /*.

  raii::MutexGuard lock(s_portal_mutex);
  if (!s_ap_shutdown_timer) {
    s_ap_shutdown_timer = xTimerCreate(
        "ap_shutdown_timer", pdMS_TO_TICKS(PORTAL_SHUTDOWN_DELAY_MS), pdFALSE,
        nullptr, ap_shutdown_timer_callback);
    if (!s_ap_shutdown_timer) {
      ESP_LOGE(TAG, "Failed to create AP shutdown timer");
    }
    event_bus_subscribe(TRONBYT_EVENT_WIFI_CONNECTED, on_sta_connected,
                        nullptr);
    event_bus_subscribe(TRONBYT_EVENT_WIFI_DISCONNECTED, on_sta_disconnected,
                        nullptr);
  }
  start_dns_server();
  s_portal_active.store(true);

  return ESP_OK;
}

void ap_register_wildcard(void) {
  http_server_register_handlers(register_portal_wildcard);
}

esp_err_t ap_stop(void) {
  raii::MutexGuard lock(s_portal_mutex);
  if (s_ap_shutdown_timer) xTimerStop(s_ap_shutdown_timer, 0);
  stop_dns_server();
  s_portal_active.store(false);
  return ESP_OK;
}

void ap_open_portal(void) {
  raii::MutexGuard lock(s_portal_mutex);
  if (!s_portal_configured || s_portal_active.load()) return;

  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &s_ap_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to reopen config portal: %s", esp_err_to_name(err));
    return;
  }
  start_dns_server();
  s_portal_active.store(true);
  ESP_LOGW(TAG, "Config portal reopened: join '%s' to reconfigure WiFi",
           DEFAULT_AP_SSID);
}

bool ap_portal_active(void) { return s_portal_active.load(); }

void ap_init_netif(void) {
  esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap();

  esp_netif_ip_info_t ip_info;
  IP4_ADDR(&ip_info.ip, AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3]);
  IP4_ADDR(&ip_info.gw, AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3]);
  IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

  esp_netif_dhcps_stop(ap_netif);

  esp_err_t ap_err = esp_netif_set_ip_info(ap_netif, &ip_info);
  if (ap_err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set AP IP info: %s", esp_err_to_name(ap_err));
  } else {
    ESP_LOGI(TAG, "AP IP address set to " IPSTR, IP2STR(&ip_info.ip));
  }

  esp_netif_dhcps_start(ap_netif);
}

void ap_configure(void) {
  if (!s_portal_mutex) s_portal_mutex = xSemaphoreCreateMutex();
  if (!s_dns_mutex) s_dns_mutex = xSemaphoreCreateMutex();

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

  s_ap_config = {};
  strcpy(reinterpret_cast<char*>(s_ap_config.ap.ssid), DEFAULT_AP_SSID);
  s_ap_config.ap.ssid_len = strlen(DEFAULT_AP_SSID);

  uint8_t random_channel = (esp_random() % 11) + 1;
  s_ap_config.ap.channel = random_channel;

  s_ap_config.ap.max_connection = 4;
  s_ap_config.ap.authmode = WIFI_AUTH_OPEN;
  s_ap_config.ap.beacon_interval = 100;

  ESP_LOGI(TAG, "Setting AP SSID: %s on channel %d", DEFAULT_AP_SSID,
           random_channel);
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_ap_config));
  s_portal_configured = true;
}

void ap_enable_auto_shutdown(void) {
  raii::MutexGuard lock(s_portal_mutex);
  s_auto_shutdown_enabled = true;
  if (!s_portal_active.load()) return;
  if (wifi_is_connected()) {
    arm_shutdown_timer_locked();
  } else {
    ESP_LOGI(TAG, "Config portal stays open until WiFi connects");
  }
}
