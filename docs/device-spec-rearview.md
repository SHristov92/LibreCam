# "Rear view camera" (BK7252N): device specification

How the stock firmware of the camera sold as "Rear view camera" works, reverse engineered from one unit's flash dump. Like the A13 mini ([device-spec.md](device-spec.md)) it is a BK7252N, but the board, the vendor firmware and the flash layout are different. Use this page as the reference for the `rearview-bk7252` board.

**Unit:** board has no silkscreen and no battery. Stock firmware `CYCam-A9` v3.2, built `Feb 26 2026 15:59:15`. Bootloader tag `CYCAM_T99_v32_0226`. P2P ID `EEE-338222-MYRYL`.

**Evidence:** addresses are CPU addresses in the app image (`backup/rearview-bk7252/parts/01_app_nocrc_logical.bin`, mapped at `0x10000`). The Ghidra decompilation is `backup/re-rearview/app.c`, with literal pools resolved in `app_r.c`:
- `backup/re-rearview/scripts/q.py fn <addr>` prints a function.
- `backup/re-rearview/scripts/blscan.py <addr>...` lists the `BL` call sites of a function, with the constant arguments loaded just before each call. Many small functions aren't in Ghidra's function list, so `blscan.py` finds calls the decompilation misses.

**Confidence tags** are the same as in the A13 spec: **[fw]** from the stock firmware, **[sdk]** from the Beken SDK, **[obs]** seen on the running unit, **[inf]** inferred and not yet confirmed. The stock boot log is `backup/rearview-bk7252/stock_bootlog.txt`; §10 lists what it confirmed.

---

## 1. SoC, memory and boot

| Item | Value | |
|---|---|---|
| SoC | Beken BK7252N. The bootloader's code matches the A13's byte for byte up to its partition table. | [fw] |
| CPU | ARM968E-S, Thumb app, linked at `0x10000` (same vector table as the A13) | [fw] |
| Flash | 2 MiB SPI NOR behind the Beken CRC layer (16-bit CRC after every 32 bytes) | [fw] |
| PSRAM | used by the stock video pool (`sdram_malloc`, 36,800 bytes) | [fw] |
| OS | RT-Thread with Beken's SDK; lwIP 2.0.2; FAL; EasyFlash; littlefs; RT-Thread OTA | [fw] |
| Console | UART1: GPIO10 is RX, GPIO11 is TX, 115200 8N1. The flash was dumped through it with `flash.py --read`. | [obs] |

### Boot

1. The ROM bootloader opens the UART download window. The link check and the reply are the same as on the A13.
2. The second-stage bootloader is at `0x0`. It is the same Beken `bootloader_bk7252n` code as on the A13, but with a different FAL partition table at `0xd2e4` (see the flash map below) and a vendor tail (`CYCAM_T99_v32_0226`, a copy of the P2P ID).
   - The CRC of its first `0xE0FC` flash bytes is `0x615D4F5E`. The A13's is `0x332C8C63`. Both appear in `KNOWN_BOOTLOADERS`.
   - It looks for OTA packages in its own `download` partition (`0x143000`). The OTA key and IV are the same as on the A13 (`0123456789ABCDEF…`).
3. The app's start-up sequence is at `0x14460`:
   1. Print `project_model:CYCam-A9` and the version.
   2. Load the EasyFlash environment (`0x21bb4`).
   3. Check the MAC (`0x16f0c`, §7).
   4. Init the button and LED.
   5. Start the factory and Wi-Fi service (`0x15190`, §7).
   6. Start video, P2P, the push service and the rest.

### Flash map (physical offsets, from the bootloader's FAL table)

| Offset | Size | FAL name | Content |
|---|---|---|---|
| `0x000000` | `0x10000` (logical) | `bootloader` | Bootloader (CRC-wrapped) |
| `0x011000` | `0x120000` (logical), up to `0x143000` | `app` | App, CRC-wrapped. The stock app ends at about `0x128000`. |
| `0x143000` | `0xBB000` | `download` | OTA staging. It ends at `0x1FE000`, so it **overlaps EasyFlash at `0x1FD000`** for packages over `0xBA000` bytes. |
| `0x1FD000` | 4 KB | `EasyFlash` | Vendor settings (key=value, below) |
| `0x1FE000` | 4 KB | `rfparam` | Beken RF calibration TLV (`TLV\0`), **with real calibration data** |
| `0x1FF000` | 4 KB | `netparam` | Empty |

The A13 used SDK addresses here (download at `0x132000`, RF calibration at `0x1E0000`), and the SDK's 2 MB partition table assumes them. On this board `0x1E0000`–`0x1FCFFF` is empty.

