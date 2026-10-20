#!/usr/bin/env python3
"""Send a file over a serial line with XMODEM-CRC, for U-Boot's `loadx`.

    tools/xmodem.py /dev/cu.usbserial-XXXX build/ky-x1/omega.bin [baud]

Start `loadx <addr>` in U-Boot first; it prints "Ready for binary (xmodem)
download" and then sends 'C' until a sender answers. Nothing else may have
the port open while this runs, or it will eat U-Boot's replies.
"""
import os
import sys
import termios
import time

SOH, EOT, ACK, NAK, CAN = 0x01, 0x04, 0x06, 0x15, 0x18
BLOCK = 128

BAUDS = {9600: termios.B9600, 19200: termios.B19200, 38400: termios.B38400,
         57600: termios.B57600, 115200: termios.B115200}


def crc16(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
    return crc & 0xFFFF


def open_port(path: str, baud: int) -> int:
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    attr = termios.tcgetattr(fd)
    attr[0] = 0                                   # iflag: raw
    attr[1] = 0                                   # oflag
    attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attr[3] = 0                                   # lflag: no echo, no canon
    attr[4] = attr[5] = BAUDS[baud]
    attr[6][termios.VMIN] = 0
    attr[6][termios.VTIME] = 1                    # reads time out after 0.1 s
    termios.tcsetattr(fd, termios.TCSANOW, attr)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def read_byte(fd: int, timeout: float) -> int | None:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        b = os.read(fd, 1)
        if b:
            return b[0]
    return None


def send(fd: int, payload: bytes) -> None:
    # Wait for the receiver to ask for CRC mode.
    for _ in range(60):
        if read_byte(fd, 1.0) == ord("C"):
            break
    else:
        raise RuntimeError("receiver never asked for a transfer (is loadx running?)")

    blocks = [payload[i:i + BLOCK] for i in range(0, len(payload), BLOCK)]
    for n, block in enumerate(blocks, 1):
        block = block.ljust(BLOCK, b"\x1a")
        packet = bytes([SOH, n & 0xFF, 0xFF - (n & 0xFF)]) + block + \
            crc16(block).to_bytes(2, "big")
        for _ in range(10):
            os.write(fd, packet)
            reply = read_byte(fd, 5.0)
            if reply == ACK:
                break
            if reply == CAN:
                raise RuntimeError(f"receiver cancelled at block {n}")
        else:
            raise RuntimeError(f"block {n} not acknowledged")
        print(f"\r{n * BLOCK * 100 // (len(blocks) * BLOCK):3d}%", end="", flush=True)

    for _ in range(10):
        os.write(fd, bytes([EOT]))
        if read_byte(fd, 5.0) == ACK:
            print("\rsent", len(payload), "bytes")
            return
    raise RuntimeError("end of transfer not acknowledged")


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    baud = int(sys.argv[3]) if len(sys.argv) == 4 else 115200
    with open(sys.argv[2], "rb") as f:
        payload = f.read()
    fd = open_port(sys.argv[1], baud)
    try:
        send(fd, payload)
    except RuntimeError as e:
        print(f"\nxmodem: {e}", file=sys.stderr)
        return 1
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
