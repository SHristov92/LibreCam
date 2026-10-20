# Hardware

## Which chip is in an "A9 mini"?

"A9" is a case style that many vendors sell, not one product, and the boards inside vary. Community teardowns report:

- **Beken BK7252 / BK7252N**: the most common. One documented board, marked `INO-IPC-A9-V2.4` (Shenzhen Pinmei / Linklemo), has a BK7252NQN481 with 2 MB SPI flash (ID `EB6015`), plus UART pads `TX1`/`RX1` and JTAG pads `TDI`/`TDO`/`TCK`/`TMS`. It has a hardware JPEG encoder and no H.264 encoder.
- **Taixin TXW817**: found in some similar-looking cameras. It uses a different SDK and different tools. It would be added as a second MCU port under `firmware/hal/`.

To identify yours, open the case and read the marking on the largest chip. A sharp photo of both sides of the board also shows the pads, sensor ribbon and flash chip.

## Tools

- A **3.3V** USB-to-UART adapter (CH340, CP2102 or FT232). Never use 5V logic.
- A fine-tip soldering iron and thin wire, or pogo pins or clips.
- A multimeter to find GND and 3V3.
- Optional: a separate 3.3V supply if the adapter can't power the board while Wi-Fi is on.

## Wiring (BK7252)

| Adapter | Camera |
|---|---|
| TX | RX1 |
| RX | TX1 |
| GND | GND |
| 3V3 (or battery/USB power) | 3V3 |

The flasher puts the chip into download mode over UART while the chip is reset or power-cycled.

## Back up first

Before writing anything, read the full 2 MB flash and keep the file. It is the only way back to stock, and it holds calibration data (RF/MAC) that custom firmware may need.

- **Easy Flasher** (BK7231GUIFlashTool, from the OpenBeken project) reads these chips in BK7252 and BK7252N modes.
- **BKFIL** also accepts BK7252N.

## References

- Elektroda teardown of the BK7252N A9 mini: https://www.elektroda.com/news/news4118499.html
- Community firmware work for BK7252 cameras: https://github.com/daniel-dona/beken7252-opencam
- Home Assistant thread on BK7252 mini cameras: https://community.home-assistant.io/t/improving-performance-of-a-mini-wifi-camera-chipset-bk7252/460543

## Findings from our unit (2026-10-20)

Backup of the full flash and the stock boot log is in `backup/` (git-ignored: it holds Wi-Fi credentials and vendor cloud keys).

- Chip: BK7252N, SCTRL_CHIP_ID `0x7252a`, device ID `0x24318000`, ROM bootloader banner `BK7252N_1.0.14`. Flash: 2 MiB.
- Stock firmware: RT-Thread 3.1.0, Beken SDK 3.0.76, vendor app (XCIoT / xcthings cloud) `sw_ver=3.00.21.01_251205`. UART console at 115200 baud with an `msh />` shell.
- The firmware has drivers for GC0307/8/9, GC0310/12, GC0328C/0329C, GC2145 and OV7740. Which sensor this board has is still unknown, because the camera never got far enough in boot to probe it.
- Battery firmware: it reads VBAT on an ADC (3309 mV here), has Button1 and Button2, and deep-sleeps with GPIO5 as the wake source whenever "power status is off".
- No RF calibration was ever written (no TLV, TXPWR or TXID in flash), so the stock firmware uses default TX power and xtal values. Our firmware can do the same.

| Physical offset | Size | Content |
|---|---|---|
| `0x000000` | `0x11000` | Bootloader (FAL partition table at `0xd364`) |
| `0x011000` | to about `0x1A3000` | App, with a CRC16 after every 32 bytes; logical address `0x10000` |
| `0x1E0000` | 4 KB | RF data (empty) |
| `0x1E1000` | 4 KB | Saved station Wi-Fi (SSID, PSK, PMK) |
| `0x1F0000` | 4 KB | BLE bonding (empty) |
| `0x1F4000` | 4 KB | Vendor JSON: MAC, product and device keys |
| `0x1F5000` | 20 KB | Vendor INI (`[iot]` did/signkey, cloud servers), MAC copy, Wi-Fi copy, last reboot reason |

### Entering the ROM bootloader

ltchiptool and bk7231tools didn't link on their own. What worked: send the link-check frame `01 E0 FC 01 00` every few ms while power-cycling the board. Once the ROM answers `04 0E 05 01 E0 FC 01 00`, run `ltchiptool flash read ... bk7251` and it connects straight away (ltchiptool doesn't know chip ID `0x7252a` and the bootloader CRC, but it still works). A full 2 MiB read at 115200 baud takes about 3.3 minutes.
