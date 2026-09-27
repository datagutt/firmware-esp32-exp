// WebP Player - Event-driven animated WebP playback task
// Modeled after matrx-fw/main/webp_player/
// State machine: IDLE <-> PLAYING
#include "webp_player.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <http_parser.h>
#include "webp_decoder.h"

#include "assets.h"
#include "display.h"
#include "nvs_settings.h"
#include "raii_utils.hpp"
#include "sockets.h"
#include "version.h"

static const char* TAG = "webp_player";

#ifndef CONFIG_BACKGROUND_DWELL_CAP_SECONDS
#define CONFIG_BACKGROUND_DWELL_CAP_SECONDS 30
#endif

ESP_EVENT_DEFINE_BASE(GFX_PLAYER_EVENTS);

int32_t effective_dwell_for_brightness(uint8_t brightness_pct,
                                       int32_t dwell_secs) {
  int cap = CONFIG_BACKGROUND_DWELL_CAP_SECONDS;
  if (cap < 5) {
    cap = 5;
  }
  // Defensive: remote_get() no longer reports out-of-range levels, but an
  // out-of-range value here means "unknown", not "very bright".
  if (brightness_pct > DISPLAY_MAX_BRIGHTNESS) {
    return dwell_secs;
  }
  if (brightness_pct == 0 && dwell_secs > cap) {
    ESP_LOGI(TAG, "Brightness 0%%: capping dwell %lds -> %ds (background fetch)",
             static_cast<long>(dwell_secs), cap);
    return static_cast<int32_t>(cap);
  }
  return dwell_secs;
}

namespace {

//------------------------------------------------------------------------------
// Configuration
//------------------------------------------------------------------------------

constexpr uint32_t TASK_STACK_SIZE = 4096;
constexpr int TASK_PRIORITY = 2;
constexpr int TASK_CORE = 1;
constexpr int DECODE_RETRY_COUNT = 3;
constexpr int DECODE_RETRY_DELAY_MS = 200;
constexpr uint32_t WAIT_IDLE_TIMEOUT_MS = 2000;
constexpr int64_t TICK_US = 1000000 / configTICK_RATE_HZ;

// Set while the task is not drawing: IDLE, or momentarily cleared while an
// idle task repaints the error indicator.
constexpr EventBits_t BIT_IDLE = BIT0;

// RGBA like decoded frames, so it goes through the same color-order path.
constexpr uint8_t INDICATOR_RGBA[4] = {100, 0, 0, 255};
constexpr uint8_t BLACK_RGBA[4] = {0, 0, 0, 255};

//------------------------------------------------------------------------------
// Player State
//------------------------------------------------------------------------------

enum class State : uint8_t { IDLE, PLAYING };
enum class InterruptRequest : uint8_t { NONE, STOP_ONLY, PREEMPT_PENDING };

//------------------------------------------------------------------------------
// Pending Command (written by API, read by task)
//------------------------------------------------------------------------------

struct PendingCmd {
  std::atomic<bool> valid{false};
  void* buf = nullptr;
  size_t len = 0;
  int32_t dwell_secs = 0;
  int counter = 0;
  gfx_source_type_t source_type = GFX_SOURCE_RAM;
  const char* embedded_name = nullptr;
};

//------------------------------------------------------------------------------
// Player Context
//------------------------------------------------------------------------------

struct PlayerContext {
  TaskHandle_t task = nullptr;
  SemaphoreHandle_t mutex = nullptr;
  EventGroupHandle_t event_group = nullptr;

  std::atomic<State> state{State::IDLE};
  std::atomic<bool> paused{false};
  std::atomic<InterruptRequest> interrupt_request{InterruptRequest::NONE};
  PendingCmd pending;
  int counter = 0;
  int loaded_counter = 0;

  // Current playback data (task-local)
  void* webp_buf = nullptr;
  size_t webp_len = 0;
  int32_t dwell_secs = 0;
  int active_counter = -1;
  gfx_source_type_t source_type = GFX_SOURCE_RAM;
  const char* embedded_name = nullptr;

  // Decoder (owns the decoded frame buffer; see WebpDecoder::get_next_frame)
  WebpDecoder decoder;
  WebpDecoderInfo decoder_info = {};
  // Last successfully decoded frame, i.e. what the panel shows. Owned by the
  // decoder and only valid until its next decode or destruction.
  const uint8_t* current_frame = nullptr;

  // Frame copies for row diffing (lazily allocated, PSRAM only, kept across
  // images). shown_frame mirrors what the panel displays; back_frame mirrors
  // the back DMA buffer, which after a flip holds the frame from two flips
  // ago. They hold frame content only, never the error indicator.
  uint8_t* shown_frame = nullptr;
  uint8_t* back_frame = nullptr;
  int prev_w = 0;  // canvas size of the last rendered frame
  int prev_h = 0;
  bool shown_valid = false;
  bool back_valid = false;

  // Error indicator overlay. Requested from any task, drawn only here.
  std::atomic<bool> indicator_wanted{false};
  bool indicator_drawn = false;
  // Frame pixel under the indicator, to restore when it is cleared.
  uint8_t pixel0[4] = {};
  bool pixel0_valid = false;
  // Both DMA buffers hold the same image, so a single pixel can be changed
  // with draw, flip, draw without the flip exposing an older frame.
  bool panel_settled = false;
  // Set by other tasks after drawing on the panel while the player was
  // stopped; the player then forgets what it believes the panel shows.
  std::atomic<bool> foreign_draw{false};

