# Design Spike 012: Centralized Hardened TLS Fetch

**Status**: investigation complete, recommendation: NO-GO on full component, PARTIAL on retry-only (Plan 004).

**Planned at**: commit `79c0128`, 2026-06-27
**Investigated against**: same commit, worktree `advisor/012-spike-tls-fetch`

---

## 1. The p3a Incident

Reference: `inspiration/p3a/docs/concurrent-tls-eagain-tabled.md`

During a periodic Giphy channel refresh, the p3a firmware fanned out 4-5
concurrent TLS sessions on the same Wi-Fi link simultaneously: page fetch N,
binary art download (kicked off after each page merge), view-tracker pingback,
and the persistent Makapix MQTT-over-TLS connection. The lwIP TCPIP receive
mailbox and pbuf pool are shared across all sockets. Under 4-5 concurrent streams
the receive mbox saturated, and socket reads returned EAGAIN (errno 11, newlib
reports it as "No more processes"). `esp_http_client` mapped `read==0` to EOF and
accepted a 2742-byte truncated fragment as "the full 93 KB JSON response", causing
a parse failure. The MQTT keepalive PING was also starved for more than 30 seconds,
triggering a broker-side session drop.

p3a's responses, in order of implementation:

| Option | Implemented | Detail |
|--------|-------------|--------|
| 2 - Retry on truncated read | 2026-05-03 | `{0, 1000, 3000}` ms backoff, 3 attempts, truncation detected via `Content-Length` mismatch or premature EOF on chunked encoding; centralized in `components/http_fetch/do_fetch()` 2026-06-05 |
| 3 - Bump lwIP recv resources | 2026-06-15 | `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE`, `CONFIG_LWIP_TCP_RECVMBOX_SIZE`, `LWIP_PBUF_POOL_SIZE`, `MEMP_NUM_TCP_SEG` |
| 4 - TLS counting semaphore | 2026-06-15 | `CONFIG_HTTP_FETCH_MAX_CONCURRENT_TLS=2` (range 1-8), gate in `http_fetch.c::tls_gate()`, lazy init via compare-and-set, slot released during inter-attempt backoff sleep to avoid pinning a slot while idle |

The p3a `http_fetch` component API (`http_fetch_to_buffer`, `http_fetch_to_file`)
is the single funnel for all content fetchers. Every fetcher was already routed
through it by 2026-06-05, so the semaphore cost was 0 lines at call sites.
The gate is intentionally NOT applied to the persistent MQTT TLS link (separate
stack) nor to infrequent best-effort one-shot paths.

---

## 2. This Repo's TLS Call-Site Map

Four call sites open TLS connections. The table below lists the client type, the
task it runs on, and whether two call sites can overlap in time.

| Call site | File | Client | Task / context | Mode active |
|-----------|------|---------|----------------|-------------|
| `remote_get()` | `main/network/remote.cpp:262` | `esp_http_client_perform` | `http_fetch` task, prio 3, CPU0 | HTTP mode only |
| `start_client_locked()` | `main/network/sockets.cpp` (WebSocket init) | `esp_websocket_client` (persistent TLS) | esp_timer callback then WS internal task | WS mode only |
| `run_ota()` | `main/system/ota.cpp:193` | `esp_https_ota_begin/perform` | `ota_task`, prio 5, tskNO_AFFINITY | any mode |
| HTTP upload server | `main/system/ota_http_upload.cpp` | inbound server (not a TLS client) | HTTP server task | any mode |

`ota_http_upload.cpp` is a server-side listener, not a TLS client; it is excluded
from the concurrency hazard analysis.

### Scheduler mode exclusivity

`runtime_orchestrator.cpp:162-165` calls either `scheduler_start_ws()` or
`scheduler_start_http()` once at boot depending on configuration. The scheduler
sets `ctx.mode` to `Mode::WEBSOCKET` or `Mode::HTTP` respectively. This mode does
not change at runtime (WS disconnect/reconnect stays in `Mode::WEBSOCKET`;
the scheduler calls `scheduler_on_ws_disconnect()` which stops timers but does not
switch to HTTP mode).

Consequence: `remote_get` and the WebSocket TLS session are **structurally
mutually exclusive** at the scheduler level.

`http_trigger_fetch()` (`scheduler.cpp:280`) guards against concurrent fetches:

```cpp
void http_trigger_fetch() {
  if (ctx.fetch_task) {
    ESP_LOGW(TAG, "Fetch already in progress");
    return;
  }
  // ...xTaskCreatePinnedToCoreWithCaps(http_fetch_task, ...)
}
```

