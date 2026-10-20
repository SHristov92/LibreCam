#!/usr/bin/env python3
"""Wrap a linked BK7252N app (.bin linked at 0x10000) into the flash format.

The flash controller checks a CRC-16 (poly 0x8005, init 0xFFFF, stored
big-endian) after every 32 bytes, so 32 logical bytes take 34 physical ones.
The result is written at physical 0x11000, after the stock/SDK bootloader.
This wrapper reproduces the stock bootloader region of our dump byte for byte.

usage: mkimage.py app.bin out.bin
"""
import sys

APP_PHYS = 0x11000
APP_MAX = 0x121000          # SDK 2 MB partition table: app 0x11000..0x132000


def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def wrap(raw):
    raw += b'\xff' * (-len(raw) % 32)
    out = bytearray()
    for i in range(0, len(raw), 32):
        blk = raw[i:i + 32]
        out += blk + crc16(blk).to_bytes(2, 'big')
    return bytes(out)


def main():
    raw = open(sys.argv[1], 'rb').read()
    if raw[:4] != bytes.fromhex('0e0000ea'):
        sys.exit('%s does not start with the BK app vector table' % sys.argv[1])
    img = wrap(raw)
    if len(img) > APP_MAX:
        sys.exit('image is %d bytes, app partition holds %d' % (len(img), APP_MAX))
    open(sys.argv[2], 'wb').write(img)
    print('%s: %d bytes (%.0f%% of app partition), flash at 0x%x'
          % (sys.argv[2], len(img), 100.0 * len(img) / APP_MAX, APP_PHYS))


if __name__ == '__main__':
    main()
