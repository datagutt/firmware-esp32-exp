#include "ota_url_utils.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace {

const char* find_scheme_sep(const char* url) {
  const char* p = strstr(url, "://");
  return p;
}

bool copy_part(char* dst, size_t dst_len, const char* src, size_t src_len) {
  if (!dst || dst_len == 0 || !src || src_len >= dst_len) return false;
  memcpy(dst, src, src_len);
  dst[src_len] = '\0';
  return true;
}

}  // namespace

bool ota_url_parse(const char* url, ota_url_parts_t* out) {
  if (!url || !out) return false;
  memset(out, 0, sizeof(*out));

  const char* scheme_sep = find_scheme_sep(url);
  if (!scheme_sep) return false;

  size_t scheme_len = static_cast<size_t>(scheme_sep - url);
  if (scheme_len == 5 && strncasecmp(url, "https", 5) == 0) {
    out->https = true;
  } else if (scheme_len == 4 && strncasecmp(url, "http", 4) == 0) {
    out->https = false;
  } else {
    return false;
  }

  const char* authority = scheme_sep + 3;
  if (*authority == '\0') return false;

  const char* path_begin = strpbrk(authority, "/?#");
  const char* authority_end = path_begin ? path_begin : (authority + strlen(authority));

  const char* at = nullptr;
  for (const char* p = authority; p < authority_end; ++p) {
    if (*p == '@') at = p;
  }

  const char* host_port = authority;
  if (at) {
    out->userinfo = authority;
    out->userinfo_len = static_cast<size_t>(at - authority);
    host_port = at + 1;
  }

  if (host_port >= authority_end) return false;

  if (*host_port == '[') {
    const char* close = nullptr;
    for (const char* p = host_port + 1; p < authority_end; ++p) {
      if (*p == ']') {
        close = p;
        break;
      }
    }
    if (!close) return false;
    out->host = host_port + 1;
    out->host_len = static_cast<size_t>(close - (host_port + 1));
    if (close + 1 < authority_end) {
      if (*(close + 1) != ':') return false;
      out->port = close + 2;
      out->port_len = static_cast<size_t>(authority_end - out->port);
    }
  } else {
    const char* colon = nullptr;
    for (const char* p = host_port; p < authority_end; ++p) {
      if (*p == ':') colon = p;
    }
    if (colon) {
      out->host = host_port;
      out->host_len = static_cast<size_t>(colon - host_port);
      out->port = colon + 1;
      out->port_len = static_cast<size_t>(authority_end - out->port);
    } else {
      out->host = host_port;
      out->host_len = static_cast<size_t>(authority_end - host_port);
    }
  }

  if (out->host_len == 0) return false;

  if (path_begin) {
    const char* q = strchr(path_begin, '?');
    const char* f = strchr(path_begin, '#');

    const char* path_end = authority + strlen(authority);
    if (q && (!f || q < f)) path_end = q;
    if (f && (!q || f < q)) path_end = f;

    if (*path_begin == '/') {
      out->path = path_begin;
      out->path_len = static_cast<size_t>(path_end - path_begin);
    }

    if (q) {
      const char* q_end = f ? f : authority + strlen(authority);
      out->query = q + 1;
      out->query_len = static_cast<size_t>(q_end - (q + 1));
    }

    if (f) {
      out->fragment = f + 1;
      out->fragment_len = strlen(f + 1);
    }
  }

  return true;
}

bool ota_url_copy_if_https(const char* url, const ota_url_parts_t* parts,
                           char* out_url, size_t out_len) {
  if (!url || !parts || !out_url || out_len == 0 || !parts->https) {
    return false;
  }
  size_t len = strlen(url);
  if (len >= out_len) return false;
  memcpy(out_url, url, len + 1);
  return true;
}

bool ota_url_rewrite_http_with_ip(const ota_url_parts_t* parts,
                                  const char* ip_str, bool is_ipv6,
                                  char* out_url, size_t out_len) {
  if (!parts || !ip_str || !out_url || out_len == 0 || parts->https) {
    return false;
  }

  int written = 0;
  auto append = [&](const char* fmt, auto... args) -> bool {
    size_t remaining = (written < static_cast<int>(out_len)) ? out_len - written : 0;
    int n = snprintf(out_url + written, remaining, fmt, args...);
    if (n < 0) return false;
    written += n;
    return written < static_cast<int>(out_len);
  };

  if (!append("http://")) return false;
  if (parts->userinfo && parts->userinfo_len > 0) {
    char ui[128];
    if (!copy_part(ui, sizeof(ui), parts->userinfo, parts->userinfo_len)) return false;
    if (!append("%s@", ui)) return false;
  }

  if (is_ipv6) {
    if (!append("[%s]", ip_str)) return false;
  } else {
    if (!append("%s", ip_str)) return false;
  }

  if (parts->port && parts->port_len > 0) {
    char port[32];
    if (!copy_part(port, sizeof(port), parts->port, parts->port_len)) return false;
    if (!append(":%s", port)) return false;
  }

  if (parts->path && parts->path_len > 0) {
    char path[256];
    if (!copy_part(path, sizeof(path), parts->path, parts->path_len)) return false;
    if (!append("%s", path)) return false;
  }

  if (parts->query && parts->query_len > 0) {
    char query[256];
    if (!copy_part(query, sizeof(query), parts->query, parts->query_len)) return false;
    if (!append("?%s", query)) return false;
  }

  if (parts->fragment && parts->fragment_len > 0) {
    char fragment[128];
    if (!copy_part(fragment, sizeof(fragment), parts->fragment, parts->fragment_len)) return false;
    if (!append("#%s", fragment)) return false;
  }

  return true;
}
