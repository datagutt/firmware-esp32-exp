#pragma once

#include <stdbool.h>
#include <stdint.h>

// Serializes heavy TLS/HTTP operations so no two clients run a handshake at the
// same time. Each esp_http_client / esp_https_ota TLS handshake transiently
// needs roughly 40KB of internal RAM; two colliding handshakes can exhaust the
// internal heap on the 520KB ESP32 gen1 boards. Callers hold the single slot
// while a connection is being established (and, for streaming transfers like
// OTA, for the whole transfer that keeps the socket open).
//
// The long-lived WebSocket connection is intentionally NOT gated by this slot:
// it is owned by the esp_websocket_client component, stays connected for the
// device lifetime, and does not repeatedly re-handshake, so it does not add to
// the transient concurrent-handshake pressure this slot bounds.

#ifdef __cplusplus
extern "C" {
#endif

// Acquires the shared HTTP slot, waiting up to timeout_ms. Returns true when
// the slot is held (caller must pair with http_slot_release), false on timeout.
// tag names the caller for the contention log. Never blocks forever: callers
// pass a bounded timeout and skip/retry their operation when acquisition fails.
bool http_slot_acquire(const char* tag, uint32_t timeout_ms);

// Releases a slot previously acquired by http_slot_acquire.
void http_slot_release(void);

#ifdef __cplusplus
}  // extern "C"

namespace http_slot {

// RAII guard for C++ callers. Acquires in the constructor, releases in the
// destructor. Evaluates to true only when the slot was actually acquired, so
// callers must check it before proceeding:
//
//   http_slot::Guard slot("remote", 5000);
//   if (!slot) { /* busy: skip this cycle */ }
class Guard {
 public:
  Guard(const char* tag, uint32_t timeout_ms)
      : held_(http_slot_acquire(tag, timeout_ms)) {}

  ~Guard() { release(); }

  explicit operator bool() const { return held_; }

  void release() {
    if (held_) {
      http_slot_release();
      held_ = false;
    }
  }

  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;

 private:
  bool held_;
};

}  // namespace http_slot

#endif  // __cplusplus
