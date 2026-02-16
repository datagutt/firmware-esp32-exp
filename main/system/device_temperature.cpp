#include "device_temperature.h"

#include <esp_err.h>

#if __has_include("driver/temperature_sensor.h")
#include <driver/temperature_sensor.h>
#include <esp_log.h>
#include <soc/soc_caps.h>
#endif

namespace {

#if __has_include("driver/temperature_sensor.h") && defined(SOC_TEMP_SENSOR_SUPPORTED)
const char* TAG = "temp";
bool s_initialized = false;
temperature_sensor_handle_t s_sensor = nullptr;

bool ensure_temp_sensor_ready() {
  if (s_initialized) {
    return s_sensor != nullptr;
  }
  s_initialized = true;

  temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
  esp_err_t err = temperature_sensor_install(&cfg, &s_sensor);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "temperature_sensor_install failed: %s", esp_err_to_name(err));
    s_sensor = nullptr;
    return false;
  }

  err = temperature_sensor_enable(s_sensor);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "temperature_sensor_enable failed: %s", esp_err_to_name(err));
    temperature_sensor_uninstall(s_sensor);
    s_sensor = nullptr;
    return false;
  }

  return true;
}
#endif

}  // namespace

bool device_temperature_get_c(float* out_celsius) {
#if __has_include("driver/temperature_sensor.h") && defined(SOC_TEMP_SENSOR_SUPPORTED)
  if (!out_celsius) return false;
  if (!ensure_temp_sensor_ready()) return false;

  float c = 0.0f;
  esp_err_t err = temperature_sensor_get_celsius(s_sensor, &c);
  if (err != ESP_OK) {
    return false;
  }
  *out_celsius = c;
  return true;
#else
  (void)out_celsius;
  return false;
#endif
}
