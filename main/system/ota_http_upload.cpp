#include "ota_http_upload.h"

#include <cstdlib>
#include <cstring>

#include <esp_app_format.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace {

const char* TAG = "ota_upload";
constexpr int OTA_BUF_SIZE = 1024;

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

  // Read first chunk and validate app image magic
  int received = httpd_req_recv(
      req, buf,
      (req->content_len < OTA_BUF_SIZE ? req->content_len : OTA_BUF_SIZE));
  if (received <= 0) {
    ESP_LOGE(TAG, "Failed to receive first OTA chunk");
    free(buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
    return ESP_FAIL;
  }

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
