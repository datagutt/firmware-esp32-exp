#include "scheduler_fsm.h"

scheduler_state_t scheduler_fsm_next_state(scheduler_mode_t mode,
                                           scheduler_state_t current,
                                           scheduler_event_t evt,
                                           bool prefetch_ready) {
  if (evt == SCHED_EVT_WS_DISCONNECTED) {
    return SCHED_STATE_IDLE;
  }

  if (mode == SCHED_MODE_WEBSOCKET) {
    if (evt == SCHED_EVT_PLAYER_STOPPED) {
      return SCHED_STATE_IDLE;
    }
    return current;
  }

  if (mode == SCHED_MODE_HTTP) {
    if (evt == SCHED_EVT_PREFETCH_TIMER && current == SCHED_STATE_PLAYING) {
      return SCHED_STATE_HTTP_PREFETCHING;
    }
    if (evt == SCHED_EVT_RETRY_TIMER) {
      return SCHED_STATE_HTTP_FETCHING;
    }
    if (evt == SCHED_EVT_PLAYER_STOPPED) {
      if (prefetch_ready) {
        return SCHED_STATE_PLAYING;
      }
      if (current == SCHED_STATE_HTTP_PREFETCHING) {
        return SCHED_STATE_HTTP_FETCHING;
      }
      return SCHED_STATE_HTTP_FETCHING;
    }
  }

  return current;
}