  // Timing
  int64_t next_frame_us = 0;
  int64_t playback_start_us = 0;

  // Error tracking
  int decode_error_count = 0;
  bool static_rendered = false;
  bool initialized = false;
};

PlayerContext ctx;

//------------------------------------------------------------------------------
// Rendering
//------------------------------------------------------------------------------
// The player task is the only code that draws on the panel while playback is
// running, and the only code that flips its buffers. Anything else shown on
// top of the image (the error indicator) is an overlay applied here.
//
// Frame diffing is ported from matrx-fw. It skips DMA-buffer writes for
// content that did not change since the previous frame: identical frames are
// skipped entirely, mostly-changed frames render in full (which hits the
// driver's fused full-frame path), and otherwise only the changed span of
// each dirty row is written, diffed against whatever the buffer being drawn
// into currently holds.

void invalidate_prev_frame() {
  ctx.shown_valid = false;
  ctx.back_valid = false;
}

// The panel was drawn on by someone else: nothing the player knows about its
// content holds any more.
void forget_panel_content() {
  invalidate_prev_frame();
  ctx.panel_settled = false;
  ctx.pixel0_valid = false;
  ctx.indicator_drawn = false;
}

uint8_t* alloc_frame_copy(size_t needed) {
#if CONFIG_SPIRAM
  // PSRAM only: the copies are an optimization, and on internal RAM they would
  // compete with TLS handshakes and task stacks. Without them every frame
  // simply renders in full.
  return static_cast<uint8_t*>(heap_caps_malloc(needed, MALLOC_CAP_SPIRAM));
#else
  (void)needed;
  return nullptr;
#endif
}

void present() {
#if CONFIG_HUB75_DOUBLE_BUFFER
#ifdef CONFIG_DISPLAY_FRAME_SYNC
  display_wait_frame(50);
#endif
  display_flip();
#endif
}

// Draws the indicator state into the buffer being drawn (no flip). Clearing
// restores the frame pixel it covered, or black if that is unknown.
void draw_indicator(bool on) {
  const uint8_t* px = on                  ? INDICATOR_RGBA
                      : ctx.pixel0_valid ? ctx.pixel0
                                         : BLACK_RGBA;
  display_draw_buffer(px, 1, 1);
}

void render_frame(const uint8_t* frame, int canvas_w, int canvas_h) {
  const size_t row_bytes = static_cast<size_t>(canvas_w) * 4;
  const size_t needed = row_bytes * canvas_h;

  // Both buffers may still carry the old indicator state, so a change forces
  // full renders until each has been rewritten.
  const bool indicator = ctx.indicator_wanted.load();
  if (indicator != ctx.indicator_drawn) {
    invalidate_prev_frame();
    ctx.indicator_drawn = indicator;
  }

  if (ctx.prev_w != canvas_w || ctx.prev_h != canvas_h) {
    heap_caps_free(ctx.shown_frame);
    ctx.shown_frame = alloc_frame_copy(needed);
#if CONFIG_HUB75_DOUBLE_BUFFER
    heap_caps_free(ctx.back_frame);
    ctx.back_frame = alloc_frame_copy(needed);
#endif
    invalidate_prev_frame();
    ctx.prev_w = canvas_w;
    ctx.prev_h = canvas_h;
  }

  // Identical frame: leave the panel untouched (no draw, no flip).
  if (ctx.shown_frame && ctx.shown_valid &&
      memcmp(frame, ctx.shown_frame, needed) == 0) {
    return;
  }

  ctx.panel_settled = false;
  memcpy(ctx.pixel0, frame, sizeof(ctx.pixel0));
  ctx.pixel0_valid = true;

  // Span writes land in the buffer that becomes visible next, so they must
  // diff against that buffer's current content: the back copy under double
  // buffering, the shown copy when drawing into the live buffer.
#if CONFIG_HUB75_DOUBLE_BUFFER
  uint8_t* ref = ctx.back_frame;
  const bool ref_valid = ctx.back_valid;
#else
  uint8_t* ref = ctx.shown_frame;
  const bool ref_valid = ctx.shown_valid;
#endif

  bool spans_drawn = false;
  if (ref && ref_valid && display_span_supported(canvas_w, canvas_h)) {
    // Single compare pass over the (PSRAM) frame copies: remember which rows
    // differ so the span loop below does not memcmp the same rows again.
    // display_span_supported bounds canvas_h to the panel height.
    uint8_t row_dirty[CONFIG_HUB75_PANEL_HEIGHT];
    int dirty_rows = 0;
    for (int y = 0; y < canvas_h; y++) {
      row_dirty[y] =
          memcmp(frame + y * row_bytes, ref + y * row_bytes, row_bytes) != 0;
      dirty_rows += row_dirty[y];
    }

    if (dirty_rows <= (canvas_h * 3) / 4) {
      for (int y = 0; y < canvas_h; y++) {
        if (!row_dirty[y]) {
          continue;
        }
        const uint32_t* cur =
            reinterpret_cast<const uint32_t*>(frame + y * row_bytes);
        uint32_t* prev = reinterpret_cast<uint32_t*>(ref + y * row_bytes);
        int first = 0;
        while (cur[first] == prev[first]) first++;
        int last = canvas_w - 1;
        while (cur[last] == prev[last]) last--;
        const int span = last - first + 1;
        display_draw_span(reinterpret_cast<const uint8_t*>(cur + first), first,
                          y, span, canvas_w, canvas_h);
        memcpy(prev + first, cur + first, static_cast<size_t>(span) * 4);
      }
      spans_drawn = true;
    }
  }

  if (!spans_drawn) {
    display_draw_buffer(frame, canvas_w, canvas_h);
  }
  // Re-applied on every draw: a span may have covered the pixel, and the
  // buffer drawn into may predate the indicator.
  if (indicator) {
    draw_indicator(true);
  }
  present();

#if CONFIG_HUB75_DOUBLE_BUFFER
  // The buffer just written is now visible and the old shown content became
  // the back buffer. Swap the copies to match.
  uint8_t* tmp = ctx.shown_frame;
  ctx.shown_frame = ctx.back_frame;
  ctx.back_frame = tmp;
  ctx.back_valid = ctx.shown_valid;
  if (spans_drawn) {
    // The spans already brought the reference copy up to date.
    ctx.shown_valid = true;
    return;
  }
#else
  if (spans_drawn) return;
#endif
  if (ctx.shown_frame) {
    memcpy(ctx.shown_frame, frame, needed);
    ctx.shown_valid = true;
  } else {
    ctx.shown_valid = false;
  }
}

// Copies the image on screen into the back buffer as well, so a later flip
// (an indicator change on a static or idle panel) cannot bring back an older
// frame. Skipped while stopped: whoever stopped the player owns the panel.
void settle_panel() {
  if (ctx.panel_settled || ctx.paused.load()) return;
#if CONFIG_HUB75_DOUBLE_BUFFER
  const uint8_t* shown = ctx.current_frame;
  if (!shown && ctx.shown_frame && ctx.shown_valid) {
    shown = ctx.shown_frame;
  }
  if (!shown) return;
  display_draw_buffer(shown, ctx.prev_w, ctx.prev_h);
  if (ctx.indicator_drawn) {
    draw_indicator(true);
  }
  if (ctx.back_frame && ctx.shown_frame && ctx.shown_valid) {
    memcpy(ctx.back_frame, ctx.shown_frame,
           static_cast<size_t>(ctx.prev_w) * ctx.prev_h * 4);
    ctx.back_valid = true;
  }
#endif
  ctx.panel_settled = true;
}

// Applies an indicator change without re-rendering the image. On a settled
// panel both buffers end up identical again; otherwise (content someone else
// drew, or a frame that could not be settled) the flip may show what the
// back buffer held, which is the best that can be done without the image.
void repaint_indicator_in_place() {
  const bool want = ctx.indicator_wanted.load();
  if (want == ctx.indicator_drawn) return;
  ctx.indicator_drawn = want;
  draw_indicator(want);
#if CONFIG_HUB75_DOUBLE_BUFFER
  present();
  draw_indicator(want);
#endif
}

// Brings the panel's indicator up to date while an image is playing.
void refresh_indicator_playing() {
  if (ctx.indicator_wanted.load() == ctx.indicator_drawn) return;
  if (ctx.panel_settled) {
    repaint_indicator_in_place();
  } else if (ctx.current_frame) {
    render_frame(ctx.current_frame, ctx.decoder_info.canvas_width,
                 ctx.decoder_info.canvas_height);
  }
  // Otherwise the next decoded frame picks it up.
}

// Brings the panel's indicator up to date while idle. gfx_wait_idle() callers
// rely on BIT_IDLE meaning "not drawing", so it is dropped for the duration
// and the pause flag re-checked after dropping it.
void refresh_indicator_idle() {
  if (ctx.indicator_wanted.load() == ctx.indicator_drawn) return;
  xEventGroupClearBits(ctx.event_group, BIT_IDLE);
  if (!ctx.paused.load()) {
    repaint_indicator_in_place();
  }
  xEventGroupSetBits(ctx.event_group, BIT_IDLE);
}

//------------------------------------------------------------------------------
// Static Asset Detection
//------------------------------------------------------------------------------

bool is_static_asset(const void* ptr) { return asset_is_static(ptr); }

//------------------------------------------------------------------------------
// Decoder Management
//------------------------------------------------------------------------------

void destroy_decoder() {
  ctx.decoder = WebpDecoder();  // Reset to default
  ctx.decoder_info = {};
  ctx.current_frame = nullptr;
}

// Returns ESP_ERR_INVALID_SIZE for a canvas larger than the panel.
esp_err_t create_decoder() {
  destroy_decoder();

  if (!ctx.webp_buf || ctx.webp_len == 0) {
    ESP_LOGE(TAG, "No WebP data");
    return ESP_ERR_INVALID_ARG;
  }

  // The panel bound is checked before the decoder allocates its canvases:
  // display_draw_buffer rejects larger frames anyway, so decoding them would
  // only burn memory and CPU on frames that never show.
  esp_err_t err = ctx.decoder.init(static_cast<const uint8_t*>(ctx.webp_buf),
                                   ctx.webp_len, CONFIG_HUB75_PANEL_WIDTH,
                                   CONFIG_HUB75_PANEL_HEIGHT);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Decoder init failed: %s", esp_err_to_name(err));
    return err;
  }

