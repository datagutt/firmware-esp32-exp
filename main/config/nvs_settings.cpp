#include "nvs_settings.h"

#include <cstdlib>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "esp_log.h"
#include "event_bus.h"
#include "nvs_flash.h"
#include "nvs_handle.h"
#include "sdkconfig.h"

namespace {

const char* TAG = "NVS_SETTINGS";

constexpr const char* NVS_NAMESPACE = "wifi_config";
constexpr const char* NVS_KEY_HOSTNAME = "hostname";
constexpr const char* NVS_KEY_SYSLOG_ADDR = "syslog_addr";
constexpr const char* NVS_KEY_SNTP_SERVER = "sntp_server";
constexpr const char* NVS_KEY_IMAGE_URL = "image_url";
constexpr const char* NVS_KEY_API_KEY = "api_key";
constexpr const char* NVS_KEY_SWAP_COLORS = "swap_colors";
constexpr const char* NVS_KEY_WIFI_POWER_SAVE = "wifi_ps";
constexpr const char* NVS_KEY_SKIP_VERSION = "skip_ver";
constexpr const char* NVS_KEY_SKIP_BOOT = "skip_boot";
constexpr const char* NVS_KEY_AP_MODE = "ap_mode";
constexpr const char* NVS_KEY_PREFER_IPV6 = "prefer_ipv6";
constexpr const char* NVS_KEY_DISABLE_TOUCH = "dis_touch";

// Atomic save keys — blob-based config persistence
constexpr const char* NVS_KEY_CFG_CUR = "cfg";
constexpr const char* NVS_KEY_CFG_NEW = "cfg_new";

// Multi-network list lives in its own namespace so the main config blob (and
// its NVS-migration risk) is untouched. A device that loses this blob degrades
// to a single-credential device via the legacy keys, not a factory reset.
constexpr const char* NVS_NETS_NAMESPACE = "wifi_nets";
constexpr const char* NVS_NETS_KEY = "nets";

// Persist a refreshed last_rssi only when it moves at least this much (or was
// previously unknown), so frequent reconnects don't grind the flash.
constexpr int RSSI_PERSIST_DELTA_DB = 5;

system_config_t s_config = {};
wifi_network_t s_nets[MAX_WIFI_NETS] = {};
SemaphoreHandle_t s_mutex = nullptr;
uint32_t s_generation = 0;

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef REMOTE_URL
#define REMOTE_URL ""
#endif

/// Atomic save: write to temp key, validate, swap to main key, erase temp.
/// Caller must hold s_mutex.
esp_err_t persist_to_nvs() {
  NvsHandle nvs(NVS_NAMESPACE, NVS_READWRITE);
  if (!nvs) return nvs.open_error();

  // Step 1: Write config blob to temp key
  esp_err_t err =
      nvs.set_blob(NVS_KEY_CFG_NEW, &s_config, sizeof(system_config_t));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write temp config blob: %s",
             esp_err_to_name(err));
    return err;
  }
  err = nvs.commit();
  if (err != ESP_OK) return err;

  // Step 2: Read back and validate temp key matches
  system_config_t verify = {};
  size_t verify_len = sizeof(verify);
  err = nvs.get_blob(NVS_KEY_CFG_NEW, &verify, &verify_len);
  if (err != ESP_OK || verify_len != sizeof(system_config_t) ||
      memcmp(&verify, &s_config, sizeof(system_config_t)) != 0) {
    ESP_LOGE(TAG, "Config blob verification failed");
    nvs.erase_key(NVS_KEY_CFG_NEW);
    nvs.commit();
    return ESP_FAIL;
  }

  // Step 3: Write validated data to main key
  err = nvs.set_blob(NVS_KEY_CFG_CUR, &s_config, sizeof(system_config_t));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write main config blob: %s",
             esp_err_to_name(err));
    return err;
  }

  // Step 4: Erase temp key and commit
  nvs.erase_key(NVS_KEY_CFG_NEW);
  err = nvs.commit();

  // Also persist individual keys for backward compatibility with
  // existing code that reads them (e.g., on downgrade)
  nvs.set_str(NVS_KEY_HOSTNAME, s_config.hostname);
  nvs.set_str(NVS_KEY_SYSLOG_ADDR, s_config.syslog_addr);
  nvs.set_str(NVS_KEY_SNTP_SERVER, s_config.sntp_server);
  nvs.set_str(NVS_KEY_IMAGE_URL, s_config.image_url);
  nvs.set_str(NVS_KEY_API_KEY, s_config.api_key);
  nvs.set_u8(NVS_KEY_SWAP_COLORS, s_config.swap_colors ? 1 : 0);
  nvs.set_u8(NVS_KEY_WIFI_POWER_SAVE,
             static_cast<uint8_t>(s_config.wifi_power_save));
  nvs.set_u8(NVS_KEY_SKIP_VERSION, s_config.skip_display_version ? 1 : 0);
  nvs.set_u8(NVS_KEY_SKIP_BOOT, s_config.skip_boot_animation ? 1 : 0);
  nvs.set_u8(NVS_KEY_AP_MODE, s_config.ap_mode ? 1 : 0);
  nvs.set_u8(NVS_KEY_PREFER_IPV6, s_config.prefer_ipv6 ? 1 : 0);
  nvs.set_u8(NVS_KEY_DISABLE_TOUCH, s_config.disable_touch ? 1 : 0);
  nvs.commit();

  return err;
}

