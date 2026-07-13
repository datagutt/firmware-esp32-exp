#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of quiet-hours windows the device stores.
#define QUIET_HOURS_MAX_WINDOWS 4

// A quiet-hours window expressed in local wall-clock time (device timezone).
//
// A window whose end is not strictly after its start (end <= start) wraps past
// midnight, so 22:00 -> 07:00 covers the night. day_mask selects the days a
// window may START on: bit0 = Sunday .. bit6 = Saturday. The wrapped morning
// portion is attributed to the day the window started, so a window that begins
// Friday night keeps the display dark into Saturday morning even when Saturday
// itself is not selected. A zero-length window (end == start) is never active.
typedef struct {
  bool enabled;
  uint8_t start_hour;  // 0-23
  uint8_t start_min;   // 0-59
  uint8_t end_hour;    // 0-23
  uint8_t end_min;     // 0-59
  uint8_t day_mask;    // bit0 = Sunday .. bit6 = Saturday
} quiet_window_t;

// True when the local wall-clock in `local` falls inside `w`. Pure: touches
// neither the system clock nor NVS, which keeps it host-testable.
bool quiet_window_contains(const quiet_window_t* w, const struct tm* local);

// True when any enabled window in `windows[0..count)` contains `local`.
bool quiet_hours_any_active(const quiet_window_t* windows, size_t count,
                            const struct tm* local);

#ifdef __cplusplus
}
#endif
