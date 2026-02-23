#include "webui_server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#include <esp_littlefs.h>
#include <esp_log.h>

#include "http_server.h"
#include "nvs_settings.h"

namespace {

const char* TAG = "webui";

constexpr const char* MOUNT_POINT = "/webui";
constexpr const char* PARTITION_LABEL = "webui";

bool s_fs_mounted = false;

// Embedded setup page (same binary used by AP mode) — serves as fallback
// when no LittleFS partition is available.
extern const char setup_html_start[] asm("_binary_setup_html_start");

// ---------------------------------------------------------------------------
// MIME type detection
// ---------------------------------------------------------------------------

const char* get_mime_type(const char* path) {
  const char* ext = strrchr(path, '.');
  if (!ext) return "application/octet-stream";

  if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
    return "text/html";
  if (strcmp(ext, ".css") == 0) return "text/css";
  if (strcmp(ext, ".js") == 0) return "application/javascript";
  if (strcmp(ext, ".json") == 0) return "application/json";
  if (strcmp(ext, ".png") == 0) return "image/png";
  if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
  if (strcmp(ext, ".ico") == 0) return "image/x-icon";
  if (strcmp(ext, ".woff2") == 0) return "font/woff2";
  if (strcmp(ext, ".woff") == 0) return "font/woff";

  return "application/octet-stream";
}

// ---------------------------------------------------------------------------
// Static file handler
// ---------------------------------------------------------------------------

esp_err_t serve_fallback_page(httpd_req_t* req) {
  // Serve the embedded setup page (same as AP mode) with current config
  auto cfg = config_get();
  const char* image_url = cfg.image_url[0] ? cfg.image_url : "";
  const char* api_key = cfg.api_key[0] ? cfg.api_key : "";

  int len = snprintf(nullptr, 0, setup_html_start, image_url, api_key, "");
  auto* buf = static_cast<char*>(malloc(len + 1));
  if (!buf) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Out of memory");
  }
  snprintf(buf, len + 1, setup_html_start, image_url, api_key, "");
  httpd_resp_set_type(req, "text/html");
  esp_err_t ret = httpd_resp_send(req, buf, len);
  free(buf);
  return ret;
}

esp_err_t static_file_handler(httpd_req_t* req) {
  // If filesystem is not mounted, serve the existing setup page as fallback
  if (!s_fs_mounted) {
    return serve_fallback_page(req);
  }

  // Build filesystem path from URI
  const char* uri = req->uri;

  // Default to index.html for root
  if (strcmp(uri, "/") == 0) {
    uri = "/index.html";
  }

  // Strip query string if present.
  // Use a practical limit — real URIs on an embedded web UI are short.
  constexpr size_t kMaxUri = 128;
  char clean_uri[kMaxUri];
  const char* query = strchr(uri, '?');
  if (query) {
    size_t len = query - uri;
    if (len >= sizeof(clean_uri)) len = sizeof(clean_uri) - 1;
    memcpy(clean_uri, uri, len);
    clean_uri[len] = '\0';
    uri = clean_uri;
  }

  // Mount point ("/webui" = 6 chars) + URI + NUL
  char filepath[8 + kMaxUri];
  snprintf(filepath, sizeof(filepath), "%.7s%.127s", MOUNT_POINT, uri);

  // Try gzipped version first
  char gz_path[8 + kMaxUri + 4];
  snprintf(gz_path, sizeof(gz_path), "%s.gz", filepath);

  struct stat st;
  bool use_gzip = (stat(gz_path, &st) == 0);
  const char* serve_path = use_gzip ? gz_path : filepath;

  if (!use_gzip && stat(filepath, &st) != 0) {
    // File not found — try serving index.html for SPA routing
    snprintf(filepath, sizeof(filepath), "%.7s/index.html", MOUNT_POINT);
    if (stat(filepath, &st) != 0) {
      httpd_resp_send_404(req);
      return ESP_OK;
    }
    serve_path = filepath;
  }

  FILE* f = fopen(serve_path, "r");
  if (!f) {
    ESP_LOGE(TAG, "Failed to open %s", serve_path);
    httpd_resp_send_404(req);
    return ESP_OK;
  }

  // Set MIME type based on original path (not .gz path)
  httpd_resp_set_type(req, get_mime_type(filepath));

  if (use_gzip) {
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  }

  // Cache static assets for 1 hour
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");

  // Stream file in chunks
  char buf[512];
  size_t read_len;
  while ((read_len = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (httpd_resp_send_chunk(req, buf, read_len) != ESP_OK) {
      fclose(f);
      httpd_resp_send_chunk(req, nullptr, 0);
      return ESP_FAIL;
    }
  }
  fclose(f);

  // End chunked response
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

}  // namespace

esp_err_t webui_server_init(void) {
  // Try to mount LittleFS
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = MOUNT_POINT;
  conf.partition_label = PARTITION_LABEL;
  conf.format_if_mount_failed = false;
  conf.dont_mount = false;

  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err == ESP_OK) {
    s_fs_mounted = true;

    size_t total = 0, used = 0;
    esp_littlefs_info(PARTITION_LABEL, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted: %u/%u bytes used", (unsigned)used,
             (unsigned)total);
  } else {
    s_fs_mounted = false;
    ESP_LOGW(TAG, "LittleFS mount failed (%s) — using fallback page",
             esp_err_to_name(err));
  }

  return ESP_OK;
}

void webui_register_wildcard(void) {
  httpd_handle_t server = http_server_handle();
  if (!server) {
    return;
  }
  const httpd_uri_t webui_uri = {
      .uri = "/*",
      .method = HTTP_GET,
      .handler = static_file_handler,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &webui_uri);
}