/// Attempt to load config from the atomic blob keys.
/// Returns true if a valid blob was found and loaded into s_config.
bool load_from_blob() {
  NvsHandle nvs(NVS_NAMESPACE, NVS_READWRITE);
  if (!nvs) return false;

  // Check for interrupted save: if temp key exists but main doesn't, recover
  system_config_t temp = {};
  size_t temp_len = sizeof(temp);
  bool has_temp =
      (nvs.get_blob(NVS_KEY_CFG_NEW, &temp, &temp_len) == ESP_OK &&
       temp_len == sizeof(system_config_t));

  system_config_t main_cfg = {};
  size_t main_len = sizeof(main_cfg);
  bool has_main =
      (nvs.get_blob(NVS_KEY_CFG_CUR, &main_cfg, &main_len) == ESP_OK &&
       main_len == sizeof(system_config_t));

  if (has_temp && !has_main) {
    // Interrupted save — recover from temp key
    ESP_LOGW(TAG, "Recovering config from interrupted save");
    nvs.set_blob(NVS_KEY_CFG_CUR, &temp, sizeof(system_config_t));
    nvs.erase_key(NVS_KEY_CFG_NEW);
    nvs.commit();
    memcpy(&s_config, &temp, sizeof(system_config_t));
    return true;
  }

  if (has_temp) {
    // Stale temp key — clean it up
    nvs.erase_key(NVS_KEY_CFG_NEW);
    nvs.commit();
  }

  if (has_main) {
    memcpy(&s_config, &main_cfg, sizeof(system_config_t));
    return true;
  }

  return false;
}

// --- Multi-network list helpers --------------------------------------------

// Write the in-memory list to its NVS blob. Best-effort. Caller holds s_mutex.
void persist_nets_locked() {
  NvsHandle nvs(NVS_NETS_NAMESPACE, NVS_READWRITE);
  if (!nvs) return;
  nvs.set_blob(NVS_NETS_KEY, s_nets, sizeof(s_nets));
  nvs.commit();
}

// Load the stored network list. If the blob is absent or malformed the list is
// left empty by design — first boot after flashing comes up to the portal for
// fresh provisioning. No migration from the old single-credential keys.
void load_nets() {
  memset(s_nets, 0, sizeof(s_nets));
  NvsHandle nvs(NVS_NETS_NAMESPACE, NVS_READONLY);
  if (!nvs) return;
  size_t len = sizeof(s_nets);
  if (nvs.get_blob(NVS_NETS_KEY, s_nets, &len) != ESP_OK ||
      len != sizeof(s_nets)) {
    memset(s_nets, 0, sizeof(s_nets));
  }
}

}  // namespace

