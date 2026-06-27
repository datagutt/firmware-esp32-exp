# Design 013: Board Capability / HAL Layer (Spike)

**Status**: Spike complete. Recommendation: **partial go** (touch/brightness coupling only).

**Planned at**: commit `79c0128`, 2026-06-27
**Based on**: grep of `main/` at worktree HEAD + read of `inspiration/p3a/components/p3a_board_ep44b/` and `inspiration/matrx-fw/main/daughterboard/`

---

## 1. Board-Conditional Inventory (Blast-Radius Map)

Total `CONFIG_BOARD_*` sites in `main/`: **~21 sites across 6 files**.

### 1.1 `main/display/display.cpp`

| Lines | Guard | What it switches |
|-------|-------|-----------------|
| 48–210 | `#if CONFIG_BOARD_TIDBYT_GEN2 / #elif CONFIG_BOARD_TRONBYT_S3_WIDE / ...` | HUB75 pin assignments (r1/g1/b1/r2/g2/b2/a/b/c/d/e/lat/oe/clk) for each board. One big `#elif` chain over all 8 boards. |
| 80–101 | `#elif CONFIG_BOARD_TRONBYT_S3` | Same pin block, but `g1/b1/g2/b2` are runtime-swapped based on `cfg.swap_colors`. |
| 119–165 | `#elif CONFIG_BOARD_MATRIXPORTAL_S3_WIDE / _S3` | Same pattern; swap_colors also conditional here. |
| 311 | `#if CONFIG_BOARD_TIDBYT_GEN1 \|\| CONFIG_BOARD_TIDBYT_GEN2` | Per-board brightness ceiling (Tidbyt uses 100, others use a lower ceiling for panel safety). |

**Assessment**: The pin preset chain is self-contained inside `display_initialize()`. All scan pattern, driver, clock, and wiring parameters come from `CONFIG_HUB75_*` Kconfig already. Adding a new board means one new `#elif` block here. Not sprawling across the codebase.

### 1.2 `main/main.cpp`

| Lines | Guard | What it switches |
|-------|-------|-----------------|
| 21–23 | `#ifdef CONFIG_BOARD_TIDBYT_GEN2` | `#include "touch_control.h"` |
| 37–89 | `#ifdef CONFIG_BOARD_TIDBYT_GEN2` | `handle_touch_event()` + `touch_task()` function definitions |
| 93–98 | `#ifdef CONFIG_BOARD_TIDBYT_GEN2` | `touch_on_brightness_set()` definition (called externally by network layer) |
| 150–165 | `#ifdef CONFIG_BOARD_TIDBYT_GEN2` | Touch init + task spawn inside `app_main` |

**Assessment**: All four sites are the same capability: the Gen2 capacitive touch pad. They are tightly coupled — the extern function `touch_on_brightness_set` leaks the touch state back into the network layer.

### 1.3 `main/config/ap.cpp`

| Lines | Guard | What it switches |
|-------|-------|-----------------|
| 36–45 | `#if CONFIG_BOARD_TIDBYT_GEN1 \|\| CONFIG_BOARD_MATRIXPORTAL_S3 \|\| CONFIG_BOARD_TRONBYT_S3` | `SWAP_COLORS_FMT` HTML constant (captive portal checkbox) |
| 47–56 | `#if CONFIG_BOARD_TIDBYT_GEN2` | `DISABLE_TOUCH_FMT` HTML constant |
| 175–176 | same as 36–45 | Use of `SWAP_COLORS_FMT` in `root_handler` |
| 184 | `#if CONFIG_BOARD_TIDBYT_GEN2` | Use of `DISABLE_TOUCH_FMT` in `root_handler` |

**Assessment**: Portal UI capability flags. Each site tests one capability (swap_colors or disable_touch). Straightforward.

### 1.4 `main/network/sta_api.cpp`

| Lines | Guard | What it switches |
|-------|-------|-----------------|
| 19–20 | `#ifdef CONFIG_BOARD_TIDBYT_GEN2` | `extern void touch_on_brightness_set(uint8_t)` declaration |
| 425–426 | `#ifdef CONFIG_BOARD_TIDBYT_GEN2` | Call to `touch_on_brightness_set()` when brightness is set via STA API |

### 1.5 `main/network/handlers.cpp`

| Lines | Guard | What it switches |
|-------|-------|-----------------|
| 15–16 | `#ifdef CONFIG_BOARD_TIDBYT_GEN2` | Same `extern` declaration |
| 300–302 | `#ifdef CONFIG_BOARD_TIDBYT_GEN2` | Call to `touch_on_brightness_set()` when brightness set via HTTP handler |

