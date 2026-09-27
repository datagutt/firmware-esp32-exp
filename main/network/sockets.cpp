// Sockets: event-driven WebSocket client with app-owned reconnects.
// Modeled on matrx-fw's sockets module, adapted for our JSON protocol.
//
// Concurrency model:
//   - A dedicated lifecycle task owns the client handle. It alone creates and
//     destroys clients, drains the outbox and applies the retry and failure
//     escalation policy; the reconnect timer and the event handlers only
//     notify it. Destroying a client blocks until its WS task exits, which
//     can take the full network timeout while that task sits in
//     esp_transport_connect, so this must not run on the shared esp_timer
//     task.
//   - The component's auto-reconnect is disabled: every attempt ends with the
//     WS task exiting (WEBSOCKET_EVENT_FINISH), so each failed attempt is seen
//     and counted here and retries have a single owner.
//   - The component dispatches events with its internal client lock held, and
//     sending needs that same lock. Nothing on the WS task may therefore wait
//     for client_mutex: event handlers only update atomics and notify, and
//     sends made from the WS task (e.g. the "queued" notification raised
//     while handling an image) are deferred to the outbox.
//   - Senders take client_mutex to use the handle. The lifecycle task swaps
//     ctx.client to nullptr under the lock and destroys the old client
//     outside it, so senders wait for the swap, not for the destroy.

#include "sockets.h"
#include "handlers.h"
#include "messages.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <ping/ping_sock.h>

#include "app_state.h"
#include "display.h"
#include "event_bus.h"
#include "nvs_settings.h"
#include "outbox_ring.h"
#include "raii_utils.hpp"
#include "retry_backoff.h"
#include "scheduler.h"
#include "webp_player.h"
#include "wifi.h"

