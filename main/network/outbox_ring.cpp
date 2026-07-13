#include "outbox_ring.h"

#include <stdlib.h>

void outbox_ring_init(outbox_ring_t* ring) {
  for (size_t i = 0; i < OUTBOX_RING_DEPTH; i++) {
    ring->slots[i].data = nullptr;
    ring->slots[i].len = 0;
  }
  ring->head = 0;
  ring->count = 0;
}

bool outbox_ring_push(outbox_ring_t* ring, char* data, size_t len) {
  bool dropped = false;
  if (ring->count == OUTBOX_RING_DEPTH) {
    free(ring->slots[ring->head].data);
    ring->slots[ring->head].data = nullptr;
    ring->head = (ring->head + 1) % OUTBOX_RING_DEPTH;
    ring->count--;
    dropped = true;
  }
  size_t idx = (ring->head + ring->count) % OUTBOX_RING_DEPTH;
  ring->slots[idx].data = data;
  ring->slots[idx].len = len;
  ring->count++;
  return dropped;
}

bool outbox_ring_pop(outbox_ring_t* ring, outbox_ring_slot_t* out) {
  if (ring->count == 0) return false;
  *out = ring->slots[ring->head];
  ring->slots[ring->head].data = nullptr;
  ring->slots[ring->head].len = 0;
  ring->head = (ring->head + 1) % OUTBOX_RING_DEPTH;
  ring->count--;
  return true;
}

size_t outbox_ring_count(const outbox_ring_t* ring) { return ring->count; }

void outbox_ring_clear(outbox_ring_t* ring) {
  outbox_ring_slot_t slot;
  while (outbox_ring_pop(ring, &slot)) free(slot.data);
}
