# A9 mini camera (BK7252N): device specification

How the stock "A9 mini" camera works, reverse engineered from one unit's flash dump. Everything here comes from the stock firmware unless a line says otherwise. Use it as the reference for our replacement firmware.

**Unit:** MAC `C8:47:8C:61:BA:0E`. Board ID string in firmware: `INNO-IPC-48N-V2.5`. Stock firmware `3.00.21.01_251205`.

**Evidence:** addresses are CPU addresses in the app image (file `backup/a9-mini-bk7252-C8478C61BA0E/parts/01_app_nocrc_logical.bin`, mapped at `0x10000`). The Ghidra decompilation is `backup/re/app.c`, with literal pools resolved in `app_r.c`. `backup/re/scripts/q.py fn <addr>` prints a function. Register maps for the BK7252N come from Beken's FreeRTOS SDK (`beken_freertos_sdk_release`, `CFG_SOC_NAME == SOC_BK7252N`), because the public RT-Thread SDK predates the N variant.

**Confidence tags:**
- **[fw]**: read from the stock firmware.
- **[sdk]**: from the Beken SDK, for the BK7252N.
- **[obs]**: seen on the running unit (UART log).
- **[inf]**: inferred, not yet confirmed on hardware.

---

## 1. SoC, memory and boot

| Item | Value | |
|---|---|---|
| SoC | Beken BK7252N, 48-pin package (board `INNO-IPC-48N-V2.5`), chip ID `0x7252a`, device ID `0x24318000` | [obs] |
| CPU | ARM968E-S (ARMv5TE); the app is almost entirely Thumb | [fw] |
| Flash | 2 MiB SPI NOR, executed in place through the Beken CRC layer (16-bit CRC after every 32 bytes) | [obs] |
| SRAM | `0x400000`, holding `.data`, `.bss` and the heap | [fw] |
| PSRAM | external QSPI PSRAM; the QSPI controller is at `0xA02000` and memory is mapped at `0x40000000` (driver `0x9dbc0`) | [fw] |
| OS | RT-Thread 3.1.0, Beken SDK 3.0.76, lwIP 2.0.2, FAL 0.4.0, ROMFS, elm-FAT on the SD card | [obs] |
| Console | UART1 (GPIO10 RX / GPIO11 TX), 115200 8N1, `msh />` shell | [obs] |

### Boot

1. The ROM bootloader (`BK7252N_1.0.14`) prints its banner and opens a short UART download window (link check `01 E0 FC 01 00`, reply `04 0E 05 01 E0 FC 01 00`).
2. The second-stage bootloader is at `0x0`. It holds the FAL partition table at `0xd364`, runs the RT-Thread OTA check and jumps to `os_addr(0x10000)`. The stock unit always logs "App verify failed! Need to recovery factory firmware" and boots the app anyway. [obs]
3. The app sets up Wi-Fi with no calibration data (no TLV, TXPWR or TXID in flash, so it uses default TX power and `xtal=50`). It then starts lwIP, Wi-Fi STA and AP devices, FAL, ROMFS and the watchdog (10 s), and enters `xc_main`. [obs]

### Flash map (physical offsets)

| Offset | Size | Content |
|---|---|---|
| `0x000000` | `0x11000` | Bootloader |
| `0x011000` | to about `0x1A3000` | App (CRC-wrapped; logical `0x10000`) |
| `0x132000` | `0xAE000` | FAL `download` partition (OTA staging; overlaps the end of the app image area) |
| `0x1E0000` | 4 KB | RF calibration (empty on this unit) |
| `0x1E1000` | 4 KB | Beken station config: SSID, PSK, PMK |
| `0x1F0000` | 4 KB | BLE bonding (empty) |
| `0x1F4000` | 4 KB | Vendor JSON: `MAC_ADDR`, `PRODUCT_KEY`, `PRODUCT_SECRET`, `DEVICE_NAME`, `DEVICE_SECRET`, `PRODUCT_ID` |
| `0x1F5000` | 4 KB | Vendor INI `[iot]`: `did`, `signkey`, `lslat`, `scode`, `gdomain`, `gipaddr` |
| `0x1F6000` | 4 KB | Magic `44 33 22 11` + MAC string |
| `0x1F7000` | 4 KB | Magic `44 33 22 11` + SSID (32 bytes) + password: the provisioned Wi-Fi |
| `0x1F8000` | 4 KB | Last reboot reason, e.g. `reboot reason:DEV net abnormal, reboot time:<ms>` |

