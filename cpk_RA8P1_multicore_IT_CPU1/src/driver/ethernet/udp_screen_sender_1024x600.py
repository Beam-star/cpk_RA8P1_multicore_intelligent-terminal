#!/usr/bin/env python3
"""
UDP screen sender for RA8P1 Titan-mini board (1024x600 RGB565).

Captures the PC desktop screen, converts to RGB565 big-endian, and sends via UDP
to the board's UDP screen receiver (ethernet_test_udp_screen_start).

Protocol (matches ethernet_test.h):
    Header (12 bytes, big-endian):
        uint16  magic         = 0x5556
        uint32  frame_id      (wraps at 2^32)
        uint16  packet_id     (0-based index within frame)
        uint16  packet_count  (total packets per frame)
        uint16  payload_len   (bytes of pixel data in this packet)
    Payload:
        PACKET_PAYLOAD_SIZE bytes of RGB565 pixel data (big-endian)

Frame: 1024x600 RGB565 = 1,228,800 bytes = 1200 packets x 1024 bytes

Usage:
    python udp_screen_sender_1024x600.py --target-ip 192.168.1.100 --local-ip 192.168.1.10
    python udp_screen_sender_1024x600.py --target-ip 192.168.1.100 --local-ip 192.168.1.10 --port 5001 --fps 5

Dependencies:
    pip install mss pillow
"""

import argparse
import socket
import struct
import time
import sys

try:
    from mss import mss
except ImportError:
    sys.exit("Error: 'mss' not installed. Run: pip install mss")

try:
    from PIL import Image
except ImportError:
    sys.exit("Error: 'pillow' not installed. Run: pip install pillow")

try:
    RESAMPLE_BILINEAR = Image.Resampling.BILINEAR
except AttributeError:
    RESAMPLE_BILINEAR = Image.BILINEAR

# ---- Protocol constants (must match ethernet_test.h) ----
MAGIC = 0x5556
FRAME_WIDTH = 1024
FRAME_HEIGHT = 600
PACKET_PAYLOAD_SIZE = 1024
PACKET_COUNT = (FRAME_WIDTH * FRAME_HEIGHT * 2) // PACKET_PAYLOAD_SIZE  # 1200
HEADER_FORMAT = "!HIHHH"  # magic(u16) + frame_id(u32) + pkt_id(u16) + pkt_cnt(u16) + payload_len(u16)
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)  # 12 bytes
DEFAULT_PORT = 5001
DEFAULT_FPS = 15          # 目标帧率
DEFAULT_PKT_DELAY = 0.00003  # 包间延迟 30μs (1200包 * 30μs ≈ 36ms/帧 ≈ 27FPS)


try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False


def rgb888_to_rgb565_be(rgb_image: Image.Image) -> bytes:
    """Convert PIL RGB image to big-endian RGB565 bytes."""
    if HAS_NUMPY:
        return _rgb888_to_rgb565_numpy(rgb_image)
    return _rgb888_to_rgb565_python(rgb_image)


def _rgb888_to_rgb565_numpy(rgb_image: Image.Image) -> bytes:
    """NumPy vectorized conversion (100x faster than Python loop)."""
    arr = np.array(rgb_image)  # shape: (H, W, 3), dtype: uint8
    r = arr[:, :, 0].astype(np.uint16)
    g = arr[:, :, 1].astype(np.uint16)
    b = arr[:, :, 2].astype(np.uint16)
    pixel = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    # Big-endian: high byte first
    hi = (pixel >> 8).astype(np.uint8)
    lo = (pixel & 0xFF).astype(np.uint8)
    result = np.stack([hi, lo], axis=-1).reshape(-1)
    return result.tobytes()


