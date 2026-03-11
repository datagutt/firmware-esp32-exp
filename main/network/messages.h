#pragma once

#include <esp_err.h>
#include <esp_websocket_client.h>

/// Initialize the messages module with the active WS client handle.
void msg_init(esp_websocket_client_handle_t client);

/// Queue a device/client info sync to be sent from the messages task.
esp_err_t msg_send_client_info();

/// Send device/client info JSON to the server from the current task.
esp_err_t msg_send_client_info_now();
