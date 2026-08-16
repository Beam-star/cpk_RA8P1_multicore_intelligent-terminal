#!/usr/bin/env python3
"""
font_to_bin.py — 把 Lvgl Font Tool 生成的 myChineseFont.c 拆成三个原始二进制文件，
用于烧录到 W25Q256，运行时由 CPU0 从 flash 加载后交给 LVGL。

输出（写到脚本所在目录）：
  font_bitmap.bin   glyph_bitmap[]      —— 4bpp 位图字节流
  font_unicode.bin  unicode_list_1[]    —— uint16 小端
  font_dsc.bin      glyph_dsc[]         —— 每个字形 8 字节 (LV_FONT_FMT_TXT_LARGE=0 布局)

用法: python font_to_bin.py <myChineseFont.c 路径>
"""
import re
import struct
import os
import sys


def strip_comments(s):
    s = re.sub(r'//[^\n]*', '', s)
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.DOTALL)
    return s


def extract_array(src, name):
    """返回数组声明体（去掉外层 { } 后的文本）。"""
    m = re.search(r'\b' + re.escape(name) + r'\s*\[\s*\]\s*=\s*\{(.*?)\n\};', src, re.DOTALL)
    if not m:
        raise SystemExit(f"ERROR: array '{name}' not found")
    return m.group(1)


def parse_bitmap(body):
    out = bytearray()
    for m in re.finditer(r'0[xX][0-9a-fA-F]+|\d+', strip_comments(body)):
        v = int(m.group(0), 16) if m.group(0).lower().startswith('0x') else int(m.group(0))
        if v > 0xFF:
            raise SystemExit(f"bitmap value {v} exceeds 255 (not a uint8 array?)")
        out.append(v)
    return bytes(out)


def parse_unicode(body):
    out = bytearray()
    for m in re.finditer(r'0[xX][0-9a-fA-F]+|\d+', strip_comments(body)):
        v = int(m.group(0), 16) if m.group(0).lower().startswith('0x') else int(m.group(0))
        out += struct.pack('<H', v & 0xFFFF)
    return bytes(out)


def parse_glyph_dsc(body):
    out = bytearray()
    pat = re.compile(
        r'\{\.bitmap_index\s*=\s*(\d+)\s*,\s*\.adv_w\s*=\s*(\d+)\s*,\s*'
        r'\.box_h\s*=\s*(\d+)\s*,\s*\.box_w\s*=\s*(\d+)\s*,\s*'
        r'\.ofs_x\s*=\s*(-?\d+)\s*,\s*\.ofs_y\s*=\s*(-?\d+)\s*\}')
    for m in pat.finditer(strip_comments(body)):
        bitmap_index, adv_w, box_h, box_w, ofs_x, ofs_y = map(int, m.groups())
        # LV_FONT_FMT_TXT_LARGE==0: uint32(bitmap_index:20 | adv_w:12) + box_w + box_h + ofs_x + ofs_y
        word = (bitmap_index & 0xFFFFF) | ((adv_w & 0xFFF) << 20)
        out += struct.pack('<I', word)
        out += struct.pack('B', box_w & 0xFF)
        out += struct.pack('B', box_h & 0xFF)
        out += struct.pack('b', ofs_x)   # signed int8
        out += struct.pack('b', ofs_y)
    if len(out) == 0:
        raise SystemExit("ERROR: no glyph_dsc entries parsed")
    return bytes(out)


def main():
    src_path = sys.argv[1] if len(sys.argv) > 1 else 'myChineseFont.c'
    with open(src_path, 'r', encoding='utf-8', errors='replace') as f:
        src = f.read()

    bitmap = parse_bitmap(extract_array(src, 'glyph_bitmap'))
    unicode = parse_unicode(extract_array(src, 'unicode_list_1'))
    dsc = parse_glyph_dsc(extract_array(src, 'glyph_dsc'))

    outdir = os.path.dirname(os.path.abspath(__file__))
    paths = {
        'font_bitmap.bin': bitmap,
        'font_unicode.bin': unicode,
        'font_dsc.bin': dsc,
    }
    total = 0
    for name, data in paths.items():
        p = os.path.join(outdir, name)
        with open(p, 'wb') as f:
            f.write(data)
        total += len(data)
        print(f"{name:<20s} {len(data):>10,d} bytes  (0x{len(data):X})")

    print("-" * 46)
    print(f"{'TOTAL':<20s} {total:>10,d} bytes  (0x{total:X})")

    # 打印 W25Q256 布局建议（4KB 对齐）
    base = 0xBE0000
    off = base
    for name, data in paths.items():
        print(f"  {name:<20s} @ 0x{off:07X}  size 0x{len(data):X}")
        off = (off + len(data) + 0xFFF) & ~0xFFF  # 4KB 对齐
    print(f"  (end @ 0x{off:07X})")


if __name__ == '__main__':
    main()
