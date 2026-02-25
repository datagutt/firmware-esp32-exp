#include "ota_http_upload.h"

#include <cstdlib>
#include <cstring>

#include <esp_app_format.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "webui_server.h"

namespace {

const char* TAG = "ota_upload";
constexpr int OTA_BUF_SIZE = 1024;
constexpr uint32_t TBUP_MAGIC = 0x50554254;  // "TBUP" little-endian
constexpr size_t TBUP_HEADER_SIZE = 16;
constexpr size_t FLASH_SECTOR_SIZE = 4096;

// Receive exactly `len` bytes from the HTTP stream into `dst`.
// Returns ESP_OK on success, ESP_FAIL on connection error.
esp_err_t recv_exact(httpd_req_t* req, char* dst, size_t len) {
  size_t got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, dst + got, len - got);
    if (r > 0) {
      got += r;
    } else if (r == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    } else {
      return ESP_FAIL;
    }
  }
  return ESP_OK;
}

// Stream `total` bytes from the HTTP request into an OTA handle, using `buf`
// of size `buf_size`. `already` bytes at the start of `buf` have already been
// received and should be written first.
esp_err_t stream_to_ota(httpd_req_t* req, esp_ota_handle_t handle, char* buf,
                        size_t buf_size, size_t already, size_t total) {
  // Write the bytes we already have in the buffer
  if (already > 0) {
    esp_err_t err = esp_ota_write(handle, buf, already);
    if (err != ESP_OK) return err;
  }

  size_t remaining = total - already;
  while (remaining > 0) {
    size_t to_read = remaining < buf_size ? remaining : buf_size;
    int received = httpd_req_recv(req, buf, to_read);
    if (received > 0) {
      esp_err_t err = esp_ota_write(handle, buf, received);
      if (err != ESP_OK) return err;
      remaining -= received;
    } else if (received == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    } else {
      return ESP_FAIL;
    }
  }
  return ESP_OK;
}

// Drain `total` bytes from the HTTP request, discarding them.
void drain_bytes(httpd_req_t* req, char* buf, size_t buf_size, size_t total) {
  size_t remaining = total;
  while (remaining > 0) {
    size_t to_read = remaining < buf_size ? remaining : buf_size;
    int r = httpd_req_recv(req, buf, to_read);
    if (r > 0) {
      remaining -= r;
    } else if (r == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    } else {
      break;
    }
  }
}

}  // namespace

