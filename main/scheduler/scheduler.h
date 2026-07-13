#pragma once

/// Initialize the scheduler (registers player event handlers, creates timers).
/// Must be called after gfx_initialize().
void scheduler_init();

/// Start in WebSocket mode (event-driven, server pushes content).
void scheduler_start_ws();

/// Start in HTTP mode with prefetch timer.
/// @param url  The image URL to poll.
void scheduler_start_http(const char* url);

/// Stop the scheduler and all timers.
void scheduler_stop();

/// Suspend playback for quiet hours: stop timers, stop the player, and blank
/// the panel. Player and timer events are ignored until resumed. Idempotent.
void scheduler_pause();

/// Leave the paused state and re-derive playback: HTTP mode refetches now,
/// WebSocket mode returns to idle to await the next server push. Idempotent.
void scheduler_resume();

/// Called by sockets module on WebSocket connect.
void scheduler_on_ws_connect();

/// Called by sockets module on WebSocket disconnect.
void scheduler_on_ws_disconnect();