def _rgb888_to_rgb565_python(rgb_image: Image.Image) -> bytes:
    """Fallback pure Python conversion."""
    rgb_bytes = rgb_image.tobytes()
    rgb565 = bytearray(len(rgb_bytes) // 3 * 2)
    dst = 0
    for src in range(0, len(rgb_bytes), 3):
        r = rgb_bytes[src]
        g = rgb_bytes[src + 1]
        b = rgb_bytes[src + 2]
        pixel = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        rgb565[dst] = (pixel >> 8) & 0xFF
        rgb565[dst + 1] = pixel & 0xFF
        dst += 2
    return bytes(rgb565)


def capture_frame(screen_grabber, monitor_index: int) -> bytes:
    """Capture screen and convert to RGB565."""
    monitor = screen_grabber.monitors[monitor_index]
    shot = screen_grabber.grab(monitor)
    image = Image.frombytes("RGB", shot.size, shot.rgb)
    image = image.resize((FRAME_WIDTH, FRAME_HEIGHT), RESAMPLE_BILINEAR)
    return rgb888_to_rgb565_be(image)


def send_frame(sock: socket.socket, target, frame_id: int, frame_bytes: bytes,
               pkt_delay: float = 0.0) -> None:
    """Send one frame as multiple UDP packets.

    Args:
        pkt_delay: delay between packets in seconds (e.g. 0.0002 = 200us).
                   Set > 0 to avoid overwhelming the MCU's network buffers.
    """
    for packet_id in range(PACKET_COUNT):
        offset = packet_id * PACKET_PAYLOAD_SIZE
        payload = frame_bytes[offset:offset + PACKET_PAYLOAD_SIZE]
        header = struct.pack(
            HEADER_FORMAT,
            MAGIC,
            frame_id,
            packet_id,
            PACKET_COUNT,
            len(payload),
        )
        sock.sendto(header + payload, target)
        if pkt_delay > 0:
            time.sleep(pkt_delay)


def generate_test_pattern() -> bytes:
    """Generate a 1024x600 RGB565 big-endian test pattern (color bars)."""
    frame = bytearray(FRAME_WIDTH * FRAME_HEIGHT * 2)
    # 8 vertical color bars: white, yellow, cyan, green, magenta, red, blue, black
    colors_rgb = [
        (255, 255, 255), (255, 255, 0),   (0, 255, 255), (0, 255, 0),
        (255, 0, 255),   (255, 0, 0),     (0, 0, 255),   (0, 0, 0),
    ]
    bar_width = FRAME_WIDTH // len(colors_rgb)

    for y in range(FRAME_HEIGHT):
        for x in range(FRAME_WIDTH):
            bar_idx = min(x // bar_width, len(colors_rgb) - 1)
            r, g, b = colors_rgb[bar_idx]
            pixel = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            offset = (y * FRAME_WIDTH + x) * 2
            frame[offset] = (pixel >> 8) & 0xFF
            frame[offset + 1] = pixel & 0xFF
    return bytes(frame)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Send PC desktop frames to RA8P1 Titan-mini via UDP.",
        epilog="Device side: call ethernet_test_udp_screen_start() or ethernet_test_run()."
    )
    parser.add_argument("--target-ip", required=True,
                        help="Board IP address (e.g. 192.168.1.100)")
    parser.add_argument("--local-ip", default="0.0.0.0",
                        help="PC local IP to bind (e.g. 192.168.1.10, default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"UDP target port (default: {DEFAULT_PORT})")
    parser.add_argument("--fps", type=int, default=DEFAULT_FPS,
                        help=f"Target send FPS (default: {DEFAULT_FPS})")
    parser.add_argument("--monitor", type=int, default=1,
                        help="MSS monitor index (1 = primary screen)")
    parser.add_argument("--pkt-delay", type=float, default=DEFAULT_PKT_DELAY,
                        help=f"Delay between packets in seconds (default: {DEFAULT_PKT_DELAY} = {DEFAULT_PKT_DELAY*1e6:.0f}us)")
    parser.add_argument("--test", action="store_true",
                        help="Send test color bars instead of screen capture")
    args = parser.parse_args()

    target = (args.target_ip, args.port)
    frame_interval = 1.0 / max(args.fps, 1)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
    sock.bind((args.local_ip, 0))  # 绑定本地 IP，确保从指定网卡发出
    frame_id = 0

    print(f"UDP Screen Sender -> RA8P1 Titan-mini")
    print(f"  Local:    {args.local_ip}")
    print(f"  Target:   {args.target_ip}:{args.port}")
    print(f"  Frame:    {FRAME_WIDTH}x{FRAME_HEIGHT} RGB565")
    print(f"  Packets:  {PACKET_COUNT} per frame ({PACKET_PAYLOAD_SIZE} bytes each)")
    print(f"  FPS:      {args.fps}")
    print(f"  PktDelay: {args.pkt_delay*1000:.1f} ms")
    print(f"  Monitor:  {args.monitor}")
    print(f"  Mode:     {'TEST color bars' if args.test else 'Screen capture'}")
    print(f"  Numpy:    {'YES (fast)' if HAS_NUMPY else 'NO (slow, pip install numpy)'}")
    print()

    test_pattern = generate_test_pattern() if args.test else None

    try:
        if args.test:
            print("Sending test color bars (white/yellow/cyan/green/magenta/red/blue/black)")
            while True:
                started_at = time.perf_counter()
                send_frame(sock, target, frame_id, test_pattern, args.pkt_delay)
                frame_id = (frame_id + 1) & 0xFFFFFFFF
                elapsed = time.perf_counter() - started_at
                if elapsed < frame_interval:
                    time.sleep(frame_interval - elapsed)
                if (frame_id % 10) == 0:
                    actual_fps = 1.0 / max(elapsed, 0.001)
                    print(f"\r  Frame #{frame_id} | {actual_fps:.1f} FPS", end="", flush=True)
        else:
            with mss() as screen_grabber:
                while True:
                    started_at = time.perf_counter()
                    frame_bytes = capture_frame(screen_grabber, args.monitor)
                    send_frame(sock, target, frame_id, frame_bytes, args.pkt_delay)
                    frame_id = (frame_id + 1) & 0xFFFFFFFF
                    elapsed = time.perf_counter() - started_at
                    if elapsed < frame_interval:
                        time.sleep(frame_interval - elapsed)
                    if (frame_id % 10) == 0:
                        actual_fps = 1.0 / max(elapsed, 0.001)
                        print(f"\r  Frame #{frame_id} | {actual_fps:.1f} FPS | "
                              f"{len(frame_bytes) / 1024:.0f} KB/frame", end="", flush=True)
    except KeyboardInterrupt:
        print(f"\n\nStopped after {frame_id} frames.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
