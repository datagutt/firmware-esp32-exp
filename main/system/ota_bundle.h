#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// TBUP bundle header layout (16 bytes):
//   [0..3]  magic = 0x50554254 ("TBUP" little-endian)
//   [4..7]  app_size  (little-endian uint32)
//   [8..11] webui_size (little-endian uint32)
//   [12..15] reserved
#define TBUP_HEADER_SIZE 16

typedef struct {
  uint32_t app_size;
  uint32_t webui_size;
  size_t   app_offset;  // always TBUP_HEADER_SIZE on TBUP_OK
} tbup_header_t;

typedef enum {
  TBUP_OK = 0,
  TBUP_NOT_BUNDLE,        // magic absent — caller uses plain-image path
  TBUP_ERR_HEADER_SHORT,  // fewer than 16 bytes available
  TBUP_ERR_SIZE_MISMATCH, // content_len != 16 + app_size + webui_size
  TBUP_ERR_APP_EMPTY,     // app_size == 0
} tbup_result_t;

// Pure TBUP bundle header parser: no I/O, no ESP-IDF dependencies.
//
// buf/received = first received chunk and its byte count;
// content_len  = HTTP Content-Length value.
//
// app_size and webui_size are populated even on TBUP_ERR_SIZE_MISMATCH so
// callers can log the mismatch detail. On TBUP_OK, out->app_offset is set to
// TBUP_HEADER_SIZE.
tbup_result_t tbup_parse_header(const uint8_t *buf, size_t received,
                                size_t content_len, tbup_header_t *out);

#ifdef __cplusplus
}
#endif
