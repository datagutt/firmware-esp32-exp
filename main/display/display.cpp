#include "display.h"

#include <hub75.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "font5x7.h"
#include "nvs_handle.h"
#include "nvs_settings.h"
#include "scheduler.h"
#include "webp_player.h"

static Hub75Driver *_matrix;

#ifdef CONFIG_DISPLAY_FRAME_SYNC
static SemaphoreHandle_t _frame_sync_sem = NULL;

static bool IRAM_ATTR frame_sync_isr(void *arg) {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &xHigherPriorityTaskWoken);
  return xHigherPriorityTaskWoken == pdTRUE;
}
#endif
static uint8_t _brightness = (CONFIG_HUB75_BRIGHTNESS * 100) / 255;
static const char *TAG = "display";

#if CONFIG_HUB75_PANEL_WIDTH == 128 && CONFIG_HUB75_PANEL_HEIGHT == 64
// Batched upscale buffer: 4 source rows → 8 output rows per draw_pixels call.
// 4 KB in BSS (internal SRAM) replaces a 32 KB PSRAM heap allocation while
// keeping the call count low (8 calls vs 1 original vs 64 in the row-at-a-time
// approach).  draw_pixels reads from fast internal SRAM instead of PSRAM.
constexpr int kUpscaleBatchSrcRows = 4;
constexpr int kUpscaleBatchDstRows = kUpscaleBatchSrcRows * 2;
static uint32_t _scale_buf[128 * kUpscaleBatchDstRows];
#endif

// ---- Panel hardware tuning (console-settable, persisted in NVS) ----
//
// These describe how the physical panel is wired and clocked, which the build
// cannot always know: the same board ships with panels whose R and B lines are
// swapped, or that will not latch cleanly at the compiled clock. They live in
// their own namespace instead of the main config struct so the captive portal,
// REST and WebSocket config contracts stay unchanged; this is a repair and
// bring-up facility, not a user preference.
//
// Note this is separate from config_get().swap_colors, which swaps the G and B
// *pin assignments* at init. This swaps R and B in the pixel data.
static const char *HW_NVS_NAMESPACE = "disp_hw";
static const char *HW_KEY_COLOR_ORDER = "color_order";  // u8: 0=rgb, 1=bgr
static const char *HW_KEY_BIT_DEPTH = "bit_depth";      // u8: 4-12, 0=default
static const char *HW_KEY_CLK_MHZ = "clk_mhz";          // u8: 8|10|16|20|32

// Retained so display_reinit() can re-apply a mutated config;
// display_initialize populates it once.
static Hub75Config _mxconfig;

// True when the panel has R and B swapped relative to a normal panel.
static bool _panel_bgr = false;

// Holds the driver across a failed re-init so the caller's revert can restart
// it instead of leaving the panel dark with no way back short of a reboot.
static Hub75Driver *_reinit_orphan = NULL;

// Our RGBA frame buffers are handed to the driver as BGR on a normal panel
// (that is the byte order libwebp produces for RGB888_32), so a swapped panel
// is the one that needs RGB. The user-facing name follows the panel, not the
// buffer, hence the inversion here.
static inline Hub75ColorOrder rgba_draw_order() {
  return _panel_bgr ? Hub75ColorOrder::RGB : Hub75ColorOrder::BGR;
}

static bool clock_speed_from_mhz(int mhz, Hub75ClockSpeed *out) {
  switch (mhz) {
    case 8:
      *out = Hub75ClockSpeed::HZ_8M;
      return true;
    case 10:
      *out = Hub75ClockSpeed::HZ_10M;
      return true;
    case 16:
      *out = Hub75ClockSpeed::HZ_16M;
      return true;
    case 20:
      *out = Hub75ClockSpeed::HZ_20M;
      return true;
    case 32:
      *out = Hub75ClockSpeed::HZ_32M;
      return true;
    default:
      return false;
  }
}

static uint8_t clock_speed_to_mhz(Hub75ClockSpeed speed) {
  return (uint8_t)((uint32_t)speed / 1000000);
}

