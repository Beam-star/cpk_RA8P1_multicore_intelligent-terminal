#!/usr/bin/env python3
"""
PCDC Flash Transfer Tool
========================
Transfers binary data (from .c array files or raw .bin files) to the
Titan-mini RA8P1's W25Q64 QSPI Flash via the USB PCDC virtual COM port.

Protocol: framed packets with XMODEM CRC16, stop-and-wait flow control.
See: src/pcdc_flash_protocol.h for the MCU-side protocol constants.

Usage:
  python pcdc_flash_tool.py COM10 info
  python pcdc_flash_tool.py COM10 write    data.c    0x071000
  python pcdc_flash_tool.py COM10 write    model.c   0x000000
  python pcdc_flash_tool.py COM10 verify   data.c    0x071000
  python pcdc_flash_tool.py COM10 read     0x071000  256  output.bin
  python pcdc_flash_tool.py COM10 erase    0x071000  0x10000
  python pcdc_flash_tool.py COM10 dir_read
  python pcdc_flash_tool.py COM10 dir_add  my_asset  0x071000  259200
  python pcdc_flash_tool.py COM10 reset
  python pcdc_flash_tool.py ping            (auto-detect COM port)

Requirements: pip install pyserial
"""

import sys
import struct
import time
import argparse
import re
import os
from typing import Tuple, Optional, List

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial not installed. Run: pip install pyserial")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Protocol constants (must match pcdc_flash_protocol.h)
# ---------------------------------------------------------------------------

STX = 0xAA
MAX_PAYLOAD = 256

CMD_PING, CMD_INFO = 0x00, 0x01
CMD_ERASE = 0x10
CMD_WRITE = 0x20
CMD_READ = 0x30
CMD_DIR_READ, CMD_DIR_ADD = 0x40, 0x41
CMD_RESET = 0xF0
CMD_ACK, CMD_NACK = 0xFE, 0xFF

ERR_NONE = 0x00
ERR_BAD_CMD, ERR_CRC = 0x01, 0x02
ERR_FLASH_WRITE, ERR_FLASH_ERASE = 0x03, 0x04
ERR_ADDR_RANGE, ERR_BAD_LEN = 0x05, 0x07
ERR_FLASH_BUSY = 0x08

ERR_STR = {
    0x01: "bad command", 0x02: "CRC mismatch", 0x03: "flash write err",
    0x04: "flash erase err", 0x05: "address out of range", 0x07: "bad length",
    0x08: "flash busy",
}

# Flash geometry (from w25q256.h / w25q256_partition.h)
W25Q256_CAPACITY = 0x02000000  # 32 MB
SECTOR_SIZE = 0x1000           # 4 KB
PAGE_SIZE = 256                # 256 B

TIMEOUT_S = 5.0
MAX_RETRIES = 3

# ---------------------------------------------------------------------------
# CRC16 (XMODEM, polynomial 0x8005) — must match pcdc_flash_protocol.c
# ---------------------------------------------------------------------------

