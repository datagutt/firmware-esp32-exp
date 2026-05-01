#pragma once

#include <stddef.h>

#include <freertos/FreeRTOS.h>

/// Initialize the sockets module (event-driven WebSocket client).
/// Sets up timers, registers WiFi/IP event handlers, and begins
/// connecting once the network is available.
void sockets_init(const char* url);

/// Deinitialize: stop client, delete timers, free resources.
void sockets_deinit();

/// True when the WebSocket connection is established.
bool sockets_is_connected();

/// Send a text frame on the active WebSocket. Mutex-protected against
/// client teardown. Returns the byte count written (>= 0) on success,
/// or a negative value on error / when not connected.
int sockets_send_text(const char* data, size_t len, TickType_t timeout);