// nvs_settings_init() runs before the display comes up, so NVS is already
// mounted here; missing keys just leave the compiled defaults in place.
static void hw_settings_load(void) {
  NvsHandle nvs(HW_NVS_NAMESPACE, NVS_READONLY);
  if (!nvs) return;

  uint8_t v = 0;
  if (nvs.get_u8(HW_KEY_COLOR_ORDER, &v) == ESP_OK) {
    _panel_bgr = (v != 0);
  }
  if (nvs.get_u8(HW_KEY_BIT_DEPTH, &v) == ESP_OK && v >= 4 && v <= 12) {
    _mxconfig.bit_depth = v;
  }
  if (nvs.get_u8(HW_KEY_CLK_MHZ, &v) == ESP_OK) {
    Hub75ClockSpeed speed;
    if (clock_speed_from_mhz(v, &speed)) {
      _mxconfig.output_clock_speed = speed;
    }
  }

  if (_mxconfig.bit_depth) {
    ESP_LOGI(TAG, "Panel tuning: color_order=%s bit_depth=%u clk=%uMHz",
             _panel_bgr ? "bgr" : "rgb", _mxconfig.bit_depth,
             clock_speed_to_mhz(_mxconfig.output_clock_speed));
  } else {
    ESP_LOGI(TAG, "Panel tuning: color_order=%s bit_depth=default clk=%uMHz",
             _panel_bgr ? "bgr" : "rgb",
             clock_speed_to_mhz(_mxconfig.output_clock_speed));
  }
}

static bool hw_setting_save(const char *key, uint8_t value) {
  NvsHandle nvs(HW_NVS_NAMESPACE, NVS_READWRITE);
  if (!nvs) {
    ESP_LOGE(TAG, "Failed to open %s namespace", HW_NVS_NAMESPACE);
    return false;
  }
  if (nvs.set_u8(key, value) != ESP_OK || nvs.commit() != ESP_OK) {
    ESP_LOGE(TAG, "Failed to persist %s", key);
    return false;
  }
  return true;
}

// Rebuild the driver from the current _mxconfig. Quiesces the render pipeline
// first so nothing is mid-draw when the DMA engine goes away, and restores the
// scheduler unless something else (quiet hours) had already paused it.
//
// The driver object itself is reused (set_config on a stopped driver); only its
// DMA engine is torn down and rebuilt. Keeping the object means a failed
// begin() leaves something to retry with.
static bool display_reinit(void) {
  if (_matrix == NULL) return false;

  const bool was_paused = scheduler_is_paused();
  scheduler_pause();  // stops timers, gfx_stop(), blanks the panel
  gfx_wait_idle();    // player task has left the draw path

  // Publish NULL before teardown so any other drawer bails at its null check
  // rather than following a dangling pointer into a freed DMA engine.
  Hub75Driver *m = _matrix;
  _matrix = NULL;
  vTaskDelay(pdMS_TO_TICKS(20));

  // begin() applies config_.brightness, so carry the live value across.
  _mxconfig.brightness = m->get_brightness();

  m->end();
  m->set_config(_mxconfig);

  const bool ok = m->begin();
  if (ok) {
    _matrix = m;
#ifdef CONFIG_DISPLAY_FRAME_SYNC
    // begin() creates a fresh platform DMA object, which is where the frame
    // callback is stored, so it has to be re-registered or display_wait_frame
    // silently times out forever.
    if (_frame_sync_sem) {
      _matrix->set_frame_callback(frame_sync_isr, _frame_sync_sem);
    }
#endif
    _matrix->clear();
  } else {
    // Leave _matrix NULL: begin() failed, so there is no DMA engine to draw
    // into. The caller reverts the setting and retries, which is why the driver
    // object is kept alive here.
    ESP_LOGE(TAG, "Display re-init failed; panel is off pending revert");
    _reinit_orphan = m;
  }

  if (!was_paused) scheduler_resume();
  return ok;
}

static void apply_bit_depth(uint8_t v) { _mxconfig.bit_depth = v; }

static void apply_clk_mhz(uint8_t v) {
  Hub75ClockSpeed speed;
  if (clock_speed_from_mhz(v, &speed)) _mxconfig.output_clock_speed = speed;
}

