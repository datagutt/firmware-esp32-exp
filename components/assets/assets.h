#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char* name;
  const uint8_t* data;
  size_t size;
} embedded_asset_t;

/// Get an embedded asset by name. Returns NULL if not found.
const embedded_asset_t* asset_find(const char* name);

/// Check if a pointer belongs to any embedded asset (flash memory, not
/// freeable).
bool asset_is_static(const void* ptr);

/// Get the boot animation asset for the configured brand.
const embedded_asset_t* asset_boot(void);

#ifdef __cplusplus
}
#endif