namespace {

const char* TAG = "sockets";

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

constexpr uint32_t RECONNECT_BASE_MS = 5000;
constexpr uint32_t RECONNECT_MAX_MS = 60000;
constexpr int64_t INITIAL_CONNECT_DELAY_US = 500 * 1000;  // 0.5 seconds
constexpr int64_t GOT_IP_CONNECT_DELAY_US = 1500 * 1000;  // 1.5 seconds

// Failure escalation ladder. Every failed attempt schedules a retry after a
// capped, jittered exponential backoff. After this many consecutive attempts
// that never reached the server we tear down the WiFi link so the wifi module
// re-scans and re-associates (its own backoff and multi-network RSSI failover
// then take over). Once this many WiFi resets have not helped, each further
// round of failures probes the local network instead: the device restarts
// only if the network itself looks dead. A reachable network with an
// unreachable server is a server problem that a restart cannot fix, so the
// device keeps backing off indefinitely. Any proof that the server is
// reachable (a live session, or an HTTP or TLS answer to the handshake such as
// a 401 or a certificate mismatch) resets the counters.
constexpr int MAX_SOCK_FAILURES_BEFORE_WIFI_RESET = 5;
constexpr int MAX_WIFI_RESETS = 3;

// Gateway probe: a few ICMP echoes with a short timeout. The session runs in
// its own short-lived "ping" task (ESP_TASK_PING_STACK, internal RAM), so the
// lifecycle task only blocks on a semaphore.
constexpr uint32_t GATEWAY_PING_COUNT = 3;
constexpr uint32_t GATEWAY_PING_INTERVAL_MS = 200;
constexpr uint32_t GATEWAY_PING_TIMEOUT_MS = 1000;

// Per-message send budget when draining the outbox. Bounds how long a stalled
// socket can hold up the lifecycle task.
constexpr TickType_t OUTBOX_SEND_TIMEOUT = pdMS_TO_TICKS(2000);

// The lifecycle task creates and destroys clients and sends queued messages
// through TLS. xTaskCreate places the stack in internal RAM.
constexpr uint32_t LIFECYCLE_STACK_SIZE = 4096;
constexpr UBaseType_t LIFECYCLE_PRIORITY = 5;

// Lifecycle task notification bits.
constexpr uint32_t NOTIFY_CONNECT = 1u << 0;     // replace the client
constexpr uint32_t NOTIFY_SESSION_UP = 1u << 1;  // handshake completed
constexpr uint32_t NOTIFY_FINISHED = 1u << 2;    // the client's WS task exited
constexpr uint32_t NOTIFY_FLUSH = 1u << 3;       // the outbox has messages
constexpr uint32_t NOTIFY_SHUTDOWN = 1u << 4;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct SocketContext {
  esp_websocket_client_handle_t client = nullptr;  // guarded by client_mutex
  char* url = nullptr;
  char* auth_header = nullptr;
  // True from WEBSOCKET_EVENT_CONNECTED until the session goes down.
  std::atomic<bool> link_up{false};
  // Set when the current attempt got any answer from the server.
  std::atomic<bool> server_answered{false};
  // The running client's WS task, so sends made from it can be deferred.
  std::atomic<TaskHandle_t> ws_task{nullptr};
  // Set while there is no IP; the next got-IP event then starts a connect.
  // Later got-IP events (a global IPv6 address after IPv4) are ignored.
  std::atomic<bool> awaiting_network{true};
};

SocketContext ctx;
SemaphoreHandle_t client_mutex = nullptr;

TaskHandle_t lifecycle_task = nullptr;
SemaphoreHandle_t shutdown_done = nullptr;
// Given by the ping task when a gateway probe ends. Never deleted, because a
// probe that overruns its wait may still signal it later.
SemaphoreHandle_t gateway_probe_done = nullptr;
esp_timer_handle_t reconnect_timer = nullptr;

// Owned by the lifecycle task.
int retry_attempt = 0;
int sock_failure_count = 0;
int wifi_disconnect_count = 0;

// Owned by the WS task: opcode of the message being received, so that
// continuation frames (opcode 0) reach the handler of the frame that started
// the message.
uint8_t rx_message_opcode = 0;

// Bounded FIFO outbox: messages produced while the socket is down (or from
// the WS task) are copied onto the ring and flushed in order by the lifecycle
// task once the link is ready. The ring itself is a pure structure
// (outbox_ring.h, host-tested); it is guarded by its own mutex here, never
// nested with client_mutex.
outbox_ring_t outbox;
SemaphoreHandle_t outbox_mutex = nullptr;
// A message popped for sending but not yet settled. Keeps the inline fast
// path from overtaking it.
bool outbox_send_inflight = false;

void notify_lifecycle(uint32_t bits) {
  if (lifecycle_task) xTaskNotify(lifecycle_task, bits, eSetBits);
}

void schedule_connect(int64_t delay_us) {
  if (!reconnect_timer) return;
  esp_timer_stop(reconnect_timer);
  esp_timer_start_once(reconnect_timer, delay_us);
}

void reconnect_timer_callback(void*) { notify_lifecycle(NOTIFY_CONNECT); }

// ---------------------------------------------------------------------------
// Sending and the bounded outbox
// ---------------------------------------------------------------------------

// Send a text frame on the live client. Returns true only if the whole frame
// was handed to the socket. Takes client_mutex; must NOT be called while
// holding outbox_mutex or from the WS task.
bool ws_send_now(const char* data, size_t len, TickType_t timeout) {
  raii::MutexGuard lock(client_mutex, timeout);
  if (!lock || !ctx.client) return false;
  if (!esp_websocket_client_is_connected(ctx.client)) return false;
  int sent = esp_websocket_client_send_text(ctx.client, data,
                                             static_cast<int>(len), timeout);
  return sent == static_cast<int>(len);
}

bool outbox_is_idle() {
  raii::MutexGuard lock(outbox_mutex);
  return !lock || (outbox_ring_count(&outbox) == 0 && !outbox_send_inflight);
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

// Pop the oldest entry into `out` (caller then owns out->data and must hand
// it back through outbox_settle). Returns false when the ring is empty.
bool outbox_dequeue(outbox_ring_slot_t* out) {
  raii::MutexGuard lock(outbox_mutex);
  if (!lock || !outbox_ring_pop(&outbox, out)) return false;
  outbox_send_inflight = true;
  return true;
}

// Finish a dequeued message: free it when sent, otherwise put it back at the
// head so it keeps its place for the next session.
void outbox_settle(const outbox_ring_slot_t& slot, bool sent) {
  raii::MutexGuard lock(outbox_mutex);
  if (lock) outbox_send_inflight = false;
  if (sent || !lock) {
    free(slot.data);
    return;
  }
  if (outbox_ring_push_front(&outbox, slot.data, slot.len)) {
    ESP_LOGW(TAG, "Outbox full, dropping oldest queued message");
  }
}

// Lifecycle task only, which keeps a single drainer and FIFO order. Stops at
// the first failed send, leaving that message and the rest queued.
void outbox_flush() {
  if (!ctx.link_up.load()) return;
  outbox_ring_slot_t slot;
  while (outbox_dequeue(&slot)) {
    bool sent = ws_send_now(slot.data, slot.len, OUTBOX_SEND_TIMEOUT);
    outbox_settle(slot, sent);
    if (!sent) return;
  }
}

// Free every queued message. Used on deinit.
void outbox_drain_free() {
  raii::MutexGuard lock(outbox_mutex);
  if (lock) outbox_ring_clear(&outbox);
}

// ---------------------------------------------------------------------------
// WebSocket event handler (WS task; ERROR may also arrive on a sender task)
// ---------------------------------------------------------------------------

void on_link_down() {
  if (!ctx.link_up.exchange(false)) return;
  event_bus_emit_simple(TRONBYT_EVENT_WS_DISCONNECTED);
  if (wifi_is_connected()) {
    app_state_set_connectivity(CONNECTIVITY_CONNECTED);
  }
  scheduler_on_ws_disconnect();
}

void note_server_answer(const esp_websocket_event_data_t* data) {
  const esp_websocket_error_codes_t& err = data->error_handle;
  if (err.esp_ws_handshake_status_code >= 100 ||
      err.esp_tls_cert_verify_flags != 0) {
    ctx.server_answered.store(true);
  }
}

void ws_event_handler(void*, esp_event_base_t, int32_t event_id,
                      void* event_data) {
  auto* data = static_cast<esp_websocket_event_data_t*>(event_data);

  switch (event_id) {
    case WEBSOCKET_EVENT_BEGIN:
      ctx.ws_task.store(xTaskGetCurrentTaskHandle());
      break;

    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "Connected");
      rx_message_opcode = 0;
      ctx.server_answered.store(true);
      ctx.link_up.store(true);
      notify_lifecycle(NOTIFY_SESSION_UP);
      msg_send_client_info();
      event_bus_emit_simple(TRONBYT_EVENT_WS_CONNECTED);
      app_state_set_connectivity(CONNECTIVITY_SERVER_ONLINE);
      scheduler_on_ws_connect();
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "Disconnected (wifi=%d)", wifi_is_connected());
      note_server_answer(data);
      draw_error_indicator_pixel();
      on_link_down();
      break;

    case WEBSOCKET_EVENT_CLOSED:
      ESP_LOGW(TAG, "Connection closed");
      on_link_down();
      break;

    case WEBSOCKET_EVENT_DATA: {
      uint8_t opcode = data->op_code;
      if (opcode == WS_TRANSPORT_OPCODES_TEXT ||
          opcode == WS_TRANSPORT_OPCODES_BINARY) {
        rx_message_opcode = opcode;
      } else if (opcode == WS_TRANSPORT_OPCODES_CONT) {
        opcode = rx_message_opcode;
      }
      if (opcode == WS_TRANSPORT_OPCODES_TEXT) {
        handle_text_message(data);
      } else if (opcode == WS_TRANSPORT_OPCODES_BINARY) {
        handle_binary_message(data);
      }
      break;
    }

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(TAG, "WebSocket error (handshake status=%d)",
               data->error_handle.esp_ws_handshake_status_code);
      note_server_answer(data);
      draw_error_indicator_pixel();
      break;

    case WEBSOCKET_EVENT_FINISH:
      ctx.ws_task.store(nullptr);
      on_link_down();
      notify_lifecycle(NOTIFY_FINISHED);
      break;
  }
}

