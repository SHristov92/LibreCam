# LibreCam

Open replacement firmware for cheap cloud-locked Wi-Fi cameras. The first supported camera is an "A13 mini" (Beken BK7252N); the code keeps chip and board specifics behind a HAL so other cameras can be added. Features:

- **Setup access point**: on first boot, or when the home network can't be joined, the camera starts an open AP `LibreCam-Setup-XXXX`. The web UI is at `http://192.168.4.1/`.
- **Web interface**: live preview, Wi-Fi settings, video settings, an optional admin password, reboot and factory reset.
- **RTSP stream**: `rtsp://<camera-ip>:554/live`, MJPEG over RTP (RFC 2435) plus microphone audio as G.711 A-law (PCMA, 8 kHz), over UDP or TCP. It works with VLC, ffmpeg, Frigate, Home Assistant and similar tools.
- **SD card recording**: photos (web UI or a short press of button 1), timelapse images at a set interval, and AVI clips with sound (web UI or holding button 1). When the card fills up, the oldest files are deleted, timelapse first. The web UI lists, downloads and deletes files.
- **Clock**: the camera has no clock battery. Time comes from SNTP on a network with internet access, or from the browser whenever the web UI is open, so files are dated even on the setup AP.

<p align="center"><img src="docs/screenshots/overview-light.png" width="420" alt="LibreCam web UI: live preview, status and SD card"></p>

## Screenshots

The web UI as served by the camera (A13 mini, GC0328C sensor, setup AP mode):

| Overview | Full page | Phone | Dark theme |
|---|---|---|---|
| [<img src="docs/screenshots/overview-light.png" width="180">](docs/screenshots/overview-light.png) | [<img src="docs/screenshots/web-ui-light.png" width="180">](docs/screenshots/web-ui-light.png) | [<img src="docs/screenshots/web-ui-phone.png" width="120">](docs/screenshots/web-ui-phone.png) | [<img src="docs/screenshots/overview-dark.png" width="180">](docs/screenshots/overview-dark.png) |

## Status

| Part | State |
|---|---|
| RTSP server (MJPEG/RTP, UDP + TCP interleaved) | Done |
| HTTP server, web UI, JSON API, basic auth | Done |
| Config storage, AP fallback, factory reset | Done |
| BK7252N port on Beken's FreeRTOS SDK: tasks, flash settings, Wi-Fi AP/STA, GPIO, battery ADC | Done |
| BK7252N camera: sensor auto-detect, stock init tables, hardware JPEG | Streaming on the board Done (GC0328C, VGA) |
| Microphone audio in RTSP (G.711 A-law, track1) | Done |
| SD card: photos, timelapse, AVI clips, cleanup, file browser | Done |
| IR night mode, speaker/talkback, PSRAM frame buffers | TODO |

Our unit is a BK7252N board (`INNO-IPC-48N-V2.5`). [docs/device-spec.md](docs/device-spec.md) describes how the stock firmware drives it: pins, camera sensors, audio, power and provisioning. [docs/hardware.md](docs/hardware.md) covers identifying the chip, wiring and flash backup.

## Layout

```
firmware/
  lib/                   shared code for every camera, with no chip-specific code
    include/lc/hal.h     the HAL interface each MCU implements
    include/lc/board.h   board settings and their defaults
    src/                 config, Wi-Fi logic, HTTP, RTSP, RTP/JPEG
    web/index.html       web UI, embedded into the binary at build time
    tests/               unit tests
  hal/                   one directory per MCU family
    host/                Linux: simulated Wi-Fi, camera replays JPEG files
    bk7252/              Beken BK7252N on the FreeRTOS SDK (build.sh, flash.py)
  boards/                one directory per camera model
    host/
    a13-mini-bk7252/      board_config.h: MCU, pins, RAM limits, defaults
docs/
  hardware.md            chip identification, UART pads, backup and flashing
```

## Adding a camera or an MCU

- **A new camera on an MCU we already support:** add `boards/<camera>/board_config.h` with its name, MCU, pins and RAM limits, plus `board.cmake` naming the HAL. Then build with that board selected.
- **A new MCU:** add `hal/<mcu>/` implementing every function in `lib/include/lc/hal.h`, plus a `hal_net.h` that provides BSD sockets, and the build glue for that vendor's SDK. See [firmware/hal/README.md](firmware/hal/README.md).

The lib never includes vendor headers. Chip-specific code lives only in `hal/`, and per-camera values live only in `boards/`.

## Try it on a PC

```sh
cd firmware
cmake -S . -B build && cmake --build build
(cd build && ctest)
./hal/host/make_test_frames.sh         # needs ffmpeg; writes test_frames/
./build/hal/lc_host                    # web on :8080, RTSP on :8554
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
| GET | `/api/storage` | SD card: mounted, total/free MB, recording state, last file, whether the clock is set |
| GET | `/api/files?dir=/DCIM` | Files in `/DCIM`, `/TIMELAPSE` or `/VIDEO` |
| GET | `/sd/<dir>/<file>` | Download a file |
| POST | `/api/files/delete` | `path=/DCIM/IMG_...jpg` |
| POST | `/api/snapshot` | Save the current frame to `/DCIM` |
| POST | `/api/record` | `action=start` or `action=stop` for a clip of `clip_s` seconds |
| POST | `/api/time` | `utc=<unix seconds>&tz=<minutes east of UTC>`. The web UI sends this on load. It sets the clock if SNTP hasn't. |

## Buttons and LED

| Button | Action |
|---|---|
| Power (button 2) held 3 s, then released | Deep sleep. The LED goes dark at 3 s to confirm. Any button wakes the camera, which then boots normally. A clip in progress is saved first. |
| Power held 10 s, then released | Factory reset. The LED flickers fast from 10 s. |
| Button 1, short press | Photo to the SD card |
| Button 1, held 1 s | Start or stop a clip |

| LED | State |
|---|---|
| Fast blink (5 Hz) | Booting / joining Wi-Fi |
| Slow blink (1 Hz) | Setup access point |
| Solid on | Connected to Wi-Fi |
| Double blink every second | Recording a clip |
| Short blip every 2 s | Battery low (below `BOARD_VBAT_LOW_MV`) |
| Three quick flashes | Photo saved |

The LED can be turned off in the settings. The power-button feedback (dark, flicker) still shows. Settings `timelapse_s` (0 = off), `clip_s` and `led` (status LED on/off) are part of `/api/config`.
