#include "remote.h"

#include <cstdlib>
#include <cstring>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_tls.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "nvs_settings.h"
#include "sdkconfig.h"
#include "version.h"
#include "webp_player.h"

namespace {

const char* TAG = "remote";

constexpr int      REMOTE_MAX_ATTEMPTS  = 3;
constexpr uint32_t REMOTE_BACKOFF_MS[]  = {0, 500, 1500};

// Retry connection errors and these "try again later" status codes only.
bool http_status_is_transient(int status) {
  return status == 408 || status == 425 || status == 429 ||
         status == 500 || status == 502 || status == 503 || status == 504;
}

struct RemoteState {
  void* buf;
  size_t len;
  size_t size;
  size_t max;
  size_t expected_len;
  uint8_t brightness;
  int32_t dwell_secs;
  char* ota_url;
  bool oversize_detected;
};

template <typename T>
constexpr T max_val(T a, T b) {
  return (a > b) ? a : b;
}

template <typename T>
constexpr T min_val(T a, T b) {
  return (a < b) ? a : b;
}

esp_err_t http_callback(esp_http_client_event_t* event) {
  esp_err_t err = ESP_OK;
  auto* state = static_cast<RemoteState*>(event->user_data);

  switch (event->event_id) {
    case HTTP_EVENT_ERROR:
      ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
      break;

    case HTTP_EVENT_ON_CONNECTED:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
      break;

    case HTTP_EVENT_HEADER_SENT:
      ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
      break;

    case HTTP_EVENT_ON_HEADER:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s",
               event->header_key, event->header_value);

      if (strcasecmp(event->header_key, "Content-Length") == 0) {
        size_t content_length =
            static_cast<size_t>(atoi(event->header_value));
        if (content_length > state->max) {
          ESP_LOGE(TAG,
                   "Content-Length (%zu bytes) exceeds allowed max (%zu bytes)",
                   content_length, state->max);
          if (gfx_display_asset("oversize") != 0) {
            ESP_LOGE(TAG, "Failed to display oversize graphic");
          }
          state->oversize_detected = true;
          err = ESP_ERR_NO_MEM;
          esp_http_client_close(event->client);
        } else {
          state->expected_len = content_length;
          if (content_length > state->size) {
            void* resized = heap_caps_realloc(state->buf, content_length,
                                              MALLOC_CAP_SPIRAM);
            if (!resized) {
              ESP_LOGE(TAG, "Failed to reserve Content-Length buffer (%zu)",
                       content_length);
              free(state->buf);
              state->buf = nullptr;
              err = ESP_ERR_NO_MEM;
              esp_http_client_close(event->client);
              break;
            }
            state->buf = resized;
            state->size = content_length;
          }
          ESP_LOGI(TAG, "Content-Length Header: %zu", content_length);
        }
      }

      if (strcasecmp(event->header_key, "Tronbyt-Brightness") == 0) {
        state->brightness =
            static_cast<uint8_t>(atoi(event->header_value));
        ESP_LOGD(TAG, "Tronbyt-Brightness value: %d%%",
                 state->brightness);
      } else if (strcasecmp(event->header_key, "Tronbyt-Dwell-Secs") ==
                 0) {
        state->dwell_secs = atoi(event->header_value);
      } else if (strcasecmp(event->header_key, "Tronbyt-OTA-URL") == 0) {
        if (state->ota_url) free(state->ota_url);
        size_t url_len = strlen(event->header_value) + 1;
        state->ota_url = static_cast<char*>(
            heap_caps_malloc(url_len, MALLOC_CAP_SPIRAM));
        if (state->ota_url) {
          memcpy(state->ota_url, event->header_value, url_len);
        }
        ESP_LOGI(TAG, "Found OTA URL: %s", state->ota_url);
      }
      break;

    case HTTP_EVENT_ON_DATA:
      if (!event->user_data) {
        ESP_LOGW(TAG, "Discarding HTTP response due to missing state");
        break;
      }

      if (state->oversize_detected) {
        ESP_LOGD(TAG, "Discarding HTTP data due to oversize detection");
        break;
      }

      if (!state->buf) {
        ESP_LOGD(TAG, "Discarding HTTP data due to freed buffer");
        break;
      }

      if (event->data_len + state->len > state->size) {
        size_t required = state->len + event->data_len;
        size_t target = required;
        if (state->expected_len > 0 && state->expected_len <= state->max) {
          target = max_val(required, state->expected_len);
        } else {
          target = max_val(min_val(state->size * 2, state->max), required);
        }
        state->size = target;
        if (state->size > state->max) {
          ESP_LOGE(TAG, "Response size exceeds allowed max (%zu bytes)",
                   state->max);
          if (gfx_display_asset("oversize") != 0) {
            ESP_LOGE(TAG, "Failed to display oversize graphic");
          }
          free(state->buf);
          state->buf = nullptr;
          state->oversize_detected = true;
          err = ESP_ERR_NO_MEM;
          esp_http_client_close(event->client);
          break;
        }

        void* resized = heap_caps_realloc(state->buf, state->size,
                                          MALLOC_CAP_SPIRAM);
        if (!resized) {
          ESP_LOGE(TAG, "Resizing response buffer failed");
          free(state->buf);
          state->buf = nullptr;
          err = ESP_ERR_NO_MEM;
          break;
        }
        state->buf = resized;
      }

      memcpy(static_cast<uint8_t*>(state->buf) + state->len,
             event->data, event->data_len);
      state->len += event->data_len;
      break;

    case HTTP_EVENT_ON_FINISH:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
      break;

    case HTTP_EVENT_DISCONNECTED: {
      ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
      int mbedtls_err = 0;
      esp_err_t tls_err = esp_tls_get_and_clear_last_error(
          static_cast<esp_tls_error_handle_t>(event->data), &mbedtls_err,
          nullptr);
      if (tls_err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP error - %s (mbedtls: 0x%x)",
                 esp_err_to_name(tls_err), mbedtls_err);
      }
    } break;

