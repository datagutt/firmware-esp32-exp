#include "retry_backoff.h"

uint32_t retry_backoff_delay_ms(int attempt, uint32_t base_ms, uint32_t max_ms,
                                uint32_t random) {
  uint32_t shift = attempt > 0 ? static_cast<uint32_t>(attempt - 1) : 0;
  if (shift > 16) shift = 16;  // guard against overflow before the shift
  uint64_t full = static_cast<uint64_t>(base_ms) << shift;
  if (full > max_ms) full = max_ms;
  uint32_t half = static_cast<uint32_t>(full / 2);
  return half + (random % (half + 1));
}
