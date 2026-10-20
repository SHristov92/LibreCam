# BK7252 / BK7252N port

The application in `src/` only talks to hardware through `include/hal.h`. This port implements that HAL on Beken's RT-Thread based SDK for the BK7252.

## Status

- Done but not compiled yet: tasks, mutexes, ticks, reboot (RT-Thread), and settings storage (EasyFlash ENV blobs).
- TODO: Wi-Fi AP and STA, the camera (DVP sensor, hardware JPEG, frame buffer), and the LED and button pins.

## Bring-up plan

1. Back up the stock flash (see `docs/hardware.md`) and capture the stock boot log on TX1 at 115200 baud. The log usually names the SDK version and the camera sensor.
2. Get the BK7252 RT-Thread SDK building a stock example, and flash it to confirm the toolchain and flasher work.
3. Add `firmware/` to the SDK's `applications/` tree; `SConscript` here builds it with `A9_PLATFORM_BK7252` defined.
4. Fill in Wi-Fi first. The web UI and RTSP server then run with no camera (snapshot returns 503).
5. Fill in the camera: probe the sensor over I2C, start the JPEG encoder, and keep the newest frame in a double buffer for `hal_camera_get_frame()`.
6. Trace the LED and button GPIOs on the board.

## RAM budget

Each RTSP client and the snapshot handler allocate one `FRAME_BUF`, set to 48 KB for this target in `SConscript`. With the default limit of 2 RTSP clients that is about 150 KB at worst. Lower `MAX_CLIENTS` in `src/rtsp_server.c` if the heap is tight, or use QVGA.
