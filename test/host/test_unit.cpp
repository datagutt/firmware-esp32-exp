#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config_contract.h"
#include "ota_bundle.h"
#include "ota_url_utils.h"
#include "scheduler_fsm.h"
#include "webp_frame.h"

static void test_ota_url_parser() {
  ota_url_parts_t parts = {};
  assert(ota_url_parse("https://example.com/fw.bin", &parts));
  assert(parts.https);
  assert(parts.host_len == strlen("example.com"));

  char out[256];
  assert(ota_url_copy_if_https("https://example.com/fw.bin", &parts, out,
                               sizeof(out)));
  assert(strcmp(out, "https://example.com/fw.bin") == 0);

  assert(ota_url_parse("http://user:pw@myhost:8080/fw.bin?a=b#x", &parts));
  assert(!parts.https);
  assert(parts.userinfo_len == strlen("user:pw"));

  assert(ota_url_rewrite_http_with_ip(&parts, "192.168.1.22", false, out,
                                      sizeof(out)));
  assert(strcmp(out, "http://user:pw@192.168.1.22:8080/fw.bin?a=b#x") == 0);
}

static void test_config_mutation() {
  config_contract_state_t state = {};
  snprintf(state.hostname, sizeof(state.hostname), "tronbyt");

  config_contract_patch_t patch = {};
  patch.has_hostname = true;
  patch.hostname = "tronbyt-new";
  patch.has_wifi_power_save = true;
  patch.wifi_power_save = 2;

  config_contract_state_t out = {};
  char err[96] = {0};
  assert(config_contract_apply_patch(&state, &patch, &out, err,
                                     sizeof(err)));
  assert(strcmp(out.hostname, "tronbyt-new") == 0);
  assert(out.wifi_power_save == 2);

  patch.wifi_power_save = 99;
  assert(!config_contract_apply_patch(&state, &patch, &out, err,
                                      sizeof(err)));
}

static void test_scheduler_fsm() {
  assert(scheduler_fsm_next_state(SCHED_MODE_WEBSOCKET, SCHED_STATE_PLAYING,
                                  SCHED_EVT_PLAYER_STOPPED,
                                  false) == SCHED_STATE_IDLE);

  assert(scheduler_fsm_next_state(SCHED_MODE_HTTP, SCHED_STATE_PLAYING,
                                  SCHED_EVT_PREFETCH_TIMER,
                                  false) == SCHED_STATE_HTTP_PREFETCHING);

  assert(scheduler_fsm_next_state(SCHED_MODE_HTTP,
                                  SCHED_STATE_HTTP_PREFETCHING,
                                  SCHED_EVT_PLAYER_STOPPED,
                                  false) == SCHED_STATE_HTTP_FETCHING);

  assert(scheduler_fsm_next_state(SCHED_MODE_HTTP, SCHED_STATE_HTTP_FETCHING,
                                  SCHED_EVT_PLAYER_STOPPED,
                                  true) == SCHED_STATE_PLAYING);
}

// Construct a 16-byte TBUP header buffer for tests.
// app_size at offset 4, webui_size at offset 8, reserved zeros at 12..15.
static void make_tbup_buf(uint8_t buf[16], uint32_t app_size,
                          uint32_t webui_size) {
  static const uint32_t magic = 0x50554254;
  memcpy(buf + 0, &magic, 4);
  memcpy(buf + 4, &app_size, 4);
  memcpy(buf + 8, &webui_size, 4);
  memset(buf + 12, 0, 4);
}

static void test_tbup_parser() {
  uint8_t buf[16];
  tbup_header_t hdr;

  // non-TBUP first word → TBUP_NOT_BUNDLE
  memset(buf, 0, sizeof(buf));
  assert(tbup_parse_header(buf, 16, 16, &hdr) == TBUP_NOT_BUNDLE);

  // received < 4 → TBUP_NOT_BUNDLE (can't read first word)
  make_tbup_buf(buf, 1000, 0);
  assert(tbup_parse_header(buf, 2, 16 + 1000, &hdr) == TBUP_NOT_BUNDLE);

  // received == 4..15 (magic present but header short) → TBUP_ERR_HEADER_SHORT
  assert(tbup_parse_header(buf, 4, 16 + 1000, &hdr) == TBUP_ERR_HEADER_SHORT);
  assert(tbup_parse_header(buf, 15, 16 + 1000, &hdr) == TBUP_ERR_HEADER_SHORT);

  // content_len mismatch → TBUP_ERR_SIZE_MISMATCH; sizes still populated
  make_tbup_buf(buf, 1000, 0);
  hdr = (tbup_header_t){};
  assert(tbup_parse_header(buf, 16, 999, &hdr) == TBUP_ERR_SIZE_MISMATCH);
  assert(hdr.app_size == 1000);
  assert(hdr.webui_size == 0);

  // app_size == 0 → TBUP_ERR_APP_EMPTY
  make_tbup_buf(buf, 0, 500);
  assert(tbup_parse_header(buf, 16, 16 + 0 + 500, &hdr) == TBUP_ERR_APP_EMPTY);

  // valid app-only bundle: webui_size == 0
  make_tbup_buf(buf, 1000, 0);
  hdr = (tbup_header_t){};
  assert(tbup_parse_header(buf, 16, 16 + 1000, &hdr) == TBUP_OK);
  assert(hdr.app_size == 1000);
  assert(hdr.webui_size == 0);
  assert(hdr.app_offset == TBUP_HEADER_SIZE);

  // valid app+webui bundle
  make_tbup_buf(buf, 2048, 512);
  hdr = (tbup_header_t){};
  assert(tbup_parse_header(buf, 16, 16 + 2048 + 512, &hdr) == TBUP_OK);
  assert(hdr.app_size == 2048);
  assert(hdr.webui_size == 512);
  assert(hdr.app_offset == TBUP_HEADER_SIZE);
}

static void test_webp_frame_offsets() {
  // in-range fragment → OK
  assert(webp_frame_check_offsets(0, 100, 200, 1024) == WEBP_FRAME_OK);

  // payload_len exceeds max → OVERSIZE
  assert(webp_frame_check_offsets(0, 0, 2000, 1024) == WEBP_FRAME_OVERSIZE);

  // end_offset exceeds max → OVERSIZE
  // offset=1000, data_len=100 → end_offset=1100 > max=1024
  assert(webp_frame_check_offsets(1000, 100, 1200, 1024) == WEBP_FRAME_OVERSIZE);

  // end_offset > payload_len → INVALID_OFFSET
  // offset=150, data_len=100 → end_offset=250 > payload_len=200
  assert(webp_frame_check_offsets(150, 100, 200, 1024) ==
         WEBP_FRAME_INVALID_OFFSET);

  // boundary: end_offset == payload_len → OK
  // offset=100, data_len=100 → end_offset=200 == payload_len=200
  assert(webp_frame_check_offsets(100, 100, 200, 1024) == WEBP_FRAME_OK);

  // boundary: end_offset == max → OK
  assert(webp_frame_check_offsets(0, 1024, 1024, 1024) == WEBP_FRAME_OK);

  // payload_len == 0 skips INVALID_OFFSET check → OK
  assert(webp_frame_check_offsets(0, 100, 0, 1024) == WEBP_FRAME_OK);
}

int main() {
  test_ota_url_parser();
  test_config_mutation();
  test_scheduler_fsm();
  test_tbup_parser();
  test_webp_frame_offsets();
  printf("host_unit_tests: PASS\n");
  return 0;
}