CRC16_TABLE = [
    0x0000, 0x8005, 0x800F, 0x000A, 0x801B, 0x001E, 0x0014, 0x8011,
    0x8033, 0x0036, 0x003C, 0x8039, 0x0028, 0x802D, 0x8027, 0x0022,
    0x8063, 0x0066, 0x006C, 0x8069, 0x0078, 0x807D, 0x8077, 0x0072,
    0x0050, 0x8055, 0x805F, 0x005A, 0x804B, 0x004E, 0x0044, 0x8041,
    0x80C3, 0x00C6, 0x00CC, 0x80C9, 0x00D8, 0x80DD, 0x80D7, 0x00D2,
    0x00F0, 0x80F5, 0x80FF, 0x00FA, 0x80EB, 0x00EE, 0x00E4, 0x80E1,
    0x00A0, 0x80A5, 0x80AF, 0x00AA, 0x80BB, 0x00BE, 0x00B4, 0x80B1,
    0x8093, 0x0096, 0x009C, 0x8099, 0x0088, 0x808D, 0x8087, 0x0082,
    0x8183, 0x0186, 0x018C, 0x8189, 0x0198, 0x819D, 0x8197, 0x0192,
    0x01B0, 0x81B5, 0x81BF, 0x01BA, 0x81AB, 0x01AE, 0x01A4, 0x81A1,
    0x01E0, 0x81E5, 0x81EF, 0x01EA, 0x81FB, 0x01FE, 0x01F4, 0x81F1,
    0x81D3, 0x01D6, 0x01DC, 0x81D9, 0x01C8, 0x81CD, 0x81C7, 0x01C2,
    0x0140, 0x8145, 0x814F, 0x014A, 0x815B, 0x015E, 0x0154, 0x8151,
    0x8173, 0x0176, 0x017C, 0x8179, 0x0168, 0x816D, 0x8167, 0x0162,
    0x8123, 0x0126, 0x012C, 0x8129, 0x0138, 0x813D, 0x8137, 0x0132,
    0x0110, 0x8115, 0x811F, 0x011A, 0x810B, 0x010E, 0x0104, 0x8101,
    0x8303, 0x0306, 0x030C, 0x8309, 0x0318, 0x831D, 0x8317, 0x0312,
    0x0330, 0x8335, 0x833F, 0x033A, 0x832B, 0x032E, 0x0324, 0x8321,
    0x0360, 0x8365, 0x836F, 0x036A, 0x837B, 0x037E, 0x0374, 0x8371,
    0x8353, 0x0356, 0x035C, 0x8359, 0x0348, 0x834D, 0x8347, 0x0342,
    0x03C0, 0x83C5, 0x83CF, 0x03CA, 0x83DB, 0x03DE, 0x03D4, 0x83D1,
    0x83F3, 0x03F6, 0x03FC, 0x83F9, 0x03E8, 0x83ED, 0x83E7, 0x03E2,
    0x83A3, 0x03A6, 0x03AC, 0x83A9, 0x03B8, 0x83BD, 0x83B7, 0x03B2,
    0x0390, 0x8395, 0x839F, 0x039A, 0x838B, 0x038E, 0x0384, 0x8381,
    0x0280, 0x8285, 0x828F, 0x028A, 0x829B, 0x029E, 0x0294, 0x8291,
    0x82B3, 0x02B6, 0x02BC, 0x82B9, 0x02A8, 0x82AD, 0x82A7, 0x02A2,
    0x82E3, 0x02E6, 0x02EC, 0x82E9, 0x02F8, 0x82FD, 0x82F7, 0x02F2,
    0x02D0, 0x82D5, 0x82DF, 0x02DA, 0x82CB, 0x02CE, 0x02C4, 0x82C1,
    0x8243, 0x0246, 0x024C, 0x8249, 0x0258, 0x825D, 0x8257, 0x0252,
    0x0270, 0x8275, 0x827F, 0x027A, 0x826B, 0x026E, 0x0264, 0x8261,
    0x0220, 0x8225, 0x822F, 0x022A, 0x823B, 0x023E, 0x0234, 0x8231,
    0x8213, 0x0216, 0x021C, 0x8219, 0x0208, 0x820D, 0x8207, 0x0202,
]


def crc16(data: bytes) -> int:
    crc = 0
    for b in data:
        crc = (crc << 8) ^ CRC16_TABLE[((crc >> 8) ^ b) & 0xFF]
    return crc & 0xFFFF


# ---------------------------------------------------------------------------
# Frame building / parsing
# ---------------------------------------------------------------------------

def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    """Build a protocol frame: STX + CMD + LEN(2B LE) + PAYLOAD + CRC16(2B LE)."""
    assert len(payload) <= MAX_PAYLOAD, f"payload too large: {len(payload)}"
    header = struct.pack("<BBH", STX, cmd, len(payload))
    body = header[1:] + payload  # CRC covers CMD + LEN + PAYLOAD
    crc = crc16(body)
    return header + payload + struct.pack("<H", crc)


