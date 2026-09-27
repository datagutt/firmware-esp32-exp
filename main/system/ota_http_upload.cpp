#include "ota_http_upload.h"

#include <cstdlib>
#include <cstring>

#include <esp_app_format.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "diag_event_ring.h"
#include "ota.h"
#include "ota_bundle.h"
#include "webui_server.h"

namespace {

const char* TAG = "ota_upload";
constexpr int OTA_BUF_SIZE = 1024;
constexpr size_t FLASH_SECTOR_SIZE = 4096;

// Each httpd_req_recv() gives up after the server's recv_wait_timeout. A
// client that stays silent this many times in a row is treated as gone, so a
// stalled upload cannot hold the httpd task and the OTA claim forever.
constexpr int kMaxConsecutiveRecvTimeouts = 3;

// An app image starts with the image header and the first segment header,
// followed by the app descriptor.
constexpr size_t kAppDescOffset =
    sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
constexpr size_t kAppHeadSize = kAppDescOffset + sizeof(esp_app_desc_t);
static_assert(TBUP_HEADER_SIZE + kAppHeadSize <= OTA_BUF_SIZE,
              "first chunk must hold the bundle header and app head");

// Owns everything a failed upload has to undo, so error paths only report.
class UploadSession {
 public:
  UploadSession() : buf_(static_cast<char*>(malloc(OTA_BUF_SIZE))) {}
  ~UploadSession() {
    if (handle_) esp_ota_abort(handle_);
    free(buf_);
    if (!committed_) ota_release();
  }
  UploadSession(const UploadSession&) = delete;
  UploadSession& operator=(const UploadSession&) = delete;

  char* buf() const { return buf_; }

  // Called once the upload's head has validated, so a stray or broken upload
  // does not end the running image's trial.
  esp_err_t begin(const esp_partition_t* part, size_t size) {
    esp_err_t err = ota_confirm_for_update();
    if (err != ESP_OK) return err;
    return esp_ota_begin(part, size, &handle_);
  }
  esp_err_t write(const void* data, size_t len) {
    return esp_ota_write(handle_, data, len);
  }
  // esp_ota_end() releases the handle whatever it returns.
  esp_err_t end() {
    esp_err_t err = esp_ota_end(handle_);
    handle_ = 0;
    return err;
  }
  // The new image is set to boot: keep the OTA claim until the caller reboots.
  void commit() { committed_ = true; }

