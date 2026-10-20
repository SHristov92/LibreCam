# LibreCam: details

Everything that isn't needed to flash a camera: project status, code layout, the HTTP API, buttons and LED, and how to add cameras. For how the stock firmware of our first camera works, see [device-spec.md](device-spec.md). For chip identification and wiring, see [hardware.md](hardware.md).

## Features

- **Setup access point**: on first boot, or when the home network can't be joined, the camera starts an open AP `LibreCam-Setup-XXXX`. The web UI is at `http://192.168.4.1/`.
- **Web interface**: live preview, Wi-Fi and video settings, an optional admin password, SD card browser, reboot and factory reset.
- **RTSP stream**: `rtsp://<camera-ip>:554/live`, MJPEG over RTP (RFC 2435) plus microphone audio as G.711 A-law (PCMA, 8 kHz), over UDP or TCP. It works with VLC, ffmpeg, Frigate, Home Assistant and similar tools.
- **SD card recording**:
  - photos, from the web UI or a short press of button 1
  - timelapse images at a set interval
  - AVI clips with sound, from the web UI or by holding button 1
  - When the card fills up, the oldest files are deleted, timelapse first. The web UI lists, downloads and deletes files.
- **Clock**: the camera has no clock battery. Time comes from SNTP on a network with internet access, or from the browser whenever the web UI is open, so files are dated even on the setup AP.

## Status

| Part | State |
|---|---|
| RTSP server (MJPEG/RTP, UDP + TCP interleaved) | Done |
| HTTP server, web UI, JSON API, basic auth | Done |
| Config storage, AP fallback, factory reset | Done |
| BK7252N port on Beken's FreeRTOS SDK: tasks, flash settings, Wi-Fi AP/STA, GPIO, battery ADC | Done |
| BK7252N camera: sensor auto-detect, stock init tables, hardware JPEG | Done (GC0328C, VGA) |
| Microphone audio in RTSP (G.711 A-law, track1) | Done |
| SD card: photos, timelapse, AVI clips, cleanup, file browser | Done |
| Deep sleep on the power button, LED patterns | Done |
| IR night mode, speaker/talkback, PSRAM frame buffers | TODO |

## Buttons and LED (A13 mini)

| Button | Action |
|---|---|
| Power (button 2) held 3 s, then released | Deep sleep. The LED goes dark at 3 s to confirm. A clip in progress is saved first. Any button wakes the camera, which then boots normally. |
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

The LED can be turned off in the settings. The power-button feedback (dark, flicker) still shows.

## HTTP API

| Method | Path | |
|---|---|---|
| GET | `/` | Web UI |
| GET | `/api/status` | Mode, IP, RSSI, uptime, RTSP URL, client count, battery, sensor and stream size |
| GET | `/api/config` | Settings (passwords are reported only as set or not set) |
| POST | `/api/config` | Form-encoded settings. Saves them and reboots. An empty password field keeps the current value; `clear_<field>=1` removes it. Settings include `timelapse_s` (0 = off), `clip_s` and `led` (status LED on/off). |
| POST | `/api/reboot`, `/api/factory-reset` | |
| GET | `/snapshot.jpg` | Latest frame |
| GET | `/api/storage` | SD card: mounted, total/free MB, recording state, last file, whether the clock is set |
| GET | `/api/files?dir=/DCIM` | Files in `/DCIM`, `/TIMELAPSE` or `/VIDEO` |
| GET | `/sd/<dir>/<file>` | Download a file |
| POST | `/api/files/delete` | `path=/DCIM/IMG_...jpg` |
| POST | `/api/snapshot` | Save the current frame to `/DCIM` |
| POST | `/api/record` | `action=start` or `action=stop` for a clip of `clip_s` seconds |
| POST | `/api/time` | `utc=<unix seconds>&tz=<minutes east of UTC>`. The web UI sends this on load. It sets the clock if SNTP hasn't. |

## Layout

```
firmware/
  lib/                   shared code for every camera, with no chip-specific code
    include/lc/hal.h     the HAL interface each MCU implements
    include/lc/board.h   board settings and their defaults
    src/                 config, Wi-Fi logic, HTTP, RTSP, RTP/JPEG, audio, recorder, LED
    web/index.html       web UI, embedded into the binary at build time
    tests/               unit tests
  hal/                   one directory per MCU family
    host/                Linux: simulated Wi-Fi, camera replays JPEG files
    bk7252/              Beken BK7252N on the FreeRTOS SDK (build.sh, flash.py)
  boards/                one directory per camera model
    host/
    a13-mini-bk7252/     board_config.h: MCU, pins, RAM limits, defaults
  third_party/fatfs/     FatFs R0.15a (SD card)
docs/
  details.md             this file
  device-spec.md         how the stock A13 mini firmware works (reverse engineered)
  hardware.md            chip identification, UART pads, backup
  screenshots/
backup/                  stock flash dump and reverse-engineering work for our unit
```

## Adding a camera or an MCU

- **A new camera on an MCU we already support:** add `boards/<camera>/board_config.h` with its name, MCU, pins and RAM limits, plus `board.cmake` naming the HAL. Then build with that board selected and add a row to the board table in the README.
- **A new MCU:** add `hal/<mcu>/` implementing every function in `lib/include/lc/hal.h`, plus a `hal_net.h` that provides BSD sockets, and the build glue for that vendor's SDK. See [firmware/hal/README.md](../firmware/hal/README.md).

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

The host build fakes the hardware with environment variables:

| Variable | Fakes |
|---|---|
| `LC_SD_DIR` | Directory that stands in for the SD card |
| `LC_SD_SIZE_MB` | Card size |
| `LC_BUTTON_FILE` | File whose existence holds the power button down |
| `LC_VBAT_MV` | Battery voltage |

Their header comments are in `firmware/hal/host/hal_host.c`.
