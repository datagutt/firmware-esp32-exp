#pragma once

#include <stddef.h>

#include <cJSON.h>

#ifdef __cplusplus
extern "C" {
#endif

bool api_validate_no_unknown_keys(const cJSON* root,
                                  const char* const* allowed_keys,
                                  size_t allowed_len, char* err,
                                  size_t err_len);

bool api_validate_optional_int(const cJSON* root, const char* key, int min_val,
                               int max_val, int* out_value, bool* out_present,
                               char* err, size_t err_len);

bool api_validate_optional_bool(const cJSON* root, const char* key,
                                bool* out_value, bool* out_present, char* err,
                                size_t err_len);

bool api_validate_optional_string(const cJSON* root, const char* key,
                                  size_t min_len, size_t max_len,
                                  const char** out_value, bool* out_present,
                                  char* err, size_t err_len);

#ifdef __cplusplus
}
#endif