def parse_frame(data: bytes) -> Tuple[int, bytes]:
    """
    Parse a received frame. Returns (cmd, payload) or raises ValueError.
    cmd = PCDC_CMD_NACK (0xFF) if frame is invalid.
    """
    if len(data) < 5 or data[0] != STX:
        raise ValueError("invalid frame: too short or bad STX")
    cmd = data[1]
    payload_len = data[2] | (data[3] << 8)
    expected = 4 + payload_len + 2
    if len(data) < expected or payload_len > MAX_PAYLOAD:
        raise ValueError(f"invalid frame: bad length ({payload_len})")
    # CRC check
    body = data[1:4 + payload_len]
    computed = crc16(body)
    received = data[4 + payload_len] | (data[5 + payload_len] << 8)
    if computed != received:
        raise ValueError(f"CRC mismatch: computed 0x{computed:04X}, got 0x{received:04X}")
    payload = data[4:4 + payload_len]
    return cmd, payload


# ---------------------------------------------------------------------------
# Serial communication
# ---------------------------------------------------------------------------

class PCDCSerial:
    def __init__(self, port: str, baudrate: int = 115200, timeout: float = TIMEOUT_S):
        self.ser = serial.Serial(port, baudrate, timeout=timeout)
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        time.sleep(0.1)  # let DTR/RTS settle

    def close(self):
        self.ser.close()

    def send_frame(self, cmd: int, payload: bytes = b"") -> bytes:
        """Send a frame and wait for ACK/NACK response. Returns response payload."""
        frame = build_frame(cmd, payload)
        for attempt in range(MAX_RETRIES):
            self.ser.reset_input_buffer()
            self.ser.write(frame)
            self.ser.flush()
            # Read response frame
            resp = self._read_response()
            if resp is None:
                if attempt < MAX_RETRIES - 1:
                    print(f"  Retry {attempt + 1}/{MAX_RETRIES}...")
                    time.sleep(0.2)
                continue
            rcmd, rpayload = resp
            if rcmd == CMD_NACK:
                ec = rpayload[0] if len(rpayload) > 0 else 0xFF
                msg = ERR_STR.get(ec, f"unknown(0x{ec:02X})")
                raise RuntimeError(f"NACK: {msg}")
            if rcmd == CMD_ACK:
                return rpayload
            # Unexpected response
            print(f"  WARNING: unexpected response cmd 0x{rcmd:02X}")
            continue
        raise RuntimeError("No response (timeout)")

    def _read_response(self) -> Optional[Tuple[int, bytes]]:
        """Read one complete frame from serial, or None on timeout."""
        # Read until we find STX
        raw = bytearray()
        start = time.time()
        while True:
            b = self.ser.read(1)
            if not b:
                if time.time() - start > TIMEOUT_S:
                    return None
                continue
            if b[0] == STX:
                raw.append(b[0])
                break
            # else: echo data, skip

        # Read the rest of the header (CMD + LEN = 3 bytes)
        raw += self.ser.read(3)
        if len(raw) < 4:
            return None

        payload_len = raw[2] | (raw[3] << 8)
        # Read payload + CRC
        remaining = payload_len + 2
        raw += self.ser.read(remaining)
        if len(raw) < 4 + payload_len + 2:
            return None

        try:
            return parse_frame(bytes(raw))
        except ValueError as e:
            print(f"  Frame parse error: {e}")
            return None


# ---------------------------------------------------------------------------
# C array file parser
# ---------------------------------------------------------------------------

