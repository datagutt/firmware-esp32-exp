#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool device_temperature_get_c(float* out_celsius);

#ifdef __cplusplus
}
#endif
