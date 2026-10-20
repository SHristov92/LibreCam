# BK7252 / BK7252N port

This implements `lib/include/a9/hal.h` on Beken's RT-Thread based SDK for the BK7252 and BK7252N. Board-specific values (pins, RAM limits) come from `boards/<board>/board_config.h`; the default board is `a9-mini-bk7252`.

## Status

- Done but not compiled yet: tasks, mutexes, ticks, reboot (RT-Thread), and settings storage (EasyFlash ENV blobs).
- TODO: Wi-Fi AP and STA, the camera (DVP sensor, hardware JPEG, frame buffer), and the LED and button pins.

## Bring-up plan

1. Back up the stock flash (see `docs/hardware.md`) and capture the stock boot log on TX1 at 115200 baud. The log usually names the SDK version and the camera sensor.
2. Get the BK7252 RT-Thread SDK building a stock example, and flash it to confirm the toolchain and flasher work.
3. Add `firmware/` to the SDK's `applications/` tree; `SConscript` here builds the lib, this HAL and the board selected by the `A9_BOARD` environment variable.
4. Fill in Wi-Fi first. The web UI and RTSP server then run with no camera (snapshot returns 503).
5. Fill in the camera: probe the sensor over I2C, start the JPEG encoder, and keep the newest frame in a double buffer for `hal_camera_get_frame()`.
6. Trace the LED and button GPIOs on the board.

## RAM budget

Each RTSP client and the snapshot handler allocate one `A9_FRAME_BUF`, set to 48 KB in `boards/a9-mini-bk7252/board_config.h`. With 2 RTSP clients that is about 150 KB at worst. Lower `A9_RTSP_MAX_CLIENTS` there if the heap is tight, or use QVGA.
