#pragma once

#include <esp_err.h>

/** Register the REST API endpoints on the central HTTP server. They are
 *  applied now if the server is running and again whenever it starts. */
esp_err_t sta_api_start(void);
