# LibreCam: details

Everything that isn't needed to flash a camera: project status, code layout, the HTTP API, buttons and LED, and how to add cameras. For how the stock firmware of our first camera works, see [device-spec.md](device-spec.md). For chip identification and wiring, see [hardware.md](hardware.md).

## Features

- **Setup access point**: on first boot, or when the home network can't be joined, the camera starts an open AP `LibreCam-Setup-XXXX`. The web UI is at `http://192.168.4.1/`.
- **Web interface**: a sidebar menu grouped into Camera, Storage, Network and System.
  - **Live view**: the camera's JPEG frames as MJPEG over HTTP (`/stream`), which any browser shows in a plain `<img>`. Browsers can't play RTSP, and the BK7252 has no H.264 encoder, so WebRTC or HLS aren't options; MJPEG is the same picture RTSP clients get, with no transcoding and no JavaScript player. It has no sound, and it uses as much bandwidth as an RTSP client. The board allows `LC_HTTP_STREAM_MAX_CLIENTS` live viewers (one on the A13 mini, for RAM); further browsers fall back to a still image every second.
  - **Image controls**: brightness, contrast, saturation, auto or manual exposure and gain, exposure compensation, anti-flicker (50/60 Hz), white balance presets, night mode (IR LEDs on, black and white) and orientation. They apply while streaming, so you can watch the effect, and are saved when a slider is let go. Which controls show depends on the sensor (`hal_camera_ctrls()`); the GC0328C supports all of them.
  - **Settings**: Wi-Fi (with a scan to pick the network), DHCP or static IP, setup access point, RTSP port and path, video format, recording, device name, LED and admin password. These save and restart the camera.
  - SD card browser, firmware update, restart and factory reset.
- **Firmware update (OTA)**: System > Maintenance installs a new version from the browser. "Check for update" reads the release manifest on GitHub Pages, downloads the package for this board and uploads it to the camera (the browser needs internet access, the camera doesn't); "Upload file…" sends a package you downloaded, such as `librecam-<board>-<version>.rbl` from a release. The camera checks the package and writes it to the update partition, header last, so an interrupted upload leaves nothing to install. It then restarts, and the stock bootloader decrypts, unpacks and installs it. Settings and SD card files stay.
- The UI is gzipped at build time (about 12 KB in flash) and served with `Content-Encoding: gzip`.
- **mDNS**: the camera answers to `<device-name>.local`. The name is the device name in lowercase, with anything but letters and digits turned into `-` ("Porch Cam" becomes `porch-cam.local`). It also advertises `_http._tcp` and `_rtsp._tcp` (DNS-SD), so service browsers and Home Assistant can find it. Works on macOS, iOS, Android, Windows 10+ and Linux with Avahi/nss-mdns. A small responder in `lib/src/mdns*.c`, since the SDK's lwIP doesn't build its own.
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
| SD card: photos, timelapse, AVI clips with sound, file browser | Done |
| SD card: deleting the oldest files when the card is full | Done, not yet tried on the board |
| Deep sleep on the power button, LED patterns | Done |
| Web UI live view (MJPEG), static IP, mDNS, Wi-Fi scan | Done |
| Image controls and manual night mode (GC0328C) | Done |
| Firmware update from the web UI (OTA through the stock bootloader) | Done |
| Automatic day/night, speaker/talkback, PSRAM frame buffers | TODO |

Everything marked Done has been run on the A13 mini.

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
| POST | `/api/config` | Form-encoded settings. Saves them and reboots. An empty password field keeps the current value; `clear_<field>=1` removes it. Settings include `timelapse_s` (0 = off), `clip_s`, `led` (status LED on/off), and `ip_static` (0 = DHCP, 1 = static) with `ip_addr`, `ip_mask`, `ip_gw` and `ip_dns` (empty = gateway). The image settings above are accepted too. |
| GET | `/api/scan` | Nearby Wi-Fi networks, one entry per name (its strongest access point), strongest first: `{"ok":true,"networks":[{"ssid","rssi" (dBm),"ch","secure"}]}`. Takes a few seconds; works on the setup AP without dropping its clients. Hidden networks are left out. |
| POST | `/api/reboot`, `/api/factory-reset` | |
| POST | `/api/ota` | Firmware update: the board's package (`/api/status` `ota` names the format, `null` if none) as the raw request body with a `Content-Length`. Returns `{"ok":true}` and restarts to install it, or 400 with `{"ok":false,"error":...}`. `curl --data-binary @librecam-<board>-<version>.rbl http://<camera>/api/ota` |
| POST | `/api/image` | Image settings, form-encoded: `brightness`, `contrast`, `saturation`, `ae_level` (each -4..4), `ae` (0/1), `exposure` (1..100), `gain` (0..100), `awb` (0 auto, 1 daylight, 2 cloudy, 3 fluorescent, 4 incandescent), `flicker` (50/60), `night` (0/1), `flip` (0..3). Applied at once; also saved with `save=1`. No restart. |
| GET | `/stream` | Live view: MJPEG (`multipart/x-mixed-replace`). `?fps=n` caps the frame rate. 503 when all live view slots are taken. |
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
web-flasher/             browser flasher (Web Serial): index.html, bkflash.js, tests with a simulated bootloader
docs/
  details.md             this file
  device-spec.md         how the stock A13 mini firmware works (reverse engineered)
  hardware.md            chip identification, UART pads, backup
  screenshots/
backup/                  stock flash dump and reverse-engineering work for our unit
```

## Adding a camera or an MCU

- **A new camera on an MCU we already support:** add `boards/<camera>/board_config.h` with its name, MCU, pins and RAM limits, plus `board.cmake` naming the HAL (`LC_HAL`) and the title the web flasher shows (`LC_BOARD_TITLE`). CI picks the new board up by itself. Add a row to the board table in the README.
- **A new MCU:** add `hal/<mcu>/` implementing every function in `lib/include/lc/hal.h`, plus a `hal_net.h` that provides BSD sockets, and the build glue for that vendor's SDK. See [firmware/hal/README.md](../firmware/hal/README.md). Also add a case to `ci/build-board.sh` that fetches the pinned SDK and toolchain and calls the build.

## CI

`.github/workflows/build.yml` runs on every push to `main`, on pull requests and on demand:

1. **host**: the Linux build, the unit tests, and the web flasher's tests against a simulated bootloader.
2. **list boards**: `ci/list-boards.sh` turns every `firmware/boards/*/board.cmake` whose HAL isn't `host` into a build matrix.
3. **one job per board**: `ci/build-board.sh <board>` builds `dist/librecam-<board>-<version>.bin` (to flash over UART) and `.rbl` (OTA package), each with a `.sha256`, uploaded as the `librecam-<board>` artifact. The version comes from `git describe`. The SDK and toolchain are pinned by commit and checksum in that script, and cached between runs.
4. **pages**: only on a push to `main`. `ci/pages-site.sh` puts the web flasher, the board images and OTA packages and a `firmware/manifest.json` together and deploys them to GitHub Pages (Settings > Pages > Source must be "GitHub Actions"). The images sit next to the page because release downloads can't be fetched from another origin; the camera's "Check for update" reads the same manifest.
5. **release**: only on a tag `v*`. It attaches all board images and OTA packages to a GitHub release.

`ci/build-board.sh` runs the same way locally.

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
