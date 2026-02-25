#!/usr/bin/env python3
"""Create a TBUP bundle file for OTA updates.

Bundle format (TBUP):
  Offset  Size  Field
  0       4     Magic: "TBUP"
  4       4     App size (uint32 LE)
  8       4     WebUI size (uint32 LE, 0 = app-only)
  12      4     Reserved/flags (0)
  16      N     App firmware binary
  16+N    M     WebUI LittleFS image (optional)

Usage:
  python create_bundle.py firmware.bin --webui webui.bin -o bundle.bin
  python create_bundle.py firmware.bin -o bundle.bin  # app-only
"""

import argparse
import struct
import sys
from pathlib import Path

TBUP_MAGIC = b"TBUP"
HEADER_SIZE = 16
# ESP app descriptor magic at offset 32 (image header + segment header)
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_OFFSET = 32


def validate_app(data: bytes) -> None:
    if len(data) < APP_DESC_OFFSET + 4:
        print(f"Error: app binary too small ({len(data)} bytes)", file=sys.stderr)
        sys.exit(1)
    magic = struct.unpack_from("<I", data, APP_DESC_OFFSET)[0]
    if magic != APP_DESC_MAGIC:
        print(
            f"Error: app binary has bad magic 0x{magic:08X} at offset {APP_DESC_OFFSET} "
            f"(expected 0x{APP_DESC_MAGIC:08X}). Is this a valid firmware.bin?",
            file=sys.stderr,
        )
        sys.exit(1)


def main() -> None:
    parser = argparse.ArgumentParser(description="Create a TBUP OTA bundle")
    parser.add_argument("app", type=Path, help="App firmware binary (firmware.bin)")
    parser.add_argument("--webui", type=Path, default=None, help="WebUI LittleFS image")
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output bundle file")
    args = parser.parse_args()

    app_data = args.app.read_bytes()
    validate_app(app_data)

    webui_data = b""
    if args.webui:
        webui_data = args.webui.read_bytes()

    header = struct.pack("<4sIII", TBUP_MAGIC, len(app_data), len(webui_data), 0)

    args.output.write_bytes(header + app_data + webui_data)

    total = HEADER_SIZE + len(app_data) + len(webui_data)
    print(f"Bundle created: {args.output} ({total} bytes)")
    print(f"  App:   {len(app_data)} bytes")
    if webui_data:
        print(f"  WebUI: {len(webui_data)} bytes")
    else:
        print("  WebUI: (none)")


if __name__ == "__main__":
    main()