// Apply a tuning value that needs a driver restart, rolling both the live
// config and the stored value back if the panel will not come up with it.
// Without the rollback a value the hardware cannot support (a bit depth that
// does not fit in DMA memory, a clock the panel will not latch) would already
// be in NVS and would kill the display on every subsequent boot.
static bool display_apply_tuning(const char *key, void (*apply)(uint8_t),
                                 uint8_t new_value, uint8_t old_value) {
  apply(new_value);
  if (!hw_setting_save(key, new_value)) {
    apply(old_value);
    return false;
  }
  if (display_reinit()) return true;

  ESP_LOGW(TAG, "Reverting %s to %u after failed re-init", key, old_value);
  apply(old_value);
  hw_setting_save(key, old_value);

  // display_reinit() parked the driver here when begin() failed; take it back
  // so the retry has an object to restart.
  if (_reinit_orphan) {
    _matrix = _reinit_orphan;
    _reinit_orphan = NULL;
  }
  if (!display_reinit()) {
    ESP_LOGE(TAG, "Revert also failed; display stays off until reboot");
  }
  return false;
}

int display_initialize(void) {
  // Get swap_colors setting
  bool swap_colors = config_get().swap_colors;

  // Initialize pin values based on hardware and swap_colors setting
  ESP_LOGI(TAG, "Initializing display with swap_colors=%s",
           swap_colors ? "true" : "false");

  // Initialize the panel. Bound to the retained config so display_reinit() can
  // re-apply it later without rebuilding the whole board preset.
  Hub75Config &mxconfig = _mxconfig;
  mxconfig.panel_width = CONFIG_HUB75_PANEL_WIDTH;
  mxconfig.panel_height = CONFIG_HUB75_PANEL_HEIGHT;

#if CONFIG_BOARD_TIDBYT_GEN2
  mxconfig.pins.r1 = 5;
  mxconfig.pins.g1 = 23;
  mxconfig.pins.b1 = 4;
  mxconfig.pins.r2 = 2;
  mxconfig.pins.g2 = 22;
  mxconfig.pins.b2 = 32;
  mxconfig.pins.a = 25;
  mxconfig.pins.b = 21;
  mxconfig.pins.c = 26;
  mxconfig.pins.d = 19;
  mxconfig.pins.e = -1;  // assign to pin 14 if using more than two panels
  mxconfig.pins.lat = 18;
  mxconfig.pins.oe = 27;
  mxconfig.pins.clk = 15;
  ESP_LOGI(TAG, "Board preset: Tidbyt Gen2");
#elif CONFIG_BOARD_TRONBYT_S3_WIDE
  mxconfig.pins.r1 = 4;
  mxconfig.pins.g1 = 5;
  mxconfig.pins.b1 = 6;
  mxconfig.pins.r2 = 7;
  mxconfig.pins.g2 = 15;
  mxconfig.pins.b2 = 16;
  mxconfig.pins.a = 17;
  mxconfig.pins.b = 18;
  mxconfig.pins.c = 8;
  mxconfig.pins.d = 3;
  mxconfig.pins.e = 46;
  mxconfig.pins.lat = 9;
  mxconfig.pins.oe = 10;
  mxconfig.pins.clk = 11;
  ESP_LOGI(TAG, "Board preset: Tronbyt S3 Wide");
#elif CONFIG_BOARD_TRONBYT_S3
  mxconfig.pins.r1 = 4;
  mxconfig.pins.r2 = 7;
  if (swap_colors) {
    mxconfig.pins.g1 = 5;
    mxconfig.pins.b1 = 6;
    mxconfig.pins.g2 = 15;
    mxconfig.pins.b2 = 16;
  } else {
    mxconfig.pins.g1 = 6;
    mxconfig.pins.b1 = 5;
    mxconfig.pins.g2 = 16;
    mxconfig.pins.b2 = 15;
  }
  mxconfig.pins.a = 17;
  mxconfig.pins.b = 18;
  mxconfig.pins.c = 8;
  mxconfig.pins.d = 3;
  mxconfig.pins.e = -1;
  mxconfig.pins.lat = 9;
  mxconfig.pins.oe = 10;
  mxconfig.pins.clk = 11;
  ESP_LOGI(TAG, "Board preset: Tronbyt S3");
#elif CONFIG_BOARD_PIXOTICKER
  mxconfig.pins.r1 = 2;
  mxconfig.pins.g1 = 4;
  mxconfig.pins.b1 = 15;
  mxconfig.pins.r2 = 16;
  mxconfig.pins.g2 = 17;
  mxconfig.pins.b2 = 27;
  mxconfig.pins.a = 5;
  mxconfig.pins.b = 18;
  mxconfig.pins.c = 19;
  mxconfig.pins.d = 21;
  mxconfig.pins.e = 12;
  mxconfig.pins.lat = 26;
  mxconfig.pins.oe = 25;
  mxconfig.pins.clk = 22;
  ESP_LOGI(TAG, "Board preset: Pixoticker");
#elif CONFIG_BOARD_MATRIXPORTAL_S3_WIDE
  // Same wiring as the MatrixPortal S3, but the E address line is routed to
  // GPIO 8 so the board can drive a 128x64 panel (1/32 scan).
  mxconfig.pins.r1 = 42;
  mxconfig.pins.r2 = 38;
  mxconfig.pins.a = 45;
  mxconfig.pins.b = 36;
  mxconfig.pins.c = 48;
  mxconfig.pins.d = 35;
  mxconfig.pins.e = 8;
  mxconfig.pins.lat = 47;
  mxconfig.pins.oe = 14;
  mxconfig.pins.clk = 2;
  if (swap_colors) {
    mxconfig.pins.g1 = 41;
    mxconfig.pins.b1 = 40;
    mxconfig.pins.g2 = 39;
    mxconfig.pins.b2 = 37;
  } else {
    mxconfig.pins.g1 = 40;
    mxconfig.pins.b1 = 41;
    mxconfig.pins.g2 = 37;
    mxconfig.pins.b2 = 39;
  }
  ESP_LOGI(TAG, "Board preset: MatrixPortal S3 Wide");
#elif CONFIG_BOARD_MATRIXPORTAL_S3
  mxconfig.pins.r1 = 42;
  mxconfig.pins.r2 = 38;
  mxconfig.pins.a = 45;
  mxconfig.pins.b = 36;
  mxconfig.pins.c = 48;
  mxconfig.pins.d = 35;
  mxconfig.pins.e = 21;
  mxconfig.pins.lat = 47;
  mxconfig.pins.oe = 14;
  mxconfig.pins.clk = 2;
  if (swap_colors) {
    mxconfig.pins.g1 = 41;
    mxconfig.pins.b1 = 40;
    mxconfig.pins.g2 = 39;
    mxconfig.pins.b2 = 37;
  } else {
    mxconfig.pins.g1 = 40;
    mxconfig.pins.b1 = 41;
    mxconfig.pins.g2 = 37;
    mxconfig.pins.b2 = 39;
  }
  ESP_LOGI(TAG, "Board preset: MatrixPortal S3");
#elif CONFIG_BOARD_WAVESHARE_S3
  // The board has 74HC245 level shifters that remap GPIO signals between the
  // ESP32 and HUB75 connector. Pins must match the ESP32 GPIO side, not the
  // HUB75 connector side. Sourced from Waveshare's official sdkconfig.defaults.
  // This variant doesn't support color swapping, use fixed pins.
  mxconfig.pins.r1 = 4;
  mxconfig.pins.g1 = 5;
  mxconfig.pins.b1 = 6;
  mxconfig.pins.r2 = 7;
  mxconfig.pins.g2 = 15;
  mxconfig.pins.b2 = 16;
  mxconfig.pins.a = 18;
  mxconfig.pins.b = 8;
  mxconfig.pins.c = 3;
  mxconfig.pins.d = 42;
  mxconfig.pins.e = 9;
  mxconfig.pins.lat = 40;
  mxconfig.pins.oe = 2;
  mxconfig.pins.clk = 41;
  ESP_LOGI(TAG, "Board preset: Waveshare ESP32-S3-RGB-Matrix");
#else  // GEN1 from here down.
  mxconfig.pins.a = 26;
  mxconfig.pins.b = 5;
  mxconfig.pins.c = 25;
  mxconfig.pins.d = 18;
  mxconfig.pins.e = -1;  // assign to pin 14 if using more than two panels
  mxconfig.pins.lat = 19;
  mxconfig.pins.oe = 32;
  mxconfig.pins.clk = 33;
  if (swap_colors) {
    // Swapped configuration for GEN1
    mxconfig.pins.r1 = 21;
    mxconfig.pins.g1 = 2;
    mxconfig.pins.b1 = 22;
    mxconfig.pins.r2 = 23;
    mxconfig.pins.g2 = 4;
    mxconfig.pins.b2 = 27;
  } else {
    // Normal configuration for GEN1
    mxconfig.pins.r1 = 2;
    mxconfig.pins.g1 = 22;
    mxconfig.pins.b1 = 21;
    mxconfig.pins.r2 = 4;
    mxconfig.pins.g2 = 27;
    mxconfig.pins.b2 = 23;
  }
  ESP_LOGI(TAG, "Board preset: Tidbyt Gen1");
#endif

  // Scan Pattern
#if defined(CONFIG_HUB75_SCAN_1_32)
  mxconfig.scan_pattern = Hub75ScanPattern::SCAN_1_32;
#elif defined(CONFIG_HUB75_SCAN_1_16)
  mxconfig.scan_pattern = Hub75ScanPattern::SCAN_1_16;
#elif defined(CONFIG_HUB75_SCAN_1_8)
  mxconfig.scan_pattern = Hub75ScanPattern::SCAN_1_8;
#endif

  // Scan wiring
#if defined(CONFIG_HUB75_WIRING_STANDARD)
  mxconfig.scan_wiring = Hub75ScanWiring::STANDARD_TWO_SCAN;
#elif defined(CONFIG_HUB75_WIRING_FOUR_SCAN_16PX)
  mxconfig.scan_wiring = Hub75ScanWiring::FOUR_SCAN_16PX_HIGH;
#elif defined(CONFIG_HUB75_WIRING_FOUR_SCAN_32PX)
  mxconfig.scan_wiring = Hub75ScanWiring::FOUR_SCAN_32PX_HIGH;
#elif defined(CONFIG_HUB75_WIRING_FOUR_SCAN_64PX)
  mxconfig.scan_wiring = Hub75ScanWiring::FOUR_SCAN_64PX_HIGH;
#endif

  // Shift Driver
#if defined(CONFIG_HUB75_DRIVER_GENERIC)
  mxconfig.shift_driver = Hub75ShiftDriver::GENERIC;
#elif defined(CONFIG_HUB75_DRIVER_FM6126A)
  mxconfig.shift_driver = Hub75ShiftDriver::FM6126A;
#elif defined(CONFIG_HUB75_DRIVER_FM6124)
  mxconfig.shift_driver = Hub75ShiftDriver::FM6124;
#elif defined(CONFIG_HUB75_DRIVER_MBI5124)
  mxconfig.shift_driver = Hub75ShiftDriver::MBI5124;
#elif defined(CONFIG_HUB75_DRIVER_DP3246)
  mxconfig.shift_driver = Hub75ShiftDriver::DP3246;
#endif

#if CONFIG_HUB75_DOUBLE_BUFFER
  mxconfig.double_buffer = true;
#else
  mxconfig.double_buffer = false;
#endif

  // Lower drive desenses the SoC's own 2.4GHz radio less; see the Kconfig
  // help. Default 3 preserves historical behavior, boards can override.
  mxconfig.gpio_drive_strength = CONFIG_HUB75_GPIO_DRIVE_STRENGTH;

  // Clock Speed
#if defined(CONFIG_HUB75_CLK_32MHZ)
  mxconfig.output_clock_speed = Hub75ClockSpeed::HZ_32M;
#elif defined(CONFIG_HUB75_CLK_20MHZ)
  mxconfig.output_clock_speed = Hub75ClockSpeed::HZ_20M;
#elif defined(CONFIG_HUB75_CLK_16MHZ)
  mxconfig.output_clock_speed = Hub75ClockSpeed::HZ_16M;
#elif defined(CONFIG_HUB75_CLK_10MHZ)
  mxconfig.output_clock_speed = Hub75ClockSpeed::HZ_10M;
#elif defined(CONFIG_HUB75_CLK_8MHZ)
  mxconfig.output_clock_speed = Hub75ClockSpeed::HZ_8M;
#endif

  mxconfig.min_refresh_rate = CONFIG_HUB75_MIN_REFRESH_RATE;
  mxconfig.latch_blanking = CONFIG_HUB75_LATCH_BLANKING;

  // Clock Phase
#ifdef CONFIG_HUB75_CLK_PHASE_INVERTED
  mxconfig.clk_phase_inverted = true;
#else
  mxconfig.clk_phase_inverted = false;
#endif

  mxconfig.brightness = CONFIG_HUB75_BRIGHTNESS;

  // Applied last so stored panel tuning wins over the compiled defaults,
  // matching the NVS > secrets.json > Kconfig override order.
  hw_settings_load();

  _matrix = new Hub75Driver(mxconfig);

  if (_matrix == NULL) {
    ESP_LOGE(TAG, "Failed to allocate Hub75Driver object");
    return 1;
  }

  if (!_matrix->begin()) {
    ESP_LOGE(TAG, "Hub75Driver begin() failed");
    delete _matrix;
    _matrix = NULL;
    return 1;
  }

#ifdef CONFIG_DISPLAY_FRAME_SYNC
  _frame_sync_sem = xSemaphoreCreateBinary();
  _matrix->set_frame_callback(frame_sync_isr, _frame_sync_sem);
#endif

  display_set_brightness((CONFIG_HUB75_BRIGHTNESS * 100) / 255);

  return 0;
}