  ctx.decoder_info = ctx.decoder.get_info();
  ESP_LOGI(TAG, "Decoder created: %u frames, %ux%u",
           ctx.decoder_info.frame_count, ctx.decoder_info.canvas_width,
           ctx.decoder_info.canvas_height);
  return ESP_OK;
}

//------------------------------------------------------------------------------
// Buffer Management
//------------------------------------------------------------------------------

void free_buffer() {
  if (ctx.webp_buf && !is_static_asset(ctx.webp_buf)) {
    free(ctx.webp_buf);
  }
  ctx.webp_buf = nullptr;
  ctx.webp_len = 0;
}

//------------------------------------------------------------------------------
// Event Emission
//------------------------------------------------------------------------------

void emit_playing_event() {
  gfx_playing_evt_t evt = {};
  evt.source_type = ctx.source_type;
  evt.embedded_name = ctx.embedded_name;
  evt.duration_ms = (ctx.dwell_secs > 0)
                        ? static_cast<uint32_t>(ctx.dwell_secs) * 1000
                        : 0;
  evt.frame_count = ctx.decoder_info.frame_count;
  esp_event_post(GFX_PLAYER_EVENTS, GFX_PLAYER_EVT_PLAYING,
                 &evt, sizeof(evt), 0);
}

void emit_error_event() {
  gfx_error_evt_t evt = {};
  evt.source_type = ctx.source_type;
  evt.embedded_name = ctx.embedded_name;
  evt.error_code = -1;
  esp_event_post(GFX_PLAYER_EVENTS, GFX_PLAYER_EVT_ERROR,
                 &evt, sizeof(evt), 0);
}

