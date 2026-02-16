#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool https;
  const char* host;
  size_t host_len;
  const char* userinfo;
  size_t userinfo_len;
  const char* port;
  size_t port_len;
  const char* path;
  size_t path_len;
  const char* query;
  size_t query_len;
  const char* fragment;
  size_t fragment_len;
} ota_url_parts_t;

bool ota_url_parse(const char* url, ota_url_parts_t* out);

bool ota_url_copy_if_https(const char* url, const ota_url_parts_t* parts,
                           char* out_url, size_t out_len);

bool ota_url_rewrite_http_with_ip(const ota_url_parts_t* parts,
                                  const char* ip_str, bool is_ipv6,
                                  char* out_url, size_t out_len);

#ifdef __cplusplus
}
#endif