uint8_t display_get_brightness() { return _brightness; }

// Per-board ceiling for the 0-100% -> set_brightness() (0-255) mapping.
// Genuine Tidbyt hardware (Gen1/Gen2) uses 100, matching the stock HDK
// convention where the brightness percentage feeds the panel 1:1 (max ~39%
// panel PWM duty). Third-party panels with no Tidbyt reference fall back to the
// legacy 230 (~90% duty) and can be tuned empirically per board.
#if CONFIG_BOARD_TIDBYT_GEN1 || CONFIG_BOARD_TIDBYT_GEN2
#define BRIGHTNESS_8BIT_MAX 100
#endif
#ifndef BRIGHTNESS_8BIT_MAX
#define BRIGHTNESS_8BIT_MAX 230
#endif

static inline uint8_t brightness_percent_to_8bit(uint8_t pct) {
  if (pct > 100) pct = 100;
  return (uint8_t)(((uint32_t)pct * BRIGHTNESS_8BIT_MAX + 50) / 100);
}

void display_set_brightness(uint8_t brightness_pct) {
  if (_matrix == NULL) return;
  if (brightness_pct != _brightness) {
    uint8_t brightness_8bit = brightness_percent_to_8bit(brightness_pct);

    ESP_LOGI(TAG, "Setting brightness to %d%% (%d)", brightness_pct,
             brightness_8bit);
    _matrix->set_brightness(brightness_8bit);
    _matrix->clear();
    _brightness = brightness_pct;
  }
}

