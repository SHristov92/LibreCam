/*
 * Hardware abstraction layer.
 *
 * Everything chip specific lives behind this interface so the application
 * (config, Wi-Fi state machine, HTTP and RTSP servers) builds unchanged for
 * the BK7252 target and for the Linux host build used for testing.
 *
 * Networking is not abstracted: both lwIP (on the camera) and Linux expose the
 * BSD socket API, so the servers use sockets directly.
 */
#ifndef A9_HAL_H
#define A9_HAL_H

#include <stddef.h>
#include <stdint.h>

/* ---- System ---------------------------------------------------------- */

void     hal_init(void);
uint32_t hal_millis(void);
void     hal_sleep_ms(uint32_t ms);
void     hal_reboot(void);
/* Fills mac[6] with the station MAC address. */
void     hal_get_mac(uint8_t mac[6]);

typedef void (*hal_task_fn)(void *arg);
/* Starts a detached task/thread. Returns 0 on success. */
int      hal_task_start(const char *name, hal_task_fn fn, void *arg,
                        uint32_t stack_bytes, int priority);

typedef struct hal_mutex hal_mutex_t;
hal_mutex_t *hal_mutex_create(void);
void         hal_mutex_lock(hal_mutex_t *m);
void         hal_mutex_unlock(hal_mutex_t *m);

/* ---- Persistent key/value storage (flash) ---------------------------- */

/* Returns number of bytes read, or -1 if the key does not exist. */
int hal_kv_get(const char *key, void *buf, size_t len);
int hal_kv_set(const char *key, const void *buf, size_t len);
int hal_kv_erase_all(void);

/* ---- Wi-Fi ----------------------------------------------------------- */

typedef enum {
    HAL_WIFI_OFF = 0,
    HAL_WIFI_AP,
    HAL_WIFI_STA_CONNECTING,
    HAL_WIFI_STA_CONNECTED,
    HAL_WIFI_STA_FAILED,
} hal_wifi_state_t;

int  hal_wifi_start_ap(const char *ssid, const char *password, uint8_t channel);
int  hal_wifi_start_sta(const char *ssid, const char *password);
void hal_wifi_stop(void);
hal_wifi_state_t hal_wifi_state(void);
/* Current IPv4 address as dotted string, or "0.0.0.0". */
void hal_wifi_ip(char *buf, size_t len);
int  hal_wifi_rssi(void);

/* ---- Camera ---------------------------------------------------------- */

typedef enum {
    HAL_RES_QVGA = 0,   /* 320x240  */
    HAL_RES_VGA,        /* 640x480  */
    HAL_RES_HD,         /* 1280x720 (only if the sensor supports it) */
} hal_resolution_t;

typedef struct {
    hal_resolution_t resolution;
    uint8_t          fps;      /* target frame rate */
    uint8_t          quality;  /* 1..100, mapped onto the JPEG encoder */
    uint8_t          flip;     /* bit0 = horizontal mirror, bit1 = vertical flip */
} hal_camera_cfg_t;

/* The camera produces baseline JPEG frames (BK7252 has a hardware JPEG
 * encoder and no H.264 encoder, so the stream is MJPEG over RTP). */
typedef struct {
    const uint8_t *data;
    size_t         len;
    uint32_t       timestamp_ms;
    uint32_t       seq;          /* increments on every new frame */
} hal_frame_t;

int  hal_camera_start(const hal_camera_cfg_t *cfg);
void hal_camera_stop(void);
/* Copies the newest frame whose seq is greater than *last_seq into buf.
 * Blocks up to timeout_ms. Returns frame length, 0 on timeout, -1 on error,
 * or -(needed) if buf is too small. Updates *last_seq and *ts_ms. */
int  hal_camera_get_frame(uint8_t *buf, size_t cap, uint32_t *last_seq,
                          uint32_t *ts_ms, uint32_t timeout_ms);

/* ---- Board GPIO ------------------------------------------------------ */

void hal_led_set(int on);
/* Returns 1 while the reset/config button is pressed. */
int  hal_button_pressed(void);

#endif