esp_err_t nvs_settings_init(void) {
  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) return ESP_ERR_NO_MEM;

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  if (ret != ESP_OK) return ret;

  memset(&s_config, 0, sizeof(s_config));

  // Kconfig defaults
#ifdef CONFIG_SWAP_COLORS
  s_config.swap_colors = true;
#endif

#ifdef CONFIG_ENABLE_WIFI_POWER_SAVE
  s_config.wifi_power_save = WIFI_PS_MIN_MODEM;
#else
  s_config.wifi_power_save = WIFI_PS_NONE;
#endif

#ifdef CONFIG_SKIP_DISPLAY_VERSION
  s_config.skip_display_version = true;
#endif

#ifdef CONFIG_SKIP_BOOT_ANIMATION
  s_config.skip_boot_animation = true;
#endif

#ifdef CONFIG_ENABLE_AP_MODE
  s_config.ap_mode = true;
#endif

#ifdef CONFIG_PREFER_IPV6
  s_config.prefer_ipv6 = true;
#endif

  // Try atomic blob load first (new format)
  if (load_from_blob()) {
    ESP_LOGI(TAG, "Config loaded from atomic blob");
  } else {
    // Fall back to individual key loading (legacy format)
    NvsHandle nvs(NVS_NAMESPACE, NVS_READONLY);
    if (nvs) {
      size_t sz;

      sz = sizeof(s_config.hostname);
      if (nvs.get_str(NVS_KEY_HOSTNAME, s_config.hostname, &sz) != ESP_OK)
        s_config.hostname[0] = '\0';

      sz = sizeof(s_config.syslog_addr);
      if (nvs.get_str(NVS_KEY_SYSLOG_ADDR, s_config.syslog_addr, &sz) !=
          ESP_OK)
        s_config.syslog_addr[0] = '\0';

      sz = sizeof(s_config.sntp_server);
      if (nvs.get_str(NVS_KEY_SNTP_SERVER, s_config.sntp_server, &sz) !=
          ESP_OK)
        s_config.sntp_server[0] = '\0';

      sz = sizeof(s_config.image_url);
      if (nvs.get_str(NVS_KEY_IMAGE_URL, s_config.image_url, &sz) != ESP_OK)
        s_config.image_url[0] = '\0';

      sz = sizeof(s_config.api_key);
      if (nvs.get_str(NVS_KEY_API_KEY, s_config.api_key, &sz) != ESP_OK)
        s_config.api_key[0] = '\0';

      uint8_t val_u8;

      if (nvs.get_u8(NVS_KEY_SWAP_COLORS, &val_u8) == ESP_OK)
        s_config.swap_colors = (val_u8 != 0);

      if (nvs.get_u8(NVS_KEY_WIFI_POWER_SAVE, &val_u8) == ESP_OK)
        s_config.wifi_power_save = static_cast<wifi_ps_type_t>(val_u8);

      if (nvs.get_u8(NVS_KEY_SKIP_VERSION, &val_u8) == ESP_OK)
        s_config.skip_display_version = (val_u8 != 0);

      if (nvs.get_u8(NVS_KEY_SKIP_BOOT, &val_u8) == ESP_OK)
        s_config.skip_boot_animation = (val_u8 != 0);

      if (nvs.get_u8(NVS_KEY_AP_MODE, &val_u8) == ESP_OK)
        s_config.ap_mode = (val_u8 != 0);

      if (nvs.get_u8(NVS_KEY_PREFER_IPV6, &val_u8) == ESP_OK)
        s_config.prefer_ipv6 = (val_u8 != 0);

      if (nvs.get_u8(NVS_KEY_DISABLE_TOUCH, &val_u8) == ESP_OK)
        s_config.disable_touch = (val_u8 != 0);
    }
  }

  // Load the multi-network list before seeding (init is single-threaded).
  load_nets();

  // Seed the first network and default URL from build-time secrets.json when
  // nothing is stored yet (fresh device). Credentials go to the network list;
  // the URL stays in the config blob.
  bool save_cfg_defaults = false;
  if (s_nets[0].ssid[0] == '\0') {
    char placeholder_ssid[MAX_SSID_LEN + 1] = WIFI_SSID;
    char placeholder_password[MAX_PASSWORD_LEN + 1] = WIFI_PASSWORD;
    char placeholder_url[MAX_URL_LEN + 1] = REMOTE_URL;

    if (strstr(placeholder_ssid, "Xplaceholder") == nullptr &&
        strlen(placeholder_ssid) > 0) {
      snprintf(s_nets[0].ssid, sizeof(s_nets[0].ssid), "%s", placeholder_ssid);
      if (strstr(placeholder_password, "Xplaceholder") == nullptr) {
        snprintf(s_nets[0].password, sizeof(s_nets[0].password), "%s",
                 placeholder_password);
      } else {
        s_nets[0].password[0] = '\0';
      }
      persist_nets_locked();
      ESP_LOGI(TAG, "Seeded WiFi network from secrets: %s", s_nets[0].ssid);
    }

    if (strlen(s_config.image_url) == 0 &&
        strstr(placeholder_url, "Xplaceholder") == nullptr &&
        strlen(placeholder_url) > 0) {
      snprintf(s_config.image_url, sizeof(s_config.image_url), "%s",
               placeholder_url);
      save_cfg_defaults = true;
    }
  }

  if (save_cfg_defaults) {
    persist_to_nvs();
  }

  // Apply brand default server URL if NVS has none and secrets.json had none
  if (strlen(s_config.image_url) == 0 &&
      strlen(CONFIG_DEFAULT_SERVER_URL) > 0) {
    snprintf(s_config.image_url, sizeof(s_config.image_url), "%s",
             CONFIG_DEFAULT_SERVER_URL);
    ESP_LOGI(TAG, "Applied brand default server URL");
  }