**EasyFlash keys** (`0x1FD000`) [fw]: `ssid0`/`passwd0` (`pdtest`/`123456789` on this unit), `workmode=ap`, `led_onoff`, `ir_status_flag`, `username0/1`=`admin` with `userpasswd0/1`=`6666`, `recmode`, `poweron_flag`, `reset_flag`, `sleep_flag`, `motion_detect_*`, `audio_detect_*`, `push_*`, `sleep_time=60`, `offline_time=10`, `bitrate_size=26`, `brightness=4`, `contrast=2`, `ptz_*`, `p2p_account_info` (JSON: `did`, `apiLincese`, `crckey`, `appkey`), `dev_p2p_did`, `dev_p2p_appkey`.

**RF calibration TLV** (`0x1FE000`) [fw]:

| Item | Value |
|---|---|
| `TXID_ID` | `0x00BC614E` (the SDK default, 12345678) |
| `TXID_MAC` | `00:00:00:16:29:28` |
| `TXID_THERMAL` | 340 |
| `TXID_CHANNEL` | 22222222 |
| `TXID_XTAL` | `0x31` |
| `TXID_ADC` | `0x03540055` |
| `TXID_LPFCAP` | I = 9, Q = 10 |
| TX power tables | B/G/N present (`TXPWR_TAB_TAB`) |

---

## 2. Pin map

| GPIO | Function | Direction / level | Evidence |
|---|---|---|---|
| 0 | **I2C2 SCL (camera SCCB)** | second function `GFUNC_MODE_I2C2`, pull-up | `0x46db4` opens `i2c2`; the driver at `0x49f28` uses I2C2 registers `0x802600` [fw] [sdk] |
| 1 | **I2C2 SDA (camera SCCB)** | as above | [fw] [sdk] |
| 2 | **IR illuminator LEDs** | output, 1 = IR on | `0x165a0`, key `ir_status_flag` [fw] |
| 5 | **Status LED** | output, 1 = on | `0x14d48` (on) / `0x14d5c` (off), key `led_onoff` [fw] |
| 7 | **Button** (setup/reset); deep-sleep wake source | input pull-up, active low | `0x1461e` is the button's read callback; wake map `0x80` in `0x16514`/`0x1656c` [fw] |
| 10, 11 | UART1 console and download RX/TX | second function | [obs] |
| 23 | TF card detect in the shared vendor code (0 = card) | input pull-up | `0x17288` [fw]. **This board has no card slot.** |
| 27, 29–39 | Camera DVP: MCLK, PCLK, HSYNC/VSYNC, D0–D7 | second functions `0x2c`–`0x2f`, same as the A13 | `0x48668` [fw] [sdk] |

Other notes [fw]:
- **SDIO:** the shared vendor code also has the SD driver (GPIO14–19), but nothing on the board uses it.
- **No other GPIO is written:** no speaker amplifier enable, IR-cut motor, PIR input or sensor reset/PWDN line.
- **External power:** "Charging" is the USB/VBUS detect bit, `SCTRL 0x8001C8` bit 15 (`0x167a4`).

**Not from the stock firmware:** the user found the UART pads with a meter while dumping. The pins in this table come from the firmware.

---

## 3. Power

- **No battery.** The battery percentage at `0x400128`, which the app and the OTA "< 40 %" check read, is never written. [fw]
- **Idle sleep:**
  - A periodic check (`0x15408`) counts 12 ticks. If the VBUS-detect bit is clear at that point, it calls `0x1656c`: deep sleep with GPIO7 as the wake source and `sleep_time` 10.
  - Settings `sleep_time=60` and `offline_time=10` tune this.
  - Board inference: this is how the vendor's battery models behave. A camera fed from the car's supply sees VBUS high and never sleeps. [fw] [inf]
- **Deep sleep from the app:** the app command `ForceDeepSleep` (`jsonDevControl`, `0x1b93c`) calls `0x16514`, which wakes on GPIO7 low. [fw]

---

## 4. Camera

### Bus and sensor auto-detect

- **SCCB bus:** the sensor is on **hardware I2C2 (GPIO0/1)**, not on I2C1 (GPIO20/21) as on the A13.
- **Bring-up order** (`0x46db4`):
  1. Power on (`0x163c8`, `0x48b10`).
  2. Turn the IO LDO on with `0x3baf4(4)`, as on the A13.
  3. Open `i2c2` and set its clock divider (`I2C2_CMD_SET_FREQ_DIV`).
  4. Probe the sensor (`0x45784`) and init it (`0x45244`).
  5. Open `ejpeg`. [fw]

