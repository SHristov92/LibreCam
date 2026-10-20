/*
 * Board configuration. Every camera model has boards/<name>/board_config.h,
 * selected at build time by putting that directory on the include path.
 * This header pulls it in and supplies defaults for anything it leaves out.
 */
#ifndef LC_BOARD_H
#define LC_BOARD_H

#include "lc/hal.h"
#include "board_config.h"

#ifndef BOARD_NAME
#error "board_config.h must define BOARD_NAME"
#endif
#ifndef BOARD_MCU
#error "board_config.h must define BOARD_MCU"
#endif

/* Largest JPEG frame accepted. Each RTSP client and the snapshot handler
 * allocate one buffer of this size, so size it to the MCU's free RAM. */
#ifndef LC_FRAME_BUF
#define LC_FRAME_BUF (96 * 1024)
#endif
#ifndef LC_RTSP_MAX_CLIENTS
#define LC_RTSP_MAX_CLIENTS 2
#endif
/* 1 keeps the snapshot buffer allocated between requests; 0 frees it. */
#ifndef LC_SNAPSHOT_KEEP_BUF
#define LC_SNAPSHOT_KEEP_BUF 1
#endif

/* Defaults used on first boot and after a factory reset. */
#ifndef BOARD_DEFAULT_RESOLUTION
#define BOARD_DEFAULT_RESOLUTION HAL_RES_VGA
#endif
#ifndef BOARD_DEFAULT_FPS
#define BOARD_DEFAULT_FPS 15
#endif
#ifndef BOARD_HOSTNAME_PREFIX
#define BOARD_HOSTNAME_PREFIX "librecam"
#endif
#ifndef BOARD_AP_SSID_PREFIX
#define BOARD_AP_SSID_PREFIX "LibreCam-Setup"
#endif

/* GPIO used by the HAL. -1 means the board has none. */
#ifndef BOARD_LED_PIN
#define BOARD_LED_PIN -1
#endif
#ifndef BOARD_LED_ACTIVE_LOW
#define BOARD_LED_ACTIVE_LOW 0
#endif
#ifndef BOARD_BUTTON_PIN
#define BOARD_BUTTON_PIN -1
#endif
#ifndef BOARD_BUTTON_ACTIVE_LOW
#define BOARD_BUTTON_ACTIVE_LOW 1
#endif

/* Supply voltage (mV) below which the board reports "low battery".
 * -1 disables the check. */
#ifndef BOARD_VBAT_LOW_MV
#define BOARD_VBAT_LOW_MV -1
#endif

#endif
