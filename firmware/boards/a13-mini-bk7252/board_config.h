/*
 * "A13 mini" camera on a Beken BK7252N (48-pin), board INNO-IPC-48N-V2.5, 2 MB flash.
 * Pins come from the stock firmware; see docs/device-spec.md section 2.
 */
#define BOARD_NAME   "a13-mini-bk7252"
#define BOARD_MCU    "bk7252"

/* RAM: frame buffers live in the 256 KB bank at 0x2000000 (hal_malloc_large),
 * leaving SRAM to Wi-Fi/lwIP. The JPEG encoder's bitrate control keeps VGA
 * frames under ~35 KB. The HAL holds two frame buffers; each RTSP client, a
 * snapshot request and a recording clip hold one more while active. */
#define LC_FRAME_BUF             (40 * 1024)
#define LC_RTSP_MAX_CLIENTS      2
#define LC_SNAPSHOT_KEEP_BUF     0
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
#define BOARD_USER_BUTTON_PIN    4    /* Button1, active low: snapshot / record */
#define BOARD_IR_LED_PIN         7    /* 1 = IR illuminator on */
#define BOARD_SPK_AMP_PIN        9    /* 1 = speaker amplifier on */
#define BOARD_SD_DETECT_PIN      12   /* 0 = card inserted */
#define BOARD_CHARGE_PIN         13   /* charger status, deep-sleep wake */
#define BOARD_CAM_I2C_PORT       1    /* I2C1: SCL GPIO20, SDA GPIO21 */
#define BOARD_CAM_SCL_PIN        20
#define BOARD_CAM_SDA_PIN        21
