#!/usr/bin/env python3
"""
filter_font.py — 从「Lvgl Font Tool」生成的 myChineseFont.c 中删除生僻字，
重新生成三个 .bin 文件，使位图 ≤ 1MB（避开 LVGL 20-bit bitmap_index 截断）。

保留策略：
  - ASCII（码点 < 0x80）
  - GB2312 一级字库（3755 个常用汉字）
删除：
  - GB2312 二级字库（3008 个生僻汉字）及之外的字形

用法:
    python filter_font.py <myChineseFont.c 路径>

输出（覆盖 scripts/ 下的）:
    font_bitmap.bin   font_unicode.bin   font_dsc.bin
并打印更新 myChineseFont.c 所需的宏值 + 下载命令。
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
    out = []
    for m in re.finditer(r'0[xX][0-9a-fA-F]+|\d+', strip_comments(body)):
        v = int(m.group(0), 16) if m.group(0).lower().startswith('0x') else int(m.group(0))
        out.append(v & 0xFFFF)
    return out


def parse_glyph_dsc(body):
    pat = re.compile(
        r'\{\.bitmap_index\s*=\s*(\d+)\s*,\s*\.adv_w\s*=\s*(\d+)\s*,\s*'
        r'\.box_h\s*=\s*(\d+)\s*,\s*\.box_w\s*=\s*(\d+)\s*,\s*'
        r'\.ofs_x\s*=\s*(-?\d+)\s*,\s*\.ofs_y\s*=\s*(-?\d+)\s*\}')
    out = []
    for m in pat.finditer(strip_comments(body)):
        bitmap_index, adv_w, box_h, box_w, ofs_x, ofs_y = map(int, m.groups())
        out.append((bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y))
    return out


def gb2312_level1():
    """GB2312 一级字库：0xB0A1..0xD7F9，共 3755 个常用汉字（Unicode 字符集）。"""
    s = set()
    for hi in range(0xB0, 0xD8):
        end = 0xFF if hi < 0xD7 else 0xFA   # 末段 hi=0xD7 到 lo=0xF9
        for lo in range(0xA1, end):
            try:
                s.add(bytes([hi, lo]).decode('gb2312'))
            except UnicodeDecodeError:
                pass
    return s


def main():
    src_path = sys.argv[1] if len(sys.argv) > 1 else 'myChineseFont.c'
    with open(src_path, 'r', encoding='utf-8', errors='replace') as f:
        src = f.read()

    bitmap  = parse_bitmap(extract_array(src, 'glyph_bitmap'))
    unicode = parse_unicode(extract_array(src, 'unicode_list_1'))
    dsc     = parse_glyph_dsc(extract_array(src, 'glyph_dsc'))

    n = min(len(unicode), len(dsc))
    print(f"parsed: unicode={len(unicode)} dsc={len(dsc)} bitmap={len(bitmap)} bytes -> using {n} glyphs")

    # 自检：所有字形位图大小之和 == 位图总长（验证 4bpp 步长公式）
    total = 0
    for bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y in dsc[:n]:
        total += ((box_w + 1) // 2) * box_h
    assert total == len(bitmap), f"size mismatch: sum={total} != bitmap={len(bitmap)}"
    print("self-check OK: glyph size sum == bitmap length")

    L1 = gb2312_level1()
    keep = lambda c: (c < 0x80) or (chr(c) in L1)

    new_bitmap = bytearray()
    new_unicode = []
    new_dsc = []
    off = 0
    dropped = 0
    for i in range(n):
        code = unicode[i]
        bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y = dsc[i]
        size = ((box_w + 1) // 2) * box_h
        if keep(code):
            new_bitmap += bitmap[bitmap_index:bitmap_index + size]
            new_unicode.append(code)
            new_dsc.append((off, adv_w, box_w, box_h, ofs_x, ofs_y))
            off += size
        else:
            dropped += 1

    ok = len(new_bitmap) < 0x100000
    print(f"kept {len(new_unicode)} glyphs, dropped {dropped}")
    print(f"new bitmap size: {len(new_bitmap)} bytes (0x{len(new_bitmap):X}) -> {'OK < 1MB' if ok else 'STILL >= 1MB!'}")

    # 组装 dsc（LV_FONT_FMT_TXT_LARGE==0 布局，8 字节/字形）
    dsc_bin = bytearray()
    for bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y in new_dsc:
        word = (bitmap_index & 0xFFFFF) | ((adv_w & 0xFFF) << 20)
        dsc_bin += struct.pack('<I', word)
        dsc_bin += struct.pack('B', box_w & 0xFF)
        dsc_bin += struct.pack('B', box_h & 0xFF)
        dsc_bin += struct.pack('b', ofs_x)
        dsc_bin += struct.pack('b', ofs_y)

    unicode_bin = struct.pack('<%dH' % len(new_unicode), *new_unicode)

    outdir = os.path.dirname(os.path.abspath(__file__))
    paths = {
        'font_bitmap.bin':  bytes(new_bitmap),
        'font_unicode.bin': unicode_bin,
        'font_dsc.bin':     bytes(dsc_bin),
    }
    for name, data in paths.items():
        with open(os.path.join(outdir, name), 'wb') as f:
            f.write(data)
        print(f"  wrote {name:<18s} {len(data):>9,d} bytes (0x{len(data):X})")

    print()
    print("=== 更新 src/lvgl_ui/myChineseFont.c ===")
    print(f"CN_FONT_GLYPH_COUNT    {len(new_unicode)}")
    print(f"CN_FONT_BITMAP_SIZE    0x{len(new_bitmap):X}")
    print(f"CN_FONT_UNICODE_SIZE   0x{len(unicode_bin):X}")
    print(f"CN_FONT_DSC_SIZE       0x{len(dsc_bin):X}")

    print()
    print("=== 下载到 W25Q256（地址不变） ===")
    print("python scripts/pcdc_flash_tool.py COMx write scripts/font_bitmap.bin  0xBE0000")
    print("python scripts/pcdc_flash_tool.py COMx write scripts/font_unicode.bin 0xD11000")
    print("python scripts/pcdc_flash_tool.py COMx write scripts/font_dsc.bin     0xD15000")


if __name__ == '__main__':
    main()
