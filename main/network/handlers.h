#pragma once

#include <esp_websocket_client.h>

/// Create the text-message queue and consumer task.
void handlers_init();

/// Stop the consumer task and drain the queue.
void handlers_deinit();

/// Reassemble inbound text (JSON) frame chunks, including continuation
/// frames, and enqueue each complete message for async processing.
void handle_text_message(esp_websocket_event_data_t* data);

/// Handle inbound binary (WebP) message chunks, with reassembly.
void handle_binary_message(esp_websocket_event_data_t* data);