---

## 2. Pin map

All GPIO numbers are BK7252N `GPIOn`. "Second function" means the pin is handed to a peripheral through `CMD_GPIO_ENABLE_SECOND` (`0xcaa0002`).

| GPIO | Function | Direction / level | Evidence |
|---|---|---|---|
| 0, 1 | UART2 (unused by the app) | | [sdk] |
| 2 | Status LED | output, 1 = on; blinks while connecting | `0x86a78` / `0x86a84` [fw] |
| 4 | Button1 | input pull-up, active low | `0x8686c` [fw] |
| 5 | Button2: power and setup button; deep-sleep wake source | input pull-up, active low; wakes on falling edge | `0x8686c`, `0x86308` [fw] [obs] |
| 7 | IR illuminator LEDs | output, 1 = IR on (night) | `0x86ab0` / `0x86ac4` [fw] |
| 9 | Speaker amplifier enable | output, 1 = amplifier on | `0xa0368`, `0xa069c` [fw] |
| 10, 11 | UART1 console RX / TX | second function | [obs] |
| 12 | SD card detect | input pull-up, 0 = card inserted | `0x8661c` [fw] |
| 13 | Charger status input; also a deep-sleep wake source | input pull-up | `0x85aa8`, `0x86308` [fw] |
| 14–19 | SD card (SDIO, 4-bit) | second function | [sdk] [inf] |
| 16–19, 24, 26 | QSPI PSRAM (CLK, CSN, 4-line data) | second function `0x23`–`0x26` | `0x9dbc0` [fw] |
| 20 | I2C1 SCL (camera SCCB) | second function, pull-up | [sdk] |
| 21 | I2C1 SDA (camera SCCB) | second function, pull-up | [sdk] |
| 22, 23 | Unknown outputs (forced low before deep sleep); IR-cut filter H-bridge suspected | output | `0x86308` [fw] [inf] |
| 27 | Camera MCLK out | second function `0x2c` | `0x9df5c` [fw] [sdk] |
| 29 | Camera PCLK in | second function `0x2f` | [fw] [sdk] |
| 30, 31 | Camera HSYNC, VSYNC | second function `0x2d` | [fw] [sdk] |
| 32–39 | Camera D0–D7 | second function `0x2e` | [fw] [sdk] |

No GPIO toggles the camera's power, PWDN or RESET. The sensor's supply is the chip's IO LDO (VDDRAM pin). The first thing stock `main` (`0x810bc`) does is turn it on through `0x91db0(4)`: `CMD_QSPI_VDDRAM_VOLTAGE` = 7, then `CMD_SCTRL_BLK_ENABLE` bit 18. The SD card code turns it on again before mounting. With the LDO off, SCL/SDA read low on GPIO20/21 even with pull-ups. [fw] [obs]

The stock "i2c1" device is a software (bit-banged) I2C on GPIO20 (SCL) and GPIO21 (SDA), driving the GPIO config registers `0x802850`/`0x802854` directly (`0x87fdc`–`0x882c6`). It is not the hardware I2C1 block. [fw]

No PIR input is read. `PirSet`/`PirGet` exist only as cloud commands, and "motion" (`xm_get_motion`, `0x887c8`) is a stored setting, not a sensor. No IR-cut motor drive was found either; IRCut mode only switches the IR LEDs and the sensor's colour/night registers (§4). GPIO22/23 remain the candidates if the board has an IR-cut filter. [fw]

---

## 3. Power and battery

