#include "api_validation.h"

#include <stdio.h>
#include <string.h>

namespace {

bool is_allowed_key(const char* key, const char* const* allowed_keys,
                    size_t allowed_len) {
  if (!key || !allowed_keys) return false;
  for (size_t i = 0; i < allowed_len; ++i) {
    if (strcmp(key, allowed_keys[i]) == 0) {
      return true;
    }
  }
  return false;
}

void set_err(char* err, size_t err_len, const char* text) {
  if (!err || err_len == 0 || !text) return;
  snprintf(err, err_len, "%s", text);
}

void set_err_key(char* err, size_t err_len, const char* fmt, const char* key) {
  if (!err || err_len == 0 || !fmt) return;
  snprintf(err, err_len, fmt, key);
}

void set_err_range(char* err, size_t err_len, const char* fmt, const char* key,
                   long min_v, long max_v) {
  if (!err || err_len == 0 || !fmt) return;
  snprintf(err, err_len, fmt, key, min_v, max_v);
}

}  // namespace

bool api_validate_no_unknown_keys(const cJSON* root,
                                  const char* const* allowed_keys,
                                  size_t allowed_len, char* err,
                                  size_t err_len) {
  if (!root || !cJSON_IsObject(root)) {
    set_err(err, err_len, "payload is not a JSON object");
    return false;
  }

  cJSON* item = nullptr;
  cJSON_ArrayForEach(item, root) {
    if (!is_allowed_key(item->string, allowed_keys, allowed_len)) {
      set_err_key(err, err_len, "unsupported field: %s", item->string);
      return false;
    }
  }
  return true;
}

bool api_validate_optional_int(const cJSON* root, const char* key, int min_val,
                               int max_val, int* out_value, bool* out_present,
                               char* err, size_t err_len) {
  if (out_present) *out_present = false;
  if (!root || !key) return false;

  cJSON* item = cJSON_GetObjectItemCaseSensitive(
      const_cast<cJSON*>(root), key);
  if (!item) return true;

  if (out_present) *out_present = true;
  if (!cJSON_IsNumber(item)) {
    set_err_key(err, err_len, "%s must be a number", key);
    return false;
  }

  int value = item->valueint;
  if (value < min_val || value > max_val) {
    set_err_range(err, err_len, "%s out of range [%ld, %ld]", key, min_val,
                  max_val);
    return false;
  }

  if (out_value) {
    *out_value = value;
  }
  return true;
}

bool api_validate_optional_bool(const cJSON* root, const char* key,
                                bool* out_value, bool* out_present, char* err,
                                size_t err_len) {
  if (out_present) *out_present = false;
  if (!root || !key) return false;

  cJSON* item = cJSON_GetObjectItemCaseSensitive(
      const_cast<cJSON*>(root), key);
  if (!item) return true;

  if (out_present) *out_present = true;
  if (!cJSON_IsBool(item)) {
    set_err_key(err, err_len, "%s must be a boolean", key);
    return false;
  }

  if (out_value) {
    *out_value = cJSON_IsTrue(item);
  }
  return true;
}

bool api_validate_optional_string(const cJSON* root, const char* key,
                                  size_t min_len, size_t max_len,
                                  const char** out_value, bool* out_present,
                                  char* err, size_t err_len) {
  if (out_present) *out_present = false;
  if (!root || !key) return false;

  cJSON* item = cJSON_GetObjectItemCaseSensitive(
      const_cast<cJSON*>(root), key);
  if (!item) return true;

  if (out_present) *out_present = true;
  if (!cJSON_IsString(item) || !item->valuestring) {
    set_err_key(err, err_len, "%s must be a string", key);
    return false;
  }

  size_t len = strlen(item->valuestring);
  if (len < min_len || len > max_len) {
    set_err_range(err, err_len, "%s length out of range [%ld, %ld]", key,
                  static_cast<long>(min_len), static_cast<long>(max_len));
    return false;
  }

  if (out_value) {
    *out_value = item->valuestring;
  }
  return true;
}
