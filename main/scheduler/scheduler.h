#pragma once

#include <cstdint>

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

/// Why playback is suspended. Each reason is held and released on its own and
/// playback resumes only once none is held, so e.g. quiet hours ending does
/// not relight a panel the user switched off by touch.
enum scheduler_pause_reason_t : uint8_t {
  SCHEDULER_PAUSE_QUIET_HOURS = 1 << 0,
  SCHEDULER_PAUSE_USER_OFF = 1 << 1,
  SCHEDULER_PAUSE_OTA = 1 << 2,
  SCHEDULER_PAUSE_DISPLAY_REINIT = 1 << 3,
};

/// Hold `reason`. The first reason held stops timers, stops the player and
/// blanks the panel; player and timer events are ignored until every reason is
/// released. Idempotent per reason. Safe to call before scheduler_init(): the
/// scheduler then starts out paused.
void scheduler_pause(scheduler_pause_reason_t reason);

/// Release `reason`. Releasing the last one re-derives playback: HTTP mode
/// refetches now, WebSocket mode returns to idle to await the next server
/// push. Idempotent per reason.
void scheduler_resume(scheduler_pause_reason_t reason);

/// True while any pause reason is held.
bool scheduler_is_paused();

/// Called by sockets module on WebSocket connect.
void scheduler_on_ws_connect();

/// Called by sockets module on WebSocket disconnect.
void scheduler_on_ws_disconnect();
