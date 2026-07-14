// Sockets — Event-driven WebSocket client with FSM and timer-based reconnect.
// Modeled on matrx-fw's sockets module, adapted for our JSON protocol.
//
// Concurrency model:
//   - The esp_timer task owns the client handle: only `reconnect_timer_callback`
//     destroys or creates the client.
//   - Other tasks (WS event handler, event_bus, callers) take `client_mutex`
//     when they need to read or send. Senders never destroy.
//   - To avoid holding the mutex during the blocking
//     `esp_websocket_client_destroy` (which waits for the WS task to exit),
//     the timer swaps `ctx.client` to nullptr under the lock, then performs
//     the destroy outside the lock.

#include "sockets.h"
#include "handlers.h"
#include "messages.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "app_state.h"
#include "display.h"
#include "event_bus.h"
#include "nvs_settings.h"
#include "outbox_ring.h"
#include "raii_utils.hpp"
#include "scheduler.h"
#include "webp_player.h"
#include "wifi.h"

namespace {

const char* TAG = "sockets";

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

constexpr int64_t RECONNECT_DELAY_US = 5000 * 1000;  // 5 seconds
constexpr int64_t INITIAL_CONNECT_DELAY_US = 500 * 1000;  // 0.5 seconds
constexpr int64_t GOT_IP_CONNECT_DELAY_US = 1500 * 1000;  // 1.5 seconds

// Failure escalation ladder (mirrors the reference cloudlink policy).
// A socket failure first schedules a plain reconnect. After this many
// consecutive failed reconnect cycles we tear down the WiFi link so the
// wifi module re-scans and re-associates (its own jittered backoff and
// multi-network RSSI failover then take over). After this many full
// WiFi-reset cycles still without a live session, the device restarts as a
// last resort. Counters reset to zero on a successful connection.
constexpr int MAX_SOCK_FAILURES_BEFORE_WIFI_RESET = 5;
constexpr int MAX_WIFI_RESETS_BEFORE_RESTART = 3;

// Per-message send budget when draining the outbox. Kept short so a flush
// triggered from the WS event task cannot stall it for long.
constexpr TickType_t OUTBOX_FLUSH_TIMEOUT = pdMS_TO_TICKS(2000);

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

enum class State : uint8_t {
  Disconnected,  // No connection attempt (waiting for network or URL)
  Ready,         // Network up, ready to connect
  Connected,     // WebSocket connected
};

struct SocketContext {
  esp_websocket_client_handle_t client = nullptr;
  std::atomic<State> state{State::Disconnected};
  char* url = nullptr;
  char* auth_header = nullptr;
  bool sent_client_info = false;
};

SocketContext ctx;
SemaphoreHandle_t client_mutex = nullptr;

int sock_failure_count = 0;
int wifi_disconnect_count = 0;

// Bounded FIFO outbox: messages produced while the socket is down are copied
// onto the ring and flushed in order once the link is ready. The ring itself
// is a pure structure (outbox_ring.h, host-tested); it is guarded by its own
// mutex here (never nested with client_mutex: a flush pops under
// outbox_mutex, releases it, then sends under client_mutex).
outbox_ring_t outbox;
SemaphoreHandle_t outbox_mutex = nullptr;

// Timers
esp_timer_handle_t reconnect_timer = nullptr;

// Forward declarations
esp_err_t start_client_locked();
void schedule_reconnect();
void outbox_flush();

// ---------------------------------------------------------------------------
// Reconnection (timer-based, no blocking loop)
// ---------------------------------------------------------------------------

// The esp_timer task is the sole owner of destroy/create. WS-task event
// handlers and other observers must NEVER call esp_websocket_client_destroy:
// per ESP-IDF docs that risks deadlock (destroy waits for the WS task to
// exit). They schedule a reconnect instead.
void reconnect_timer_callback(void*) {
  ESP_LOGI(TAG, "Reconnect timer fired");

  // Swap the handle out under the lock, then destroy without the lock so
  // pending senders block briefly (waiting for the swap) rather than for
  // the full destroy duration.
  esp_websocket_client_handle_t to_destroy = nullptr;
  {
    raii::MutexGuard lock(client_mutex);
    if (lock) {
      to_destroy = ctx.client;
      ctx.client = nullptr;
    }
  }
  if (to_destroy) {
    esp_websocket_client_stop(to_destroy);
    esp_websocket_client_destroy(to_destroy);
  }

  if (ctx.state.load() == State::Ready || ctx.state.load() == State::Disconnected) {
    if (wifi_is_connected()) {
      raii::MutexGuard lock(client_mutex);
      if (lock) {
        ctx.state.store(State::Ready);
        start_client_locked();
      }
    } else {
      ctx.state.store(State::Disconnected);
      ESP_LOGW(TAG, "Network not available, will retry when IP acquired");
    }
  }
}

void schedule_reconnect() {
  if (reconnect_timer) {
    esp_timer_stop(reconnect_timer);  // Stop any pending reconnect
    esp_timer_start_once(reconnect_timer, RECONNECT_DELAY_US);
    ESP_LOGI(TAG, "Scheduled reconnect in %lld ms",
             RECONNECT_DELAY_US / 1000);
  }
}

// ---------------------------------------------------------------------------
// Bounded outbox
// ---------------------------------------------------------------------------

// Send a text frame on the live client. Returns true only if the whole frame
// was handed to the socket. Takes client_mutex; must NOT be called while
// holding outbox_mutex.
bool ws_send_now(const char* data, size_t len, TickType_t timeout) {
  raii::MutexGuard lock(client_mutex, timeout);
  if (!lock || !ctx.client) return false;
  if (!esp_websocket_client_is_connected(ctx.client)) return false;
  int sent = esp_websocket_client_send_text(ctx.client, data,
                                             static_cast<int>(len), timeout);
  return sent >= 0;
}

bool ws_is_connected_now() {
  raii::MutexGuard lock(client_mutex);
  return lock && ctx.client &&
         esp_websocket_client_is_connected(ctx.client);
}

bool outbox_is_empty() {
  raii::MutexGuard lock(outbox_mutex);
  return !lock || outbox_ring_count(&outbox) == 0;
}

// Take ownership of `copy` (heap-allocated, `len` bytes) and append it. On a
// full ring the oldest entry is freed and dropped to make room. Frees `copy`
// itself if the mutex cannot be taken so no message ever leaks.
void outbox_enqueue(char* copy, size_t len) {
  raii::MutexGuard lock(outbox_mutex);
  if (!lock) {
    free(copy);
    return;
  }
  if (outbox_ring_push(&outbox, copy, len)) {
    ESP_LOGW(TAG, "Outbox full, dropping oldest queued message");
  }
}

// Pop the oldest entry into `out` (caller then owns out->data). Returns false
// when the ring is empty.
bool outbox_dequeue(outbox_ring_slot_t* out) {
  raii::MutexGuard lock(outbox_mutex);
  return lock && outbox_ring_pop(&outbox, out);
}

// Drain queued messages in FIFO order while the socket keeps accepting them.
// Returns early (leaving the queue intact) when the link is down, so a send
// attempted while disconnected does not silently discard the backlog. If the
// link drops mid-drain the in-flight message is lost and the rest stay queued
// for the next connection.
void outbox_flush() {
  if (!ws_is_connected_now()) return;
  outbox_ring_slot_t slot;
  while (outbox_dequeue(&slot)) {
    bool ok = slot.data && ws_send_now(slot.data, slot.len,
                                        OUTBOX_FLUSH_TIMEOUT);
    free(slot.data);
    if (!ok) break;
  }
}

// Free every queued message. Used on deinit.
void outbox_drain_free() {
  outbox_ring_slot_t slot;
  while (outbox_dequeue(&slot)) free(slot.data);
}

// ---------------------------------------------------------------------------
// Failure escalation (called from ws_event_handler on WS task)
// ---------------------------------------------------------------------------

// On WS-task context: bump failure counters. Never destroys the client
// directly — that's the timer's job. Returns true if a hard wifi reset was
// requested (caller may then call esp_wifi_disconnect, which is safe from
// any task).
bool escalate_socket_failure() {
  sock_failure_count++;
  ESP_LOGW(TAG, "Socket failure %d/%d (wifi resets: %d/%d)",
           sock_failure_count, MAX_SOCK_FAILURES_BEFORE_WIFI_RESET,
           wifi_disconnect_count, MAX_WIFI_RESETS_BEFORE_RESTART);

  if (sock_failure_count < MAX_SOCK_FAILURES_BEFORE_WIFI_RESET) {
    schedule_reconnect();
    return false;
  }

  wifi_disconnect_count++;
  sock_failure_count = 0;

  if (wifi_disconnect_count >= MAX_WIFI_RESETS_BEFORE_RESTART) {
    ESP_LOGE(TAG, "Too many WiFi resets (%d), restarting",
             wifi_disconnect_count);
    esp_restart();
  }

  ESP_LOGW(TAG, "Too many socket failures, disconnecting WiFi (%d/%d)",
           wifi_disconnect_count, MAX_WIFI_RESETS_BEFORE_RESTART);
  return true;
}

// ---------------------------------------------------------------------------
// WebSocket event handler
// ---------------------------------------------------------------------------

void ws_event_handler(void*, esp_event_base_t, int32_t event_id,
                      void* event_data) {
  auto* data = static_cast<esp_websocket_event_data_t*>(event_data);

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "Connected");
      ctx.state.store(State::Connected);
      sock_failure_count = 0;
      wifi_disconnect_count = 0;
      ctx.sent_client_info = false;
      msg_send_client_info();
      ctx.sent_client_info = true;
      // Drain anything queued while the link was down, in order.
      outbox_flush();
      event_bus_emit_simple(TRONBYT_EVENT_WS_CONNECTED);
      app_state_set_connectivity(CONNECTIVITY_SERVER_ONLINE);
      scheduler_on_ws_connect();
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "Disconnected (state=%d wifi=%d)",
               static_cast<int>(ctx.state.load()), wifi_is_connected());
      draw_error_indicator_pixel();
      if (ctx.state.load() != State::Ready) {
        bool wifi_up = wifi_is_connected();
        ctx.state.store(wifi_up ? State::Ready : State::Disconnected);
        event_bus_emit_simple(TRONBYT_EVENT_WS_DISCONNECTED);
        if (wifi_up) {
          app_state_set_connectivity(CONNECTIVITY_CONNECTED);
        }
        scheduler_on_ws_disconnect();

        if (escalate_socket_failure()) {
          esp_wifi_disconnect();  // safe from event handlers
        }
      }
      break;

    case WEBSOCKET_EVENT_DATA:
      if (data->op_code == 1 && data->data_len > 0) {
        handle_text_message(data);
      } else if (data->op_code == 2 || data->op_code == 0) {
        handle_binary_message(data);
      }
      break;

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(TAG, "WebSocket error (state=%d wifi=%d)",
               static_cast<int>(ctx.state.load()), wifi_is_connected());
      draw_error_indicator_pixel();
      if (ctx.state.load() != State::Ready) {
        bool wifi_up = wifi_is_connected();
        ctx.state.store(wifi_up ? State::Ready : State::Disconnected);
        scheduler_on_ws_disconnect();

        if (escalate_socket_failure()) {
          esp_wifi_disconnect();
        }
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// Client lifecycle
// ---------------------------------------------------------------------------

// Caller must hold client_mutex. Assumes ctx.client is null (the timer
// nulls it before calling).
esp_err_t start_client_locked() {
  if (ctx.client) {
    // Should not happen with the new ownership model. Log and bail rather
    // than calling destroy here (we are on the timer task and destroying
    // is fine, but a non-null client at this point indicates a bug).
    ESP_LOGE(TAG, "start_client_locked called with non-null client");
    return ESP_ERR_INVALID_STATE;
  }

  if (!ctx.url) {
    ESP_LOGE(TAG, "No URL configured");
    return ESP_ERR_INVALID_STATE;
  }

  esp_websocket_client_config_t ws_cfg = {};
  ws_cfg.uri = ctx.url;
  ws_cfg.buffer_size = 4096;
  // Message handlers and JSON parsing run in this task's event callbacks,
  // which overflows the component's 4096 default.
  ws_cfg.task_stack = CONFIG_WS_TASK_STACK_SIZE;
  ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
  ws_cfg.reconnect_timeout_ms = 10000;
  ws_cfg.network_timeout_ms = 10000;
  ws_cfg.ping_interval_sec = 30;
  ws_cfg.pingpong_timeout_sec = 60;

  ws_cfg.enable_close_reconnect = true;

  // Set Authorization header if API key is configured
  if (ctx.auth_header) {
    free(ctx.auth_header);
    ctx.auth_header = nullptr;
  }
  auto cfg = config_get();
  if (cfg.api_key[0] != '\0') {
    // Format: "Authorization: Bearer <key>\r\n"
    size_t hdr_len = strlen("Authorization: Bearer \r\n") +
                     strlen(cfg.api_key) + 1;
    ctx.auth_header = static_cast<char*>(malloc(hdr_len));
    if (ctx.auth_header) {
      snprintf(ctx.auth_header, hdr_len, "Authorization: Bearer %s\r\n",
               cfg.api_key);
      ws_cfg.headers = ctx.auth_header;
      ESP_LOGI(TAG, "Auth header set (%d chars)", (int)strlen(cfg.api_key));
    }
  } else {
    ESP_LOGW(TAG, "No API key configured, connecting without auth");
  }

  ctx.client = esp_websocket_client_init(&ws_cfg);
  if (!ctx.client) {
    ESP_LOGE(TAG, "Failed to init WS client");
    return ESP_FAIL;
  }

  esp_websocket_register_events(ctx.client, WEBSOCKET_EVENT_ANY,
                                ws_event_handler, nullptr);

  esp_err_t err = esp_websocket_client_start(ctx.client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start WS client: %s", esp_err_to_name(err));
    esp_websocket_client_destroy(ctx.client);
    ctx.client = nullptr;
    schedule_reconnect();
    return err;
  }

  ESP_LOGI(TAG, "Client started, connecting to %s", ctx.url);
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// WiFi event handlers (via event bus)
// ---------------------------------------------------------------------------

void on_wifi_event(const tronbyt_event_t* event, void*) {
  if (event->type == TRONBYT_EVENT_WIFI_DISCONNECTED) {
    if (ctx.state.load() != State::Disconnected) {
      ESP_LOGW(TAG, "WiFi disconnected");
      ctx.state.store(State::Disconnected);
    }
  } else if (event->type == TRONBYT_EVENT_WIFI_CONNECTED) {
    ESP_LOGI(TAG, "Got IP, state=%d", static_cast<int>(ctx.state.load()));
    if (ctx.state.load() == State::Disconnected) {
      ctx.state.store(State::Ready);
      // Avoid first TLS handshake during peak startup activity.
      if (reconnect_timer) {
        esp_timer_stop(reconnect_timer);
        esp_timer_start_once(reconnect_timer, GOT_IP_CONNECT_DELAY_US);
      }
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void sockets_init(const char* url) {
  client_mutex = xSemaphoreCreateMutex();
  if (!client_mutex) {
    ESP_LOGE(TAG, "Failed to create client mutex");
    return;
  }
  outbox_mutex = xSemaphoreCreateMutex();
  if (!outbox_mutex) {
    ESP_LOGE(TAG, "Failed to create outbox mutex");
    vSemaphoreDelete(client_mutex);
    client_mutex = nullptr;
    return;
  }
  outbox_ring_init(&outbox);

  handlers_init();
  ctx.url = strdup(url);

  msg_init();

  // Create reconnect timer
  esp_timer_create_args_t reconnect_args = {};
  reconnect_args.callback = reconnect_timer_callback;
  reconnect_args.name = "sock_reconn";
  reconnect_args.skip_unhandled_events = true;
  esp_timer_create(&reconnect_args, &reconnect_timer);

  // Subscribe to WiFi events via event bus for reconnection
  event_bus_subscribe(TRONBYT_EVENT_WIFI_CONNECTED, on_wifi_event, nullptr);
  event_bus_subscribe(TRONBYT_EVENT_WIFI_DISCONNECTED, on_wifi_event, nullptr);

  // If already connected, start immediately
  if (wifi_is_connected()) {
    ctx.state.store(State::Ready);
    // Defer first connect slightly to let boot-time tasks (including app_main)
    // release stack/heap before websocket task allocation.
    esp_timer_start_once(reconnect_timer, INITIAL_CONNECT_DELAY_US);
    ESP_LOGI(TAG, "Network ready, deferring initial WS connect by %lld ms",
             INITIAL_CONNECT_DELAY_US / 1000);
  } else {
    ctx.state.store(State::Disconnected);
    ESP_LOGI(TAG, "Waiting for network...");
  }
}

void sockets_deinit() {
  // Stop timers
  if (reconnect_timer) {
    esp_timer_stop(reconnect_timer);
    esp_timer_delete(reconnect_timer);
    reconnect_timer = nullptr;
  }
  // Unsubscribe from event bus
  event_bus_unsubscribe(on_wifi_event);

  // Destroy client (swap then destroy without holding the lock)
  esp_websocket_client_handle_t to_destroy = nullptr;
  {
    raii::MutexGuard lock(client_mutex);
    if (lock) {
      to_destroy = ctx.client;
      ctx.client = nullptr;
    }
  }
  if (to_destroy) {
    esp_websocket_client_stop(to_destroy);
    esp_websocket_client_destroy(to_destroy);
  }

  // Free URL and auth header
  if (ctx.url) {
    free(ctx.url);
    ctx.url = nullptr;
  }
  if (ctx.auth_header) {
    free(ctx.auth_header);
    ctx.auth_header = nullptr;
  }

  ctx.state.store(State::Disconnected);

  // Free any messages still queued, then drop the outbox mutex.
  outbox_drain_free();
  if (outbox_mutex) {
    vSemaphoreDelete(outbox_mutex);
    outbox_mutex = nullptr;
  }

  handlers_deinit();

  if (client_mutex) {
    vSemaphoreDelete(client_mutex);
    client_mutex = nullptr;
  }
}

bool sockets_is_connected() {
  raii::MutexGuard lock(client_mutex);
  if (!lock || !ctx.client) return false;
  return esp_websocket_client_is_connected(ctx.client);
}

int sockets_send_text(const char* data, size_t len, TickType_t timeout) {
  if (!data || len == 0) return -1;

  // Fast path: nothing queued and the link is up, so send inline. This keeps
  // latency low and honours the caller's timeout. The send is bounded by
  // `timeout`; the reconnect timer blocks on the mutex for at most this long
  // before swapping the handle. Acceptable.
  if (outbox_is_empty() && ws_send_now(data, len, timeout)) {
    return static_cast<int>(len);
  }

  // Link down, mutex busy, or messages already queued ahead of this one:
  // copy onto the outbox so it is delivered in order once the link is ready.
  // The message is accepted (returns len) even though delivery is deferred.
  char* copy = static_cast<char*>(malloc(len));
  if (!copy) {
    ESP_LOGE(TAG, "Failed to alloc %u bytes for outbox", (unsigned)len);
    return -1;
  }
  memcpy(copy, data, len);
  outbox_enqueue(copy, len);
  outbox_flush();
  return static_cast<int>(len);
}