// ---------------------------------------------------------------------------
// Client lifecycle (lifecycle task only)
// ---------------------------------------------------------------------------

// Returns a heap "Authorization: Bearer <key>\r\n" line, or nullptr when no
// API key is configured. Out of line so the ~1 KB config snapshot is off the
// stack before the client is created.
__attribute__((noinline)) char* build_auth_header() {
  system_config_t cfg = config_get();
  if (cfg.api_key[0] == '\0') {
    ESP_LOGW(TAG, "No API key configured, connecting without auth");
    return nullptr;
  }
  size_t hdr_len =
      strlen("Authorization: Bearer \r\n") + strlen(cfg.api_key) + 1;
  auto* hdr = static_cast<char*>(malloc(hdr_len));
  if (hdr) {
    snprintf(hdr, hdr_len, "Authorization: Bearer %s\r\n", cfg.api_key);
    ESP_LOGI(TAG, "Auth header set (%d chars)", (int)strlen(cfg.api_key));
  }
  return hdr;
}

void teardown_client() {
  esp_websocket_client_handle_t old = nullptr;
  {
    raii::MutexGuard lock(client_mutex);
    if (lock) {
      old = ctx.client;
      ctx.client = nullptr;
    }
  }
  if (!old) return;
  // Waits for the WS task to exit (up to network_timeout_ms mid-connect).
  esp_websocket_client_destroy(old);
  // That task posted NOTIFY_FINISHED before exiting; it refers to the client
  // just destroyed, not to whatever gets created next.
  ulTaskNotifyValueClear(nullptr, NOTIFY_FINISHED);
}

