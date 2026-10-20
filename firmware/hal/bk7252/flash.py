#!/usr/bin/env python3
"""Flash the A9 app (or restore stock) on a BK7252N over its UART bootloader.

Only the app region is written (physical 0x11000 onwards). The bootloader at
0x0 and the config/calibration blocks at 0x1E0000+ are never touched.

  flash.py --port /dev/ttyUSB1 build/bk7252/a9_app.bin
  flash.py --port /dev/ttyUSB1 --restore-stock backup/.../flash_full_2MiB.bin

Start it, then power-cycle the camera: the script keeps sending the ROM
link-check until the chip answers (the stock firmware doesn't reboot into the
ROM by itself, and the tools' own link loop is too slow to catch it).
Needs pyserial and bk7231tools (pip install bk7231tools).
"""
import argparse
import io
import sys
import time
import zlib

import serial
from bk7231tools.serial import BK7231Serial
from bk7231tools.serial.base import BkProtocolType

APP_START = 0x11000
APP_END_STOCK = 0x1E0000      # stock app region ends where RF/config blocks start
BOOT_LEN = 0x11000
LINK_CHECK = bytes.fromhex('01e0fc0100')
LINK_REPLY = bytes.fromhex('040e0501e0fc0100')


def wait_for_rom(port, timeout):
    """Spam the link-check every few ms until the ROM bootloader answers."""
    s = serial.Serial(port, 115200, timeout=0.003)
    print(f'Waiting for the bootloader on {port} - power-cycle the camera now '
          f'(up to {timeout:.0f} s)...', flush=True)
    t0, rx = time.time(), b''
    try:
        while time.time() - t0 < timeout:
            s.write(LINK_CHECK)
            rx = (rx + s.read(256))[-64:]
            if LINK_REPLY in rx:
                print(f'Bootloader answered after {time.time() - t0:.1f} s', flush=True)
                return True
    finally:
        s.close()
    return False


def show_boot_log(port, seconds):
    print(f'--- boot log ({seconds} s) ---', flush=True)
    s = serial.Serial(port, 115200, timeout=0.2)
    t0 = time.time()
    try:
        while time.time() - t0 < seconds:
            d = s.read(4096)
            if d:
                sys.stdout.write(d.decode('utf-8', 'replace'))
                sys.stdout.flush()
    finally:
        s.close()
    print('\n--- end of boot log ---')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', help='a9_app.bin (CRC-wrapped app), or the full stock dump with --restore-stock')
    ap.add_argument('--port', default='/dev/ttyUSB1')
    ap.add_argument('--baud', type=int, default=115200, help='transfer baud rate after linking')
    ap.add_argument('--backup', help='full stock dump; its bootloader must match the chip (default: required check '
                    'uses this file, or the image itself with --restore-stock)')
    ap.add_argument('--restore-stock', action='store_true', help='write the stock app region from a full dump')
    ap.add_argument('--wait', type=float, default=120, help='seconds to wait for the bootloader')
    ap.add_argument('--log', type=float, default=30, help='seconds of boot log to show after flashing')
    a = ap.parse_args()

    data = open(a.image, 'rb').read()
    if a.restore_stock:
        if len(data) != 0x200000:
            sys.exit('--restore-stock needs the full 2 MiB dump')
        backup, payload = data, data[APP_START:APP_END_STOCK]
    else:
        if not a.backup:
            sys.exit('--backup <full stock dump> is required, to check we talk to the right chip')
        backup, payload = open(a.backup, 'rb').read(), data
    if payload[:4] != bytes.fromhex('0e0000ea'):
        sys.exit('image does not start with the BK app vector table; refusing to flash')
    if APP_START + len(payload) > APP_END_STOCK:
        sys.exit('image would overwrite the RF/config area at 0x1E0000; refusing')
    payload += b'\xff' * (-len(payload) % 0x1000)

    if not wait_for_rom(a.port, a.wait):
        sys.exit('No answer from the bootloader. Check wiring (adapter TX->RX1, RX->TX1, GND) and power-cycle '
                 'while this runs.')

    bk = BK7231Serial(a.port, baudrate=a.baud, link_timeout=10)
    bk.connect()
    flash_id = bk.flash_read_id()['id']
    print(f'protocol {bk.protocol_type.name}, flash id {flash_id.hex()}, size {bk.flash_size // 1024} KiB')
    if bk.protocol_type != BkProtocolType.FULL:
        sys.exit('bootloader protocol is not FULL; cannot unprotect flash safely, aborting before any write')

    boot_crc = bk.read_flash_range_crc(0, BOOT_LEN)
    if boot_crc != zlib.crc32(backup[:BOOT_LEN]):
        sys.exit(f'bootloader on chip (crc {boot_crc:08x}) does not match the backup '
                 f'({zlib.crc32(backup[:BOOT_LEN]):08x}); wrong device or dump - aborting')
    print('bootloader matches the backup')

    print(f'writing {len(payload)} bytes at 0x{APP_START:x}...', flush=True)
    t0 = time.time()
    done = 0
    for n in bk.program_flash(io.BytesIO(payload), len(payload), APP_START, crc_check=True):
        done += n
        print(f'\r  {done * 100 // len(payload):3d}%', end='', flush=True)
    print(f'\nwritten in {time.time() - t0:.0f} s')

    crc = bk.read_flash_range_crc(APP_START, APP_START + len(payload))
    if crc != zlib.crc32(payload):
        sys.exit(f'VERIFY FAILED: chip crc {crc:08x} != image crc {zlib.crc32(payload):08x}. '
                 f'Run again, or restore stock with --restore-stock.')
    print('verify OK, rebooting')
    bk.reboot_chip()
    bk.close()
    if a.log > 0:
        show_boot_log(a.port, a.log)


if __name__ == '__main__':
    main()
