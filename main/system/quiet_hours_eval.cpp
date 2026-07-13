#include "quiet_hours_eval.h"

namespace {

int minutes_of_day(int hour, int min) { return hour * 60 + min; }

}  // namespace

bool quiet_window_contains(const quiet_window_t* w, const struct tm* local) {
  if (!w || !local || !w->enabled) return false;
  if (w->start_hour > 23 || w->end_hour > 23 || w->start_min > 59 ||
      w->end_min > 59) {
    return false;
  }

  const int start = minutes_of_day(w->start_hour, w->start_min);
  const int end = minutes_of_day(w->end_hour, w->end_min);
  const int now = minutes_of_day(local->tm_hour, local->tm_min);
  const int wday = ((local->tm_wday % 7) + 7) % 7;  // 0=Sunday .. 6=Saturday
  const int prev_wday = (wday + 6) % 7;

  const bool starts_today = (w->day_mask & (1u << wday)) != 0;
  const bool started_yesterday = (w->day_mask & (1u << prev_wday)) != 0;

  if (end > start) {
    // Same-day window: active only on a selected day, within [start, end).
    return starts_today && now >= start && now < end;
  }
  if (end == start) {
    // Zero-length window: never active.
    return false;
  }
  // Overnight window: the evening portion (>= start) belongs to the start day,
  // the morning portion (< end) belongs to the day the window started, i.e.
  // yesterday relative to "now".
  return (starts_today && now >= start) || (started_yesterday && now < end);
}

bool quiet_hours_any_active(const quiet_window_t* windows, size_t count,
                            const struct tm* local) {
  if (!windows || !local) return false;
  for (size_t i = 0; i < count; ++i) {
    if (quiet_window_contains(&windows[i], local)) return true;
  }
  return false;
}
