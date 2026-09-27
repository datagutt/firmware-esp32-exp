#pragma once

#include <esp_websocket_client.h>

/// Create the text-message mailbox and the consumer task that processes
/// server messages, saves the settings they carry and sends client_info.
void handlers_init();

/// Drop queued and partially reassembled text messages.
void handlers_deinit();

/// Ask the consumer task to send client_info. Coalesces with pending requests.
/// Returns ESP_ERR_INVALID_STATE before handlers_init().
esp_err_t handlers_request_client_info();

/// Reassemble inbound text (JSON) frame chunks, including continuation
/// frames, and enqueue each complete message for async processing.
void handle_text_message(esp_websocket_event_data_t* data);

/// Handle inbound binary (WebP) message chunks, with reassembly.
void handle_binary_message(esp_websocket_event_data_t* data);