The probe tries each entry 3 times, 100 ms apart. The detected ID goes to `0x407220`, and the address to the I2C op struct at `0x40bb90+1`. **Our unit has a GC0311** (boot log: `GC0311 init finish`). Every sensor below is in `camera_bk7252.c`. "Ours" says how:
- **A13+RV:** the A13 mini's firmware knows it too. The A13 board uses the A13 table; `rearview-bk7252` uses this firmware's table (`BOARD_STOCK_TABLES_REARVIEW`).
- **RV:** only this firmware knows it. The table is in `stock_sensor_tables_rearview.h`, generated by `backup/re-rearview/scripts/extract_sensor_tables.py`, and probed on every board after the A13's sensors.

| # | Sensor | Addr | ID register = value | Init table (pairs) | Ours |
|---|---|---|---|---|---|
| 1 | GC0328C | `0x21` | `F0`=`9D` | `0xffdf2` (371) | A13+RV |
| 1 | GC0309 / GC0308 | `0x21` | `00`=`A0` / `9B` | `0xfe4d8` (224) / `0xfda84` (297) | A13+RV |
| 2 | GC0311 | `0x33` | `F0`=`BB` | `0xfeb80` (264) | A13+RV |
| 3 | GC0312 | `0x21` | `F0`=`B3` | `0xfedda` (287) | A13+RV |
| 4 | GC0329 | `0x31` | `FC`←`16`, then `00`=`C0` | `0xfcebe` (256) | RV (the A13 firmware has GC0329C, `FB`=`62`) |
| 5 | HI704 | `0x3C` | `04`=`96` | `0x100294` (383) | RV |
| 6 | OV7670 | `0x21` | `0A`=`76` | `0xfe066` (170) | RV |
| 7 | OV7673 | `0x33` | `01`=`0C` | `0xfd12e` (148) | RV |
| 8 | SP0A19 | `0x21` | `02`=`A6` | `0xfde10` (271) | A13+RV |
| 9 | SP0A38 | `0x21` | `01`=`0A` | `0xfde10` (the SP0A19 table) | RV |
| 10 | SP2518 | `0x30` | `02`=`53` | `0xfd278` (361) | RV |
| 11 | SC030 | `0x68` | `F8`=`46` | `0xff3e8` (188) | RV |
| 12 | PAS6329 / PAS6375 | `0x40` | `00`=`63` | 3 pages `0x100142`/`0xfe1dc`/`0xfe300` / `0xfe6aa` (619) | RV, both tables written as stock does |
| 13 | BF2013 | `0x6E` | `00`=`63` | `0x1005c0` (135) | RV |
| 14 | BF3A03 | `0x6E` | `FC`=`3A` | `0x100716` (126) | RV |
| 15 | HM1055 | `0x24` | 16-bit reg `0001`=`09` | `0xff56a` (540 × {u16 reg, u8 val}) | RV; probed only on I2C1 boards (below) |
| 16 | GC2145 | `0x3C` | `F0`=`21` | `0xfd576` (637) | A13+RV; GC2145 keeps the A13 table on every board, with its size modes |
| 17 | BF20A6 | `0x6E` | `FC`=`20` | `0xfdd06` (133) | A13+RV |

Notes on the vendor probe [fw]:
- **Duplicate entries:** the probe tries SP0A19, SP0A38 and SP2518 a second time after BF3A03. They are left out of the table above.
- **ID `0x63` collision:** PAS6329, PAS6375 and BF2013 all report ID `0x63`. The init function `0x45244` then runs every table whose ID matches, so a PAS6329 also gets the PAS6375 and BF2013 tables. Only the address tells them apart.
- **Unreachable GC0308 table:** the probe stores GC0308's ID as `0x9B`, but the init function checks for `0x9A`. On this firmware a GC0308 is detected but never gets its table.
- **Table format:** `{reg, val}` byte pairs, except HM1055.
- **Boot size:** the firmware's initialised data sets PPI 1 (VGA) and fps type 2 at boot (`0x400104`/`0x400100`, loaded from `0x115574`). It applies the PPI sub-tables only when the app changes the video quality (`0x166a0` → `0x46ce8` → `0x465dc`). So every init table outputs 640 × 480, and the new sensors are VGA-only in our HAL. GC0328C and GC0311 also have QVGA sub-tables (GC0311: `FE=00, 59=22`).
- **Stock I2C2 driver vs the SDK's:** both are interrupt-per-byte with a 2 s busy-wait, and stock opens the bus with the same settings (400 kHz, divider 19, idle 3, SCL 4, timeout and free detect on). Stock differs in two places:
  - **Read NACK:** its ISR (`0x49b64`) marks the transfer done on a read NACK (`0x49d0a`–`0x49d16`). The FreeRTOS SDK's `i2c2.c` leaves that out, so each probe of an absent sensor waited 2 s.
  - **Command `0x0ABF000D`:** sent after opening (`0x46db4`), it sets bits [1:0] of an undocumented register at `0x0080260C` (handler `0x498b0`).
