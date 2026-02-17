/**
 * touch_control.c
 *
 * Touch control for Tidbyt Gen 2
 * Single touch zone on GPIO33 (Touch Channel 8)
 *
 * Gestures:
 *   - Single tap: Next app
 *   - Double tap: Cycle brightness (10% -> 25% -> 50% -> 75%)
 *   - Long hold (2s): Toggle display on/off
 */

#include "touch_control.h"

#include <string.h>

static const char* TAG = "TouchControl";

// Touch channel 8 = GPIO33 on ESP32
#define TOUCH_CHANNEL_ID 8

// Global handles for the new touch sensor driver
static touch_sensor_handle_t s_sens_handle = NULL;
static touch_channel_handle_t s_chan_handle = NULL;

#define TOUCH_HOLD_MS 2000
#define DOUBLE_TAP_WINDOW_MS 500  // Window for second tap to count as double-tap
#define MIN_TAP_DURATION_MS 20    // Minimum to register as a tap at all

typedef enum {
  STATE_IDLE,
  STATE_TOUCHING,
  STATE_WAIT_FOR_DOUBLE_TAP,
  STATE_HOLD_FIRED
} touch_fsm_state_t;

// Adaptive baseline tracking parameters
#define BASELINE_UPDATE_INTERVAL_MS 200   // Update baseline every 200ms
#define BASELINE_ALPHA 0.15f              // Faster adaptation (15% new, 85% old)
#define BASELINE_ALPHA_FAST 0.5f          // Fast adaptation during warmup
#define WARMUP_PERIOD_MS 5000             // Use fast adaptation for first 5 seconds
#define TOUCH_DROP_THRESHOLD 35           // Touch must drop at least this much from baseline

typedef struct {
  uint16_t threshold;
  uint32_t debounce_ms;
  bool initialized;
  uint16_t baseline;
  float adaptive_baseline;  // Floating point for smooth adaptation
  uint32_t last_baseline_update;
  uint32_t init_time;       // Time of initialization for warmup period
  touch_fsm_state_t state;
  uint32_t touch_start_time;
  uint32_t release_time;
  uint32_t last_event_time;
  bool is_late_tap;  // True if current touch was too late for double-tap (will be swallowed)
} touch_state_t;

static touch_state_t g_touch = {.threshold = TOUCH_THRESHOLD_DEFAULT,
                                .debounce_ms = TOUCH_DEBOUNCE_MS,
                                .initialized = false,
                                .baseline = 0,
                                .adaptive_baseline = 0,
                                .last_baseline_update = 0,
                                .init_time = 0,
                                .state = STATE_IDLE,
                                .touch_start_time = 0,
                                .release_time = 0,
                                .last_event_time = 0,
                                .is_late_tap = false};

static uint32_t get_time_ms(void) {
  return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint16_t read_touch_filtered(void) {
  uint32_t data = 0;
  esp_err_t ret = touch_channel_read_data(s_chan_handle, TOUCH_CHAN_DATA_TYPE_SMOOTH, &data);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to read smooth data: %s", esp_err_to_name(ret));
    return 65535;
  }
  return (uint16_t)data;
}

// Touch pad on Tidbyt Gen 2: GPIO33 (Touch Channel 8)
// Based on ESPHome configuration: https://community.home-assistant.io/t/esphome-on-tidbyt-gen-2/830367