And `http_fetch_task` calls `remote_get` only if `ctx.mode == Mode::HTTP`
(`scheduler.cpp:253`).

---

## 3. Concurrency Hazard Confirmation

### Scenario A: HTTP mode + OTA (refuted)

OTA is triggered in HTTP mode only when `http_apply_prefetch()` finds an
`ota_url` in the prefetch result. By that point `http_fetch_task` has already
called `esp_http_client_cleanup(http)` and exited (`remote.cpp:296`,
`scheduler.cpp:276`). The `ota_task` is spawned after that. The two TLS sessions
never overlap.

**Verdict: NO overlap in HTTP mode.**

### Scenario B: WS mode + OTA (confirmed)

OTA can be triggered directly from a WebSocket message. In `handlers.cpp:306-317`:

```cpp
if (has_ota_url) {
  // ...copy URL to SPIRAM...
  ESP_LOGI(TAG, "OTA URL received via WS: %s", ota_url);
  BaseType_t ota_rc = xTaskCreatePinnedToCoreWithCaps(
      ota_task_entry, "ota_task", 8192, ota_url, 5,
      nullptr, tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  // ...
}
```

The WebSocket client (`esp_websocket_client`) is still connected at this point.
`run_ota()` does not call any sockets-layer teardown; it calls `gfx_stop()` and
then immediately begins the `esp_https_ota` download. The WS TLS session and the
OTA TLS session are therefore simultaneously active.

**Verdict: REAL overlap in WS mode. Maximum concurrent TLS sessions: 2.**

### Why this differs from p3a

p3a's incident required 4-5 near-simultaneous TLS sessions with fresh handshakes
and concurrent bulk data flows. The lwIP receive mbox saturated because multiple
sockets were simultaneously queuing TCP segments.

In this firmware the WS mode overlap is 1 established session doing 30-second
keepalive pings (tiny frames, no bulk data) plus 1 OTA bulk GET. The WS session
contributes negligible receive-queue pressure. The effective concurrency for
lwIP receive purposes is closer to 1.5 sessions, not 5.

Additionally, `ota_task` sets `s_ota_in_progress` via
`compare_exchange_strong` (`ota.cpp:147`), so a second OTA call during the first
returns immediately. That guard prevents the only other plausible multi-OTA
scenario.

The practical risks of the 2-session WS-mode overlap are:

1. **WS keepalive starvation during OTA download**: `pingpong_timeout_sec = 60`
   (`sockets.cpp::start_client_locked`). A firmware image of 1-2 MB at a modest
   throughput completes well within 60 seconds. If it does stall past 60 seconds
   the WebSocket disconnects and auto-reconnects in 10 seconds after OTA completes.
   OTA is intentional and the device reboots on success anyway.

2. **EAGAIN on OTA read**: Unlikely. With only 1 active bulk consumer plus 1
   near-silent keepalive, the lwIP receive path is nowhere near the saturation
   level p3a observed.

---

## 4. Proposed `tls_fetch` API (if the hazard were severe)

This section is included for completeness because the plan required it, but the
hazard analysis above argues against building it now. If future call-site
additions change the topology, this sketch is a starting point.

```c
// components/tls_fetch/include/tls_fetch.h

typedef struct {
    const char   *url;
    int           timeout_ms;    // 0 -> 20000
    int           max_attempts;  // 0 -> 3
    const uint32_t *backoff_ms;  // NULL -> {0, 1000, 3000}
    int64_t       max_size;      // 0 -> no cap
    // optional: extra headers, user_agent, ...
} tls_fetch_opts_t;

// Fetches URL body into a caller-owned buffer with retry/backoff and a
// process-wide counting semaphore (CONFIG_TLS_FETCH_MAX_CONCURRENT, default 2).
// Returns ESP_OK or ESP_ERR_NOT_FOUND / ESP_ERR_INVALID_RESPONSE / ESP_FAIL.
esp_err_t tls_fetch_get(const char *url, const tls_fetch_opts_t *opts,
                        uint8_t **buf_out, size_t *len_out);

// Identical to esp_https_ota but slot-gated.
esp_err_t tls_fetch_ota(const char *url, const tls_fetch_opts_t *opts);
```

Concurrency gate design (from p3a, directly applicable):

