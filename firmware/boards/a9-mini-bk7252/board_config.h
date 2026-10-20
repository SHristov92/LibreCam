/*
 * "A9 mini" camera on a Beken BK7252/BK7252N.
 * Reference PCB: INO-IPC-A9-V2.4 (Shenzhen Pinmei / Linklemo), 2 MB flash.
 * GPIO numbers are not traced yet; fill them in from the actual board.
 */
#define BOARD_NAME   "a9-mini-bk7252"
#define BOARD_MCU    "bk7252"

#define A9_FRAME_BUF             (48 * 1024)
#define A9_RTSP_MAX_CLIENTS      2
#define BOARD_DEFAULT_RESOLUTION HAL_RES_VGA
#define BOARD_DEFAULT_FPS        15

#define BOARD_LED_PIN            -1   /* TODO */
#define BOARD_LED_ACTIVE_LOW     0
#define BOARD_BUTTON_PIN         -1   /* TODO */
#define BOARD_BUTTON_ACTIVE_LOW  1
