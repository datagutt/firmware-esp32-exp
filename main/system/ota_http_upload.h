#pragma once

#include <esp_err.h>
#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Perform OTA upload from an HTTP request.
///
/// Receives firmware data in chunks, validates the app image header,
/// writes to the next OTA partition, and sets the boot partition on success.
///
/// On failure, sends an error HTTP response and returns an error code.
/// On success, returns ESP_OK — the caller should send its own success
/// response and trigger a reboot.
esp_err_t ota_http_upload_perform(httpd_req_t* req);

#ifdef __cplusplus
}
#endif
