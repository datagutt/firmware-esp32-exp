#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fixed depth keeps the queue bounded on a long outage. On overflow the
// OLDEST entry is dropped (a display device favours the newest state).
#define OUTBOX_RING_DEPTH 12

typedef struct {
  char* data;
  size_t len;
} outbox_ring_slot_t;

// Bounded FIFO of heap-owned messages. Pure data structure with no RTOS or
// ESP dependencies so it is host-testable; it is NOT thread safe, callers
// serialize access (sockets.cpp guards it with a mutex).
typedef struct {
  outbox_ring_slot_t slots[OUTBOX_RING_DEPTH];
  size_t head;   // index of the oldest queued message
  size_t count;  // number of occupied slots
} outbox_ring_t;

void outbox_ring_init(outbox_ring_t* ring);

// Takes ownership of data (heap allocated, len bytes) and appends it. On a
// full ring the oldest entry is freed and dropped to make room; returns true
// when that happened.
bool outbox_ring_push(outbox_ring_t* ring, char* data, size_t len);

// Pops the oldest entry into *out (the caller then owns out->data). Returns
// false when the ring is empty.
bool outbox_ring_pop(outbox_ring_t* ring, outbox_ring_slot_t* out);

size_t outbox_ring_count(const outbox_ring_t* ring);

// Frees every queued message.
void outbox_ring_clear(outbox_ring_t* ring);

#ifdef __cplusplus
}
#endif
