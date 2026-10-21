# BK7252N port (Beken FreeRTOS SDK)

This implements `lib/include/lc/hal.h` on Beken's FreeRTOS SDK
([beken_freertos_sdk_release](https://github.com/YanMinge/beken_freertos_sdk_release), SDK 3.0.76,
`CFG_SOC_NAME == SOC_BK7252N`). The public RT-Thread SDKs predate the N variant. Board values (pins, RAM limits) come from `boards/<board>/board_config.h`.

| File | |
|---|---|
| `hal_bk7252.c` | Tasks, mutexes, time, KV storage in flash, Wi-Fi AP and STA, GPIO, battery ADC, `user_main()` entry |
| `camera_bk7252.c` | Sensor auto-detect over I2C1 (stock firmware order), stock init tables, hardware JPEG through GDMA into a double frame buffer; `camreg` shell command |
| `audio_bk7252.c` | Microphone: on-chip audio ADC, 8 kHz mono |
| `storage_bk7252.c` | SD card: `third_party/fatfs` (FatFs R0.15a, `lcff_` symbol prefix) on the SDK's SDIO block driver, card detect on GPIO12, one global lock |
| `stock_sensor_tables.h` | Generated from the stock firmware by `backup/re/scripts/extract_sensor_tables.py` |
| `sdk.mk` | Adds our sources to the SDK's `application.mk` |
| `build.sh` | Copies the SDK to `build/bk7252/sdk`, hooks in `sdk.mk`, builds, and wraps the image |
| `mkimage.py` | Adds the per-32-byte CRC-16 the flash controller expects |
| `flash.py` | Writes the app at `0x11000` over the UART bootloader, verifies it, and shows the boot log |

## Build

You need:

- the SDK checkout
- the 64-bit GNU Arm toolchain 10.3-2021.10. The SDK's own GCC 5.4 is a 32-bit build.

Then run:

```sh
BK_SDK=~/workspace/refs/beken_freertos_sdk_release \
FREERTOS_EXEC_PATH=~/tools/gcc-arm-none-eabi-10.3-2021.10/bin/ \
firmware/hal/bk7252/build.sh
```

The outputs are `firmware/build/bk7252/lc_app.bin`, for flashing over UART, and `lc_app.rbl`, the OTA package for the web UI's firmware update. The version (`git describe`, or `LC_VERSION`) goes into both.

## OTA

The stock bootloader embeds RT-Thread OTA. At boot it looks for an RBL package in the FAL `download` partition (`0x132000`, `0xAE000` bytes), decrypts it, inflates it into the app partition, checks it and erases the package. Its strings say it refuses unencrypted packages and CRC-wrapped images. `build.sh` packs the plain `out/bk7231_bsp.bin` with the SDK's `tools/rtt_ota/rt_ota_packaging_tool_cli-x64` (gzip, AES-256-CBC with the SDK's default key and IV, which the bootloader carries) and fails if the package doesn't fit the partition.

`ota_bk7252.c` streams the upload into the partition and keeps the 96-byte header in RAM. It checks the header (magic, CRC, partition `app`, encrypted, sizes), verifies the body's CRC32 from a flash read-back, and only then writes the header. A partial or corrupt upload therefore never looks valid to the bootloader. Tested on the A13 mini.

## Flash

