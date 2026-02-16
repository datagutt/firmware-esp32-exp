#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cJSON.h>

#include "api_validation.h"

static void random_payload(char* out, size_t len) {
  if (!out || len == 0) return;
  const char alphabet[] =
      "{}[],:\"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_ ";
  size_t n = (rand() % (len - 1)) + 1;
  for (size_t i = 0; i < n; ++i) {
    out[i] = alphabet[rand() % (sizeof(alphabet) - 1)];
  }
  out[n] = '\0';
}

int main() {
  srand(12345);

  const char* const allowed[] = {"brightness", "hostname", "ap_mode"};
  char err[128];

  for (int i = 0; i < 10000; ++i) {
    char input[256];
    random_payload(input, sizeof(input));

    cJSON* root = cJSON_Parse(input);
    if (!root) continue;

    memset(err, 0, sizeof(err));
    api_validate_no_unknown_keys(root, allowed, 3, err, sizeof(err));

    int brightness = 0;
    bool has_brightness = false;
    api_validate_optional_int(root, "brightness", 0, 255, &brightness,
                              &has_brightness, err, sizeof(err));

    const char* hostname = nullptr;
    bool has_hostname = false;
    api_validate_optional_string(root, "hostname", 1, 32, &hostname,
                                 &has_hostname, err, sizeof(err));

    bool ap_mode = false;
    bool has_ap_mode = false;
    api_validate_optional_bool(root, "ap_mode", &ap_mode, &has_ap_mode,
                               err, sizeof(err));

    cJSON_Delete(root);
  }

  printf("host_json_fuzz: PASS\n");
  return 0;
}
