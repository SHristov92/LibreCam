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
#ifndef LC_HAL_H
#define LC_HAL_H

#include <stddef.h>
#include <stdint.h>

/* ---- System ---------------------------------------------------------- */

void     hal_init(void);
uint32_t hal_millis(void);
void     hal_sleep_ms(uint32_t ms);
void     hal_reboot(void);
/* Powers everything down until a button is pressed; the chip then boots
 * from scratch. Never returns. */
void     hal_deep_sleep(void);
/* Fills mac[6] with the station MAC address. */
void     hal_get_mac(uint8_t mac[6]);

/* Memory for large buffers (camera frames). On chips with a separate RAM
 * bank this keeps them out of the heap the network stack needs. Release
 * with free(). */
void    *hal_malloc_large(size_t size);

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

/* Static IPv4 settings for station mode, as dotted strings. */
typedef struct {
    const char *ip;
    const char *mask;
    const char *gw;
    const char *dns;
} hal_ip_cfg_t;

int  hal_wifi_start_ap(const char *ssid, const char *password, uint8_t channel);
/* ip == NULL: get the address by DHCP. */
int  hal_wifi_start_sta(const char *ssid, const char *password, const hal_ip_cfg_t *ip);
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

typedef struct {
    const char *sensor;       /* detected sensor, NULL if none */
    uint16_t    width;        /* size actually streamed, 0 if not running */
    uint16_t    height;
    uint8_t     resolutions;  /* bit n set: hal_resolution_t n is supported;
                                 0 = unknown/none of them */
} hal_camera_info_t;

int  hal_camera_start(const hal_camera_cfg_t *cfg);
/* Describes the detected sensor and the stream. Valid after hal_camera_start. */
void hal_camera_info(hal_camera_info_t *info);
void hal_camera_stop(void);
/* Image controls. Values use these ranges on every sensor; the HAL maps
 * them onto the sensor's registers. Controls can change while streaming. */
typedef enum {
    HAL_CTRL_BRIGHTNESS = 0,  /* -4..4, 0 = sensor default */
    HAL_CTRL_CONTRAST,        /* -4..4 */
    HAL_CTRL_SATURATION,      /* -4..4, -4 = black and white */
    HAL_CTRL_AE,              /* 1 = auto exposure and gain, 0 = manual */
    HAL_CTRL_AE_LEVEL,        /* -4..4, auto exposure brightness target */
    HAL_CTRL_EXPOSURE,        /* manual exposure time, 1..100, shortest to longest */
    HAL_CTRL_GAIN,            /* manual gain, 0..100, lowest to highest */
    HAL_CTRL_AWB,             /* white balance: hal_awb_t */
    HAL_CTRL_FLICKER,         /* mains frequency for anti-flicker: 50 or 60 */
    HAL_CTRL_FLIP,            /* bit0 = horizontal mirror, bit1 = vertical flip */
    HAL_CTRL_IR,              /* 1 = IR illuminator on */
    HAL_CTRL_COUNT
} hal_ctrl_t;

typedef enum {
    HAL_AWB_AUTO = 0,
    HAL_AWB_DAYLIGHT,
    HAL_AWB_CLOUDY,
    HAL_AWB_FLUORESCENT,
    HAL_AWB_INCANDESCENT,
} hal_awb_t;

/* Bit n set: control n works with the detected sensor and board. */
uint32_t hal_camera_ctrls(void);
/* Returns 0, or -1 if the control isn't supported or the write failed. */
int  hal_camera_set_ctrl(hal_ctrl_t ctrl, int value);

/* Copies the newest frame whose seq is greater than *last_seq into buf.
 * Blocks up to timeout_ms. Returns frame length, 0 on timeout, -1 on error,
 * or -(needed) if buf is too small. Updates *last_seq and *ts_ms. */
int  hal_camera_get_frame(uint8_t *buf, size_t cap, uint32_t *last_seq,
                          uint32_t *ts_ms, uint32_t timeout_ms);

/* ---- Microphone ------------------------------------------------------ */

/* Starts capture of mono 16-bit PCM at `rate` Hz. Returns 0 on success. */
int  hal_audio_start(uint32_t rate);
/* Reads up to `samples` samples, waiting up to timeout_ms for the first.
 * Returns the number read, 0 on timeout, -1 if capture isn't running. */
int  hal_audio_read(int16_t *buf, size_t samples, uint32_t timeout_ms);

/* ---- Storage (SD card) ----------------------------------------------
 *
 * Paths are absolute from the card root ("/DCIM/IMG_x.jpg"). All calls are
 * safe from any task. hal_storage_poll() is called about once a second and
 * mounts/unmounts as the card is inserted or removed. */

typedef struct hal_file hal_file_t;

/* Returns 1 while a card is mounted. */
int  hal_storage_poll(void);
/* Total and free bytes on the mounted card; -1 if none. */
int  hal_storage_space(uint64_t *total, uint64_t *free_bytes);
/* mode: 'r' read, 'w' create/truncate for writing. NULL on failure. */
hal_file_t *hal_file_open(const char *path, char mode);
int  hal_file_read(hal_file_t *f, void *buf, size_t len);         /* bytes, -1 */
int  hal_file_write(hal_file_t *f, const void *buf, size_t len);  /* bytes, -1 */
int  hal_file_seek(hal_file_t *f, uint32_t pos);
uint32_t hal_file_size(hal_file_t *f);
int  hal_file_close(hal_file_t *f);                               /* flushes */
int  hal_file_remove(const char *path);
/* Creates a directory; succeeds if it already exists. */
int  hal_mkdir(const char *path);
/* Calls cb for each entry (not . or ..); a non-zero return stops the walk. */
typedef int (*hal_dir_cb)(const char *name, uint32_t size, int is_dir, void *ctx);
int  hal_dir_list(const char *path, hal_dir_cb cb, void *ctx);

/* ---- Board GPIO ------------------------------------------------------ */

void hal_led_set(int on);
/* Returns 1 while the reset/config button is pressed. */
int  hal_button_pressed(void);
/* Returns 1 while the user button (snapshot/record) is pressed; 0 if the
 * board has none. */
int  hal_user_button_pressed(void);

/* Supply/battery voltage in mV, or -1 if the board can't measure it.
 * May block for a few ms; the app samples it periodically. */
int  hal_battery_mv(void);

#endif