esp_err_t ota_http_upload_perform(httpd_req_t* req) {
  esp_ota_handle_t update_handle = 0;

  auto* buf = static_cast<char*>(malloc(OTA_BUF_SIZE));
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Alloc failed");
    return ESP_FAIL;
  }

  const esp_partition_t* update_partition =
      esp_ota_get_next_update_partition(nullptr);
  if (!update_partition) {
    ESP_LOGE(TAG, "No OTA partition found");
    free(buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No partition");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%lx",
           update_partition->subtype, update_partition->address);

  // Read first chunk and detect format
  int received = httpd_req_recv(
      req, buf,
      (req->content_len < OTA_BUF_SIZE ? req->content_len : OTA_BUF_SIZE));
  if (received <= 0) {
    ESP_LOGE(TAG, "Failed to receive first OTA chunk");
    free(buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
    return ESP_FAIL;
  }

  // Check for TBUP bundle magic
  uint32_t first_word = 0;
  if (static_cast<size_t>(received) >= sizeof(uint32_t)) {
    memcpy(&first_word, buf, sizeof(first_word));
  }

  const bool is_bundle = (first_word == TBUP_MAGIC);

  if (is_bundle) {
    // --- Bundle mode: TBUP header + app binary + optional webui image ---
    ESP_LOGI(TAG, "TBUP bundle detected");

    if (static_cast<size_t>(received) < TBUP_HEADER_SIZE) {
      ESP_LOGE(TAG, "First chunk too small for TBUP header");
      free(buf);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid bundle header");
      return ESP_FAIL;
    }

    uint32_t app_size = 0;
    uint32_t webui_size = 0;
    memcpy(&app_size, buf + 4, sizeof(app_size));
    memcpy(&webui_size, buf + 8, sizeof(webui_size));

    ESP_LOGI(TAG, "Bundle: app=%lu bytes, webui=%lu bytes",
             (unsigned long)app_size, (unsigned long)webui_size);

    // Validate content length matches header
    if (req->content_len != TBUP_HEADER_SIZE + app_size + webui_size) {
      ESP_LOGE(TAG, "Content-Length %zu != header+app+webui (%lu)",
               req->content_len,
               (unsigned long)(TBUP_HEADER_SIZE + app_size + webui_size));
      free(buf);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bundle size mismatch");
      return ESP_FAIL;
    }

    if (app_size == 0) {
      ESP_LOGE(TAG, "Bundle app_size is zero");
      free(buf);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty app in bundle");
      return ESP_FAIL;
    }

    // The first chunk contains the header + start of app data.
    // app data starts at offset TBUP_HEADER_SIZE in the buffer.
    size_t app_in_buf = received - TBUP_HEADER_SIZE;

    // Validate app magic within the buffered app data
    constexpr size_t kAppDescOffset =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (app_in_buf >= kAppDescOffset + sizeof(uint32_t)) {
      uint32_t app_magic = 0;
      memcpy(&app_magic, buf + TBUP_HEADER_SIZE + kAppDescOffset,
             sizeof(app_magic));
      if (app_magic != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "App in bundle has bad magic 0x%08lx at offset %u",
                 (unsigned long)app_magic, (unsigned)kAppDescOffset);
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Invalid app image in bundle");
        return ESP_FAIL;
      }
    }

    // --- Phase 1: Write app firmware via OTA API ---
    esp_err_t err =
        esp_ota_begin(update_partition, app_size, &update_handle);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
      free(buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "OTA begin failed");
      return ESP_FAIL;
    }

    // Write the app data already in the buffer (after TBUP header)
    size_t app_already = app_in_buf < app_size ? app_in_buf : app_size;
    // Move app data to start of buffer for the initial write
    memmove(buf, buf + TBUP_HEADER_SIZE, app_already);

    err = stream_to_ota(req, update_handle, buf, OTA_BUF_SIZE, app_already,
                        app_size);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "App streaming failed (%s)", esp_err_to_name(err));
      esp_ota_end(update_handle);
      free(buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "App write failed");
      return ESP_FAIL;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
      free(buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "OTA end failed");
      return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)",
               esp_err_to_name(err));
      free(buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Set boot failed");
      return ESP_FAIL;
    }

    ESP_LOGI(TAG, "App OTA written and boot partition set");

    // --- Phase 2: Write WebUI LittleFS image (if present) ---
    if (webui_size > 0) {
      const esp_partition_t* webui_part = esp_partition_find_first(
          ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "webui");

      if (!webui_part) {
        ESP_LOGW(TAG,
                 "No webui partition found (4MB board?) — draining %lu bytes",
                 (unsigned long)webui_size);
        drain_bytes(req, buf, OTA_BUF_SIZE, webui_size);
      } else {
        if (webui_size > webui_part->size) {
          ESP_LOGE(TAG, "WebUI image (%lu) exceeds partition size (%lu)",
                   (unsigned long)webui_size, (unsigned long)webui_part->size);
          drain_bytes(req, buf, OTA_BUF_SIZE, webui_size);
          // App was already written successfully — return OK
          free(buf);
          ESP_LOGI(TAG, "OTA upload successful (app only, webui too large)");
          return ESP_OK;
        }

        // Unmount filesystem before erasing
        webui_unmount();

        // Erase partition (rounded up to sector boundary)
        size_t erase_size =
            (webui_size + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1);
        err = esp_partition_erase_range(webui_part, 0, erase_size);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "WebUI partition erase failed (%s)",
                   esp_err_to_name(err));
          drain_bytes(req, buf, OTA_BUF_SIZE, webui_size);
          free(buf);
          ESP_LOGI(TAG, "OTA upload successful (app only, webui erase failed)");
          return ESP_OK;
        }

        // Stream webui data directly to the partition
        size_t written = 0;
        size_t remaining = webui_size;
        bool webui_ok = true;

        while (remaining > 0) {
          size_t to_read = remaining < (size_t)OTA_BUF_SIZE ? remaining
                                                            : (size_t)OTA_BUF_SIZE;
          int r = httpd_req_recv(req, buf, to_read);
          if (r > 0) {
            err = esp_partition_write(webui_part, written, buf, r);
            if (err != ESP_OK) {
              ESP_LOGE(TAG, "WebUI partition write failed at offset %u (%s)",
                       (unsigned)written, esp_err_to_name(err));
              // Drain remaining bytes
              remaining -= r;
              drain_bytes(req, buf, OTA_BUF_SIZE, remaining);
              webui_ok = false;
              break;
            }
            written += r;
            remaining -= r;
          } else if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
          } else {
            ESP_LOGE(TAG, "WebUI receive failed");
            webui_ok = false;
            break;
          }
        }

        if (webui_ok) {
          ESP_LOGI(TAG, "WebUI partition written (%lu bytes)",
                   (unsigned long)written);
        } else {
          ESP_LOGW(TAG, "WebUI write incomplete — will use fallback page");
        }
      }
    }

    free(buf);
    ESP_LOGI(TAG, "Bundle OTA upload successful");
    return ESP_OK;
  }

  // --- Plain mode: standard app-only upload (existing behavior) ---

  // A valid app binary has the app descriptor magic (0xABCD5432) at byte 32
  // (24-byte image header + 8-byte segment header).
  constexpr size_t kAppDescOffset =
      sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  if (static_cast<size_t>(received) >= kAppDescOffset + sizeof(uint32_t)) {
    uint32_t magic = 0;
    memcpy(&magic, buf + kAppDescOffset, sizeof(magic));
    if (magic != ESP_APP_DESC_MAGIC_WORD) {
      ESP_LOGE(TAG,
               "Not a valid app image (magic 0x%08lx at offset %u, expected "
               "0x%08lx). Did you upload merged_firmware.bin instead of the "
               "app binary?",
               (unsigned long)magic, (unsigned)kAppDescOffset,
               (unsigned long)ESP_APP_DESC_MAGIC_WORD);
      free(buf);
      httpd_resp_send_err(
          req, HTTPD_400_BAD_REQUEST,
          "Invalid firmware file. Use the app .bin, not merged_firmware.bin");
      return ESP_FAIL;
    }
  }

  esp_err_t err =
      esp_ota_begin(update_partition, req->content_len, &update_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
    free(buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "OTA begin failed");
    return ESP_FAIL;
  }

  // Write the first chunk we already received
  err = esp_ota_write(update_handle, buf, received);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
    esp_ota_end(update_handle);
    free(buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
    return ESP_FAIL;
  }

  int remaining = req->content_len - received;
  while (remaining > 0) {
    received = httpd_req_recv(
        req, buf, (remaining < OTA_BUF_SIZE ? remaining : OTA_BUF_SIZE));
    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      ESP_LOGE(TAG, "File receive failed");
      esp_ota_end(update_handle);
      free(buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Receive failed");
      return ESP_FAIL;
    }

    err = esp_ota_write(update_handle, buf, received);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
      esp_ota_end(update_handle);
      free(buf);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Write failed");
      return ESP_FAIL;
    }

    remaining -= received;
  }

  free(buf);

  err = esp_ota_end(update_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
    return ESP_FAIL;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)",
             esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Set boot failed");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "OTA upload successful");
  return ESP_OK;
}
