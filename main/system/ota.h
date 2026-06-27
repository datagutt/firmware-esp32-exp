#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void run_ota(const char* url);
bool ota_in_progress(void);
void ota_confirm_running_app(void);
void ota_schedule_health_confirm(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif
