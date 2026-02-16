#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  SCHED_MODE_NONE = 0,
  SCHED_MODE_WEBSOCKET,
  SCHED_MODE_HTTP,
} scheduler_mode_t;

typedef enum {
  SCHED_STATE_IDLE = 0,
  SCHED_STATE_PLAYING,
  SCHED_STATE_HTTP_FETCHING,
  SCHED_STATE_HTTP_PREFETCHING,
} scheduler_state_t;

typedef enum {
  SCHED_EVT_PLAYER_STOPPED = 0,
  SCHED_EVT_PREFETCH_TIMER,
  SCHED_EVT_RETRY_TIMER,
  SCHED_EVT_WS_DISCONNECTED,
} scheduler_event_t;

scheduler_state_t scheduler_fsm_next_state(scheduler_mode_t mode,
                                           scheduler_state_t current,
                                           scheduler_event_t evt,
                                           bool prefetch_ready);

#ifdef __cplusplus
}
#endif
