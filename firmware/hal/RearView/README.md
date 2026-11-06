# Rear view camera port (BK7252N, Beken FreeRTOS SDK)

This implements `lib/include/lc/hal.h` for the "Rear view camera" (`boards/rearview-bk7252`). It started as a copy of [`hal/A13`](../A13/README.md): same chip, SDK 3.0.76, toolchain and build. It is kept separate so changes for this board can't break the A13 mini. How the stock firmware works, and where each value below comes from, is in [docs/device-spec-rearview.md](../../../docs/device-spec-rearview.md).

| File | |
|---|---|
| `hal_bk7252.c` | Tasks, mutexes, time, KV storage in flash, Wi-Fi AP and STA, GPIO, `user_main()` entry. The MAC comes from the stock RF calibration TLV (`BOARD_RF_TLV_ADDR`, `0x1FE000`). |
| `camera_bk7252.c` | **Sensor bus:** software SCCB on GPIO0/1 (`BOARD_CAM_SCCB_SOFT`), with UART2, which shares those pins, taken down. **Sensors:** every sensor of both stock firmwares, using the rear view firmware's tuning (`BOARD_STOCK_TABLES_REARVIEW`). **Image controls:** GC0311 and GC0328C. **Frames:** hardware JPEG through GDMA. **Shell:** `camreg` command. |
| `audio_bk7252.c` | Microphone: on-chip audio ADC, 8 kHz mono |
| `storage_bk7252.c` | SD card code; this board has none (`BOARD_HAS_SD 0`), so it reports no card and never touches the SDIO pins |
| `ota_bk7252.c` | Web firmware update into this bootloader's `download` partition (`BOARD_OTA_DL_ADDR`, `0x143000`) |
| `stock_sensor_tables*.h` | Sensor tables from both stock firmwares (generators in `backup/re/scripts` and `backup/re-rearview/scripts`) |
| `sdk.mk`, `build.sh` | Build glue, as in `hal/A13` |

Flashing uses the shared `firmware/tools/bk7252/flash.py` (see the [README](../../../README.md#rear-view-camera-bk7252n)). It recognises this camera's bootloader and refuses images built for another board.

## Build

```sh
ci/build-board.sh rearview-bk7252     # -> dist/librecam-rearview-bk7252-<version>.bin and .rbl
```

## Why the sensor bus is software

The stock firmware drives the sensor with the chip's I2C2 block. The SDK's I2C2 driver is interrupt-per-byte, so every byte waits for an interrupt. With this firmware's JPEG and Wi-Fi interrupt load it failed in three ways:
- it waited out 2 s timeouts on absent addresses
- it hit SCL timeouts while image controls were written
- it reported transfers as done that never happened

That left garbled registers and hangs, even with the stock driver's two differences added. Software SCCB checks every ACK and doesn't depend on interrupt timing. Details are in the spec, §4.

## Status

- **Working:** boots, joins Wi-Fi, detects the GC0311, streams 640 × 480, web UI, RTSP, microphone.
- **Being tested:** image controls on the GC0311, now over software SCCB.
- **Not yet done:** this camera's own RF calibration. The SDK reads the calibration from `0x1E0000`, which is empty here, so it uses its defaults; the stock firmware calibrates at boot and uses XTAL 49.