- **Our bus is software SCCB.** We tried the SDK's I2C2 driver with both stock differences added. Under our interrupt load, writing image controls while streaming still caused SCL timeouts (`i` from the ISR), lost transfers, and calls that returned success after a stale "done" flag without touching the bus. The results were a green tint, garbled registers and hangs. `BOARD_CAM_SCCB_SOFT` drives SCCB on GPIO0/1 from the CPU instead: every byte's ACK is checked, and 16-bit registers (HM1055) work too.

### Controls

- **Mirror/flip** (`0x46354`), modes 0–3:
  - GC0328C: register `0x17` = `0x10`–`0x13`.
  - GC0329, GC0311 and GC0312: `0x17` = `0x14`–`0x17`.
  - HI704: `0x11` = `0x90`–`0x93` on page 0.
  - GC0308/GC0309: a separate routine (`0x4514c`).
- **Night/day colour:** per sensor (`0x464d4`, `0x46514`, `0x4653c`), driven by the IR mode (§5).
- **Saved image settings:** `brightness` and `contrast` (EasyFlash), plus the JPEG size target `bitrate_size`.

### Frames

- Same SDK path as the A13: the `ejpeg` hardware encoder with a GDMA frame pool in PSRAM (`video_capture.c`, `0x13f1c`), 36,800 bytes.
- Frames go to the P2P clients (§7).

---

## 5. LED, IR and button

- **Status LED (GPIO5):** `led_onoff` mode, set by `0x14dec`:
  - 0 and 3: off
  - 1: blink (timer `0x14d70` toggles the pin)
  - 2: on

  It blinks while connecting and stays on once a client is connected. [fw]
- **IR LEDs (GPIO2):** `ir_status_flag` mode, set by `0x165a0`:
  - 0: day (IR off, colour)
  - 1: night (IR on, sensor night mode)
  - 2: auto
  - 3 and 4: off or on without saving the setting

  Auto mode sets the flag at `0x4052b8`. A video-thread routine then switches using the sensor's exposure. There is no light sensor or ADC input. [fw] [inf]
- **Button (GPIO7):** active low, uses the RT-Thread FlexibleButton library (`0x1468c`, callback table at `0x115580`). The events are:
  - **Short press** (`0x14642`): sends push message 1 to the vendor push server and turns the LED on.
  - **Repeated clicks** (`0x14634`): set `poweron_flag` and reboot.
  - **Long press** (`0x14664`): factory reset (`setRestoreFactory`). It keeps the P2P identity and the brightness, contrast and bitrate settings, clears everything else, then reboots. [fw]

---

## 6. Audio

- **Firmware support:** the vendor firmware has the on-chip microphone path (`audio_device_mic_*`, `audioEncoderThread`, `audio_detect_*`) and the DAC (`aud_dac`). No GPIO enables a speaker amplifier. [fw]
- **Hardware:** it is not known whether this board has a microphone (§10).

---

## 7. Network and provisioning

- **MAC** (`0x16f0c`): read from the RF calibration TLV, which gives `00:00:00:16:29:28` on this unit. If the MAC is still the SDK default `c8:47:8c:42:00:48`, the firmware does three things:
  1. runs the RF auto-calibration
  2. makes a random MAC `12:34:xx:xx:xx:xx`
  3. writes both into the TLV (`AutoWriteRFCalibValRandMac`). [fw]
- **Wi-Fi start-up** (`0x15190`):
  - **TF-card config:** if an `.ini` file is on the TF card, the firmware joins the router it names (`ssid`, `passwd`).
  - **Factory test:** if the network `CAM_VTestX_159` is visible, the firmware joins it with password `123456789`.
  - **Otherwise:** it follows `workmode`, which is `ap` by default.
- **Soft AP** (`0x14848`):
  - **SSID:** the P2P ID (`EEE-338222-MYRYL`), or `EEE-000000-GCYEG` without one.
  - **Security:** open, no password.
  - **Addressing:** IP `192.168.10.1/24`, DHCP on, channel 11.
