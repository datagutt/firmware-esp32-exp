#pragma once

#include <esp_err.h>
#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Mount the LittleFS web UI partition (or note that fallback page will be
/// used). Must be called after http_server_init().
esp_err_t webui_server_init(void);

/// Returns true if the LittleFS web UI partition is mounted.
bool webui_fs_mounted(void);

/// Register the wildcard static file handler on the HTTP server.
/// Must be called AFTER all API and specific routes are registered,
/// because httpd matches by registration order and /* would shadow them.
void webui_register_wildcard(void);

#ifdef __cplusplus
}
#endif