def parse_c_array_file(path: str) -> bytes:
    """
    Parse a .c file containing a C array initializer.
    Extracts all hex/decimal values between the first '{' and matching '}'.
    Handles 0xNN, 0xNNNN, decimal, comments, and line continuations.
    """
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        text = f.read()

    # Strip comments
    text = re.sub(r'//.*', '', text)
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)

    # Find the array body between { and };
    start = text.find('{')
    if start < 0:
        raise ValueError(f"No '{{' found in {path} — not a C array file?")
    end = text.rfind('}')
    if end < start:
        raise ValueError(f"No '}}' after '{{' in {path}")

    body = text[start + 1:end]

    # Extract all numbers
    values = []
    for m in re.finditer(r'0x[0-9a-fA-F]+|[0-9]+', body):
        s = m.group(0)
        if s.startswith('0x') or s.startswith('0X'):
            v = int(s, 16)
        else:
            v = int(s)
        if v > 255:
            # Split multi-byte values into little-endian bytes
            # (e.g., a uint16 array element)
            while v > 0:
                values.append(v & 0xFF)
                v >>= 8
            if v == 0 and len(s) > 3:
                # For values like 0x1234 in a uint16 array, it's 2 bytes LE
                pass
        else:
            values.append(v)

    if not values:
        raise ValueError(f"No numeric values found in {path}")

    return bytes(values)


def parse_bin_file(path: str) -> bytes:
    """Read a raw .bin file."""
    with open(path, 'rb') as f:
        return f.read()


def load_data(path: str) -> bytes:
    """Auto-detect file type and load binary data."""
    if path.endswith('.c') or path.endswith('.h'):
        return parse_c_array_file(path)
    else:
        return parse_bin_file(path)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_info(dev: PCDCSerial) -> None:
    """Query flash geometry."""
    resp = dev.send_frame(CMD_INFO)
    total, sector, page, max_payload = struct.unpack("<IIHH", resp[:12])
    print(f"Flash capacity:    {total / 1024 / 1024:.1f} MB ({total} bytes)")
    print(f"Sector size:       {sector} bytes ({sector / 1024:.0f} KB)")
    print(f"Page size:         {page} bytes")
    print(f"Max payload/frame: {max_payload} bytes")


def cmd_erase(dev: PCDCSerial, addr: int, size: int) -> None:
    """Erase flash range."""
    payload = struct.pack("<II", addr, size)
    print(f"Erasing 0x{addr:08X} +{size} ({size / 1024:.1f} KB)...")
    dev.send_frame(CMD_ERASE, payload)
    print("Done.")


def cmd_write(dev: PCDCSerial, path: str, addr: int) -> None:
    """Write file data to flash."""
    data = load_data(path)
    print(f"File:  {path}")
    print(f"Size:  {len(data)} bytes ({len(data) / 1024:.1f} KB)")
    print(f"Addr:  0x{addr:08X}")
    print(f"Chunks: {(len(data) + MAX_PAYLOAD - 5) // (MAX_PAYLOAD - 4)} "
          f"({MAX_PAYLOAD - 4} B payload each)")

    if addr % PAGE_SIZE != 0:
        print(f"WARNING: address 0x{addr:08X} is not page-aligned "
              f"({PAGE_SIZE} B). Write speed may suffer.")

    written = 0
    t0 = time.time()
    last_pct = -1

    while written < len(data):
        chunk = data[written:written + MAX_PAYLOAD - 4]
        chunk_addr = addr + written
        hdr = struct.pack("<I", chunk_addr)
        dev.send_frame(CMD_WRITE, hdr + chunk)
        written += len(chunk)

        pct = written * 100 // len(data)
        if pct > last_pct:
            elapsed = time.time() - t0
            speed = written / elapsed / 1024 if elapsed > 0 else 0
            eta = (len(data) - written) / (speed * 1024) if speed > 0 else 0
            print(f"  {pct:3d}%  {written / 1024:.0f}/{len(data) / 1024:.0f} KB  "
                  f"{speed:.1f} KB/s  ETA {eta:.0f}s")
            last_pct = pct

    elapsed = time.time() - t0
    print(f"Done. {len(data)} bytes in {elapsed:.1f}s "
          f"({len(data) / elapsed / 1024:.1f} KB/s)")


