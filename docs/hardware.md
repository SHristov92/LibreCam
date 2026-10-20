# Hardware

## Which chip is in an "A9 mini"?

"A9" is a case style that many vendors sell, not one product, and the boards inside vary. Community teardowns report:

- **Beken BK7252 / BK7252N**: the most common. One documented board, marked `INO-IPC-A9-V2.4` (Shenzhen Pinmei / Linklemo), has a BK7252NQN481 with 2 MB SPI flash (ID `EB6015`), plus UART pads `TX1`/`RX1` and JTAG pads `TDI`/`TDO`/`TCK`/`TMS`. It has a hardware JPEG encoder and no H.264 encoder.
- **Taixin TXW817**: found in some similar-looking cameras. It uses a different SDK and different tools. The `hal.h` split exists so this can be a second `platform/` port if needed.

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
