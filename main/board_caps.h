#pragma once

// Semantic board-capability flags.
//
// Application code should test what a board can *do* (BOARD_HAS_TOUCH) rather
// than what hardware it *is* (CONFIG_BOARD_TIDBYT_GEN2). Adding a board that
// shares a capability then means editing this one file instead of hunting down
// every scattered `#if CONFIG_BOARD_*` disjunction.
//
// Each flag expands to a literal 0 or 1 so it is usable in both `#if` and
// runtime expressions. We deliberately avoid `#define BOARD_HAS_X
// defined(CONFIG_...)`: using `defined` inside a macro that is later evaluated
// by `#if` is undefined behavior in the C preprocessor.

#include "sdkconfig.h"

// Capacitive touch pad. Currently the Tidbyt Gen2 only.
#if CONFIG_BOARD_TIDBYT_GEN2
#define BOARD_HAS_TOUCH 1
#else
#define BOARD_HAS_TOUCH 0
#endif

// Boards that expose the G/B channel swap as a user-settable portal option.
//
// NOTE: display.cpp also honors swap_colors on MATRIXPORTAL_S3_WIDE and
// WAVESHARE_S3, but those boards do not surface the portal checkbox today, so
// they are intentionally excluded here to preserve existing behavior. Whether
// the checkbox should be offered on those panels is an open hardware question
// (does the waveshare panel actually have the quirk?) tracked in design doc
// 013, and is deliberately NOT changed by this capability shim.
#if CONFIG_BOARD_TIDBYT_GEN1 || CONFIG_BOARD_MATRIXPORTAL_S3 || \
    CONFIG_BOARD_TRONBYT_S3
#define BOARD_HAS_SWAP_COLORS 1
#else
#define BOARD_HAS_SWAP_COLORS 0
#endif