**Assessment**: The network layer should not know about touch state. The `extern` pattern forces two network files to `#include`-guard against a hardware peripheral in `main.cpp`. This is the only genuine cross-cutting concern in the codebase.

### 1.6 `main/CMakeLists.txt`

| Line | Guard | What it switches |
|------|-------|-----------------|
| 18–20 | `if(NOT CONFIG_BOARD_TIDBYT_GEN2)` | Excludes `touch_control.cpp` from the build |

### 1.7 `sdkconfig.defaults.*` and `boards/*.csv`

Per-board build overlays (panel dimensions, scan pattern, clock, driver, partition sizes). These are already cleanly structured as named files per target and are not considered sprawl — they are the correct mechanism.

---

## 2. Reference Interface Summaries

### 2.1 p3a — `inspiration/p3a/components/p3a_board_ep44b/`

p3a targets a single board (ESP32-P4-WIFI6-Touch-LCD-4B with MIPI-DSI display). Its `p3a_board.h` is a single-board HAL, not a multi-board abstraction.

**Pattern**: Capability macros driven by Kconfig, with functions guarded by `#if P3A_HAS_*`:

```c
// Compile-time capability flags (from Kconfig)
#define P3A_HAS_TOUCH     (CONFIG_P3A_HAS_TOUCH ? 1 : 0)
#define P3A_HAS_BRIGHTNESS (CONFIG_P3A_HAS_BRIGHTNESS_CONTROL ? 1 : 0)
#define P3A_HAS_BUTTONS   (CONFIG_P3A_HAS_BUTTONS ? 1 : 0)

// Required functions (every board):
esp_err_t p3a_board_display_init(uint8_t bg_r, uint8_t bg_g, uint8_t bg_b);
esp_lcd_panel_handle_t p3a_board_get_panel(void);
uint8_t* p3a_board_get_buffer(int index);
esp_err_t p3a_board_set_brightness(int percent);

// Conditional functions:
#if P3A_HAS_TOUCH
esp_err_t p3a_board_touch_init(esp_lcd_touch_handle_t *handle);
#endif
#if P3A_HAS_BUTTONS
esp_err_t p3a_board_button_init(void);
#endif
```

**Key lesson**: Capability flags are named semantically (what it does), not by board name (what hardware it is). Application code tests `P3A_HAS_TOUCH`, not `CONFIG_BOARD_EP44B`. This is the useful pattern.

**Applicability**: p3a uses BSP (board support package) for the display — the display init is BSP-delegated. This firmware drives HUB75 panels directly via custom pin maps; the BSP pattern does not apply here.

### 2.2 matrx-fw — `inspiration/matrx-fw/main/daughterboard/`

A single-file HAL for one specific optional peripheral add-on board (3 buttons + VEML6030 light sensor over I2C).

**Interface** (`daughterboard.h`):

```c
// Hardcoded GPIO assignments for this one peripheral
#define DAUGHTERBOARD_BUTTON_A_GPIO  GPIO_NUM_5
#define DAUGHTERBOARD_BUTTON_B_GPIO  GPIO_NUM_6
#define DAUGHTERBOARD_BUTTON_C_GPIO  GPIO_NUM_7
#define DAUGHTERBOARD_I2C_SDA_GPIO   GPIO_NUM_2
#define DAUGHTERBOARD_I2C_SCL_GPIO   GPIO_NUM_1

// ESP event-based output
ESP_EVENT_DECLARE_BASE(DAUGHTERBOARD_EVENTS);
typedef enum {
    DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED,
    DAUGHTERBOARD_EVENT_BUTTON_B_PRESSED,
    DAUGHTERBOARD_EVENT_BUTTON_C_PRESSED,
    DAUGHTERBOARD_EVENT_LIGHT_READING,
} daughterboard_event_t;

esp_err_t daughterboard_init(void);
uint16_t daughterboard_get_lux(void);
bool daughterboard_is_button_pressed(uint8_t id);
esp_err_t daughterboard_set_veml_config(uint16_t config);
```

**Key lesson**: The event bus pattern (posting `DAUGHTERBOARD_EVENT_*` via `esp_event`) cleanly decouples the peripheral from its consumers. Consumers subscribe to events rather than being called via extern linkage. This is exactly what the `touch_on_brightness_set` extern coupling needs.

**Applicability**: This firmware already has an `event_bus` component. The pattern maps directly.

---