void emit_stopped_event() {
  esp_event_post(GFX_PLAYER_EVENTS, GFX_PLAYER_EVT_STOPPED,
                 nullptr, 0, 0);
}

//------------------------------------------------------------------------------
// WebSocket Notifications
//------------------------------------------------------------------------------

constexpr TickType_t WS_SEND_TIMEOUT = pdMS_TO_TICKS(2000);

void send_displaying_notification(int counter) {
  char message[64];
  int len =
      snprintf(message, sizeof(message), "{\"displaying\":%d}", counter);
  if (len > 0 && static_cast<size_t>(len) < sizeof(message)) {
    int sent = sockets_send_text(message, len, WS_SEND_TIMEOUT);
    if (sent < 0) {
      ESP_LOGD(TAG, "WS send skipped (displaying:%d)", counter);
    } else {
      ESP_LOGD(TAG, "WS send: %s", message);
    }
  }
}

void send_queued_notification(int counter) {
  char message[64];
  int len =
      snprintf(message, sizeof(message), "{\"queued\":%d}", counter);
  if (len > 0 && static_cast<size_t>(len) < sizeof(message)) {
    int sent = sockets_send_text(message, len, WS_SEND_TIMEOUT);
    if (sent < 0) {
      ESP_LOGD(TAG, "WS send skipped (queued:%d)", counter);
    } else {
      ESP_LOGD(TAG, "WS Send: %s", message);
    }
  }
}

//------------------------------------------------------------------------------
// State Transitions
//------------------------------------------------------------------------------

void goto_idle() {
  // The image stays on the panel while idle; make both buffers hold it so an
  // indicator change can flip without showing an older frame.
  settle_panel();
  destroy_decoder();
  ctx.state.store(State::IDLE);
  xEventGroupSetBits(ctx.event_group, BIT_IDLE);
}

esp_err_t start_playback() {
  ctx.decode_error_count = 0;
  ctx.static_rendered = false;

  esp_err_t err = create_decoder();
  if (err != ESP_OK) {
    return err;
  }

  ctx.playback_start_us = esp_timer_get_time();
  ctx.next_frame_us = ctx.playback_start_us;
  ctx.state.store(State::PLAYING);
  xEventGroupClearBits(ctx.event_group, BIT_IDLE);

  send_displaying_notification(ctx.active_counter);
  emit_playing_event();
  ESP_LOGI(TAG, "Playback started: counter=%d, dwell=%ld",
           ctx.active_counter, static_cast<long>(ctx.dwell_secs));
  return ESP_OK;
}

//------------------------------------------------------------------------------
// Duration Check — applies to both animated and static images
//------------------------------------------------------------------------------

bool check_dwell_expired() {
  // Embedded sprites loop forever
  if (ctx.source_type == GFX_SOURCE_EMBEDDED) {
    return false;
  }

  // Unlimited duration
  if (ctx.dwell_secs <= 0) {
    return false;
  }

  int64_t dwell_us = static_cast<int64_t>(ctx.dwell_secs) * 1000000;
  int64_t elapsed_us = esp_timer_get_time() - ctx.playback_start_us;
  return elapsed_us >= dwell_us;
}

//------------------------------------------------------------------------------
// Decode Error Handling
//------------------------------------------------------------------------------

void give_up_decode() {
  emit_error_event();
  free_buffer();
  // Show oversize asset for RAM-sourced images (not embedded, to avoid loops)
  if (ctx.source_type == GFX_SOURCE_RAM) {
    goto_idle();
    gfx_play_embedded("oversize", false);
  } else {
    gfx_set_error_indicator(true);
    goto_idle();
  }
}

