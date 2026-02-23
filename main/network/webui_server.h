#pragma once

#include <esp_err.h>
#include <esp_http_server.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Mount the LittleFS web UI partition and register the static file handler
/// on the HTTP server. Must be called after http_server_init().
/// If the LittleFS mount fails, a minimal fallback page is served instead.
esp_err_t webui_server_init(void);

#ifdef __cplusplus
}
#endif