 private:
  char* buf_;
  esp_ota_handle_t handle_ = 0;
  bool committed_ = false;
};

int recv_bounded(httpd_req_t* req, char* buf, size_t len) {
  for (int timeouts = 0; timeouts < kMaxConsecutiveRecvTimeouts; timeouts++) {
    int r = httpd_req_recv(req, buf, len);
    if (r != HTTPD_SOCK_ERR_TIMEOUT) return r;
  }
  ESP_LOGE(TAG, "Client sent nothing for %d receive timeouts; giving up",
           kMaxConsecutiveRecvTimeouts);
  return HTTPD_SOCK_ERR_TIMEOUT;
}

// Receive until `want` bytes are buffered; httpd_req_recv() may return short.
int recv_at_least(httpd_req_t* req, char* buf, size_t want) {
  size_t have = 0;
  while (have < want) {
    int r = recv_bounded(req, buf + have, want - have);
    if (r <= 0) return r;
    have += r;
  }
  return static_cast<int>(have);
}

// Stream `total` bytes from the HTTP request into the OTA session, using `buf`
// of size `buf_size`. `already` bytes at the start of `buf` have already been
// received and are written first.
esp_err_t stream_to_ota(httpd_req_t* req, UploadSession& session, char* buf,
                        size_t buf_size, size_t already, size_t total) {
  if (already > 0) {
    esp_err_t err = session.write(buf, already);
    if (err != ESP_OK) return err;
  }

  size_t remaining = total - already;
  while (remaining > 0) {
    size_t to_read = remaining < buf_size ? remaining : buf_size;
    int received = recv_bounded(req, buf, to_read);
    if (received <= 0) return ESP_FAIL;
    esp_err_t err = session.write(buf, received);
    if (err != ESP_OK) return err;
    remaining -= received;
  }
  return ESP_OK;
}

// Drain `total` bytes from the HTTP request, discarding them.
void drain_bytes(httpd_req_t* req, char* buf, size_t buf_size, size_t total) {
  size_t remaining = total;
  while (remaining > 0) {
    size_t to_read = remaining < buf_size ? remaining : buf_size;
    int r = recv_bounded(req, buf, to_read);
    if (r <= 0) break;
    remaining -= r;
  }
}

// Validates the app image head before esp_ota_begin() erases anything. On
// failure it has already sent the HTTP error.
esp_err_t check_app_head(httpd_req_t* req, const char* app, size_t len) {
  if (len < kAppHeadSize) {
    ESP_LOGE(TAG, "Image too small (%u bytes)", static_cast<unsigned>(len));
    diag_event_log("ERROR", "ota_validate_fail", -1, "Uploaded image too small");
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Image too small");
    return ESP_ERR_OTA_VALIDATE_FAILED;
  }

  esp_image_header_t hdr;
  esp_app_desc_t desc;
  memcpy(&hdr, app, sizeof(hdr));
  memcpy(&desc, app + kAppDescOffset, sizeof(desc));

  if (hdr.magic != ESP_IMAGE_HEADER_MAGIC ||
      desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
    ESP_LOGE(TAG,
             "Not a valid app image (header magic 0x%02x, app magic 0x%08lx "
             "at offset %u). Did you upload merged_firmware.bin instead of "
             "the app binary?",
             hdr.magic, static_cast<unsigned long>(desc.magic_word),
             static_cast<unsigned>(kAppDescOffset));
    diag_event_log("ERROR", "ota_validate_fail", -1,
                   "Uploaded firmware image magic invalid");
    httpd_resp_send_err(
        req, HTTPD_400_BAD_REQUEST,
        "Invalid firmware file. Use the app .bin, not merged_firmware.bin");
    return ESP_ERR_OTA_VALIDATE_FAILED;
  }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
  // Catches an image built for another chip (an ESP32 build on an S3 board),
  // an unsupported chip revision or a different flash mode, none of which the
  // magic check sees and all of which would fail to boot.
  esp_err_t err = esp_ota_check_image_validity(ESP_PARTITION_TYPE_APP, &hdr,
                                               &desc);
  if (err != ESP_OK) {
    const bool flash_mode = err == ESP_ERR_OTA_SPI_MODE_MISMATCH;
    ESP_LOGE(TAG, "Image not valid for this device (%s): chip id %d",
             esp_err_to_name(err), static_cast<int>(hdr.chip_id));
    diag_event_log("ERROR", "ota_validate_fail", err, esp_err_to_name(err));
    httpd_resp_send_err(
        req, HTTPD_400_BAD_REQUEST,
        flash_mode ? "Firmware was built for a different flash mode"
                   : "Firmware was built for a different chip or revision");
    return err;
  }
#endif
  return ESP_OK;
}

}  // namespace