// Expects no live client (callers tear down first).
bool start_client() {
  ctx.server_answered.store(false);
  if (!ctx.url) {
    ESP_LOGE(TAG, "No URL configured");
    return false;
  }

  // The previous client, which may still reference the old header, is gone.
  free(ctx.auth_header);
  ctx.auth_header = build_auth_header();

  esp_websocket_client_config_t ws_cfg = {};
  ws_cfg.uri = ctx.url;
  ws_cfg.headers = ctx.auth_header;
  ws_cfg.buffer_size = 4096;
  // Message handlers and JSON parsing run in this task's event callbacks,
  // which overflows the component's 4096 default.
  ws_cfg.task_stack = CONFIG_WS_TASK_STACK_SIZE;
  ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
  ws_cfg.disable_auto_reconnect = true;
  ws_cfg.network_timeout_ms = 10000;
  ws_cfg.ping_interval_sec = 30;
  ws_cfg.pingpong_timeout_sec = 60;

  esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
  if (!client) {
    ESP_LOGE(TAG, "Failed to init WS client");
    return false;
  }
  esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_handler,
                                nullptr);
  {
    raii::MutexGuard lock(client_mutex);
    if (!lock) {
      esp_websocket_client_destroy(client);
      return false;
    }
    ctx.client = client;
  }

  esp_err_t err = esp_websocket_client_start(client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start WS client: %s", esp_err_to_name(err));
    teardown_client();
    return false;
  }

  ESP_LOGI(TAG, "Client started, connecting to %s", ctx.url);
  return true;
}

void schedule_retry() {
  uint32_t delay_ms = retry_backoff_delay_ms(retry_attempt, RECONNECT_BASE_MS,
                                             RECONNECT_MAX_MS, esp_random());
  ESP_LOGI(TAG, "Reconnect attempt %d in %lu ms", retry_attempt,
           (unsigned long)delay_ms);
  schedule_connect(static_cast<int64_t>(delay_ms) * 1000);
}

void on_gateway_probe_end(esp_ping_handle_t, void* args) {
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(args));
}

// Whether the local network looks alive, judged by the default gateway
// answering ICMP echo. Why this signal: it exercises exactly what a restart
// can repair (our WiFi driver and lwIP stack plus the link to the AP) and
// nothing a restart cannot (the ISP uplink, DNS upstream, the server). DNS
// would conflate an internet outage with a dead link (and says nothing for
// IP-literal server URLs), and "has an IP" stays true on a wedged stack.
// Losing the IP entirely is handled by the wifi module, not here. Known
// trade-off: a gateway that drops ICMP reads as dead, so such a network
// reboots the device once per escalation round while the server is down;
// home routers answer echo from the LAN.
bool local_network_alive() {
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip_info = {};
  if (!netif || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK ||
      ip_info.ip.addr == 0 || ip_info.gw.addr == 0) {
    // IPv6-only or no default route: nothing to probe, so do not restart.
    return true;
  }
  if (!gateway_probe_done) {
    gateway_probe_done = xSemaphoreCreateBinary();
    if (!gateway_probe_done) return true;
  }
  xSemaphoreTake(gateway_probe_done, 0);

  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.count = GATEWAY_PING_COUNT;
  cfg.interval_ms = GATEWAY_PING_INTERVAL_MS;
  cfg.timeout_ms = GATEWAY_PING_TIMEOUT_MS;
  ip_addr_set_ip4_u32_val(cfg.target_addr, ip_info.gw.addr);

  esp_ping_callbacks_t cbs = {};
  cbs.on_ping_end = on_gateway_probe_end;
  cbs.cb_args = gateway_probe_done;

  esp_ping_handle_t ping = nullptr;
  if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
    ESP_LOGW(TAG, "Gateway probe could not start, assuming network is up");
    return true;
  }
  esp_ping_start(ping);
  TickType_t budget =
      pdMS_TO_TICKS(GATEWAY_PING_COUNT *
                        (GATEWAY_PING_INTERVAL_MS + GATEWAY_PING_TIMEOUT_MS) +
                    1000);
  if (xSemaphoreTake(gateway_probe_done, budget) != pdTRUE) {
    esp_ping_stop(ping);
  }
  uint32_t replies = 0;
  esp_ping_get_profile(ping, ESP_PING_PROF_REPLY, &replies, sizeof(replies));
  esp_ping_delete_session(ping);

  ESP_LOGI(TAG, "Gateway " IPSTR " answered %lu/%lu probes",
           IP2STR(&ip_info.gw), (unsigned long)replies,
           (unsigned long)GATEWAY_PING_COUNT);
  return replies > 0;
}

