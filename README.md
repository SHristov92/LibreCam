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
| BK7252 port: tasks, timers, flash storage | Written against RT-Thread APIs, not yet compiled |
| BK7252 port: Wi-Fi, camera sensor + JPEG, GPIO | TODO, needs the board in hand |

The hardware isn't confirmed for this specific unit yet. See [docs/hardware.md](docs/hardware.md) to identify the chip.

## Layout

```
firmware/
  include/hal.h          chip abstraction: tasks, flash KV, Wi-Fi, camera, GPIO
  src/                   portable application (config, Wi-Fi logic, HTTP, RTSP, RTP/JPEG)
  web/index.html         web UI, embedded into the binary at build time
  platform/host/         Linux HAL: simulated Wi-Fi, camera replays JPEG files
  platform/bk7252/       BK7252/BK7252N HAL for Beken's RT-Thread SDK (in progress)
  tests/                 unit tests
docs/
  hardware.md            chip identification, UART pads, backup and flashing
```

## Try it on a PC

```sh
cd firmware
cmake -S . -B build && cmake --build build
(cd build && ctest)
./tools/make_test_frames.sh            # needs ffmpeg; writes test_frames/
./build/a9_host                        # web on :8080, RTSP on :8554
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
