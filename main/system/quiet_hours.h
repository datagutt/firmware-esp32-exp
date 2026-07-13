#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <cJSON.h>

#include "quiet_hours_eval.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the quiet-hours subsystem: load stored windows from NVS, run an initial
// evaluation, and start the periodic evaluator. Must be called after the event
// bus, display, scheduler, and NTP have been initialized, because the initial
// evaluation may already publish a display-off event that the scheduler acts
// on. Safe to call once.
void quiet_hours_init(void);

// True while the device is inside an enabled quiet window. Fails open (returns
// false) until the system clock has been set via NTP, so an unsynced device
// never goes dark unexpectedly.
bool quiet_hours_is_active(void);

// Copy the stored windows into out[] (up to `max`). Returns the count written.
size_t quiet_hours_get_windows(quiet_window_t* out, size_t max);

// Replace the entire stored window set (empty-slot entries are kept as given,
// capped at QUIET_HOURS_MAX_WINDOWS), persist to NVS, and re-evaluate now.
void quiet_hours_set_windows(const quiet_window_t* windows, size_t count);

// Force an immediate re-evaluation. Used on config change and time sync so the
// display responds without waiting for the next periodic tick.
void quiet_hours_reevaluate(void);

// Report the server-driven quiet signal (the Tronbyt-Quiet header on the HTTP
// /next response). This is OR-combined with the local window evaluation, so the
// display goes dark when either the server or a local window says quiet. The
// local engine remains a standalone fallback when the server sends no signal.
// Re-evaluates immediately, so display-off/on events fire on the edge.
void quiet_hours_set_remote_active(bool active);

// Parse and apply a quiet-hours config object of the shape
//   { "windows": [ {enabled, start_hour, start_min, end_hour, end_min, days} ] }
// where "days" is an optional bitmask (bit0 = Sunday .. bit6 = Saturday,
// default all days) and "enabled" is optional (default true). A missing or
// empty "windows" array clears all windows (disables quiet hours). On success
// the stored windows are replaced; on invalid input returns false and writes a
// human-readable message into err. This is the single validator shared by the
// WebSocket config path and the REST endpoint.
bool quiet_hours_apply_json(const cJSON* obj, char* err, size_t err_len);

// Build a JSON object describing the current state:
//   { "active": bool, "windows": [ ... ] }
// Returns a newly allocated cJSON object the caller owns, or nullptr on OOM.
cJSON* quiet_hours_to_json(void);

#ifdef __cplusplus
}
#endif
