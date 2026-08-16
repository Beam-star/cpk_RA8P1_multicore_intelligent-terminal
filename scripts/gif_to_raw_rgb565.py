#!/usr/bin/env python3
"""
Convert GIF animation frames to raw uncompressed RGB565 .bin files
for W25Q64 raw-flash boot animation on Titan-mini RA8P1.

Usage:
  python gif_to_raw_rgb565.py handsome.gif 480 270
  python gif_to_raw_rgb565.py handsome.gif 480 270 3  (skip every 3rd frame)

Output: frame_000.bin, frame_001.bin, ... in ./output_raw/ directory.
Each frame is exactly W×H×2 bytes of raw RGB565 pixel data — NO headers, NO compression.
"""

import sys
import os
from PIL import Image

def rgb888_to_rgb565(r, g, b):
    """Convert 8-bit RGB to RGB565."""
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)

    gif_path = sys.argv[1]
    width = int(sys.argv[2])
    height = int(sys.argv[3])
    step = int(sys.argv[4]) if len(sys.argv) > 4 else 1

    out_dir = "output_raw"
    os.makedirs(out_dir, exist_ok=True)

    gif = Image.open(gif_path)
    frame_count = 0
    idx = 0
    out_idx = 0

    try:
        while True:
            gif.seek(idx)
            if idx % step == 0:
                frame = gif.convert("RGB").resize((width, height), Image.LANCZOS)
                pixels = frame.load()

                # Build raw RGB565 byte array
                raw = bytearray(width * height * 2)
                pos = 0
                for y in range(height):
                    for x in range(width):
                        r, g, b = pixels[x, y]
                        rgb565 = rgb888_to_rgb565(r, g, b)
                        raw[pos] = rgb565 & 0xFF
                        raw[pos + 1] = (rgb565 >> 8) & 0xFF
                        pos += 2

                fname = os.path.join(out_dir, f"frame_{out_idx:03d}.bin")
                with open(fname, "wb") as f:
                    f.write(raw)

                print(f"  {fname}: {len(raw)} bytes ({width}×{height})")
                out_idx += 1

            idx += 1
    except EOFError:
        pass

    total = sum(os.path.getsize(os.path.join(out_dir, f))
                for f in os.listdir(out_dir) if f.endswith(".bin"))
    print(f"\nDone: {out_idx} frames, {total / 1024 / 1024:.2f} MB total")
    print(f"Flash offset per frame: {width * height * 2} bytes (0x{width * height * 2:X})")

if __name__ == "__main__":
    main()