def cmd_verify(dev: PCDCSerial, path: str, addr: int) -> None:
    """Verify flash content against file."""
    data = load_data(path)
    print(f"Verifying {path} ({len(data)} bytes) against flash @ 0x{addr:08X}...")

    errors = 0
    offset = 0
    while offset < len(data):
        size = min(MAX_PAYLOAD, len(data) - offset)
        payload = struct.pack("<IH", addr + offset, size)
        resp = dev.send_frame(CMD_READ, payload)
        if resp != data[offset:offset + size]:
            for i in range(size):
                exp = data[offset + i]
                got = resp[i] if i < len(resp) else -1
                if exp != got:
                    print(f"  MISMATCH @ 0x{addr + offset + i:08X}: "
                          f"expected 0x{exp:02X}, got 0x{got:02X}")
                    errors += 1
                    if errors >= 10:
                        print("  ... too many errors, stopping")
                        sys.exit(1)
        offset += size
        if offset % (MAX_PAYLOAD * 64) == 0:
            print(f"  {offset * 100 // len(data)}%")

    if errors == 0:
        print(f"VERIFY PASS — {len(data)} bytes match")
    else:
        print(f"VERIFY FAIL — {errors} mismatches")
        sys.exit(1)


def cmd_read(dev: PCDCSerial, addr: int, size: int, output: str) -> None:
    """Read flash data to file."""
    data = bytearray()
    offset = 0
    print(f"Reading {size} bytes from 0x{addr:08X}...")
    while offset < size:
        chunk_size = min(MAX_PAYLOAD, size - offset)
        payload = struct.pack("<IH", addr + offset, chunk_size)
        resp = dev.send_frame(CMD_READ, payload)
        data += resp
        offset += len(resp)
        if offset % (MAX_PAYLOAD * 64) == 0:
            print(f"  {offset * 100 // size}%")

    with open(output, 'wb') as f:
        f.write(data)
    print(f"Saved {len(data)} bytes to {output}")


def cmd_dir_read(dev: PCDCSerial) -> None:
    """Read and display the asset directory."""
    resp = dev.send_frame(CMD_DIR_READ)
    if len(resp) < 8:
        print("Directory too small — likely empty or unprogrammed.")
        return

    magic = struct.unpack_from("<I", resp, 0)[0]
    count = struct.unpack_from("<I", resp, 4)[0]
    print(f"Directory magic: 0x{magic:08X} {'(OK)' if magic == 0x41535354 else '(INVALID)'}")
    print(f"Entry count:     {count}")
    print()

    if count == 0:
        print("(empty)")
        return

    entry_size = 32 + 4 + 4 + 4  # name(32) + offset(4) + size(4) + crc32(4) = 44
    print(f"{'Name':<32s} {'Offset':>10s} {'Size':>10s} {'CRC32':>10s}")
    print("-" * 66)
    for i in range(min(count, 127)):
        off = 8 + i * entry_size
        if off + entry_size > len(resp):
            break
        name = resp[off:off + 32].rstrip(b'\x00').decode('ascii', errors='replace')
        e_off = struct.unpack_from("<I", resp, off + 32)[0]
        e_size = struct.unpack_from("<I", resp, off + 36)[0]
        e_crc = struct.unpack_from("<I", resp, off + 40)[0]
        print(f"{name:<32s} 0x{e_off:08X} {e_size:>10d} 0x{e_crc:08X}")


def cmd_dir_add(dev: PCDCSerial, name: str, offset: int, size: int) -> None:
    """Add an entry to the asset directory."""
    name_bytes = name.encode('ascii', errors='replace')[:31].ljust(32, b'\x00')
    payload = name_bytes + struct.pack("<II", offset, size)
    print(f"Adding '{name}' offset=0x{offset:08X} size={size}...")
    dev.send_frame(CMD_DIR_ADD, payload)
    print("Done.")


