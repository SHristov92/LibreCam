# HAL ports

Each directory here is one MCU family. A port provides:

1. **Every function in `lib/include/lc/hal.h`**: system and tasks, mutexes, key/value flash storage, Wi-Fi AP and STA, camera frames as baseline JPEG, and LED and button GPIO. For settings, a port can instead build `lib/src/kv_flash.c` and provide just `hal_kv_flash_read/write/erase` for one reserved 4 KB flash sector. Parts a port doesn't have yet (audio, SD card, OTA) return the "not available" values `hal.h` documents.
2. **`hal_net.h`**: includes the platform's BSD socket headers (lwIP, libc, ...) and defines `closesocket()` if the platform doesn't have it.
3. **An entry point** that calls `hal_init()` and then `lc_main()` from a task with enough stack (4 KB or more).
4. **Build glue** for the vendor SDK (SCons, Make, CMake...) that compiles `lib/src/*.c`, the port's own sources and the generated `web_ui.c` (from `lib/tools/embed_web.py`). It needs `lib/include`, the port directory and `boards/<board>` on the include path.
5. **A case in `ci/build-board.sh`** that fetches the pinned SDK and toolchain and runs the build glue. Boards are found by `ci/list-boards.sh`; a board that hasn't run on hardware sets `set(LC_BOARD_EXPERIMENTAL 1)` in its `board.cmake`, so CI builds it without letting it block the others.

Pin numbers and memory limits don't belong in a port. They go in `boards/<board>/board_config.h`, which the port reads through `lc/board.h`.

## Camera contract

`hal_camera_get_frame()` returns baseline JPEG frames (4:2:0 or 4:2:2, standard Huffman tables, 8-bit quantization) because the RTSP server carries them as RFC 2435 MJPEG. An MCU with only H.264 would need a second packetizer in the lib.

## Ports

| Port | MCU | State |
|---|---|---|
| `host` | Linux PC | Working; used for testing |
| `bk7252` | Beken BK7252N (FreeRTOS SDK) | Runs on the A13 mini |