void display_shutdown(void) {
  if (_matrix == NULL) {
#ifdef CONFIG_DISPLAY_FRAME_SYNC
    if (_frame_sync_sem) {
      vSemaphoreDelete(_frame_sync_sem);
      _frame_sync_sem = NULL;
    }
#endif
    return;
  }

#ifdef CONFIG_DISPLAY_FRAME_SYNC
  _matrix->set_frame_callback(nullptr, nullptr);
#endif

  _matrix->clear();
  _matrix->end();
  delete _matrix;
  _matrix = NULL;

#ifdef CONFIG_DISPLAY_FRAME_SYNC
  if (_frame_sync_sem) {
    vSemaphoreDelete(_frame_sync_sem);
    _frame_sync_sem = NULL;
  }
#endif
}

bool display_wait_frame(uint32_t timeout_ms) {
#ifdef CONFIG_DISPLAY_FRAME_SYNC
  if (_frame_sync_sem == NULL) return false;
  // Drain any stale tokens so we wait for the NEXT frame boundary,
  // not one that fired while we were drawing to the back buffer.
  while (xSemaphoreTake(_frame_sync_sem, 0) == pdTRUE) {}
  return xSemaphoreTake(_frame_sync_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
#else
  return false;
#endif
}

void display_draw_buffer(const uint8_t *pix, int width, int height) {
  if (!pix || width <= 0 || height <= 0) return;
  if (_matrix == NULL) return;
  if (width > CONFIG_HUB75_PANEL_WIDTH ||
      height > CONFIG_HUB75_PANEL_HEIGHT) {
    ESP_LOGW(TAG, "Rejecting oversized frame: %dx%d (panel %dx%d)", width,
             height, CONFIG_HUB75_PANEL_WIDTH, CONFIG_HUB75_PANEL_HEIGHT);
    return;
  }

#if CONFIG_HUB75_PANEL_WIDTH == 128 && CONFIG_HUB75_PANEL_HEIGHT == 64
  if (width == 64 && height == 32) {
    // Batched 2x upscale: process kUpscaleBatchSrcRows source rows at a
    // time into a static internal-SRAM buffer, then hand the batch to
    // draw_pixels in a single call.  This keeps the call count low (8)
    // while reading from fast SRAM instead of PSRAM.
    const uint32_t *src32 = (const uint32_t *)pix;
    for (int batch_y = 0; batch_y < height; batch_y += kUpscaleBatchSrcRows) {
      for (int y = 0; y < kUpscaleBatchSrcRows; y++) {
        const uint32_t *src_row = &src32[(batch_y + y) * width];
        uint32_t *dst_row1 = &_scale_buf[(y * 2) * 128];
        uint32_t *dst_row2 = &_scale_buf[(y * 2 + 1) * 128];
        for (int sx = 0; sx < width; sx++) {
          uint32_t pixel = src_row[sx];
          dst_row1[sx * 2] = pixel;
          dst_row1[sx * 2 + 1] = pixel;
          dst_row2[sx * 2] = pixel;
          dst_row2[sx * 2 + 1] = pixel;
        }
      }
      _matrix->draw_pixels(0, batch_y * 2, 128, kUpscaleBatchDstRows,
                           (uint8_t *)_scale_buf,
                           Hub75PixelFormat::RGB888_32, rgba_draw_order());
    }
    return;
  }
#endif

  // Default path: bulk transfer for native resolution
  _matrix->draw_pixels(0, 0, width, height, pix, Hub75PixelFormat::RGB888_32,
                       rgba_draw_order());
}

// True when a canvas of this size renders through a path that
// display_draw_span can reproduce (1:1 native, or the 2x upscale used for
// 64x32 content on 128x64 panels).
bool display_span_supported(int canvas_w, int canvas_h) {
#if CONFIG_HUB75_PANEL_WIDTH == 128 && CONFIG_HUB75_PANEL_HEIGHT == 64
  if (canvas_w == 64 && canvas_h == 32) return true;
#endif
  return canvas_w == CONFIG_HUB75_PANEL_WIDTH &&
         canvas_h == CONFIG_HUB75_PANEL_HEIGHT;
}

// Draw a single row segment of RGBA pixels given in canvas coordinates,
// applying the same scaling display_draw_buffer would use for that canvas.
// Writes into the active buffer without flipping, so it is only meaningful
// when double buffering is disabled and the active buffer is live.
void display_draw_span(const uint8_t *pix, int x, int y, int width,
                       int canvas_w, int canvas_h) {
  if (!pix || width <= 0) return;
  if (_matrix == NULL) return;

#if CONFIG_HUB75_PANEL_WIDTH == 128 && CONFIG_HUB75_PANEL_HEIGHT == 64
  if (canvas_w == 64 && canvas_h == 32) {
    // 2x upscale: one canvas row span becomes a doubled-width two-row blit.
    if (x < 0 || y < 0 || x + width > 64 || y >= 32) return;
    const int dst_w = width * 2;
    const uint32_t *src = (const uint32_t *)pix;
    uint32_t *dst_row1 = &_scale_buf[0];
    uint32_t *dst_row2 = &_scale_buf[dst_w];
    for (int sx = 0; sx < width; sx++) {
      uint32_t pixel = src[sx];
      dst_row1[sx * 2] = pixel;
      dst_row1[sx * 2 + 1] = pixel;
      dst_row2[sx * 2] = pixel;
      dst_row2[sx * 2 + 1] = pixel;
    }
    _matrix->draw_pixels(x * 2, y * 2, dst_w, 2, (uint8_t *)_scale_buf,
                         Hub75PixelFormat::RGB888_32, rgba_draw_order());
    return;
  }
#endif

  if (canvas_w != CONFIG_HUB75_PANEL_WIDTH ||
      canvas_h != CONFIG_HUB75_PANEL_HEIGHT) {
    return;  // Unsupported scale factor for span drawing
  }
  if (x < 0 || y < 0 || x + width > CONFIG_HUB75_PANEL_WIDTH ||
      y >= CONFIG_HUB75_PANEL_HEIGHT) {
    return;
  }
  _matrix->draw_pixels(x, y, width, 1, pix, Hub75PixelFormat::RGB888_32,
                       rgba_draw_order());
}

bool display_get_panel_bgr(void) { return _panel_bgr; }

bool display_set_panel_bgr(bool bgr) {
  if (!hw_setting_save(HW_KEY_COLOR_ORDER, bgr ? 1 : 0)) return false;
  // Read per draw call, so no re-init is needed; the next frame is correct.
  _panel_bgr = bgr;
  return true;
}

uint8_t display_get_bit_depth(void) { return _mxconfig.bit_depth; }

bool display_set_bit_depth(uint8_t depth) {
  if (depth < 4 || depth > 12) return false;
  return display_apply_tuning(HW_KEY_BIT_DEPTH, apply_bit_depth, depth,
                              _mxconfig.bit_depth);
}

uint8_t display_get_clock_mhz(void) {
  return clock_speed_to_mhz(_mxconfig.output_clock_speed);
}

bool display_set_clock_mhz(uint8_t mhz) {
  Hub75ClockSpeed speed;
  if (!clock_speed_from_mhz(mhz, &speed)) return false;
  return display_apply_tuning(HW_KEY_CLK_MHZ, apply_clk_mhz, mhz,
                              clock_speed_to_mhz(_mxconfig.output_clock_speed));
}

void display_draw(const uint8_t *pix, int width, int height) {
  if (!pix || width <= 0 || height <= 0) return;
  if (width > CONFIG_HUB75_PANEL_WIDTH ||
      height > CONFIG_HUB75_PANEL_HEIGHT) {
    ESP_LOGW(TAG, "Rejecting oversized frame: %dx%d (panel %dx%d)", width,
             height, CONFIG_HUB75_PANEL_WIDTH, CONFIG_HUB75_PANEL_HEIGHT);
    return;
  }
  display_draw_buffer(pix, width, height);
  if (_matrix != NULL) _matrix->flip_buffer();
}

void display_clear(void) { if (_matrix != NULL) _matrix->clear(); }

void display_draw_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (_matrix != NULL) {
    _matrix->set_pixel(x, y, r, g, b);
    _matrix->flip_buffer();
  }
}

