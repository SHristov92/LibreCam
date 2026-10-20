/*
 * "A9 mini" camera on a Beken BK7252N (48-pin), board INNO-IPC-48N-V2.5, 2 MB flash.
 * Pins come from the stock firmware; see docs/device-spec.md section 2.
 */
#define BOARD_NAME   "a9-mini-bk7252"
#define BOARD_MCU    "bk7252"

/* RAM: 256 KB SRAM, ~150 KB heap after the SDK. The JPEG encoder's bitrate
 * control keeps VGA frames under ~35 KB. The HAL holds two frame buffers,
 * each RTSP client and each snapshot request one more. */
#define A9_FRAME_BUF             (40 * 1024)
#define A9_RTSP_MAX_CLIENTS      1
#define A9_SNAPSHOT_KEEP_BUF     0
#define BOARD_DEFAULT_RESOLUTION HAL_RES_VGA
#define BOARD_DEFAULT_FPS        15

#define BOARD_LED_PIN            2    /* status LED, 1 = on */
#define BOARD_LED_ACTIVE_LOW     0
#define BOARD_BUTTON_PIN         5    /* Button2: power/setup, deep-sleep wake */
#define BOARD_BUTTON_ACTIVE_LOW  1

/* Our unit runs from an external 3.3 V regulator. Stock firmware sleeps at
 * <= 3400 mV, which would never wake on 3.3 V, so flag low battery below 3.1 V. */
#define BOARD_VBAT_LOW_MV        3100

/* Not used by the HAL yet. */
#define BOARD_BUTTON1_PIN        4    /* active low */
#define BOARD_IR_LED_PIN         7    /* 1 = IR illuminator on */
#define BOARD_SPK_AMP_PIN        9    /* 1 = speaker amplifier on */
#define BOARD_SD_DETECT_PIN      12   /* 0 = card inserted */
#define BOARD_CHARGE_PIN         13   /* charger status, deep-sleep wake */
#define BOARD_CAM_I2C_PORT       1    /* I2C1: SCL GPIO20, SDA GPIO21 */