void handle_decode_error() {
  ctx.decode_error_count++;
  ESP_LOGW(TAG, "Decode error %d/%d", ctx.decode_error_count,
           DECODE_RETRY_COUNT);

  if (ctx.decode_error_count >= DECODE_RETRY_COUNT) {
    ESP_LOGE(TAG, "Max retries reached");
    give_up_decode();
    return;
  }

  // Retry: recreate decoder after delay. The actual retry happens in the
  // player loop on the next iteration, which calls decode_and_render_frame
  // against the freshly recreated decoder. If recreation itself fails, give
  // up immediately rather than recursing into handle_decode_error (which
  // would blow the stack if create_decoder keeps failing).
  vTaskDelay(pdMS_TO_TICKS(DECODE_RETRY_DELAY_MS));
  if (create_decoder() != ESP_OK) {
    ESP_LOGE(TAG, "Decoder recreation failed, giving up");
    give_up_decode();
  }
}

//------------------------------------------------------------------------------
// Command Handling
//------------------------------------------------------------------------------

void handle_pending_command(bool emit_stopped_before_replace = false) {
  if (!ctx.pending.valid.load(std::memory_order_acquire)) {
    return;
  }

  // Should the new image fail to start, the current one stays on the panel.
  settle_panel();

  // Play command — accept pending content
  {
    raii::MutexGuard lock(ctx.mutex);
    if (!lock) return;

    if (emit_stopped_before_replace &&
        ctx.state.load(std::memory_order_acquire) == State::PLAYING) {
      emit_stopped_event();
    }

    // New content clears the indicator; a failure to start it raises the
    // indicator again through the error event.
    gfx_set_error_indicator(false);

    destroy_decoder();
    free_buffer();

    ctx.webp_buf = ctx.pending.buf;
    ctx.webp_len = ctx.pending.len;
    ctx.dwell_secs = ctx.pending.dwell_secs;
    ctx.active_counter = ctx.pending.counter;
    ctx.loaded_counter = ctx.pending.counter;
    ctx.source_type = ctx.pending.source_type;
    ctx.embedded_name = ctx.pending.embedded_name;

    ctx.pending.buf = nullptr;
    ctx.pending.len = 0;
    ctx.pending.embedded_name = nullptr;
    ctx.pending.valid.store(false, std::memory_order_release);
  }

  // Stop current if playing
  if (ctx.state.load() == State::PLAYING) {
    destroy_decoder();
  }

  // Start new playback
  esp_err_t err = start_playback();
  if (err == ESP_ERR_INVALID_SIZE) {
    ESP_LOGE(TAG, "Image does not fit the %dx%d panel",
             CONFIG_HUB75_PANEL_WIDTH, CONFIG_HUB75_PANEL_HEIGHT);
    give_up_decode();
  } else if (err != ESP_OK) {
    ESP_LOGE(TAG, "start_playback failed");
    emit_error_event();
    free_buffer();
    goto_idle();
  }
}

//------------------------------------------------------------------------------
// Frame Decode and Render
//------------------------------------------------------------------------------

// Sleep for a static image that is already on the panel: until its dwell
// ends, re-checking at least once a minute.
int static_sleep_ms() {
  constexpr int MAX_SLEEP_MS = 60000;
  if (ctx.dwell_secs <= 0) return MAX_SLEEP_MS;
  const int64_t dwell_us = static_cast<int64_t>(ctx.dwell_secs) * 1000000;
  const int64_t remaining_ms =
      (dwell_us - (esp_timer_get_time() - ctx.playback_start_us)) / 1000;
  if (remaining_ms <= 0) return 0;
  if (remaining_ms > MAX_SLEEP_MS) return MAX_SLEEP_MS;
  return static_cast<int>(remaining_ms);
}

// Returns the time until the next frame in ms, or -1 on error.
int decode_and_render_frame() {
  if (!ctx.decoder.is_valid()) return -1;

  // Static images: after the first render, the DMA buffers hold the frame.
  // Skip decode and display writes; just compute the sleep duration.
  if (!ctx.decoder_info.is_animated && ctx.static_rendered) {
    return static_sleep_ms();
  }

  const uint8_t* frame = nullptr;
  if (ctx.decoder.get_next_frame(&frame) != ESP_OK) {
    // A failed decode may have left the canvas half written.
    ctx.current_frame = nullptr;
    return -1;
  }
  ctx.current_frame = frame;
  ctx.decode_error_count = 0;

  render_frame(frame, ctx.decoder_info.canvas_width,
               ctx.decoder_info.canvas_height);

  if (!ctx.decoder_info.is_animated) {
    ctx.static_rendered = true;
    settle_panel();
    return static_sleep_ms();
  }

  const int delay_ms = static_cast<int>(ctx.decoder.get_frame_delay());
  return (delay_ms > 0) ? delay_ms : 1;
}

//------------------------------------------------------------------------------
// Frame Timing
//------------------------------------------------------------------------------
// Deadlines are absolute and kept in microseconds so tick rounding never
// accumulates: at 250 Hz a 50 ms frame is not a whole number of ticks, and
// per-frame tick math ran animations up to 25% fast.

