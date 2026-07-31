#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#define DISPLAY_MAX_BRIGHTNESS 100
#define DISPLAY_MIN_BRIGHTNESS 0

#ifdef __cplusplus
extern "C" {
#endif
int display_initialize(void);
uint8_t display_get_brightness(void);
void display_set_brightness(uint8_t brightness_pct);
void display_shutdown(void);

void display_draw(const uint8_t* pix, int width, int height);
void display_draw_buffer(const uint8_t* pix, int width, int height);
void display_draw_span(const uint8_t* pix, int x, int y, int width,
                       int canvas_w, int canvas_h);
bool display_span_supported(int canvas_w, int canvas_h);
void display_clear(void);
void display_draw_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);
void display_fill_rect(int x, int y, int w, int h, uint8_t r, uint8_t g,
                       uint8_t b);
void draw_error_indicator_pixel(void);
void clear_error_indicator_pixel(void);
void display_text(const char* text, int x, int y, uint8_t r, uint8_t g,
                  uint8_t b, int scale);
void display_flip(void);
bool display_wait_frame(uint32_t timeout_ms);

// Panel hardware tuning. Each setter persists to NVS and returns false if the
// value is out of range or the store failed. Color order applies to the next
// frame; bit depth and clock speed re-initialize the driver, which briefly
// blanks the panel and pauses playback.
bool display_get_panel_bgr(void);
bool display_set_panel_bgr(bool bgr);
uint8_t display_get_bit_depth(void);  // 0 = compile-time default
bool display_set_bit_depth(uint8_t depth);
uint8_t display_get_clock_mhz(void);
bool display_set_clock_mhz(uint8_t mhz);

#ifdef __cplusplus
}
#endif
