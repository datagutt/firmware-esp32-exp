#include "ota_bundle.h"

#include <string.h>

static const uint32_t TBUP_MAGIC = 0x50554254;  // "TBUP" little-endian

tbup_result_t tbup_parse_header(const uint8_t *buf, size_t received,
                                size_t content_len, tbup_header_t *out) {
  uint32_t first_word = 0;
  if (received >= sizeof(uint32_t)) {
    memcpy(&first_word, buf, sizeof(first_word));
  }

  if (first_word != TBUP_MAGIC) {
    return TBUP_NOT_BUNDLE;
  }

  if (received < TBUP_HEADER_SIZE) {
    return TBUP_ERR_HEADER_SHORT;
  }

  uint32_t app_size = 0;
  uint32_t webui_size = 0;
  memcpy(&app_size, buf + 4, sizeof(app_size));
  memcpy(&webui_size, buf + 8, sizeof(webui_size));

  // Populate sizes before the mismatch check so callers can log the detail.
  if (out) {
    out->app_size = app_size;
    out->webui_size = webui_size;
    out->app_offset = TBUP_HEADER_SIZE;
  }

  if (content_len != TBUP_HEADER_SIZE + app_size + webui_size) {
    return TBUP_ERR_SIZE_MISMATCH;
  }

  if (app_size == 0) {
    return TBUP_ERR_APP_EMPTY;
  }

  return TBUP_OK;
}
