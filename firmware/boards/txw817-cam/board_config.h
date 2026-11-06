/*
 * Mini Wi-Fi camera on a Taixin TXW817-810 (C-SKY CK803, 1 MB internal flash,
 * 272 KB SRAM, DVP sensor such as GC0328C/SP0828/GC0329, hardware JPEG).
 *
 * PLACEHOLDER: nothing here has been read from a real board yet. Pins and the
 * settings sector come from the stock firmware dump and the PCB; until then
 * the HAL runs without LED, buttons and IR, and picks the settings sector
 * itself (see hal/txw81x/README.md).
 */
#define BOARD_NAME   "txw817-cam"
#define BOARD_MCU    "txw81x"

/* RAM: 272 KB for everything, Wi-Fi and lwIP included. The HAL holds two
 * frame buffers plus 60 KB of JPEG encoder node memory; each RTSP client,
 * the web live view and a snapshot hold one more frame buffer while active. */
#define LC_FRAME_BUF             (32 * 1024)
#define LC_RTSP_MAX_CLIENTS      1
#define LC_HTTP_STREAM_MAX_CLIENTS 1
#define LC_SNAPSHOT_KEEP_BUF     0
#define BOARD_DEFAULT_RESOLUTION HAL_RES_VGA
#define BOARD_DEFAULT_FPS        15

/* GPIO numbers are the SDK's enum pin_name: PA0..PA15 = 0..15,
 * PB0..PB15 = 16..31, PC0..PC15 = 32..47. -1 = not wired / unknown.
 * On several A9-style TXW817 boards the blue LED is on PA10, which is also
 * the debug data pin; confirm before enabling it. */
#define BOARD_LED_PIN            -1
#define BOARD_LED_ACTIVE_LOW     0
#define BOARD_BUTTON_PIN         -1
#define BOARD_BUTTON_ACTIVE_LOW  1

/* Settings sector (4 KB, absolute flash address). Leave undefined to use the
 * sector just below the SDK's syscfg sectors; set it once the dump shows
 * which sectors the stock layout leaves free. */
/* #define BOARD_KV_ADDR 0xF0000 */
