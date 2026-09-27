#pragma once

#include <esp_err.h>

/// Queue a device/client info sync to be sent from the handlers consumer task.
esp_err_t msg_send_client_info();

/// Send device/client info JSON to the server from the current task.
esp_err_t msg_send_client_info_now();