void schedule_next_frame(int delay_ms) {
  ctx.next_frame_us += static_cast<int64_t>(delay_ms) * 1000;
  // Behind schedule (slow decode): restart the schedule from now rather than
  // rushing through frames to catch up.
  const int64_t now = esp_timer_get_time();
  if (ctx.next_frame_us < now) {
    ctx.next_frame_us = now;
  }
}

// A tick-based wait can only end within one tick of a deadline, so a frame
// that close counts as due.
bool frame_due() {
  return ctx.next_frame_us - esp_timer_get_time() < TICK_US;
}

// Always at least one tick, so the task blocks every iteration even when
// decoding cannot keep up, and IDLE on this core still gets to run.
TickType_t ticks_until_next_frame() {
  const int64_t remaining_us = ctx.next_frame_us - esp_timer_get_time();
  if (remaining_us <= TICK_US) return 1;
  return static_cast<TickType_t>((remaining_us + TICK_US - 1) / TICK_US);
}

//------------------------------------------------------------------------------
// Version Info Display (boot screen)
//------------------------------------------------------------------------------

void display_version_info(const char* img_url) {
  forget_panel_content();
  display_clear();
  char version_text[32];
  snprintf(version_text, sizeof(version_text), "v%s", FIRMWARE_VERSION);

  if (img_url && strlen(img_url) > 0) {
    ESP_LOGI(TAG, "Full URL: %s", img_url);
    char host_only[64] = {0};
    char last_two[32] = {0};

    struct http_parser_url u;
    http_parser_url_init(&u);

    if (http_parser_parse_url(img_url, strlen(img_url), 0, &u) == 0) {
      if (u.field_set & (1 << UF_HOST)) {
        size_t host_len = u.field_data[UF_HOST].len;
        if (host_len >= sizeof(host_only)) host_len = sizeof(host_only) - 1;
        memcpy(host_only, img_url + u.field_data[UF_HOST].off, host_len);
        host_only[host_len] = '\0';
      }

      if (u.field_set & (1 << UF_PATH)) {
        const char* path = img_url + u.field_data[UF_PATH].off;
        size_t path_len = u.field_data[UF_PATH].len;
        const char* last_slash = nullptr;
        const char* second_last_slash = nullptr;

        for (size_t i = 0; i < path_len; i++) {
          if (path[i] == '/') {
            second_last_slash = last_slash;
            last_slash = path + i;
          }
        }

        const char* src = second_last_slash ? second_last_slash : path;
        size_t len = static_cast<size_t>((path + path_len) - src);
        if (len >= sizeof(last_two)) len = sizeof(last_two) - 1;
        memcpy(last_two, src, len);
        last_two[len] = '\0';
      }
    }

    if (strlen(host_only) > 0) {
      ESP_LOGI(TAG, "Displaying host: '%s' at y=0", host_only);
      display_text(host_only, 0, 0, 255, 255, 255, 1);
    }

    if (strlen(last_two) > 0) {
      const char* disp = last_two;
      size_t plen = strlen(last_two);
      if (plen > 11) disp = last_two + (plen - 11);
      ESP_LOGI(TAG, "Displaying path: '%s' at y=10", disp);
      display_text(disp, 0, 10, 255, 255, 255, 1);
    }
  }

  // Display 3 colored boxes RGB horizontally centered above version
  int box_x = (64 - 11) / 2;  // Center 11 pixels (3 boxes + 2 gaps)
  display_fill_rect(box_x, 20, 3, 3, 255, 0, 0);      // Red box
  display_fill_rect(box_x + 4, 20, 3, 3, 0, 255, 0);  // Green box
  display_fill_rect(box_x + 8, 20, 3, 3, 0, 0, 255);  // Blue box

  // Display version at the bottom, centered
  int text_width = static_cast<int>(strlen(version_text)) * 6;
  int x = (64 - text_width) / 2;
  display_text(version_text, x, 24, 255, 255, 255, 1);
  display_flip();
  vTaskDelay(pdMS_TO_TICKS(2000));
}

//------------------------------------------------------------------------------
// Player Task
//------------------------------------------------------------------------------

