#!/bin/bash
# Asset pipeline: renders .star files via Pixlet and copies .webp originals
# into resources/webp/ — the directory CMake reads at build time.
#
# Usage: ./build.sh
#
# The webp/ directory is checked into git. Run this script whenever you
# modify a .star source or add a new brand/asset. CMake handles the
# webp → C byte array conversion automatically during firmware builds.
#
# Directory structure:
#   sources/{brand}/boot/boot.webp   — original boot animation (1x only)
#   sources/{brand}/boot/boot.star   — starlark boot animation (renders 1x + 2x)
#   sources/common/{name}/{name}.star — common asset (renders 1x + 2x)
#
# Source priority: .webp takes precedence over .star (original assets preferred).
# Requirements: pixlet (tronbyt fork with canvas/2x support)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCES_DIR="$SCRIPT_DIR/sources"
WEBP_DIR="$SCRIPT_DIR/webp"

# ── Pixlet check ────────────────────────────────────────────────────
ensure_pixlet() {
  if command -v pixlet &>/dev/null; then
    echo "pixlet: $(pixlet version 2>/dev/null || echo 'found')"
    return 0
  fi
  echo "ERROR: pixlet not found. Install the tronbyt fork:"
  echo "  https://github.com/tronbyt/pixlet"
  exit 1
}

# ── Find source file (.webp preferred over .star) ────────────────────
find_source() {
  local dir="$1"
  local name="$2"
  for ext in webp star; do
    local f="$dir/$name/$name.$ext"
    if [ -f "$f" ]; then
      echo "$f"
      return 0
    fi
  done
  return 1
}

# ── Process a single asset ───────────────────────────────────────────
# .webp → copy (1x only)
# .star → render 1x + 2x
process_asset() {
  local src="$1"
  local out_base="$2"

  if [[ "$src" == *.webp ]]; then
    echo "  Copy   $(basename "$src") -> ${out_base}.webp"
    cp "$src" "$WEBP_DIR/${out_base}.webp"
  elif [[ "$src" == *.star ]]; then
    echo "  Render $(basename "$src") -> ${out_base}.webp (1x)"
    pixlet render "$src" -o "$WEBP_DIR/${out_base}.webp"
    echo "  Render $(basename "$src") -> ${out_base}_2x.webp (2x)"
    pixlet render "$src" -2 -o "$WEBP_DIR/${out_base}_2x.webp"
  fi
}

# ── Main ──────────────────────────────────────────────────────────────
main() {
  ensure_pixlet
  mkdir -p "$WEBP_DIR"

  echo "=== Asset Pipeline ==="
  echo ""

  # Auto-discover brands (any directory under sources/ except 'common')
  echo "--- Boot animations ---"
  for brand_dir in "$SOURCES_DIR"/*/; do
    local brand
    brand=$(basename "$brand_dir")
    [ "$brand" = "common" ] && continue

    local src
    src=$(find_source "$brand_dir" "boot") || {
      echo "  SKIP: No boot source for $brand"
      continue
    }
    process_asset "$src" "${brand}_boot"
  done

  # Auto-discover common assets (any subdirectory under sources/common/)
  echo ""
  echo "--- Common assets ---"
  if [ -d "$SOURCES_DIR/common" ]; then
    for asset_dir in "$SOURCES_DIR/common"/*/; do
      local asset_name
      asset_name=$(basename "$asset_dir")

      local src
      src=$(find_source "$SOURCES_DIR/common" "$asset_name") || {
        echo "  SKIP: No source for $asset_name"
        continue
      }
      process_asset "$src" "$asset_name"
    done
  fi

  echo ""
  echo "=== Done ==="
  echo ""
  for f in "$WEBP_DIR"/*.webp; do
    [ -f "$f" ] || continue
    local size
    size=$(stat -c%s "$f" 2>/dev/null || stat -f%z "$f")
    printf "  %-30s %6d bytes\n" "$(basename "$f")" "$size"
  done
  echo ""
  echo "Commit webp/ to git. CMake converts to C at build time."
}

main "$@"
