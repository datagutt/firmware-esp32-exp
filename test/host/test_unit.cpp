#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config_contract.h"
#include "ota_url_utils.h"
#include "scheduler_fsm.h"

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

int main() {
  test_ota_url_parser();
  test_config_mutation();
  test_scheduler_fsm();
  printf("host_unit_tests: PASS\n");
  return 0;
}
