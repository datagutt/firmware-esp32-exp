#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  WEBP_FRAME_OK = 0,
  WEBP_FRAME_OVERSIZE,        // declared total or end_offset exceeds max buffer
  WEBP_FRAME_INVALID_OFFSET,  // end_offset > declared payload total
} webp_frame_check_t;

// Pure WebSocket binary frame offset validator: no I/O, no ESP-IDF dependencies.
//
// max = CONFIG_HTTP_BUFFER_SIZE_MAX (passed in to keep the module ESP-IDF-free).
//
// Checks (in order):
//   1. payload_len > max → WEBP_FRAME_OVERSIZE      (start-of-frame guard)
//   2. end_offset = payload_offset + data_len > max  → WEBP_FRAME_OVERSIZE
//   3. payload_len > 0 && end_offset > payload_len   → WEBP_FRAME_INVALID_OFFSET
webp_frame_check_t webp_frame_check_offsets(uint32_t payload_offset,
                                            uint32_t data_len,
                                            uint32_t payload_len,
                                            size_t max);

#ifdef __cplusplus
}
#endif