- Static `SemaphoreHandle_t` initialized lazily via create-outside-critical + compare-and-set (`portMUX_TYPE` spinlock).
- Acquired after any non-network setup (URL copy, buffer alloc). Released once after the hop/retry loop, briefly given back during inter-attempt backoff sleep.
- N configurable via `CONFIG_TLS_FETCH_MAX_CONCURRENT` Kconfig (default 2, range 1-4), set to 1 to fully serialize.
- WebSocket NOT gated: `esp_websocket_client` manages its own persistent TLS session outside this path.

**Blast radius** (if this were built):
- New component: `components/tls_fetch/` (~200 lines C + header + CMakeLists + Kconfig)
- `main/network/remote.cpp` — replace `esp_http_client_perform` block with `tls_fetch_get` (~30 lines changed)
- `main/system/ota.cpp` — replace `esp_https_ota_begin/perform/finish` block with `tls_fetch_ota` (~60 lines changed)
- `main/network/sockets.cpp` — no change (WebSocket not gated by design)
- `main/system/ota_http_upload.cpp` — no change (server, not client)
- `CMakeLists.txt` — add component dependency
- Total: 3 files touched, ~90 lines changed, 1 new component (~200 lines)

---

## 5. Recommendation

**NO-GO on the centralized `tls_fetch` component.**

**PARTIAL: proceed with Plan 004 (retry/backoff in `remote_get`) independently.**

### Rationale

The concurrency hazard is structural but subcritical for this firmware's workload.
The only confirmed overlap is 1 WS keepalive session (established, trivial traffic)
plus 1 OTA download task. This is categorically different from p3a's 4-5
simultaneous fresh TLS handshakes and concurrent bulk transfers. The lwIP receive
queue saturation that p3a observed requires much higher concurrency than anything
this firmware can generate.

The cost of the full component (1 new component, 3 files changed) is not justified
against the actual observed risk: there are no EAGAIN reports from this firmware,
no WS keepalive failures during OTA documented, and the worst-case WS disconnect
during OTA is self-healing (reconnects in 10 s; device reboots on OTA success
anyway).

| Scenario | Concurrent TLS | Hazard level | Verdict |
|----------|---------------|-------------|---------|
| HTTP prefetch only | 1 | none | — |
| HTTP prefetch + OTA | never overlaps | none | — |
| WS keepalive + OTA | 2 (one near-silent) | LOW | acceptable as-is |
| Multiple remote_get | impossible (guarded) | none | — |

### What IS worth doing

1. **Plan 004 (P2, M)** — add retry/backoff inside `remote_get`. This is
   independently valuable for Wi-Fi blips and transient server errors. It does
   NOT require a shared semaphore because `remote_get` cannot be called
   concurrently (scheduler guard). The {0, 1000, 3000} ms backoff and 3-attempt
   limit from p3a are directly applicable.

2. **Low-effort WS resilience during OTA** — if WS keepalive failures during OTA
   are ever observed, the cheapest fix is not a shared semaphore but increasing
   `pingpong_timeout_sec` from 60 to 120, or having `run_ota()` call
   `sockets_disconnect()` before starting the download so the WS does not try to
   ping during a large transfer. Neither requires a new component.

3. **Trigger for revisiting** — if any of the following appear, reconsider building
   `tls_fetch`:
   - A second persistent outbound HTTPS path is added (e.g., telemetry, weather feed).
   - OTA over HTTP mode is observed failing with `esp_tls_conn_read errno=11`.
   - `remote_get` retries from Plan 004 hit their cap regularly under normal operation.

### Relationship to Plan 004

Plan 012 does NOT supersede Plan 004. Plan 004 addresses a different, real problem:
`remote_get` treats 429/503/504 and network blips as permanent failures. That
tactical retry is the right fix for that narrow bug, regardless of the concurrency
question. Plan 012 asked whether to also build a shared concurrency gate on top of
that; the answer is no for the current call-site count.

If Plan 012 were ever upgraded to "go", it would subsume Plan 004 (the retry logic
would live in `tls_fetch` rather than `remote_get`). Until then, implement Plan 004
as planned and leave Plan 012 on the shelf.

### Open questions that would block a "go" decision

- Does the firmware run on hardware/IDF configurations with smaller-than-default
  lwIP recv mailboxes? Checking `sdkconfig` values for `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE`
  and `CONFIG_LWIP_TCP_RECVMBOX_SIZE` across board variants would quantify the
  true headroom.
- Is OTA delivery ever done over a throttled or congested link where downloads
  exceed 60 seconds? If yes, WS keepalive starvation is a concrete risk, and
  `sockets_disconnect()` before OTA is the right tactical fix.
- Are there plans to add new outbound HTTPS paths (telemetry, push notification
  ack, weather)? Each new concurrent path raises the hazard level.