void handle_attempt_failed() {
  if (!wifi_is_connected()) {
    ctx.awaiting_network.store(true);
    ESP_LOGW(TAG, "Network not available, will retry when IP acquired");
    return;
  }
  retry_attempt++;

  if (ctx.server_answered.load()) {
    sock_failure_count = 0;
    wifi_disconnect_count = 0;
    schedule_retry();
    return;
  }

  sock_failure_count++;
  ESP_LOGW(TAG, "Socket failure %d/%d (wifi resets: %d/%d)", sock_failure_count,
           MAX_SOCK_FAILURES_BEFORE_WIFI_RESET, wifi_disconnect_count,
           MAX_WIFI_RESETS);
  if (sock_failure_count < MAX_SOCK_FAILURES_BEFORE_WIFI_RESET) {
    schedule_retry();
    return;
  }
  sock_failure_count = 0;

  if (wifi_disconnect_count < MAX_WIFI_RESETS) {
    wifi_disconnect_count++;
    retry_attempt = 0;
    // The wifi module reconnects; TRONBYT_EVENT_WIFI_CONNECTED then schedules
    // the next attempt.
    ESP_LOGW(TAG, "Too many socket failures, disconnecting WiFi (%d/%d)",
             wifi_disconnect_count, MAX_WIFI_RESETS);
    esp_wifi_disconnect();
    return;
  }

  if (!local_network_alive()) {
    ESP_LOGE(TAG, "Local network unresponsive after %d WiFi resets, restarting",
             wifi_disconnect_count);
    esp_restart();
  }
  ESP_LOGW(TAG,
           "Local network is up but the server is unreachable, backing off");
  schedule_retry();
}

void handle_connect() {
  // A pending retry or a repeated got-IP event must not replace a live
  // session; a broken one ends in NOTIFY_FINISHED instead.
  if (ctx.link_up.load()) return;
  teardown_client();
  if (!wifi_is_connected()) {
    ctx.awaiting_network.store(true);
    ESP_LOGW(TAG, "Network not available, will retry when IP acquired");
    return;
  }
  if (!start_client()) handle_attempt_failed();
}

void handle_session_up() {
  if (reconnect_timer) esp_timer_stop(reconnect_timer);
  retry_attempt = 0;
  sock_failure_count = 0;
  wifi_disconnect_count = 0;
  outbox_flush();
}

void lifecycle_task_main(void*) {
  while (true) {
    uint32_t bits = 0;
    xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);

    if (bits & NOTIFY_SHUTDOWN) {
      teardown_client();
      xSemaphoreGive(shutdown_done);
      vTaskDelete(nullptr);
    }
    if (bits & NOTIFY_SESSION_UP) handle_session_up();
    if ((bits & NOTIFY_FINISHED) && ctx.client) {
      teardown_client();
      handle_attempt_failed();
      // The failure policy has chosen the next step.
      bits &= ~NOTIFY_CONNECT;
    }
    if (bits & NOTIFY_CONNECT) handle_connect();
    if (bits & NOTIFY_FLUSH) outbox_flush();
  }
}

// ---------------------------------------------------------------------------
// WiFi events (via event bus)
// ---------------------------------------------------------------------------