def cmd_reset(dev: PCDCSerial) -> None:
    """Reboot MCU."""
    print("Sending RESET...")
    try:
        dev.send_frame(CMD_RESET)
    except RuntimeError:
        pass  # MCU may reset before sending ACK
    print("Device should reboot now.")


def cmd_ping(dev: PCDCSerial) -> None:
    """Ping the device."""
    resp = dev.send_frame(CMD_PING)
    version = resp.rstrip(b'\x00').decode('ascii', errors='replace')
    print(f"Device responds: {version}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def auto_port() -> Optional[str]:
    """Auto-detect a Renesas PCDC COM port (VID:045B PID:5001)."""
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x045B and p.pid == 0x5001:
            return p.device
    return None


def parse_int(s: str) -> int:
    """Parse hex (0x...) or decimal integer."""
    s = s.strip()
    if s.startswith('0x') or s.startswith('0X'):
        return int(s, 16)
    return int(s)


def main():
    parser = argparse.ArgumentParser(
        description="PCDC Flash Transfer Tool for Titan-mini RA8P1")
    parser.add_argument("port", nargs="?", default=None,
                        help="COM port (e.g. COM10). Auto-detected if omitted.")
    parser.add_argument("--port", "-p", dest="port_opt", default=None,
                        help=argparse.SUPPRESS)  # hidden alias

    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("info", help="Query flash geometry")
    sub.add_parser("ping", help="Ping device")

    p_write = sub.add_parser("write", help="Write file to flash")
    p_write.add_argument("file", help="Input file (.c array or .bin)")
    p_write.add_argument("addr", help="Flash target address (hex or decimal)")

    p_verify = sub.add_parser("verify", help="Verify flash against file")
    p_verify.add_argument("file", help="Input file (.c array or .bin)")
    p_verify.add_argument("addr", help="Flash address to compare")

    p_read = sub.add_parser("read", help="Read flash to file")
    p_read.add_argument("addr", help="Flash start address")
    p_read.add_argument("size", help="Bytes to read (hex or decimal)")
    p_read.add_argument("output", help="Output file path")

    p_erase = sub.add_parser("erase", help="Erase flash range")
    p_erase.add_argument("addr", help="Start address")
    p_erase.add_argument("size", help="Bytes to erase")

    sub.add_parser("dir_read", help="Read asset directory")
    p_dir_add = sub.add_parser("dir_add", help="Add asset directory entry")
    p_dir_add.add_argument("name", help="Asset name (max 31 chars)")
    p_dir_add.add_argument("offset", help="Flash offset")
    p_dir_add.add_argument("size", help="Asset size in bytes")

    sub.add_parser("reset", help="Reboot MCU")

    args = parser.parse_args()

    port = args.port or args.port_opt or auto_port()
    if not port:
        print("ERROR: No COM port specified and no Renesas PCDC device found.")
        print("Available ports:")
        for p in serial.tools.list_ports.comports():
            print(f"  {p.device} — {p.description} "
                  f"(VID:{p.vid or '?'} PID:{p.pid or '?'})")
        sys.exit(1)

    print(f"Connecting to {port}...")
    dev = PCDCSerial(port)

    try:
        if args.command == "info":
            cmd_info(dev)
        elif args.command == "ping":
            cmd_ping(dev)
        elif args.command == "write":
            cmd_write(dev, args.file, parse_int(args.addr))
        elif args.command == "verify":
            cmd_verify(dev, args.file, parse_int(args.addr))
        elif args.command == "read":
            cmd_read(dev, parse_int(args.addr), parse_int(args.size), args.output)
        elif args.command == "erase":
            cmd_erase(dev, parse_int(args.addr), parse_int(args.size))
        elif args.command == "dir_read":
            cmd_dir_read(dev)
        elif args.command == "dir_add":
            cmd_dir_add(dev, args.name,
                        parse_int(args.offset), parse_int(args.size))
        elif args.command == "reset":
            cmd_reset(dev)
    finally:
        dev.close()


if __name__ == "__main__":
    main()
