#pragma once

#include <esp_err.h>

/// Initialize the messages module. Spawns the client_info task.
/// Must be called once at startup, before sockets_init connects.
void msg_init();

/// Queue a device/client info sync to be sent from the messages task.
esp_err_t msg_send_client_info();

/// Send device/client info JSON to the server from the current task.
esp_err_t msg_send_client_info_now();