## 3. Proposed Boundary

### 3.1 What stays in Kconfig/sdkconfig (no change)

- Panel width/height (`CONFIG_HUB75_PANEL_WIDTH/HEIGHT`)
- Scan pattern, clock speed, shift driver, scan wiring (`CONFIG_HUB75_*`)
- Memory layout and partition tables (`boards/*.csv`)
- Brightness initial value (`CONFIG_HUB75_BRIGHTNESS`)

These are already at the right abstraction level. Kconfig is the correct place for compile-time hardware parameters.

### 3.2 What stays in `#ifdef CONFIG_BOARD_*` (no change)

- **Display pin presets in `display.cpp`**: Already centralized in one function. The `#elif` chain is a lookup table expressed as C preprocessor. It is correct and complete. A refactor to a `static const` struct array would save a few lines but would not remove any ifdefs (the array itself would need per-board entries). Leave as-is.

- **`SWAP_COLORS_FMT` / `DISABLE_TOUCH_FMT` in `ap.cpp`**: Small, stable, UI-only. If `board_caps.h` (see 3.3) is added, these can migrate to `#if BOARD_HAS_SWAP_COLORS` / `#if BOARD_HAS_TOUCH` — a cosmetic improvement.

### 3.3 Recommended addition: `main/board_caps.h` (thin capability shim)

A header that maps board IDs to semantic capability flags. **Purely additive — no production logic moves.**

```c
// main/board_caps.h
#pragma once
#include "sdkconfig.h"

// Touch: Gen2 capacitive pad
#define BOARD_HAS_TOUCH   defined(CONFIG_BOARD_TIDBYT_GEN2)

// Swap-colors: boards where G/B channel swap is a board-level quirk
#define BOARD_HAS_SWAP_COLORS  (defined(CONFIG_BOARD_TIDBYT_GEN1)      || \
                                 defined(CONFIG_BOARD_MATRIXPORTAL_S3)   || \
                                 defined(CONFIG_BOARD_MATRIXPORTAL_S3_WIDE) || \
                                 defined(CONFIG_BOARD_TRONBYT_S3))
```

Callers replace `#if CONFIG_BOARD_TIDBYT_GEN1 || CONFIG_BOARD_MATRIXPORTAL_S3 || CONFIG_BOARD_TRONBYT_S3` with `#if BOARD_HAS_SWAP_COLORS`. Adding a new board only updates one file.

### 3.4 Recommended fix: decouple touch from the network layer via event bus

**Current problem**: `sta_api.cpp` and `handlers.cpp` both declare `extern void touch_on_brightness_set(uint8_t)` and call it under `#ifdef CONFIG_BOARD_TIDBYT_GEN2`. The network layer must know which board has touch and hold a reference to a function in `main.cpp`.

**Proposed fix**: Post a `BOARD_EVENT_BRIGHTNESS_SET` event on the existing `event_bus` when brightness is changed. The touch controller in `main.cpp` subscribes to this event and updates its `display_power_on` / `saved_brightness` state internally.

Result:
- `sta_api.cpp` and `handlers.cpp` lose both `#ifdef CONFIG_BOARD_TIDBYT_GEN2` sites
- `touch_on_brightness_set()` is deleted; the extern declarations disappear
- The network layer becomes board-agnostic

This is the only fix with meaningful cross-file scope reduction. It eliminates 5 of the ~21 board-conditional sites (2 extern declarations + 2 call sites + 1 extern in main.cpp public scope) while actually improving the design.

### 3.5 No full HAL component

A p3a-style board component (`board_init()`, `board_buttons()`, `board_display_config()`) is not warranted here:

