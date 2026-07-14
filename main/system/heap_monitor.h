#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t internal_free;
    size_t internal_min;
    size_t internal_largest_block;
    size_t spiram_free;
    size_t spiram_min;
    size_t spiram_largest_block;
    size_t dma_free;
} heap_snapshot_t;

typedef struct {
    uint32_t uptime_ms;
    size_t internal_free;
    size_t internal_min;
    size_t spiram_free;
    size_t spiram_min;
} heap_trend_point_t;

/** Initialize heap monitoring and record baseline. Call early in app_main. */
void heap_monitor_init(void);

/** Log current heap status with per-capability breakdown. */
void heap_monitor_log_status(const char* tag);

/** Take a checkpoint for later delta comparison. */
void heap_monitor_checkpoint(const char* label);

/** Log delta since last checkpoint. */
void heap_monitor_check_since_checkpoint(const char* label);

/** Fill snapshot with current heap state. */
void heap_monitor_get_snapshot(heap_snapshot_t* snapshot);

/** Capture the current heap values into the trend ring. */
void heap_monitor_capture_sample(void);

/** Return recent heap trend points (newest first). */
size_t heap_monitor_get_trend(heap_trend_point_t* out, size_t max_points);

/** Run heap integrity check; returns true if OK. */
bool heap_monitor_check_integrity(const char* location);

/**
 * Fragmentation percentage (0-100) for a heap region.
 *
 * Reports how much of the free space is unavailable as a single contiguous
 * block: 0 means the largest free block equals total free (an allocation can
 * use all of it), higher values mean the free space is split into smaller
 * pieces. A region with lots of free bytes but a high fragmentation percentage
 * can still fail a large allocation. Pure function, safe to unit test on host.
 */
uint8_t heap_monitor_fragmentation_pct(size_t free_bytes, size_t largest_block);

/** Dump detailed per-region heap info to log. */
void heap_monitor_dump_info(void);