void on_wifi_event(const tronbyt_event_t* event, void*) {
  if (event->type == TRONBYT_EVENT_WIFI_DISCONNECTED) {
    ctx.awaiting_network.store(true);
    return;
  }
  if (event->type != TRONBYT_EVENT_WIFI_CONNECTED) return;
  if (!ctx.awaiting_network.exchange(false)) return;
  // Avoid the first TLS handshake during peak startup activity.
  ESP_LOGI(TAG, "Got IP, connecting in %lld ms",
           GOT_IP_CONNECT_DELAY_US / 1000);
  schedule_connect(GOT_IP_CONNECT_DELAY_US);
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

  if (xTaskCreate(lifecycle_task_main, "ws_lifecycle", LIFECYCLE_STACK_SIZE,
                  nullptr, LIFECYCLE_PRIORITY, &lifecycle_task) != pdPASS) {
    lifecycle_task = nullptr;
    ESP_LOGE(TAG, "Failed to create WS lifecycle task");
    return;
  }

  esp_timer_create_args_t reconnect_args = {};
  reconnect_args.callback = reconnect_timer_callback;
  reconnect_args.name = "sock_reconn";
  reconnect_args.skip_unhandled_events = true;
  esp_timer_create(&reconnect_args, &reconnect_timer);

  event_bus_subscribe(TRONBYT_EVENT_WIFI_CONNECTED, on_wifi_event, nullptr);
  event_bus_subscribe(TRONBYT_EVENT_WIFI_DISCONNECTED, on_wifi_event, nullptr);

  if (wifi_is_connected() && ctx.awaiting_network.exchange(false)) {
    // Defer first connect slightly to let boot-time tasks (including
    // app_main) release stack/heap before websocket task allocation.
    schedule_connect(INITIAL_CONNECT_DELAY_US);
    ESP_LOGI(TAG, "Network ready, deferring initial WS connect by %lld ms",
             INITIAL_CONNECT_DELAY_US / 1000);
  } else {
    ESP_LOGI(TAG, "Waiting for network...");
  }
}

void sockets_deinit() {
  if (reconnect_timer) {
    esp_timer_stop(reconnect_timer);
    esp_timer_delete(reconnect_timer);
    reconnect_timer = nullptr;
  }
  event_bus_unsubscribe(on_wifi_event);

  // The lifecycle task owns the client, so it destroys it before exiting.
  // Clearing the handle first stops further notifications to it.
  TaskHandle_t task = lifecycle_task;
  lifecycle_task = nullptr;
  if (task) {
    shutdown_done = xSemaphoreCreateBinary();
    if (shutdown_done) {
      xTaskNotify(task, NOTIFY_SHUTDOWN, eSetBits);
      xSemaphoreTake(shutdown_done, portMAX_DELAY);
      vSemaphoreDelete(shutdown_done);
      shutdown_done = nullptr;
    } else {
      ESP_LOGE(TAG, "Failed to stop WS lifecycle task");
    }
  }
  ctx.link_up.store(false);
  ctx.awaiting_network.store(true);

  if (ctx.url) {
    free(ctx.url);
    ctx.url = nullptr;
  }
  if (ctx.auth_header) {
    free(ctx.auth_header);
    ctx.auth_header = nullptr;
  }

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

bool sockets_is_connected() { return ctx.link_up.load(); }

int sockets_send_text(const char* data, size_t len, TickType_t timeout) {
  if (!data || len == 0) return -1;

  // Fast path: nothing queued or in flight and the link is up, so send inline
  // with the caller's timeout. Never from the WS task (see top of file).
  if (xTaskGetCurrentTaskHandle() != ctx.ws_task.load() && outbox_is_idle() &&
      ws_send_now(data, len, timeout)) {
    return static_cast<int>(len);
  }

  // Link down, mutex busy, called from the WS task, or messages already
  // queued ahead of this one: copy onto the outbox so the lifecycle task
  // delivers it in order once the link is ready. The message is accepted
  // (returns len) even though delivery is deferred.
  char* copy = static_cast<char*>(malloc(len));
  if (!copy) {
    ESP_LOGE(TAG, "Failed to alloc %u bytes for outbox", (unsigned)len);
    return -1;
  }
  memcpy(copy, data, len);
  outbox_enqueue(copy, len);
  notify_lifecycle(NOTIFY_FLUSH);
  return static_cast<int>(len);
}