#ifdef CONFIG_LOCK_SERVER_URL
  // When server URL is locked, always override with the Kconfig value
  if (strlen(CONFIG_DEFAULT_SERVER_URL) > 0) {
    snprintf(s_config.image_url, sizeof(s_config.image_url), "%s",
             CONFIG_DEFAULT_SERVER_URL);
    ESP_LOGI(TAG, "Server URL locked to brand default");
  }
#endif

  ESP_LOGI(TAG, "Settings initialized. Networks: %u, URL: %s, AP Mode: %d",
           (unsigned)wifi_network_list_count(), s_config.image_url,
           s_config.ap_mode);

  return ESP_OK;
}

system_config_t config_get(void) {
  system_config_t copy;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  memcpy(&copy, &s_config, sizeof(system_config_t));
  xSemaphoreGive(s_mutex);
  return copy;
}

void config_set(const system_config_t* cfg) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  memcpy(&s_config, cfg, sizeof(system_config_t));
  persist_to_nvs();
  s_generation++;
  xSemaphoreGive(s_mutex);

  event_bus_emit_i32(TRONBYT_EVENT_CONFIG_CHANGED,
                     static_cast<int32_t>(s_generation));
}

uint32_t config_generation(void) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  uint32_t gen = s_generation;
  xSemaphoreGive(s_mutex);
  return gen;
}

// --- Multi-network list public API -----------------------------------------

namespace {
// Persist a list change, bump the config generation (so the web UI sees it),
// and notify. Caller holds s_mutex; this releases it.
void commit_list_change_locked() {
  persist_nets_locked();
  s_generation++;
  uint32_t gen = s_generation;
  xSemaphoreGive(s_mutex);
  event_bus_emit_i32(TRONBYT_EVENT_CONFIG_CHANGED, static_cast<int32_t>(gen));
}
}  // namespace