    case HTTP_EVENT_REDIRECT:
      ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
      esp_http_client_set_redirection(event->client);
      break;

    case HTTP_EVENT_ON_HEADERS_COMPLETE:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADERS_COMPLETE");
      break;

    case HTTP_EVENT_ON_STATUS_CODE:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_STATUS_CODE");
      break;
  }

  return err;
}

}  // namespace

int remote_get(const char* url, uint8_t** buf, size_t* len,
               uint8_t* brightness_pct, int32_t* dwell_secs,
               int* return_status_code, char** ota_url) {
  RemoteState state = {
      .buf = heap_caps_malloc(CONFIG_HTTP_BUFFER_SIZE_DEFAULT,
                              MALLOC_CAP_SPIRAM),
      .len = 0,
      .size = CONFIG_HTTP_BUFFER_SIZE_DEFAULT,
      .max = CONFIG_HTTP_BUFFER_SIZE_MAX,
      .expected_len = 0,
      .brightness = 255,
      .dwell_secs = -1,
      .ota_url = nullptr,
      .oversize_detected = false,
  };

  if (!state.buf) {
    ESP_LOGE(TAG, "couldn't allocate HTTP receive buffer");
    return 1;
  }

  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = http_callback;
  config.user_data = &state;
  config.timeout_ms = 20000;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  // Read auth config once; API key is stable for the lifetime of this call.
  auto cfg = config_get();

  for (int attempt = 0; attempt < REMOTE_MAX_ATTEMPTS; ++attempt) {
    if (attempt > 0) {
      uint32_t base   = REMOTE_BACKOFF_MS[attempt];
      uint32_t jitter = base ? (esp_random() % (base / 2 + 1)) : 0;
      vTaskDelay(pdMS_TO_TICKS(base + jitter));

      // Reset per-attempt accumulation state.
      state.len              = 0;
      state.expected_len     = 0;
      state.oversize_detected = false;
      if (state.ota_url) { free(state.ota_url); state.ota_url = nullptr; }
      state.brightness  = 255;
      state.dwell_secs  = -1;

      // A previous attempt's callback may have freed the buffer on an OOM/
      // realloc failure (sets buf to nullptr). Re-allocate so this attempt
      // starts with a clean receive buffer.
      if (!state.buf) {
        state.buf  = heap_caps_malloc(CONFIG_HTTP_BUFFER_SIZE_DEFAULT,
                                      MALLOC_CAP_SPIRAM);
        state.size = CONFIG_HTTP_BUFFER_SIZE_DEFAULT;
        if (!state.buf) {
          ESP_LOGE(TAG, "couldn't reallocate HTTP receive buffer");
          // state.ota_url is nullptr (reset above); nothing else to free.
          return 1;
        }
      }
    }

    esp_http_client_handle_t http = esp_http_client_init(&config);
    if (!http) {
      // Re-create fails on this attempt; treat as transient and retry.
      ESP_LOGW(TAG, "HTTP client init failed (attempt %d/%d)",
               attempt + 1, REMOTE_MAX_ATTEMPTS);
      continue;
    }

    if (esp_http_client_set_header(http, "X-Firmware-Version",
                                   FIRMWARE_VERSION) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to set firmware version header");
    }

    if (cfg.api_key[0] != '\0') {
      char auth_header[MAX_API_KEY_LEN + 8];  // "Bearer " + key
      snprintf(auth_header, sizeof(auth_header), "Bearer %s", cfg.api_key);
      ESP_LOGD(TAG, "Using Authorization Bearer header");
      if (esp_http_client_set_header(http, "Authorization",
                                     auth_header) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Authorization header");
      }
    }

    esp_err_t err = esp_http_client_perform(http);

    if (err != ESP_OK) {                     // connection / TLS / timeout
      ESP_LOGW(TAG, "fetch attempt %d/%d failed: %s", attempt + 1,
               REMOTE_MAX_ATTEMPTS, esp_err_to_name(err));
      esp_http_client_cleanup(http);
      continue;                              // transient -> retry
    }

    if (state.oversize_detected) {           // fatal: never retry
      ESP_LOGI(TAG, "Request aborted due to oversize content");
      *return_status_code = 413;
      esp_http_client_cleanup(http);
      free(state.buf);                       // safe even if nullptr (OOM path)
      free(state.ota_url);
      return 1;
    }

    int status_code       = esp_http_client_get_status_code(http);
    *return_status_code   = status_code;

    if (status_code == 200) {               // success: transfer buffer ownership
      *buf           = static_cast<uint8_t*>(state.buf);
      *len           = state.len;
      *brightness_pct = state.brightness;
      if (state.dwell_secs > -1 && state.dwell_secs < 300)
        *dwell_secs = state.dwell_secs;
      *ota_url = state.ota_url;
      esp_http_client_cleanup(http);
      return 0;
    }

    ESP_LOGW(TAG, "HTTP status %d (attempt %d/%d)", status_code,
             attempt + 1, REMOTE_MAX_ATTEMPTS);
    esp_http_client_cleanup(http);

    if (!http_status_is_transient(status_code)) {  // 4xx (not 408/429): fatal
      free(state.buf);
      free(state.ota_url);
      return 1;
    }
    // transient status -> loop continues to next attempt
  }

  // All attempts exhausted without a successful response.
  ESP_LOGE(TAG, "fetch failed after %d attempts", REMOTE_MAX_ATTEMPTS);
  free(state.buf);      // safe if nullptr (callback OOM'd the buffer)
  free(state.ota_url);  // safe if nullptr (no OTA header or reset between attempts)
  return 1;
}