- **App (P2P):**
  - Uses the CS2 Network PPCS SDK (`PPCS_*`), with `p2p_account_info`: DID `EEE-338222-MYRYL`, apiLicense `MXIMHQ`, crckey `SHIX`.
  - The phone app logs in with `admin`/`6666`.
- **Push:** `cypush` to `http://120.79.76.240:9093/push/login`.
- **OTA:**
  - **Firmware updates:** an HTTP download into the `download` partition.
  - **From the TF card:** an `.rbl` file.
- **Not used:** QR provisioning. **BLE** (`ble ssid:%s, passwd:%s, did:%s`) gives an alternative provisioning path.

---

## 8. What our firmware needs for this board

| Area | A13 mini (`a13-mini-bk7252`) | Rear view (`rearview-bk7252`) | Change |
|---|---|---|---|
| SoC, SDK, DVP pins, JPEG path | BK7252N | same | none |
| Sensor bus | I2C1, GPIO20/21 | **GPIO0/1** (stock: I2C2) | `BOARD_CAM_SCCB_SOFT`: software SCCB; UART2 taken down |
| Sensors | 15 from the A13 firmware | GC0311 fitted; 17 in this firmware | all 26 probed on both boards; `BOARD_STOCK_TABLES_REARVIEW` selects this firmware's tuning |
| OTA `download` | `0x132000`, `0xAE000` | **`0x143000`, `0xBB000`** | `BOARD_OTA_DL_ADDR/SIZE` |
| RF calibration / MAC | none; MAC from the vendor JSON | TLV at **`0x1FE000`** with the MAC | `BOARD_RF_TLV_ADDR`: the MAC is read from it |
| Settings sector | `0x1FD000` (free) | `0x1FD000` = **stock EasyFlash** | Same address. "Restore stock" now also writes that sector back from the backup. |
| Bootloader check | CRC `0x332C8C63` | **CRC `0x615D4F5E`** | known-bootloader list, tagged with the board, so the A13 image can't be installed here by mistake |
| SD card | GPIO12 detect, SDIO | **none** | `BOARD_HAS_SD 0`: storage reports "no card", and the web UI hides storage and recording |
| Battery | SARADC 0 × 3 | **none** | `BOARD_HAS_VBAT 0`: `hal_battery_mv()` returns -1 |
| LED | GPIO2, active high | **GPIO5**, active high | board pins |
| IR LEDs | GPIO7 | **GPIO2** | board pins |
| Buttons | GPIO5 (setup), GPIO4 (user) | **GPIO7** (setup) only | board pins |
| Deep-sleep wake | GPIO4/5 | **GPIO7** | `BOARD_WAKE_PIN_MASK` |
| Speaker amplifier, charge pin, SD detect | GPIO9, 13, 12 | none | left undefined |
| Microphone | on-chip ADC | the stock firmware opens it (§10) | kept |

This board has its own HAL, `hal/RearView`, a copy of `hal/A13` with the changes above, so work on one camera can't break the other. Pins and addresses are in `boards/rearview-bk7252/board_config.h`.

---

## 9. Restoring stock

The stock firmware depends on the following areas. A full restore must put all of them back:
- the app region `0x11000`–`0x143000`
- the **EasyFlash sector at `0x1FD000`**, which holds the P2P identity, the app password and settings, and which LibreCam's settings overwrite

`flash.py --restore-stock` and the web flasher's "Restore stock" both write the app region and that sector from the backup. They never write the RF calibration at `0x1FE000`.

---

## 10. Stock boot log

`backup/rearview-bk7252/stock_bootlog.txt` (UART1, 115200) confirms:
- **Chip:** `chip id=7252a device id=24318000`, ROM `BK7252N_1.0.14`, RT-Thread 3.1.0, SDK 3.0.76.
- **Partitions:** the FAL table as in §1.
- **SD card:** the SD init fails (`CMD8 SEND_IF_COND err`, `tf not insert`), as expected with no slot.
- **Sensor:** `GC0311 init finish`, then `camera_intfer_init`.
- **MAC:** the BLE MAC is `00:00:00:16:29:29`, the TLV Wi-Fi MAC plus one, so the TLV MAC `00:00:00:16:29:28` is the unit's.
- **RF:** the stock firmware runs a full TX power calibration at boot and uses XTAL 49 from flash. Our SDK build still uses its defaults for both.
- **Microphone:** the firmware opens the on-chip mic at 8 kHz, ADC channel 1, volume 90. Whether a microphone is fitted isn't visible in the log.
- **Wi-Fi:** it scans, then starts the soft AP with DHCP `192.168.10.100`–`.254`.
- **A stock bug:** the log shows `jpg err` / `jpeg reset OK!` repeatedly after start-up.