- There is no filesystem abstraction needed (no SD card variants)
- There is no light sensor (matrx-fw's primary use case for daughterboard)
- The display layer already works correctly for all 8 boards via the single preset chain
- The only multi-board peripheral (touch) exists on exactly one board

Introducing a full HAL would add indirection and a new component dependency for a benefit that does not exist yet.

---

## 4. Cost vs. Benefit

| Change | Files affected | `#ifdef` sites removed | Risk |
|--------|---------------|----------------------|------|
| Add `board_caps.h` | `ap.cpp`, `board_caps.h` (new) | 4 (swap into semantic macros) | Very low — purely additive |
| Event-bus brightness coupling | `main.cpp`, `sta_api.cpp`, `handlers.cpp` | 5 | Low — well-contained, event_bus already exists |
| Display pin presets refactor | `display.cpp` | 0 (no ifdef removed) | Medium risk for zero gain |
| Full HAL component | All 6 files + new component | ~21 (theoretical) | High — every board must be retested, no behavior change |

The event-bus fix plus `board_caps.h` collectively address all cross-module coupling (the only real problem) at low risk and a scope of 3 files.

---

## 5. Recommendation

**Partial go**: two targeted improvements, no full HAL.

**P1 (worth doing)**: Decouple `touch_on_brightness_set` via the event bus. The extern linkage from network handlers into touch control is the one genuinely bad coupling in the codebase.

**P2 (quality-of-life)**: Add `board_caps.h` as a thin semantic shim. Makes board capability conditions readable and keeps new-board additions in one place.

**No-go**: Full board HAL / component. Sprawl is tolerable everywhere except the touch/network coupling identified above. The display preset chain is correct and compact. No new boards are likely to add new capability dimensions (no light sensor, no SD card). The refactor cost would exceed the benefit.

---

## 6. Per-Board Regression Checklist

Any change that touches the board-conditional sites (including the recommended event-bus fix) must verify these boards still build and behave identically. The existing `.github/workflows/main.yml` CI matrix covers all build targets — use it.

| Board | Kconfig target | Touch | Swap-colors portal | Brightness ceiling | HUB75 preset |
|-------|---------------|-------|-------------------|-------------------|--------------|
| **tidbyt-gen1** | `CONFIG_BOARD_TIDBYT_GEN1` | No | Yes | 100 (Tidbyt ceiling) | Fallthrough/default |
| **tidbyt-gen2** | `CONFIG_BOARD_TIDBYT_GEN2` | Yes (GPIO33, capacitive) | No | 100 (Tidbyt ceiling) | GEN2 pin map |
| **pixoticker** | `CONFIG_BOARD_PIXOTICKER` | No | No | Board-specific | Pixoticker pin map (ESP32 classic) |
| **tronbyt-s3** | `CONFIG_BOARD_TRONBYT_S3` | No | Yes | Board-specific | S3 pin map, runtime G/B swap |
| **tronbyt-s3-wide** | `CONFIG_BOARD_TRONBYT_S3_WIDE` | No | No | Board-specific | S3 wide pin map (e=46, 64 rows) |
| **matrixportal-s3** | `CONFIG_BOARD_MATRIXPORTAL_S3` | No | Yes | Board-specific | AdaFruit pin map (e=21) |
| **matrixportal-s3-waveshare** | `CONFIG_BOARD_MATRIXPORTAL_S3_WIDE` | No | Yes | Board-specific | AdaFruit wide pin map (e=8, 128x64) |
| **waveshare-s3** | `CONFIG_BOARD_WAVESHARE_S3` | No | No | Board-specific | Waveshare S3 pin map |

**For the event-bus fix specifically**, regression focus is:
1. tidbyt-gen2: brightness commands from both STA API and HTTP handler must still toggle the display-on state in the touch controller
2. All non-gen2 boards: no behavior change (the event subscriber simply doesn't exist)

---

## 7. Open Questions

1. **`matrixportal-s3-waveshare` vs `matrixportal-s3-wide`**: The firmware uses `CONFIG_BOARD_MATRIXPORTAL_S3_WIDE` for what the plan calls "matrixportal-s3-waveshare". Confirm these are the same build target (128x64 waveshare panel on matrixportal-s3 hardware with e-line on GPIO8). If there's a distinct `CONFIG_BOARD_MATRIXPORTAL_S3_WAVESHARE` planned, it needs its own pin block and sdkconfig overlay.

2. **Swap-colors on MATRIXPORTAL_S3_WIDE**: `display.cpp` applies `swap_colors` conditional pins to `MATRIXPORTAL_S3_WIDE` but the current `board_caps.h` sketch above includes it in `BOARD_HAS_SWAP_COLORS`. Verify the portal should actually show the checkbox for this variant (is the hardware quirk present on the waveshare panel?).

3. **Future touch-capable boards**: If a second board adds touch, the event-bus approach scales naturally (subscribe to the same event). The `BOARD_HAS_TOUCH` macro would expand. No additional network layer changes needed — this validates the recommendation.

4. **`CONFIG_BOARD_TIDBYT_GEN1` default/fallthrough**: In `display.cpp`, GEN1 has no explicit `#elif` block — it falls through to a `#else` or hits the `CONFIG_HUB75_*`-only path. Confirm there is no missing GEN1 pin preset (verify the GEN1 sdkconfig.defaults sets the HUB75 pins via Kconfig directly or relies on compile-time defaults).