esp_err_t ota_http_upload_perform(httpd_req_t* req) {
  // Claimed before anything is written so a server-driven OTA, reboot or
  // image-URL change cannot restart the device mid-write.
  if (!ota_claim()) {
    diag_event_log("WARN", "ota_busy", 0,
                   "Upload refused because an update is already running");
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_sendstr(req, "Another update is already running");
    return ESP_FAIL;
  }
  UploadSession session;
  char* buf = session.buf();
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Alloc failed");
    return ESP_FAIL;
  }

  const esp_partition_t* update_partition =
      esp_ota_get_next_update_partition(nullptr);
  if (!update_partition) {
    ESP_LOGE(TAG, "No OTA partition found");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No partition");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%lx",
           update_partition->subtype, update_partition->address);
  diag_event_log("INFO", "ota_start", 0, "HTTP upload OTA started");

  // Buffer enough of the upload to see the bundle header and the app image
  // head behind it; a short upload is buffered whole and rejected below.
  size_t head_want = TBUP_HEADER_SIZE + kAppHeadSize;
  if (req->content_len < head_want) head_want = req->content_len;
  int received = recv_at_least(req, buf, head_want);
  if (received <= 0) {
    ESP_LOGE(TAG, "Failed to receive first OTA chunk");
    diag_event_log("ERROR", "ota_receive_fail", received,
                   "Failed to receive first OTA chunk");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
    return ESP_FAIL;
  }

  // Parse and validate the TBUP bundle header (pure; no I/O).
  tbup_header_t tbup = {};
  tbup_result_t tbup_result = tbup_parse_header(
      reinterpret_cast<const uint8_t*>(buf),
      static_cast<size_t>(received),
      req->content_len,
      &tbup);

  if (tbup_result == TBUP_ERR_HEADER_SHORT) {
    ESP_LOGE(TAG, "First chunk too small for TBUP header");
    diag_event_log("ERROR", "ota_validate_fail", -1,
                   "Bundle header too small");
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid bundle header");
    return ESP_FAIL;
  } else if (tbup_result == TBUP_ERR_SIZE_MISMATCH) {
    ESP_LOGE(TAG, "Content-Length %zu != header+app+webui (%lu)",
             req->content_len,
             (unsigned long)(TBUP_HEADER_SIZE + tbup.app_size + tbup.webui_size));
    diag_event_log("ERROR", "ota_validate_fail", -1,
                   "Bundle content length mismatch");
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bundle size mismatch");
    return ESP_FAIL;
  } else if (tbup_result == TBUP_ERR_APP_EMPTY) {
    ESP_LOGE(TAG, "Bundle app_size is zero");
    diag_event_log("ERROR", "ota_validate_fail", -1,
                   "Bundle app size is zero");
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty app in bundle");
    return ESP_FAIL;
  }

  const bool is_bundle = (tbup_result != TBUP_NOT_BUNDLE);

  if (is_bundle) {
    // --- Bundle mode: TBUP header + app binary + optional webui image ---
    ESP_LOGI(TAG, "TBUP bundle detected");

    uint32_t app_size = tbup.app_size;
    uint32_t webui_size = tbup.webui_size;

    ESP_LOGI(TAG, "Bundle: app=%lu bytes, webui=%lu bytes",
             (unsigned long)app_size, (unsigned long)webui_size);

    // The first chunk contains the header + start of app data.
    // app data starts at offset tbup.app_offset (= TBUP_HEADER_SIZE) in the buffer.
    size_t app_in_buf = received - tbup.app_offset;
    size_t app_already = app_in_buf < app_size ? app_in_buf : app_size;

    if (check_app_head(req, buf + tbup.app_offset, app_already) != ESP_OK) {
      return ESP_FAIL;
    }

    // --- Phase 1: Write app firmware via OTA API ---
    esp_err_t err = session.begin(update_partition, app_size);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
      diag_event_log("ERROR", "ota_begin_fail", err, "OTA begin failed");
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "OTA begin failed");
      return ESP_FAIL;
    }

    // Move app data to start of buffer for the initial write
    memmove(buf, buf + TBUP_HEADER_SIZE, app_already);

    err = stream_to_ota(req, session, buf, OTA_BUF_SIZE, app_already,
                        app_size);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "App streaming failed (%s)", esp_err_to_name(err));
      diag_event_log("ERROR", "ota_write_fail", err, "App streaming failed");
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "App write failed");
      return ESP_FAIL;
    }

    err = session.end();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
      diag_event_log("ERROR", "ota_finish_fail", err, "OTA end failed");
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "OTA end failed");
      return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)",
               esp_err_to_name(err));
      diag_event_log("ERROR", "ota_set_boot_fail", err,
                     "Set boot partition failed");
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Set boot failed");
      return ESP_FAIL;
    }
    session.commit();

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
          int r = recv_bounded(req, buf, to_read);
          if (r <= 0) {
            ESP_LOGE(TAG, "WebUI receive failed");
            webui_ok = false;
            break;
          }
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
        }

        if (webui_ok) {
          ESP_LOGI(TAG, "WebUI partition written (%lu bytes)",
                   (unsigned long)written);
        } else {
          ESP_LOGW(TAG, "WebUI write incomplete — will use fallback page");
        }
      }
    }

    ESP_LOGI(TAG, "Bundle OTA upload successful");
    diag_event_log("INFO", "ota_success", 0,
                   webui_size > 0 ? "Bundle OTA upload successful"
                                  : "App-only OTA upload successful");
    return ESP_OK;
  }

  // --- Plain mode: standard app-only upload ---

  if (check_app_head(req, buf, received) != ESP_OK) {
    return ESP_FAIL;
  }

  esp_err_t err = session.begin(update_partition, req->content_len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
    diag_event_log("ERROR", "ota_begin_fail", err, "OTA begin failed");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "OTA begin failed");
    return ESP_FAIL;
  }

  err = stream_to_ota(req, session, buf, OTA_BUF_SIZE, received,
                      req->content_len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OTA upload failed (%s)", esp_err_to_name(err));
    diag_event_log("ERROR", "ota_write_fail", err, "OTA upload failed");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
    return ESP_FAIL;
  }

  err = session.end();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
    diag_event_log("ERROR", "ota_finish_fail", err, "OTA end failed");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
    return ESP_FAIL;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)",
             esp_err_to_name(err));
    diag_event_log("ERROR", "ota_set_boot_fail", err,
                   "Set boot partition failed");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Set boot failed");
    return ESP_FAIL;
  }
  session.commit();

  ESP_LOGI(TAG, "OTA upload successful");
  diag_event_log("INFO", "ota_success", 0, "OTA upload successful");
  return ESP_OK;
}
