#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_EVENT_LEVEL_MAX_LEN 7
#define DIAG_EVENT_TYPE_MAX_LEN 23
#define DIAG_EVENT_MESSAGE_MAX_LEN 127

typedef struct {
  uint32_t seq;
  uint32_t uptime_ms;
  int32_t code;
  char level[DIAG_EVENT_LEVEL_MAX_LEN + 1];
  char type[DIAG_EVENT_TYPE_MAX_LEN + 1];
  char message[DIAG_EVENT_MESSAGE_MAX_LEN + 1];
} diag_event_t;

void diag_event_ring_init(void);

void diag_event_ring_set_enabled(bool enabled);

bool diag_event_ring_is_enabled(void);

void diag_event_log(const char* level, const char* type, int32_t code,
                    const char* message);

size_t diag_event_get_recent(diag_event_t* out, size_t max_events);

size_t diag_event_get_recent_by_type(const char* type, diag_event_t* out,
                                     size_t max_events);

size_t diag_event_get_recent_by_prefix(const char* prefix, diag_event_t* out,
                                       size_t max_events);

#ifdef __cplusplus
}
#endif
