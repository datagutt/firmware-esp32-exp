# Asset Pipeline

Embedded display assets — boot animations and error/status screens rendered on the LED matrix.

## Quick Start

```bash
# Render all .star sources into webp/ (requires pixlet)
./build.sh

# Firmware build converts webp/ → C byte arrays automatically via CMake
```

## Directory Structure

```
resources/
├── build.sh              # Renders sources → webp/
├── sources/              # Editable source files
│   ├── tronbyt/boot/     # Tronbyt boot animation (boot.webp — original)
│   ├── windytron/boot/   # Windytron boot animation (boot.webp — original)
│   ├── parrot/boot/      # Party Parrot boot animation (boot.star — starlark)
│   └── common/           # Shared assets (starlark)
│       ├── config/       # "SETUP MODE" screen
│       ├── error_404/    # "404 NOT FOUND" screen
│       ├── no_connect/   # "NO WIFI" screen
│       └── oversize/     # "TOO LARGE" screen
└── webp/                 # Rendered WebPs (checked into git, CMake reads these)
    ├── tronbyt_boot.webp
    ├── windytron_boot.webp
    ├── parrot_boot.webp      # 1x (64x32)
    ├── parrot_boot_2x.webp   # 2x (128x64)
    ├── config.webp
    ├── config_2x.webp
    └── ...
```

## Source Formats

| Format | Used for | 2x support | Requires |
|--------|----------|------------|----------|
| `.webp` | Original hand-crafted animations | 1x only (firmware upscales) | Nothing |
| `.star` | Starlark animations via Pixlet | Native 1x + 2x via `canvas` API | [tronbyt/pixlet](https://github.com/tronbyt/pixlet) |

When both exist, `.webp` takes priority over `.star`.

## Writing Starlark Assets

Use the `canvas` module for resolution-aware rendering:

```starlark
load("render.star", "render", "canvas")

def main(config):
    scale = 2 if canvas.is2x() else 1
    title_font = "10x20" if canvas.is2x() else "6x13"
    sub_font = "6x13" if canvas.is2x() else "tom-thumb"

    return render.Root(
        delay = 150 // scale,  # halve delay at 2x
        child = render.Box(
            color = "#000",
            child = render.Column(
                expanded = True,
                main_align = "center",
                cross_align = "center",
                children = [
                    render.Text("HELLO", color = "#00d4ff", font = title_font),
                ],
            ),
        ),
    )
```

Preview locally:

```bash
pixlet serve resources/sources/common/config/config.star      # 1x
pixlet serve resources/sources/common/config/config.star -2    # 2x
```

## Adding a New Brand

1. Create `resources/sources/{brand}/boot/boot.star` (or `boot.webp`)
2. Run `./build.sh`
3. Add `CONFIG_BOOT_WEBP_{BRAND}=y` to `brands/{brand}.cfg`
4. Add the Kconfig choice entry in `main/Kconfig.projbuild` under "Boot Animation"

## Adding a New Common Asset

1. Create `resources/sources/common/{name}/{name}.star`
2. Run `./build.sh`
3. The asset is automatically available via `asset_find("{name}")` in C code

## How CMake Integration Works

1. `tools/generate_assets.py` scans `resources/webp/` at configure time
2. Generates `asset_data.h` with `#if` conditionals for boot brand + panel resolution
3. Generates `asset_registry.inc` with registry entries for `assets.cpp`
4. Generates `asset_jobs.txt` listing all webp → C conversion jobs
5. CMake reads the jobs list and creates `add_custom_command` rules
6. `tools/webp_to_c.h.py` converts each `.webp` to a `_c` byte array in the build dir

No `_c` files are committed to git — they're build artifacts.
