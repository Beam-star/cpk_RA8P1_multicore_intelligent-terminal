#!/usr/bin/env python3
"""
Calculate the binary size of a C array file (.c or .h).

Parses the first { ... } block and reports the exact byte count.
Useful for computing flash offsets when chaining multiple writes.

Usage:
    python carray_size.py sub_0000_model_data.c
    python carray_size.py sub_0000_model_data.c --next-offset 0x000000
"""

import re
import sys
import struct


def parse_c_array(filepath):
    """Parse a C array file, return raw bytes."""
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        text = f.read()

    # Remove C comments
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)
    text = re.sub(r'//[^\n]*', '', text)

    # Find the first { ... } block
    start = text.find('{')
    end = text.rfind('}')
    if start < 0 or end < start:
        raise ValueError("No C array block { ... } found")

    body = text[start + 1:end]

    # Extract all numbers (hex or decimal)
    tokens = re.findall(r'0[xX][0-9a-fA-F]+|\d+', body)
    result = bytearray()
    for tok in tokens:
        val = int(tok, 16 if tok.lower().startswith('0x') else 10)
        if val <= 0xFF:
            result.append(val)
        elif val <= 0xFFFF:
            result.extend(struct.pack('<H', val))
        elif val <= 0xFFFFFFFF:
            result.extend(struct.pack('<I', val))
        else:
            result.extend(struct.pack('<Q', val))
    return bytes(result)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    filepath = sys.argv[1]
    next_offset = None
    if len(sys.argv) >= 4 and sys.argv[2] == '--next-offset':
        next_offset = int(sys.argv[3], 16)

    data = parse_c_array(filepath)
    size = len(data)

    print(f"File:    {filepath}")
    print(f"Entries: {size:,}  bytes")
    if size >= 1024 * 1024:
        print(f"         {size / (1024*1024):.2f} MB")
    elif size >= 1024:
        print(f"         {size / 1024:.2f} KB")
    print(f"         {size:#010X}")

    if next_offset is not None:
        new_offset = next_offset + size
        print(f"\nOffset chain:")
        print(f"  Write @ {next_offset:#010X}")
        print(f"  Size   {size:#010X}  ({size:,} B)")
        print(f"  ─────────────────")
        print(f"  Next → {new_offset:#010X}")
        print(f"\n  python pcdc_flash_tool.py COM10 write {filepath} {next_offset:#010X}")
        print(f"  python pcdc_flash_tool.py COM10 write <next_file>  {new_offset:#010X}")


if __name__ == '__main__':
    main()
