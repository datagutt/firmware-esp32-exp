#include "webp_frame.h"

webp_frame_check_t webp_frame_check_offsets(uint32_t payload_offset,
                                            uint32_t data_len,
                                            uint32_t payload_len,
                                            size_t max) {
  // Start-of-frame guard: reject if declared total exceeds max buffer.
  if ((size_t)payload_len > max) {
    return WEBP_FRAME_OVERSIZE;
  }

  // Reject if the end of this fragment would exceed max buffer.
  size_t end_offset = (size_t)payload_offset + (size_t)data_len;
  if (end_offset > max) {
    return WEBP_FRAME_OVERSIZE;
  }

  // Reject if fragment offsets exceed the declared payload total.
  if (payload_len > 0 && end_offset > (size_t)payload_len) {
    return WEBP_FRAME_INVALID_OFFSET;
  }

  return WEBP_FRAME_OK;
}