esp_err_t touch_control_init(void) {
  ESP_LOGI(TAG, "Initializing touch control on GPIO33...");

  // Step 1: Create controller with sample config
  // Voltage settings equivalent to legacy: HVOLT=2.7V, LVOLT=0.5V, ATTEN=1V → H=1.7V, L=0.5V
  touch_sensor_sample_config_t sample_cfg = TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(
      5.0, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_1V7);
  touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, &sample_cfg);
  esp_err_t ret = touch_sensor_new_controller(&sens_cfg, &s_sens_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create touch controller: %s", esp_err_to_name(ret));
    return ret;
  }

  // Step 2: Register channel 8 (GPIO33)
  touch_channel_config_t chan_cfg = {
      .abs_active_thresh = {0},  // We use software adaptive threshold, not hardware
      .charge_speed = TOUCH_CHARGE_SPEED_7,
      .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
      .group = TOUCH_CHAN_TRIG_GROUP_BOTH,
  };
  ret = touch_sensor_new_channel(s_sens_handle, TOUCH_CHANNEL_ID, &chan_cfg, &s_chan_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create touch channel: %s", esp_err_to_name(ret));
    touch_sensor_del_controller(s_sens_handle);
    return ret;
  }

  // Step 3: Configure software filter (10ms interval, default IIR filter)
  touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
  ret = touch_sensor_config_filter(s_sens_handle, &filter_cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure filter: %s", esp_err_to_name(ret));
  }

  // Step 4: Enable and do initial scanning to warm up channel data
  ret = touch_sensor_enable(s_sens_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to enable touch sensor: %s", esp_err_to_name(ret));
    return ret;
  }
  for (int i = 0; i < 3; i++) {
    touch_sensor_trigger_oneshot_scanning(s_sens_handle, 2000);
  }

  // Step 5: Start continuous scanning
  ret = touch_sensor_start_continuous_scanning(s_sens_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start scanning: %s", esp_err_to_name(ret));
    return ret;
  }

  vTaskDelay(pdMS_TO_TICKS(100));

  touch_control_calibrate();

  g_touch.initialized = true;
  g_touch.state = STATE_IDLE;
  g_touch.init_time = get_time_ms();

  ESP_LOGI(TAG, "Touch control ready (GPIO33)");
  ESP_LOGI(TAG, "  TAP = Next app | DOUBLE-TAP = Brightness | HOLD 2s = Toggle display");

  return ESP_OK;
}

touch_event_t touch_control_check(void) {
  if (!g_touch.initialized) {
    return TOUCH_EVENT_NONE;
  }

  uint32_t now = get_time_ms();
  uint16_t value = read_touch_filtered();

  // Initialize adaptive baseline on first read
  if (g_touch.adaptive_baseline == 0) {
    g_touch.adaptive_baseline = (float)value;
  }

  // Calculate delta from adaptive baseline (positive = finger touching = value dropped)
  int16_t delta = (int16_t)g_touch.adaptive_baseline - (int16_t)value;

  // Touch detected if value dropped significantly from adaptive baseline
  bool is_touched = (delta >= TOUCH_DROP_THRESHOLD);

  // Update adaptive baseline when NOT touching
  // This handles drift from display EMI, temperature, etc.
  // Use fast adaptation during warmup period to quickly track display EMI
  if (!is_touched && (now - g_touch.last_baseline_update >= BASELINE_UPDATE_INTERVAL_MS)) {
    bool in_warmup = (now - g_touch.init_time) < WARMUP_PERIOD_MS;
    float alpha = in_warmup ? BASELINE_ALPHA_FAST : BASELINE_ALPHA;
    g_touch.adaptive_baseline = (alpha * (float)value) +
                                 ((1.0f - alpha) * g_touch.adaptive_baseline);
    g_touch.last_baseline_update = now;
  }

#if TOUCH_DEBUG_ENABLED
  static uint32_t last_debug = 0;

  // Every 5 seconds, show touch debug info
  if (now - last_debug > 5000) {
    ESP_LOGI(TAG, "=== TOUCH DEBUG (adaptive baseline) ===");
    ESP_LOGI(TAG, "Current: %d, Adaptive baseline: %.0f, Delta: %d",
             value, g_touch.adaptive_baseline, delta);
    ESP_LOGI(TAG, "Touch threshold: %d drop, Touched: %s",
             TOUCH_DROP_THRESHOLD, is_touched ? "YES" : "NO");
    ESP_LOGI(TAG, "State: %d", g_touch.state);
    ESP_LOGI(TAG, "========================================");
    last_debug = now;
  }
#endif

  touch_event_t event = TOUCH_EVENT_NONE;

  switch (g_touch.state) {
    case STATE_IDLE:
      if (is_touched) {
        g_touch.state = STATE_TOUCHING;
        g_touch.touch_start_time = now;
        g_touch.is_late_tap = false;  // Fresh tap from idle
      }
      break;

    case STATE_TOUCHING:
      if (!is_touched) {
        uint32_t duration = now - g_touch.touch_start_time;

        if (duration >= TOUCH_HOLD_MS) {
          g_touch.state = STATE_IDLE;
        } else if (g_touch.is_late_tap) {
          // Late tap (came after double-tap window expired) - swallow it
          ESP_LOGI(TAG, "Late tap swallowed (%ldms) - no skip", duration);
          g_touch.state = STATE_IDLE;
        } else if (duration >= MIN_TAP_DURATION_MS) {
          g_touch.release_time = now;
          g_touch.state = STATE_WAIT_FOR_DOUBLE_TAP;
        } else {
          g_touch.state = STATE_IDLE;
        }
      } else {
        uint32_t duration = now - g_touch.touch_start_time;
        if (duration >= TOUCH_HOLD_MS) {
          event = TOUCH_EVENT_HOLD;
          g_touch.state = STATE_HOLD_FIRED;
          g_touch.last_event_time = now;
          ESP_LOGI(TAG, "HOLD detected");
        }
      }
      break;

    case STATE_WAIT_FOR_DOUBLE_TAP:
      if (is_touched) {
        uint32_t gap = now - g_touch.release_time;
        if (gap <= DOUBLE_TAP_WINDOW_MS) {
          // Second tap in time - double-tap!
          event = TOUCH_EVENT_DOUBLE_TAP;
          g_touch.last_event_time = now;
          g_touch.is_late_tap = false;
          ESP_LOGI(TAG, "DOUBLE-TAP detected");
        } else {
          // Second tap came too late - mark it so it gets swallowed on release
          g_touch.is_late_tap = true;
          ESP_LOGI(TAG, "Late second tap (gap %ldms > %dms)", gap, DOUBLE_TAP_WINDOW_MS);
        }
        g_touch.state = STATE_TOUCHING;
        g_touch.touch_start_time = now;
      } else {
        uint32_t wait_time = now - g_touch.release_time;
        if (wait_time > DOUBLE_TAP_WINDOW_MS) {
          // No second tap came - this was a single tap
          event = TOUCH_EVENT_TAP;
          g_touch.last_event_time = now;
          g_touch.state = STATE_IDLE;
          ESP_LOGI(TAG, "TAP detected (single)");
        }
      }
      break;

    case STATE_HOLD_FIRED:
      if (!is_touched) {
        g_touch.state = STATE_IDLE;
      }
      break;
  }

  return event;
}

