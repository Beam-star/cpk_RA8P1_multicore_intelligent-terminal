#!/usr/bin/env python3
"""
Convert a GIF animation into ONE contiguous raw RGB565 .bin file (N frames,
each W*H*2 bytes, concatenated back-to-back) for the RA8P1 UI frame players
(lvgl_ui_anim.c). Frames are read from W25Q256 at:
    frame[i] = base_offset + i * (W*H*2)

Usage:
  python gif_to_rgb565_frames.py in.gif out.bin 130 130
  python gif_to_rgb565_frames.py in.gif out.bin 96 96 --step 4 --crop 200 100 400 400
  python gif_to_rgb565_frames.py in.gif out.bin 96 96 --bg 0C3F5E
  python gif_to_rgb565_frames.py in.gif out.bin 96 96 --bg 0C3F5E --chroma 12

  --step N       : keep 1 out of every N source frames (default 1)
  --crop X Y W H : crop this box before resizing (default: whole frame)
  --bg RRGGBB    : background colour for compositing (transparent GIFs) or
                   chroma-key replacement (opaque GIFs with --chroma)
  --chroma N     : replace pixels whose max(R,G,B) <= N with --bg (for GIFs
                   with a solid near-black background that you want tinted)
"""
import sys
import argparse
import numpy as np
from PIL import Image


def parse_bg(s):
    s = s.lstrip("#")
    if len(s) != 6:
        raise argparse.ArgumentTypeError("--bg must be RRGGBB hex")
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


def main():
    ap = argparse.ArgumentParser(description="GIF -> contiguous RGB565 frames .bin")
    ap.add_argument("gif")
    ap.add_argument("out")
    ap.add_argument("width", type=int)
    ap.add_argument("height", type=int)
    ap.add_argument("--step", type=int, default=1)
    ap.add_argument("--crop", nargs=4, type=int, metavar=("X", "Y", "W", "H"))
    ap.add_argument("--bg", type=parse_bg, default=None,
                    help="Background RRGGBB")
    ap.add_argument("--chroma", type=int, default=None,
                    help="Replace pixels with max(RGB) <= N by --bg")
    args = ap.parse_args()

    W, H = args.width, args.height
    im = Image.open(args.gif)
    has_alpha = "transparency" in im.info
    bg = args.bg if args.bg is not None else (0, 0, 0)

    frames = 0
    idx = 0
    with open(args.out, "wb") as out:
        while True:
            try:
                im.seek(idx)
            except EOFError:
                break
            if idx % args.step == 0:
                f = im.convert("RGBA")
                if args.crop:
                    x, y, w, h = args.crop
                    f = f.crop((x, y, x + w, y + h))
                f = f.resize((W, H), Image.LANCZOS)

                rgba = np.asarray(f, dtype=np.uint16)          # H x W x 4
                a = rgba[:, :, 3].astype(np.uint16)

                if has_alpha and args.bg is not None:
                    # Composite transparent pixels onto bg by alpha.
                    for c in range(3):
                        rgba[:, :, c] = (rgba[:, :, c] * a + bg[c] * (255 - a) + 127) // 255
                elif args.chroma is not None and args.bg is not None:
                    # Opaque GIF: chroma-key the near-black background.
                    mx = rgba[:, :, :3].max(axis=2)
                    mask = mx <= args.chroma
                    for c in range(3):
                        rgba[:, :, c][mask] = bg[c]
                # else: keep RGB (transparent -> black, as before)

                r = (rgba[:, :, 0] >> 3) & 0x1F
                g = (rgba[:, :, 1] >> 2) & 0x3F
                b = (rgba[:, :, 2] >> 3) & 0x1F
                rgb565 = (r << 11) | (g << 5) | b
                rgb565.astype("<u2").tofile(out)
                frames += 1
            idx += 1

    nbytes = frames * W * H * 2
    print(f"Wrote {args.out}: {frames} frames x {W}x{H}x2 = {nbytes} bytes "
          f"(0x{nbytes:X})")
    if has_alpha and args.bg is not None:
        print(f"  (transparent bg composited onto #{''.join('%02X' % c for c in bg)})")
    elif args.chroma is not None:
        print(f"  (dark bg <= {args.chroma} replaced with "
              f"#{''.join('%02X' % c for c in bg)})")


if __name__ == "__main__":
    main()