size_t wifi_network_list_get(wifi_network_t* out, size_t max) {
  if (!out || max == 0) return 0;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  size_t n = 0;
  for (size_t i = 0; i < MAX_WIFI_NETS && n < max; i++) {
    if (s_nets[i].ssid[0] != '\0') out[n++] = s_nets[i];
  }
  xSemaphoreGive(s_mutex);
  return n;
}

size_t wifi_network_list_count(void) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  size_t n = 0;
  for (size_t i = 0; i < MAX_WIFI_NETS; i++) {
    if (s_nets[i].ssid[0] != '\0') n++;
  }
  xSemaphoreGive(s_mutex);
  return n;
}

void wifi_network_list_set(const wifi_network_t* list, size_t count) {
  if (!list) return;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  memset(s_nets, 0, sizeof(s_nets));
  size_t n = 0;
  for (size_t i = 0; i < count && n < MAX_WIFI_NETS; i++) {
    if (list[i].ssid[0] == '\0') continue;
    s_nets[n++] = list[i];
  }
  commit_list_change_locked();
}

bool wifi_network_list_add(const char* ssid, const char* password) {
  if (!ssid || ssid[0] == '\0') return false;
  xSemaphoreTake(s_mutex, portMAX_DELAY);

  int found = -1;
  int free_slot = -1;
  for (int i = 0; i < MAX_WIFI_NETS; i++) {
    if (s_nets[i].ssid[0] == '\0') {
      if (free_slot < 0) free_slot = i;
      continue;
    }
    if (strncmp(s_nets[i].ssid, ssid, MAX_SSID_LEN) == 0) {
      found = i;
      break;
    }
  }

  int slot = (found >= 0) ? found : free_slot;
  if (slot < 0) {
    xSemaphoreGive(s_mutex);  // list full and SSID is new
    return false;
  }

  if (found < 0) {
    memset(&s_nets[slot], 0, sizeof(s_nets[slot]));
  }
  snprintf(s_nets[slot].ssid, sizeof(s_nets[slot].ssid), "%s", ssid);
  snprintf(s_nets[slot].password, sizeof(s_nets[slot].password), "%s",
           password ? password : "");

  commit_list_change_locked();
  return true;
}

bool wifi_network_list_remove(const char* ssid) {
  if (!ssid || ssid[0] == '\0') return false;
  xSemaphoreTake(s_mutex, portMAX_DELAY);

  int found = -1;
  for (int i = 0; i < MAX_WIFI_NETS; i++) {
    if (s_nets[i].ssid[0] != '\0' &&
        strncmp(s_nets[i].ssid, ssid, MAX_SSID_LEN) == 0) {
      found = i;
      break;
    }
  }
  if (found < 0) {
    xSemaphoreGive(s_mutex);
    return false;
  }

  for (int i = found; i < MAX_WIFI_NETS - 1; i++) {
    s_nets[i] = s_nets[i + 1];
  }
  memset(&s_nets[MAX_WIFI_NETS - 1], 0, sizeof(s_nets[MAX_WIFI_NETS - 1]));

  commit_list_change_locked();
  return true;
}

void wifi_network_note_rssi(const char* ssid, int8_t rssi) {
  if (!ssid || ssid[0] == '\0') return;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  for (int i = 0; i < MAX_WIFI_NETS; i++) {
    if (s_nets[i].ssid[0] == '\0' ||
        strncmp(s_nets[i].ssid, ssid, MAX_SSID_LEN) != 0) {
      continue;
    }
    int prev = s_nets[i].last_rssi;
    s_nets[i].last_rssi = rssi;
    // Persist only on a meaningful change to spare the flash on flaky links.
    bool worth_persisting =
        prev == 0 || abs(static_cast<int>(rssi) - prev) >= RSSI_PERSIST_DELTA_DB;
    if (worth_persisting) {
      persist_nets_locked();
    }
    break;
  }
  xSemaphoreGive(s_mutex);
}