void player_task(void*) {
  ESP_LOGD(TAG, "Player task started on core %d", xPortGetCoreID());

  while (true) {
    // --- Truly an useless log, but can be helpful for verifying task is running and not stuck in a dead loop ---
    //UBaseType_t stack_free = uxTaskGetStackHighWaterMark(NULL);
    //ESP_LOGI(TAG, "Stack remaining: %u bytes", stack_free);

    if (ctx.foreign_draw.exchange(false)) {
      forget_panel_content();
    }

    State state = ctx.state.load();

    // --- IDLE: block until command ---
    if (state == State::IDLE) {
      // Drain any stale interrupt flag — irrelevant once idle.
      ctx.interrupt_request.store(InterruptRequest::NONE,
                                  std::memory_order_relaxed);

      // While stopped, pending content waits for gfx_start(), and so does the
      // indicator: the panel belongs to whoever stopped the player. Either way
      // the task blocks below instead of polling.
      const bool paused = ctx.paused.load();
      if (!paused) {
        // Content queued while PLAYING is consumed without waiting for a new
        // notification.
        if (ctx.pending.valid.load(std::memory_order_acquire)) {
          handle_pending_command();
          continue;
        }
        refresh_indicator_idle();
      }

      // Use a periodic wake-up instead of infinite block so we can detect
      // stale pending commands that arrived without a notification.
      constexpr TickType_t IDLE_POLL_TICKS = pdMS_TO_TICKS(30000);
      uint32_t got = ulTaskNotifyTake(pdTRUE, IDLE_POLL_TICKS);
      if (!got && !ctx.paused.load() &&
          ctx.pending.valid.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Idle wake: stale pending command detected, consuming");
      }
      continue;
    }

    // --- PLAYING ---

    // Handle pause
    if (ctx.paused.load()) {
      goto_idle();
      emit_stopped_event();
      ESP_LOGI(TAG, "Paused");
      continue;
    }

    // Check if dwell time expired (applies to animated AND static)
    if (check_dwell_expired()) {
      ESP_LOGI(TAG, "Dwell expired (counter=%d)", ctx.active_counter);
      emit_stopped_event();
      goto_idle();
      // If a new image is already queued, start it now
      if (ctx.pending.valid.load(std::memory_order_acquire)) {
        handle_pending_command();
      }
      continue;
    }

    // Decode and render one frame. A wake-up before the frame is due (a
    // queued image, an indicator change) must not advance the animation.
    if (frame_due()) {
      int delay_ms = decode_and_render_frame();
      if (delay_ms < 0) {
        handle_decode_error();
        continue;
      }
      schedule_next_frame(delay_ms);
    }
    refresh_indicator_playing();

    // Wait for the next frame OR a notification
    uint32_t notified = ulTaskNotifyTake(pdTRUE, ticks_until_next_frame());

    if (notified) {
      InterruptRequest req = ctx.interrupt_request.exchange(
          InterruptRequest::NONE, std::memory_order_acq_rel);
      if (req == InterruptRequest::STOP_ONLY) {
        if (ctx.state.load(std::memory_order_acquire) == State::PLAYING) {
          goto_idle();
          emit_stopped_event();
          ESP_LOGI(TAG, "Stopped by interrupt");
        }
        continue;
      }
      if (req == InterruptRequest::PREEMPT_PENDING) {
        if (ctx.pending.valid.load(std::memory_order_acquire)) {
          handle_pending_command(true);
        } else if (ctx.state.load(std::memory_order_acquire) ==
                   State::PLAYING) {
          goto_idle();
          emit_stopped_event();
          ESP_LOGI(TAG, "Stopped by interrupt");
        }
        continue;
      }
      // Any other notification is a plain wake-up: a queued image waits for
      // the dwell to expire, and pause / indicator changes are picked up at
      // the top of the loop.
    }
  }
}

}  // namespace

//------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------

int gfx_initialize(const char* img_url) {
  if (ctx.initialized) {
    ESP_LOGE(TAG, "Already initialized");
    return 1;
  }

  ESP_LOGI(TAG, "Largest heap block: %d",
           heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

  // Boot animation — use static asset directly (unless skipped)
  if (!config_get().skip_boot_animation) {
    auto* boot = asset_boot();
    ctx.webp_buf = const_cast<void*>(static_cast<const void*>(boot->data));
    ctx.webp_len = boot->size;
    ctx.dwell_secs = 0;
    ctx.active_counter = 0;
    ctx.source_type = GFX_SOURCE_EMBEDDED;
    ctx.embedded_name = "boot";
  }

  ctx.mutex = xSemaphoreCreateMutex();
  if (!ctx.mutex) {
    ESP_LOGE(TAG, "Could not create mutex");
    return 1;
  }

  ctx.event_group = xEventGroupCreate();
  if (!ctx.event_group) {
    ESP_LOGE(TAG, "Could not create event group");
    return 1;
  }

  ctx.initialized = true;

  if (display_initialize()) return 1;

  auto cfg = config_get();

  if (cfg.skip_boot_animation) {
    display_clear();
  }

  if (!cfg.skip_display_version) {
    display_version_info(img_url);
  }

  // Pre-initialize decoder so task starts in PLAYING state
  if (create_decoder() == ESP_OK) {
    ctx.playback_start_us = esp_timer_get_time();
    ctx.next_frame_us = ctx.playback_start_us;
    ctx.state.store(State::PLAYING);
  } else {
    // Nothing to play (boot animation skipped): idle from the start, or
    // gfx_wait_idle() would wait for a transition that never comes.
    xEventGroupSetBits(ctx.event_group, BIT_IDLE);
  }

  BaseType_t ret = xTaskCreatePinnedToCore(
      player_task, "webp_player", TASK_STACK_SIZE, nullptr,
      TASK_PRIORITY, &ctx.task, TASK_CORE);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Could not create player task");
    return 1;
  }

  ESP_LOGI(TAG, "WebP player initialized (task core=%d, stack=%u)", TASK_CORE,
           TASK_STACK_SIZE);
  return 0;
}

