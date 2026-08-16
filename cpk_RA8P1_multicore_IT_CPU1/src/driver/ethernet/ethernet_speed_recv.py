"""
ethernet_speed_recv.py - PC-side TCP receiver for RA8P1 speed test

Usage:
    python ethernet_speed_recv.py [port]

Default port: 5001
"""

import socket
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5001
RECV_TIMEOUT = 2.0

# Device sends repeating 1460-byte chunks: chunk[i] = i & 0xFF
CHUNK_SIZE = 1460

def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.settimeout(1.0)
    srv.bind(("0.0.0.0", PORT))
    srv.listen(1)
    print(f"[Speed Test Receiver] Listening on port {PORT} ...")
    print(f"Waiting for RA8P1 device to connect... (Ctrl+C to stop)")

    while True:
        try:
            conn, addr = srv.accept()
            break
        except socket.timeout:
            continue
        except KeyboardInterrupt:
            print("\nStopped.")
            srv.close()
            return

    conn.settimeout(RECV_TIMEOUT)
    print(f"Connected: {addr[0]}:{addr[1]}")

    total_recv = 0
    chunks = 0
    errors = 0
    first_data_time = None
    last_data_time = None

    try:
        while True:
            try:
                data = conn.recv(65536)
            except socket.timeout:
                print(f"\nNo data for {RECV_TIMEOUT}s, transfer complete.")
                break

            if not data:
                print("\nConnection closed by device.")
                break

            now = time.time()
            if first_data_time is None:
                first_data_time = now
            last_data_time = now

            # Validate: each byte should be (position_in_chunk & 0xFF)
            # position_in_chunk = (total_recv + i) % CHUNK_SIZE
            for i, b in enumerate(data):
                expected = (total_recv + i) % CHUNK_SIZE & 0xFF
                if b != expected:
                    errors += 1
                    if errors <= 5:
                        print(f"  MISMATCH at byte {total_recv + i}: "
                              f"got 0x{b:02X}, expected 0x{expected:02X}")

            total_recv += len(data)
            chunks += 1

            elapsed_so_far = now - first_data_time
            if elapsed_so_far > 0:
                speed = total_recv * 8 / elapsed_so_far / 1_000_000
                print(f"  {total_recv} bytes ({speed:.2f} Mbit/s)")

    except ConnectionResetError:
        print("\nConnection reset by device.")
    except KeyboardInterrupt:
        print("\nInterrupted by user.")

    conn.close()
    srv.close()

    if first_data_time and last_data_time and total_recv > 0:
        transfer_time = last_data_time - first_data_time
        throughput_mbps = total_recv * 8 / transfer_time / 1_000_000 if transfer_time > 0 else 0
        print(f"\n--- Results ---")
        print(f"Received:    {total_recv} bytes ({total_recv/1024:.1f} KB)")
        print(f"Chunks:      {chunks}")
        print(f"Transfer:    {transfer_time:.3f} s")
        print(f"Throughput:  {throughput_mbps:.2f} Mbit/s")
        if errors > 0:
            print(f"Data errors: {errors} bytes mismatch!")
        else:
            print(f"Data check:  OK")
    else:
        print("No data received.")

if __name__ == "__main__":
    main()
