#pragma once

#include <stdbool.h>
#include <stdint.h>

// Retrieves url via HTTP GET. Caller is responsible for freeing buf,
// ota_url, and image_url (if not NULL) on success.
//
// `image_url` receives the value of a Tronbyt-Image-URL response header (the
// server asking the device to repoint at a different endpoint) and
// `reboot_requested` the value of Tronbyt-Reboot. Both are only written on a
// successful response (200/304); on any error path they are left untouched and
// nothing is allocated.
int remote_get(const char* url, uint8_t** buf, size_t* len,
               uint8_t* brightness_pct, int32_t* dwell_secs, int* return_code,
               char** ota_url, char** image_url, bool* reboot_requested);

// Drops the cached conditional-GET validator so the next remote_get performs a
// full download instead of possibly receiving a 304. Call this after the panel
// has been blanked while the displayed content is unchanged (for example when
// resuming from quiet hours): a 304 would tell the scheduler to keep showing
// content that is no longer on screen, leaving the panel blank.
void remote_reset_cache(void);