void display_fill_rect(int x, int y, int w, int h, uint8_t r, uint8_t g,
                       uint8_t b) {
  if (_matrix != NULL) {
    _matrix->fill(x, y, w, h, r, g, b);
    // Note: No flip here, caller must flip
  }
}

void draw_error_indicator_pixel(void) { display_draw_pixel(0, 0, 100, 0, 0); }

void clear_error_indicator_pixel(void) { display_draw_pixel(0, 0, 0, 0, 0); }

void display_text(const char *text, int x, int y, uint8_t r, uint8_t g,
                  uint8_t b, int scale) {
  if (_matrix == NULL || text == NULL) {
    return;
  }

  int cursor_x = x;
  int cursor_y = y;

  // Iterate through each character in the string
  for (int i = 0; text[i] != '\0'; i++) {
    char c = text[i];

    // Check if character is in font range
    if (c < FONT5X7_FIRST_CHAR || c > FONT5X7_LAST_CHAR) {
      c = ' ';  // Replace unsupported characters with space
    }

    // Get font data for this character
    int char_index = c - FONT5X7_FIRST_CHAR;
    const uint8_t *char_data = font5x7[char_index];

    // Draw each column of the character
    for (int col = 0; col < FONT5X7_CHAR_WIDTH; col++) {
      uint8_t column_data = char_data[col];

      // Draw each row in the column
      for (int row = 0; row < FONT5X7_CHAR_HEIGHT; row++) {
        if (column_data & (1 << row)) {
          int px = cursor_x + (col * scale);
          int py = cursor_y + (row * scale);

          if (scale > 1) {
            // Optimize scaled text using fill
            _matrix->fill(px, py, scale, scale, r, g, b);
          } else {
            // Draw pixel(s) based on scale
            // Check bounds
            if (px >= 0 && px < CONFIG_HUB75_PANEL_WIDTH && py >= 0 &&
                py < CONFIG_HUB75_PANEL_HEIGHT) {
              _matrix->set_pixel(px, py, r, g, b);
            }
          }
        }
      }
    }

    // Move cursor to next character position (5 pixels + 1 pixel spacing)
    cursor_x += (FONT5X7_CHAR_WIDTH + 1) * scale;
  }

  // Note: Not flipping buffer here anymore - caller must call display_flip()
}

void display_flip(void) {
  if (_matrix != NULL) {
    _matrix->flip_buffer();
  }
}
