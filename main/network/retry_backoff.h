#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Exponential backoff with jitter: returns a delay in [50%, 100%] of
// base_ms * 2^(attempt - 1), capped at max_ms. The jitter de-synchronizes a
// fleet of devices that all lost the same server or AP at the same moment.
// `random` is any uniformly distributed 32-bit value (esp_random() on
// device); it is a parameter so the function stays pure and host-testable.
uint32_t retry_backoff_delay_ms(int attempt, uint32_t base_ms, uint32_t max_ms,
                                uint32_t random);

#ifdef __cplusplus
}
#endif
