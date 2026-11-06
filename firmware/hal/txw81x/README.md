# TXW81x port (Taixin SDK via OpenTXW81X)

This implements `lib/include/lc/hal.h` for the Taixin TXW817/TXW818, a C-SKY CK803 Wi-Fi SoC with a DVP camera interface and a hardware JPEG encoder (no H.264), as used in many A9-style mini cameras. The SDK is Taixin's, as published in [OpenTXW81X](https://github.com/NonPIayerCharacter/OpenTXW81X) (pinned in `ci/build-board.sh`). It runs AliOS Rhino behind Taixin's OSAL and lwIP, and the repository carries the Linux `csky-elfabiv2` GCC 6.3 toolchain.

**State:** written against the SDK's headers and the vendor demo, not yet compiled locally or run on hardware. CI builds it as an experimental board (`txw817-cam`).

| File | |
|---|---|
| `hal_txw81x.c` | `main()` entry, tasks, mutexes, time, settings flash for `lib/src/kv_flash.c`, MAC, Wi-Fi AP and STA with scan, GPIO |
| `camera_txw81x.c` | Sensor power and auto-detect through the SDK (`csi_cfg`), hardware JPEG, frame pump from the SDK stream framework into two frame buffers |
| `stubs_txw81x.c` | Audio, SD card and OTA: report "not available" for now |
| `sdk.mk` | Adds our sources to OpenTXW81X's `project/Makefile` |
| `build.sh` | Copies the SDK and this tree to `build/<board>`, unpacks the toolchain, builds `lc_app.elf` / `lc_app.bin` |

## Build

```sh
git clone https://github.com/NonPIayerCharacter/OpenTXW81X ~/refs/OpenTXW81X
TXW_SDK=~/refs/OpenTXW81X firmware/hal/txw81x/build.sh
# or: ci/build-board.sh txw817-cam
```

`lc_app.bin` is the raw linked image. The vendor flow then runs `project/BinScript.exe` and `makecode.exe` (Windows programs; OpenBeken runs them in WSL on a Windows runner) to add the loader and the board's parameter block and to make the compressed OTA image. That step isn't wired up yet, and it should start from the parameter block in the camera's own dump, which holds board settings.

## What's still open

- **Packaging** (above), so the image can be flashed.
- **Board values** in `boards/txw817-cam/board_config.h`: LED, buttons, IR LED and the settings sector, all from the stock dump and the PCB. Pin numbers are the SDK's `enum pin_name` (PA0 = 0, PB0 = 16, PC0 = 32).
- **Settings sector**: without `BOARD_KV_ADDR` the port uses the 4 KB just below the SDK's syscfg sectors and prints the address at boot. Check it against the dump before trusting it.
- **Camera**: resolution, quality, flip and image controls; the SDK picks the size from the sensor driver.
- **Audio** (SDK `audio_adc`), **SD card** (SDK `sdhost`), **OTA** (`libota_write_fw`), **deep sleep** and **battery ADC**.

## Reading and writing the flash

The TXW817 has no UART bootloader that tools know of, and its flash is inside the chip (1 MB on the TXW817-810, 2 MB on the -824), so neither `flash.py` nor a CH341A clip works. It is read and written over the chip's two-wire C-SKY debug port with a CK-Link probe ([Elektroda guide](https://www.elektroda.com/news/news4123724.html)):

1. **Probe:** a CK-Link Lite, bought, or built from an STM32F103 "Blue Pill" ([how](https://www.elektroda.com/rtvforum/topic4120455.html)).
2. **Wiring:** GND to GND, PA9 (debug clock) to Blue Pill A5, PA10 (debug data) to Blue Pill A1. Power the camera from its own USB. Posts disagree on which of PA9/PA10 is clock, so swap them if the probe doesn't connect. On some boards PA10 also drives the blue LED.
3. **Software:** T-Head/C-SKY Flash Programmer (Windows) with the TXW81x flash algorithm ELF from the guide. Local JTAG tab: ICE clock 1200 kHz.
4. **Read:** Advance → Read, Bin, length 1 MB (0x100000) from 0x0, press Read. Keep that file: it is the only way back to stock.
5. **Write:** remove the Hex entry, add the Bin at address 0x0, Advance → Erase Type: Chip Erase, enable program and verify, Start.

The UART pads (TX, 921600 baud on most boards) show the stock boot log, which names the SDK version and the sensor.