The [web flasher](../../../web-flasher/) (published at https://shristov92.github.io/LibreCam/) does the same as `flash.py` from a browser over Web Serial: `bkflash.js` ports the bk7231tools BootROM protocol, and `web-flasher/test/` runs it against a simulated bootloader with the stock dump (`node web-flasher/test/test.mjs`). The CI `pages` job publishes it with the latest images from `main`; the repository's Pages source must be set to GitHub Actions.

The script waits for the ROM bootloader. Start it, then power-cycle the camera:

```sh
~/tools/bkflash/bin/python -I firmware/hal/bk7252/flash.py --port /dev/ttyUSB1 \
  --backup backup/a13-mini-bk7252-C8478C61BA0E/flash_full_2MiB.bin \
  firmware/build/bk7252/lc_app.bin
```

`~/tools/bkflash` is a venv with `pip install bk7231tools pyserial`. Before and after writing, the script checks that:

- the chip's bootloader CRC matches the backup, or, without `--backup`, the known Beken `bootloader_bk7252n_uart1_v1.0.14` (its code, `0x0`–`0xE0FC`)
- the image doesn't reach the RF/config area at `0x1E0000`
- the written region's CRC32 matches the image

To go back to stock, pass the full dump with `--restore-stock`. That rewrites only the stock app region (`0x11000`–`0x1E0000`).

## Flash layout

The layout matches both the stock firmware and the SDK's 2 MB partition table.

| Offset | |
|---|---|
| `0x000000` | Bootloader. The stock one is identical to the SDK's `bootloader_bk7252n_uart1_v1.0.14.bin`. Never written. |
| `0x011000` | App, CRC-wrapped, linked at `0x10000` (partition up to `0x132000`) |
| `0x132000` | OTA download partition (`0xAE000`): the update package while it waits for the bootloader |
| `0x1E0000` | RF calibration (SDK) |
| `0x1E1000` | Net parameters (SDK) |
| `0x1F0000` | BLE bonding (SDK) |
| `0x1F4000`–`0x1F8FFF` | Stock vendor config, left alone |
| `0x1FD000` | Our settings (`hal_kv_*`), one 4 KB sector |

## Status and known gaps

- **Hardware status:** boots, runs the AP and web UI, streams the GC0328C at VGA with working image controls and night mode, streams microphone audio, records photos and clips with sound to the SD card, scans Wi-Fi, and updates itself over the web. Not yet seen on the board: the cleanup that deletes the oldest files when the card is full.
- **FatFs:** the SDK's own FatFs is a read-only music-player subset, so we vendor FatFs R0.15a with long names and exFAT. Its symbols carry an `lcff_` prefix so they can't collide with the SDK's copy.
- **Live sensor tuning:** on the UART console, `camreg <reg> [value]` (hex) reads or writes the detected sensor's registers. On GalaxyCore parts, select the page with `camreg fe 0` first.
- **BLE stays enabled.** It's unused, but the SDK's `CFG_SUPPORT_BLE=0` configuration doesn't compile.
- **RAM:** about 350 KB of heap is free at boot and about 230 KB with the camera streaming (measured), with 48 KB frame buffers and 2 RTSP clients. The external QSPI PSRAM (memory-mapped at `0x40000000`) is still unused.
- **Image controls (GC0328C):** brightness, contrast, saturation, auto/manual exposure and gain, AE target, anti-flicker, white balance presets and mirror/flip are register writes after GalaxyCore's reference driver; defaults are the stock table's values. Checked on the board. If one misbehaves, `camreg` reads the registers back (`camreg fe 0` selects page 0). Other sensors get only the IR LED switch for now.
- **MAC address:** the SDK reads the MAC from the RF calibration area, and this camera doesn't store it there, so the SDK falls back to its one built-in default (`C8:47:8C:42:88:48`) on every unit: same address, same `LibreCam-Setup-8848`. `hal_init()` replaces that default before Wi-Fi starts with the camera's own MAC from the stock config JSON (`MAC_ADDR` at `BOARD_VENDOR_MAC_JSON`, `0x1F4000`). Without one it makes a random, locally administered MAC once and keeps it in the settings sector. The boot log names the source (`[lc] MAC … (stock config)`).
- **Static IP:** the SDK's `bk_wlan_start()` forces DHCP unless `CFG_WLAN_FAST_CONNECT_STATIC_IP` is set, so `hal_wifi_start_sta()` sets the static address with `ip_address_set()` right after starting the connection, before the link comes up.
- **Not implemented yet:** JPEG quality (the encoder's bitrate control decides), automatic day/night switching, speaker playback.
