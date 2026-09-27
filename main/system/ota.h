#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void run_ota(const char* url);
bool ota_in_progress(void);

/// Claim the device-wide OTA slot for an update written outside run_ota() (the
/// HTTP upload): pauses playback and shows the OTA screen. Returns false while
/// another update holds it. After a successful write the caller reboots with
/// the claim still held, so no server reboot or URL change can restart the
/// device first; after a failure it calls ota_release().
bool ota_claim(void);
void ota_release(void);

/// On the first boot of a new OTA image (pending verify), confirm it once it
/// proves healthy and roll back if it has not within 10 minutes. No-op on any
/// other boot. `server_configured` selects what counts as healthy (see
/// ota.cpp). Call before WiFi starts so no health event is missed.
void ota_start_health_check(bool server_configured);

#ifdef __cplusplus
}
#endif