int gfx_update(void* webp, size_t len, int32_t dwell_secs) {
  raii::MutexGuard lock(ctx.mutex);
  if (!lock) {
    ESP_LOGE(TAG, "Could not take mutex");
    return -1;
  }

  // Free any unconsumed pending buffer (frame-dropping).
  // This also cleans up buffers left behind by an interrupt.
  if (ctx.pending.buf && !is_static_asset(ctx.pending.buf)) {
    ESP_LOGW(TAG, "Dropping queued image (counter %d)", ctx.counter);
    free(ctx.pending.buf);
  }

  ctx.counter++;
  int counter = ctx.counter;

  ctx.pending.buf = webp;
  ctx.pending.len = len;
  ctx.pending.dwell_secs = dwell_secs;
  ctx.pending.counter = counter;
  ctx.pending.source_type = GFX_SOURCE_RAM;
  ctx.pending.embedded_name = nullptr;
  ctx.pending.valid.store(true, std::memory_order_release);

  ESP_LOGI(TAG, "Queued image counter=%d size=%zu dwell=%ld",
           counter, len, static_cast<long>(dwell_secs));

  lock.release();

  // Always notify after enqueue to avoid races where state flips to IDLE
  // between queueing and the task's next wait. This does not force preemption:
  // PLAYING state still keeps queued images until dwell expires unless an
  // explicit preempt request arrives.
  if (ctx.task) {
    xTaskNotifyGive(ctx.task);
  }

  send_queued_notification(counter);
  return counter;
}

int gfx_get_loaded_counter(void) {
  if (!ctx.initialized) return -1;
  raii::MutexGuard lock(ctx.mutex);
  if (!lock) return -1;
  return ctx.loaded_counter;
}

int gfx_play_embedded(const char* name, bool immediate) {
  const embedded_asset_t* asset = asset_find(name);
  if (!asset) {
    ESP_LOGE(TAG, "Unknown embedded sprite: %s", name);
    return 1;
  }

  if (immediate) {
    gfx_interrupt();
  }

  raii::MutexGuard lock(ctx.mutex);
  if (!lock) {
    ESP_LOGE(TAG, "Could not take mutex");
    return 1;
  }

  // Free any unconsumed pending buffer.
  if (ctx.pending.buf && !is_static_asset(ctx.pending.buf)) {
    free(ctx.pending.buf);
  }

  ctx.counter++;
  int counter = ctx.counter;

  ctx.pending.buf =
      const_cast<void*>(static_cast<const void*>(asset->data));
  ctx.pending.len = asset->size;
  ctx.pending.dwell_secs = 0;  // Embedded sprites loop forever
  ctx.pending.counter = counter;
  ctx.pending.source_type = GFX_SOURCE_EMBEDDED;
  ctx.pending.embedded_name = asset->name;
  ctx.pending.valid.store(true, std::memory_order_release);

  ESP_LOGI(TAG, "Queued embedded sprite '%s' counter=%d", name, counter);

  lock.release();
  xTaskNotifyGive(ctx.task);
  return 0;
}

int gfx_display_asset(const char* asset_type) {
  return gfx_play_embedded(asset_type, true);
}

void gfx_display_text(const char* text, int x, int y, uint8_t r, uint8_t g,
                      uint8_t b, int scale) {
  ctx.foreign_draw.store(true);
  display_text(text, x, y, r, g, b, scale);
}

void gfx_stop(void) {
  ctx.paused.store(true);
  if (ctx.task) xTaskNotifyGive(ctx.task);
  ESP_LOGI(TAG, "Paused");
}

void gfx_start(void) {
  // Other code (OTA screens, quiet hours) may have drawn while paused.
  ctx.foreign_draw.store(true);
  ctx.paused.store(false);
  if (ctx.task) xTaskNotifyGive(ctx.task);
  ESP_LOGI(TAG, "Resumed");
}

void gfx_shutdown(void) { display_shutdown(); }

void gfx_safe_restart(void) {
  gfx_stop();
  gfx_wait_idle();
  gfx_shutdown();
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
}

void gfx_interrupt(void) {
  ctx.interrupt_request.store(InterruptRequest::STOP_ONLY,
                              std::memory_order_release);
  if (ctx.task) xTaskNotifyGive(ctx.task);
}

void gfx_preempt(void) {
  ctx.interrupt_request.store(InterruptRequest::PREEMPT_PENDING,
                              std::memory_order_release);
  if (ctx.task) xTaskNotifyGive(ctx.task);
}

bool gfx_wait_idle(void) {
  if (!ctx.event_group) return true;
  if (xTaskGetCurrentTaskHandle() == ctx.task) return false;
  const EventBits_t bits =
      xEventGroupWaitBits(ctx.event_group, BIT_IDLE, pdFALSE, pdTRUE,
                          pdMS_TO_TICKS(WAIT_IDLE_TIMEOUT_MS));
  if (bits & BIT_IDLE) return true;
  ESP_LOGW(TAG, "Player not idle after %lu ms",
           static_cast<unsigned long>(WAIT_IDLE_TIMEOUT_MS));
  return false;
}

void gfx_set_error_indicator(bool on) {
  if (ctx.indicator_wanted.exchange(on) == on) return;
  // The player applies the change itself. It is only woken from other tasks:
  // a self-notification would cut its next frame wait short for nothing.
  if (ctx.task && xTaskGetCurrentTaskHandle() != ctx.task) {
    xTaskNotifyGive(ctx.task);
  }
}

bool gfx_is_animating(void) {
  if (!ctx.event_group) return false;
  return (xEventGroupGetBits(ctx.event_group) & BIT_IDLE) == 0;
}