void touch_control_calibrate(void) {
  ESP_LOGI(TAG, "Calibrating (don't touch!)...");

  // Match official Tidbyt HDK: use maximum of 3 readings
  uint16_t max_value = 0;
  const int samples = 3;

  for (int i = 0; i < samples; i++) {
    uint16_t val = read_touch_filtered();
    if (val > max_value) {
      max_value = val;
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // 100ms between samples like HDK
  }

  g_touch.baseline = max_value;
  g_touch.adaptive_baseline = (float)max_value;

  ESP_LOGI(TAG, "Baseline (max of %d samples): %d", samples, g_touch.baseline);
  ESP_LOGI(TAG, "Using adaptive tracking + delta threshold: %d",
           TOUCH_DROP_THRESHOLD);
}

void touch_control_debug_all_pads(void) {
  ESP_LOGI(TAG, "=== Touch Control Debug ===");

  uint16_t current = read_touch_filtered();
  int16_t delta = (int16_t)g_touch.adaptive_baseline - (int16_t)current;

  ESP_LOGI(TAG, "Main pad (GPIO33): Channel %d", TOUCH_CHANNEL_ID);
  ESP_LOGI(TAG, "Current: %d, Adaptive baseline: %.0f", current, g_touch.adaptive_baseline);
  ESP_LOGI(TAG, "Delta: %d (need %d+ for touch)", delta, TOUCH_DROP_THRESHOLD);
  ESP_LOGI(TAG, "=========================");
}

void touch_control_set_threshold(uint16_t threshold) {
  g_touch.threshold = threshold;
  ESP_LOGI(TAG, "Threshold set to: %d", threshold);
}

uint16_t touch_control_get_threshold(void) { return g_touch.threshold; }

void touch_control_set_debounce(uint32_t ms) {
  g_touch.debounce_ms = ms;
  ESP_LOGI(TAG, "Debounce set to: %d ms", ms);
}

uint16_t touch_control_read_raw(void) {
  uint32_t data = 0;
  touch_channel_read_data(s_chan_handle, TOUCH_CHAN_DATA_TYPE_RAW, &data);
  return (uint16_t)data;
}

bool touch_control_is_initialized(void) { return g_touch.initialized; }

const char* touch_event_to_string(touch_event_t event) {
  switch (event) {
    case TOUCH_EVENT_NONE:
      return "NONE";
    case TOUCH_EVENT_TAP:
      return "TAP";
    case TOUCH_EVENT_DOUBLE_TAP:
      return "DOUBLE_TAP";
    case TOUCH_EVENT_HOLD:
      return "HOLD";
    default:
      return "UNKNOWN";
  }
}
