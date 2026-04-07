#include "assets.h"

#include <cstring>

#include "asset_data.h"

namespace {

const embedded_asset_t s_assets[] = {
#include "asset_registry.inc"
};

constexpr size_t kAssetCount = sizeof(s_assets) / sizeof(s_assets[0]);

}  // namespace

const embedded_asset_t* asset_find(const char* name) {
  if (!name) return nullptr;
  for (size_t i = 0; i < kAssetCount; i++) {
    if (strcmp(s_assets[i].name, name) == 0) return &s_assets[i];
  }
  return nullptr;
}

bool asset_is_static(const void* ptr) {
  for (size_t i = 0; i < kAssetCount; i++) {
    if (ptr == s_assets[i].data) return true;
  }
  return false;
}

const embedded_asset_t* asset_boot(void) { return asset_find("boot"); }