- **Battery voltage:** read on SARADC channel 0 (the chip's VBAT sense) through the `saradc_intf` thread. Each reading is `raw * 3` mV, averaged over 10 samples (`0x85ad0`). [fw]
- **Battery level reported to the app** (`0x85a6c`):
  - above 3999 mV: 100 %
  - 3800–3999 mV: 70 %
  - 3600–3799 mV: 40 %
  - below 3600 mV: 5 %
  - no reading yet: 0
- **Stock low-battery cutoff:** at or below **3400 mV**, unless the charger input (GPIO13) reports charging, it logs "low power to save data", saves state and enters deep sleep (`0x85ad0`). [fw]
  - Consequence: from a 3.3 V supply the stock firmware reads about 3309 mV and goes back to sleep within about 2 s of every boot. That is the loop in the captured boot log. [obs]
  - **Our firmware:** the unit runs from an external 3.3 V regulator, so we flag low battery only **below 3100 mV** (`BOARD_VBAT_LOW_MV` in `boards/a9-mini-bk7252/board_config.h`).
- **Power off:** a long press of Button2 logs "power off by button long press, enter deep sleep" (`0x865dc`).
- **Deep sleep** (`0x86308` calls `bk_enter_deep_sleep_mode` at `0x95d94`):
  1. Drive GPIO22, GPIO23 and GPIO7 low.
  2. Stop the camera if it is running.
  3. Sleep with wake mode GPIO, wake map `0x2020` (GPIO5 and GPIO13), edge map `0x20` (GPIO5 falling, GPIO13 rising), and LPO parameter 10.

  On wake the log shows `start_type:3`, `deep sleep waked by GPIO5`. [fw] [obs]
- **Power state machine:** the vendor "mcu_obj" state (`0x8887c`) holds two flags: power on/off (persisted as `xdev_opt`, magic `0x11223344`) and the running module. With power "off", the camera is never initialised ("camera is not init, power keep"), and the core thread waits for a Button2 press to power on. [fw] [obs]

---

## 4. Camera

### Bus and clocks

- **Interface:** DVP, 8-bit parallel, with PCLK, HSYNC and VSYNC on the pins listed in §2. [fw]
- **Clocks:** the BK7252N ICU enables the JPEG/DVP clock (`0x9e3b8`); MCLK comes out on GPIO27. [fw]
- **Encoder:** the hardware JPEG encoder ("ejpeg", base `0xA03000` [sdk]) compresses the sensor's YUV422 stream. GDMA moves the compressed data into a frame pool, using the 2 ms high-resolution delay timer from the SDK design. [fw] [sdk]
- **Sensor control:** SCCB over I2C1 with 7-bit slave addresses and 8-bit registers, through the `i2c_operater` struct at `0x40c40a` (`.salve_id` at `+1`). Reads go through `0x9af34`, writes through `0x9aea4`. Each transfer is retried up to 6 times. [fw]

### Sensor auto-detect (`0x9af6c`)

The firmware probes the sensors below in order. It makes up to 52 passes, then logs "camera id no match", waits 3 s and reboots. The type code goes to `0x40c412`; a per-sensor value goes to `0x40c408` and `0x40c411`, later written to the sensor's flip/mirror register.

| # | Sensor | Addr | ID register = value | Type code | Init table | Pairs |
|---|---|---|---|---|---|---|
| 1 | GC2145 | `0x3C` | `F0`=`21`, `F1`=`45` | `'2'` | code `0x9ca54` | |
| 2 | OV7740 | `0x21` | `0A`=`77`, `0B`=`40`/`42` | `'4'` | code `0x9cc90` | |
| 3 | SP0A39 | `0x21` | `00`=`0A`, `01`=`39` | `'L'` | `0x16e52a` | 352 |
| 4 | SP0A20 | `0x21` | `02`=`2B` | `'S'` (flip 1) | `0x16ed1e` | 399 |
| 5 | SP0A28 | `0x3D` | `02`=`A2` | `'b'` | `0x16ea48` | 280 |
| 6 | SP0828 | `0x18` | `02`=`0C` | `'T'` (flip `0x10`) | `0x16f836` | 268 |
| 7 | SP0A19 | `0x21` | `02`=`A6` | `'Y'` | `0x16da26` | 289 |
| 8 | BF20A6 | `0x6E` | `FC`=`20` | `'R'` | `0x16e1ac` | 133 |
| 9 | GC0307 | `0x21` | `00`=`99` | `'Q'` (flip `0x82`) | `0x16d466` | 315 |
| 10 | GC0308 | `0x21` | `00`=`9B` | `'_'` (flip `0x13`) | `0x16e7fe` | 284 |
| 11 | GC0312 / GC0310 | `0x21` | `F0`=`B3` / `A3` | `'d'` (flip `0x13` / `0x10`) | `0x16e2b6` | 314 |
| 12 | GC0311 | `0x33` | `F0`=`BB` | `'a'` (flip `0x17`) | `0x16d258` | 263 |
| 13 | GC0328C | `0x21` | `F0`=`9D` | `']'` (flip `0x14`) | `0x16de6e` | 375 |
| 14 | GC0309 | `0x21` | `00`=`A0` | `` '`' `` (flip `0x10`) | `0x16d6dc` | 218 |
| 15 | GC0329C | `0x31` | `FB`=`62` | `'c'` (flip `0x14`) | `0x16dc68` | 259 |

- **Other drivers:** HI257 (addr `0x20`, type `'3'`) and SIV121 (addr `0x33`, type `'N'`, table `0x16d890`) have init code but are not in the probe loop. [fw]
- **Table format:** `{reg, value}` byte pairs written in order. The decompiler shows each table as start/end pointers; the counts above are `(end - start) / 2`.
- **Fitted sensor unknown:** the stock firmware never reached the probe on our unit because of the low-battery sleep (§3). Two ways to identify it:
  - Read the marking on the sensor module.
  - Let our firmware run the same probe sequence.

  Once the camera is up, the shell command `xm_read <reg>` reads one register of the detected sensor.
- **Stock tables for our HAL:** every init table above, plus the resolution and day/night sub-tables, is extracted to `firmware/hal/bk7252/stock_sensor_tables.h`, generated by `backup/re/scripts/extract_sensor_tables.py`. GC2145 has a 637-pair base table plus two 23-pair mode tables. OV7740 has 122 pairs.

### Resolution (`0x9ae54`)

The JPEG X/Y size is set in units of 8 pixels:

| PPI code | Size |
|---|---|
| 0 | 320 × 240 (QVGA) |
| 1 | 640 × 480 (VGA); 240 × 320 when the sensor is SP0828 |
| 2 | 800 × 600 |
| 3 | 1280 × 720 |
| other | 640 × 480 (logs "cm PPI unknown, use QVGA") |

- **Default and runtime changes:** right after detection the dispatcher selects PPI 1 (VGA). The cloud "VideoQosSet" path calls the PPI setter (`0x9c238`) to change resolution at runtime. That setter reprograms the JPEG X/Y size (ejpeg commands `0xebc0007` / `0xebc000d`). GC0328C and GC0311 also get their QVGA or VGA window sub-tables.
- **Typical use:** the VGA sensors (GC03xx, SP0Axx) run QVGA or VGA, and GC2145 handles 800 × 600 and 1280 × 720. [fw]
- **Frame transport:** the JPEG interrupt is IRQ 23, priority 14 (`0x9e42c`). `video_transfer_main` (`0x9c574`) allocates a 36,800-byte pool of 25 packets of 1472 bytes, and GDMA fills them. Packet headers must be 4-byte aligned. [fw]

### Day/night (`0x9b22c`)

- **Per-sensor night mode:** each sensor has a night-mode register set. For example, GC0328C writes `FE=00, D1=00, D2=00` for night; for day it writes `D1=22, D2=21`, which sets saturation for colour.
- **IR illuminator and mode:** the IR LEDs (GPIO7) follow the IR-cut setting. Mode 1 is forced night, mode 2 forced day, and mode 3 is automatic by clock (night outside 07:00–17:59). Logic in the LED thread at `0x86ad8`. [fw]

---

## 5. Audio

- **Microphone:** the on-chip audio ADC (`aud_adc`, audio block at `0xA06000` [sdk]) records mono at **8000 Hz**, 16-bit (`0x85d14`). [fw]
  - The encoder thread `rec_encoder_thread` reads 320-byte (20 ms) chunks and buffers 1600 bytes (100 ms).
  - It encodes each 100 ms block to 800 bytes of **G.711 A-law** (`0x82400`; sign masks `0xD5`/`0x55`, segment table at `0x1641f8`) and sends it to the cloud A/V SDK as audio type `0x15`.
- **Speaker:** the on-chip DAC (`aud_dac`) defaults to 44100 Hz (`0xa0658`). The `audio_codec_control` path (`0xa09e0`) sets the DAC rate and volume. GPIO9 enables the external class-D amplifier only while the sound device is open (`0xa069c`). [fw]
- **Boot-time gains:** `set dac vol:65 - indx:11,dig:30,ana:1a` and `set adc vol: 124 - 124`. [obs]
- **Talkback:** the app sends audio with `TalkbackPlay`/`TalkbackPause`, and volume goes through `VolumeSet`/`VolumeGet`.
- **Shell test commands:** `mic_dac_loop` plays the mic back through the speaker, and `audio_dump` dumps the audio state. [fw]

---

## 6. Buttons, LED, SD card

- **Buttons:** handled by a multi-button style library (`0xc7e58`). Button1 is on GPIO4 and Button2 on GPIO5, both active low with pull-ups. [fw]
  - Event callbacks are registered for click (1) and long press (3).
  - A long press of Button2 means power off. A Wi-Fi reset is also reachable from the shell (`cli_xwifi_reset`) and from the cloud (`Reset`).
- **LED:** the status LED is on GPIO2.
  - It blinks at 200 ms or 1 s while unbound or connecting.
  - It stays steady once bound.
  - It turns off in "led object mode 2".
  - After a long time unbound the device resets Wi-Fi ("long time is unbind status, will wifi_reset"). [fw]
- **SD card:**
  - Card detect is on GPIO12 (0 = inserted). It is mounted as `sd0` on `/sd` with elm-FAT. [fw]
  - At boot the firmware reads `/sd/config_json.txt` (`0x87efc`), a factory-test file parsed by `0x87dbc`. Keys:
    - `test_mode`: `laohua` for burn-in or `zhengji` for whole-unit test
    - `wifi_ssid`, `wifi_key`: if present, it joins that network ("test wifi,ssid=%s, passwd=%s")
    - `led_time`
    - `step_motor`: present for pan/tilt variants

    [fw]
  - Recordings go to `/sd/<date>/<hh>.jpg` and similar paths (strings `%s/%d/%02d/%02d.jpg`). The A/V thread (`0x86de8`) also writes frames there.

---

## 7. Wi-Fi and provisioning

- **Radio:** 2.4 GHz b/g/n, station plus soft AP (Beken `wlan0`/`wlan1`). No RF calibration is stored, so the firmware uses default power tables. [obs]
- **Boot flow** (`0x850c0`):
  1. Read the device ID (did) from `xc config`; reboot if it is missing.
  2. Mount the SD card and apply the test Wi-Fi from it if present.
  3. Start video and audio.
  4. Read the saved Wi-Fi from `0x1F7000`.
  5. With a saved network, join it as a station. Without one, start provisioning. [fw]
- **Provisioning, run together:**
  - **Soft AP** (`0x871f4`): SSID `LLM_<did[2..5]>_<did[6..11]>` and fixed password `12345678` (`0x293c8`; security mode `0x400004`, assumed to be WPA2). This unit's AP is `LLM_H0A9_191BFD`. The phone app joins it and sends `WifiSet` over the vendor RPC (below).
  - **BLE** (`0x81504`): Beken BLE 5 stack, advertising every 100 ms (`0xA0`) with flags plus the complete local name, provisioning through `avsdk_bdn`.
  - No QR-code scanning: the camera has no image decoder. `avsdk_qr_netcfg_decode` only decodes a QR payload string handed to it.
- **Watchdogs:**
  - The device reboots and records why ("DEV net abnormal", "DEV fps abnormal" when no frame arrives for more than 10 s), as shown in the `0x1F8000` block.
  - After a long time unbound it resets Wi-Fi. [fw]

## 8. Vendor cloud (XCIoT, xcthings.com): overview

Kept brief, because our firmware drops it.

- **Server discovery:** `gdomain = prod.glbs.xcthings.com`, with fallback IPs `47.240.1.244`, `47.252.5.225`, `8.209.77.49`, `39.108.59.60`. `GetServers`/`GetConnServers` return the message broker (PPMQ), the API gateway and relay servers.
- **Identity:** `did`, `signkey` and `scode` (`0x1F5000`), plus product and device keys (`0x1F4000`). Local payloads use AES-256-CBC (mbedTLS).
- **Transports:** PPRPC (protobuf RPC) over TCP, UDP or KCP (`ikcp.c`). Connection order:
  1. LAN direct: UDP server on port **20190**, plus LAN broadcast discovery and `LanAuth`.
  2. P2P: UDP hole punching (`NatProbe`, `P2PStepOne` to `P2PStepThree`, `P2PHole`).
  3. TCP relay (`RelayStep1` to `RelayStep3`).
  4. A UPnP client for port mapping.
- **Media:**
  - Video is JPEG frames sliced into pieces of up to 3000 bytes (`avsdk_write_video_slice`).
  - Audio is G.711A in 100 ms blocks.
  - Up to 2 viewers at once (`avsdk_video_max_conn`).
- **App commands (RPC names):**
  - Media: `VideoPlay`, `VideoPause`, `VideoQosSet`, `VideoChanChange`, `AudioPlay`, `AudioPause`, `TalkbackPlay`, `TalkbackPause`, `Screenshot`
  - Wi-Fi: `WifiAPGet`, `WifiSet`, `WifiGet`
  - Image and lights: `FlipSet`, `FlipGet`, `IRCutSet`, `IRCutGet`, `LedSet`, `LedGet`, `OsdGet`, `OsdSet`, `PowerFreqSet`, `PowerFreqGet` (50/60 Hz anti-flicker)
  - Motion and alarms: `MotionzoneSet`, `MotionzoneGet`, `PirSet`, `PirGet`, `AlarmSet`, `AlarmGet`
  - Pan/tilt: `PtzCtrl`, `PspAdd`, `PspDel`, `PspList`, `PspCall`, `TimedcruiseSet`, `TimedcruiseGet`
  - Recording and SD card: `History*` (SD playback), `RecordStart`, `RecordStop`, `EventRecordGet`, `EventRecordSet`, `StorageInfo`, `StorageFormat`
  - Time and settings: `TimeSet`, `TimeGet`, `ConfigGet`, `VolumeSet`, `VolumeGet`, `SecretSet`, `SecretGet`, `NotifySet`, `NotifyGet`
  - Device: `Reboot`, `Reset`, `Unbind`, `FirmwareNotify`, `FirmwareCheck`, `FirmwareRate` (OTA into the FAL `download` partition)
  - Files: `File*`, `Dir*`
  - Other: `CustomCmd`, `GetNetworkInfo`
- **Status reported to the cloud:** `netQos`, `battery` (percent, §3), `powerModel`, `timeOffset`.

## 9. Notes for our firmware

- **SDK base:** use the **FreeRTOS** Beken SDK (`YanMinge/beken_freertos_sdk_release`). It supports `SOC_BK7252N`; the public RT-Thread `bdk_rtt` trees don't. `firmware/hal/bk7252` now targets it; see its README for build and flashing.
- **Camera bring-up:**
  1. Enable the DVP second functions (`0x2c` to `0x2f`) and I2C1.
  2. Probe sensors in the order in §4.
  3. Load the matching table from `stock_sensor_tables.h`.
  4. Start ejpeg at VGA.
- **Battery:** read SARADC channel 0 × 3 for VBAT. Report low battery below 3100 mV (`BOARD_VBAT_LOW_MV`), and don't deep-sleep on the stock 3400 mV threshold.
- **Pins worth exposing in `board_config.h`:**
  - LED: GPIO2 (active high)
  - IR LEDs: GPIO7
  - speaker amplifier: GPIO9
  - buttons: GPIO4 and GPIO5 (active low)
  - SD detect: GPIO12
  - charge status: GPIO13
- **Calibration:** keep the RF block at `0x1E0000` untouched; it's empty here anyway. Keep `0x1F4000`–`0x1F8FFF` too if returning to stock should stay possible.

---

## Appendix: serial shell commands

| Command | Function | Notes |
|---|---|---|
| `xm_read <reg>` | `0x883f9` | Read one register of the current camera sensor over SCCB |
| `xm_cmd_power_on` | `0x88325` | Set the vendor "power on" flag (`0x404141`) |
| `regset <reg> <val> [...]` | `0x88331` | Write sensor register pairs, read each back and print it |
| `xc` | `0x825a1` | Vendor cloud CLI |
| `cli_xwifi_reset` | `0x873f1` | Clear Wi-Fi config |
| `linkkey` | `0x81961` | Load config file |
| `mac` | `0x811cd` | Read or set the MAC |
| `adc_check` | `0x917d1` | |
| `video_buffer` | `0x92a31` | |
| `audio_dump` | `0xa05c1` | |
| `mic_dac_loop` | `0xa4c3d` | |
| `sleep_mode`, `pm_level` | `0xa4bb9`, `0xa51c5` | |
| `wifi`, `wifi_demo` | `0xbf7d9`, `0x9460d` | |
| `ifconfig`, `ping`, `netstat`, `dns`, `ntp_sync`, `netio_init` | | |
| `txevm`, `rxsens`, `rfcali_*` | | RF test and calibration |
| `fal`, `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `mkfs`, `df`, `date`, `free`, `ps`, `list_*`, `reboot`, `wdg_*`, `set_log` | | Standard RT-Thread commands |
