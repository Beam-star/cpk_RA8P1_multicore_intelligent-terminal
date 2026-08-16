#!/usr/bin/env python3
"""
Convert an image (JPG/PNG/...) to a raw, headerless RGB565 .bin for the
Titan-mini RA8P1 W25Q64 asset store (LVGL images / boot logo).

Output format: width*height*2 bytes, RGB565, little-endian — NO header, NO
compression. Matches what boot_logo.c / lvgl_ui load via memory-mapped XIP.

The image is fit "contain" (aspect preserved) onto a width x height canvas and
centered; empty margins are filled with the background color. If the image is
already exactly width x height, it is used as-is.

Usage:
  python img_to_rgb565.py logo.jpg 1024 600 logo.bin
  python img_to_rgb565.py logo.jpg 1024 600 logo.bin --bg 000000
  python img_to_rgb565.py icon.png 40 40 icon.bin --stretch

Requirements: pip install pillow numpy
"""

import sys
import argparse

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow not installed. Run: pip install pillow")
    sys.exit(1)

try:
    import numpy as np
except ImportError:
    print("ERROR: numpy not installed. Run: pip install numpy")
    sys.exit(1)


def parse_bg(s: str):
    """Parse a 'RRGGBB' hex color into an (r, g, b) tuple."""
    s = s.lstrip("#")
    if len(s) != 6:
        raise argparse.ArgumentTypeError("--bg must be RRGGBB hex, e.g. 000000")
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


def main():
    ap = argparse.ArgumentParser(description="Image -> raw RGB565 .bin")
    ap.add_argument("image", help="Input image (JPG/PNG/...)")
    ap.add_argument("width", type=int, help="Output width in pixels")
    ap.add_argument("height", type=int, help="Output height in pixels")
    ap.add_argument("output", help="Output .bin path")
    ap.add_argument("--bg", type=parse_bg, default=(0, 0, 0),
                    help="Background color RRGGBB for letterbox margins (default 000000)")
    ap.add_argument("--stretch", action="store_true",
                    help="Stretch to fill (ignore aspect ratio) instead of contain-fit")
    args = ap.parse_args()

    W, H = args.width, args.height
    src = Image.open(args.image).convert("RGB")

    if src.size == (W, H):
        canvas = src
    elif args.stretch:
        canvas = src.resize((W, H), Image.LANCZOS)
    else:
        # Contain-fit: scale to fit inside WxH, center on a background canvas
        scale = min(W / src.width, H / src.height)
        nw, nh = max(1, round(src.width * scale)), max(1, round(src.height * scale))
        resized = src.resize((nw, nh), Image.LANCZOS)
        canvas = Image.new("RGB", (W, H), args.bg)
        canvas.paste(resized, ((W - nw) // 2, (H - nh) // 2))

    arr = np.asarray(canvas, dtype=np.uint16)          # H x W x 3
    r = (arr[:, :, 0] >> 3) & 0x1F
    g = (arr[:, :, 1] >> 2) & 0x3F
    b = (arr[:, :, 2] >> 3) & 0x1F
    rgb565 = (r << 11) | (g << 5) | b                  # H x W, uint16
    rgb565.astype("<u2").tofile(args.output)           # little-endian raw

    nbytes = W * H * 2
    print(f"Wrote {args.output}: {nbytes} bytes (0x{nbytes:X}) for {W}x{H} RGB565")
    print(f"Flash it, then register as an asset. Example (boot logo):")
    print(f"  python pcdc_flash_tool.py COM10 erase 0x071000 0x{((nbytes + 0xFFFF) & ~0xFFFF):X}")
    print(f"  python pcdc_flash_tool.py COM10 write {args.output} 0x071000")
    print(f"  python pcdc_flash_tool.py COM10 dir_add logo 0x071000 {nbytes}")


if __name__ == "__main__":
    main()
