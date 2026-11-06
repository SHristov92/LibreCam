/*
 * "Rear view camera" on a Beken BK7252N, 2 MB flash, no battery, no SD card
 * slot, no silkscreen. Stock firmware "CYCam-A9" v3.2. Pins and flash layout
 * come from the stock firmware; see docs/device-spec-rearview.md.
 */
#define BOARD_NAME   "rearview-bk7252"
#define BOARD_MCU    "bk7252"

/* Same chip and RAM as the A13 mini, so the same buffer budget. */
#define LC_FRAME_BUF             (40 * 1024)
#define LC_RTSP_MAX_CLIENTS      2
#define LC_HTTP_STREAM_MAX_CLIENTS 1
#define LC_SNAPSHOT_KEEP_BUF     0
#define BOARD_DEFAULT_RESOLUTION HAL_RES_VGA
#define BOARD_DEFAULT_FPS        15

#define BOARD_LED_PIN            5    /* status LED, 1 = on */
#define BOARD_LED_ACTIVE_LOW     0
#define BOARD_BUTTON_PIN         7    /* setup/reset button, deep-sleep wake */
#define BOARD_BUTTON_ACTIVE_LOW  1
#define BOARD_WAKE_PIN_MASK      (1u << 7)
#define BOARD_IR_LED_PIN         2    /* 1 = IR illuminator on */

/* Powered from the car, no battery: nothing to measure. */
#define BOARD_HAS_VBAT           0
#define BOARD_HAS_SD             0

/* The sensor is on GPIO0 (SCL) and GPIO1 (SDA), the I2C2/UART2 pins; the
 * console stays on UART1. Stock drives it with the I2C2 block, but the
 * SDK's I2C2 driver loses transfers under our interrupt load, so we drive
 * SCCB in software (camera_bk7252.c) and take UART2 down. */
#define BOARD_CAM_SCCB_SOFT      1
#define BOARD_CAM_SCL_PIN        0
#define BOARD_CAM_SDA_PIN        1

/* The stock bootloader's FAL table differs from the SDK's 2 MB one: OTA
 * packages go to "download" at 0x143000. Its 0xBB000 bytes run into the
 * settings sector at 0x1FD000, so we use only what ends below it. */
#define BOARD_OTA_DL_ADDR        0x143000u
#define BOARD_OTA_DL_SIZE        0xBA000u

/* RF calibration TLV, with this unit's MAC, in the stock "rfparam"
 * partition rather than at the SDK's 0x1E0000. */
#define BOARD_RF_TLV_ADDR        0x1FE000

/* The stock firmware found a GC0311 on our unit (boot log). Use this
 * firmware's sensor tables, not the A13 mini's, for the sensors both know:
 * they carry this camera's tuning and orientation. */
#define BOARD_STOCK_TABLES_REARVIEW 1
