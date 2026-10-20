# A9 Camera custom firmware

Open replacement firmware for the cheap "A9 mini" Wi-Fi cameras, which ship with cloud-locked stock firmware. Goals:

- **Setup access point**: on first boot, or when the home network can't be joined, the camera starts an open AP `A9-Setup-XXXX`. The web UI is at `http://192.168.4.1/`.
- **Web interface**: live preview, Wi-Fi settings, video settings, an optional admin password, reboot and factory reset.
- **RTSP stream**: `rtsp://<camera-ip>:554/live`, MJPEG over RTP (RFC 2435), over UDP or TCP. It works with VLC, ffmpeg, Frigate, Home Assistant and similar tools.

## Status

| Part | State |
|---|---|
| RTSP server (MJPEG/RTP, UDP + TCP interleaved) | Working in the host build, tested with ffmpeg |
| HTTP server, web UI, JSON API, basic auth | Working in the host build |
| Config storage, AP fallback, factory reset | Working in the host build |
| BK7252N port on Beken's FreeRTOS SDK: tasks, flash settings, Wi-Fi AP/STA, GPIO, battery ADC | Builds; not yet run on the board |
| BK7252N camera: sensor auto-detect, stock init tables, hardware JPEG | Builds; not yet run on the board |
| Audio, IR night mode, PSRAM frame buffers | TODO |

Our unit is a BK7252N board (`INNO-IPC-48N-V2.5`). [docs/device-spec.md](docs/device-spec.md) describes how the stock firmware drives it: pins, camera sensors, audio, power and provisioning. [docs/hardware.md](docs/hardware.md) covers identifying the chip, wiring and flash backup.

## Layout

```
firmware/
  lib/                   shared code for every camera, with no chip-specific code
    include/a9/hal.h     the HAL interface each MCU implements
    include/a9/board.h   board settings and their defaults
    src/                 config, Wi-Fi logic, HTTP, RTSP, RTP/JPEG
    web/index.html       web UI, embedded into the binary at build time
    tests/               unit tests
  hal/                   one directory per MCU family
    host/                Linux: simulated Wi-Fi, camera replays JPEG files
    bk7252/              Beken BK7252N on the FreeRTOS SDK (build.sh, flash.py)
  boards/                one directory per camera model
    host/
    a9-mini-bk7252/      board_config.h: MCU, pins, RAM limits, defaults
docs/
  hardware.md            chip identification, UART pads, backup and flashing
```

## Adding a camera or an MCU

- **A new camera on an MCU we already support:** add `boards/<camera>/board_config.h` with its name, MCU, pins and RAM limits, plus `board.cmake` naming the HAL. Then build with that board selected.
- **A new MCU:** add `hal/<mcu>/` implementing every function in `lib/include/a9/hal.h`, plus a `hal_net.h` that provides BSD sockets, and the build glue for that vendor's SDK. See [firmware/hal/README.md](firmware/hal/README.md).

The lib never includes vendor headers. Chip-specific code lives only in `hal/`, and per-camera values live only in `boards/`.

## Try it on a PC

```sh
cd firmware
cmake -S . -B build && cmake --build build
(cd build && ctest)
./hal/host/make_test_frames.sh         # needs ffmpeg; writes test_frames/
./build/hal/a9_host                    # web on :8080, RTSP on :8554
ffplay rtsp://127.0.0.1:8554/live
```

## HTTP API

| Method | Path | |
|---|---|---|
| GET | `/` | Web UI |
| GET | `/api/status` | Mode, IP, RSSI, uptime, RTSP URL, client count |
| GET | `/api/config` | Settings (passwords are reported only as set or not set) |
| POST | `/api/config` | Form-encoded settings. Saves them and reboots. An empty password field keeps the current value; `clear_<field>=1` removes it |
| POST | `/api/reboot`, `/api/factory-reset` | |
| GET | `/snapshot.jpg` | Latest frame |

Holding the button for 5 seconds also does a factory reset.
