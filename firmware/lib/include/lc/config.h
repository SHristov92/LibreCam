#ifndef LC_CONFIG_H
#define LC_CONFIG_H

#include <stdint.h>
#include "lc/hal.h"

#define CFG_STR_LEN 64

/* Image settings. They apply while streaming (see image.h). */
typedef struct {
    int8_t   brightness;    /* -4..4 */
    int8_t   contrast;      /* -4..4 */
    int8_t   saturation;    /* -4..4 */
    int8_t   ae_level;      /* -4..4, auto exposure target */
    uint8_t  ae;            /* 1 = auto exposure, 0 = manual exposure and gain */
    uint8_t  exposure;      /* manual exposure, 1..100 */
    uint8_t  gain;          /* manual gain, 0..100 */
    uint8_t  awb;           /* hal_awb_t */
    uint8_t  flicker;       /* mains frequency, 50 or 60 Hz */
    uint8_t  night;         /* IR LEDs on, black and white */
} lc_image_t;

typedef struct {
    char     device_name[32];
    /* Station mode: the home network to join. Empty ssid => stay in AP mode. */
    char     sta_ssid[33];
    char     sta_pass[65];
    /* Setup access point. Empty ap_pass => open network. */
    char     ap_ssid[33];
    char     ap_pass[65];
    uint8_t  ap_channel;
    /* Web UI basic-auth password. Empty => no auth. */
    char     admin_pass[33];
    uint16_t http_port;
    uint16_t rtsp_port;
    char     rtsp_path[32];
    hal_camera_cfg_t camera;
    /* Fields below were added in version 2; keep appending at the end so
     * older stored configs load with defaults for the new fields. */
    uint16_t reserved0;     /* was the timelapse interval; kept for the layout */
    uint16_t clip_s;        /* length of a recorded clip */
    uint8_t  led;           /* status LED indication on/off */
    int16_t  tz_min;        /* local time offset from UTC, minutes */
    /* Version 3. */
    uint8_t  ip_static;     /* station mode: 1 = the address below, 0 = DHCP */
    char     ip_addr[16];
    char     ip_mask[16];
    char     ip_gw[16];
    char     ip_dns[16];    /* empty = use the gateway */
    lc_image_t image;
} lc_config_t;

extern lc_config_t g_config;

void config_defaults(lc_config_t *c);
/* Loads from flash, falling back to defaults. */
void config_load(void);
int  config_save(void);
void config_factory_reset(void);

/* Sets one field from its string key (as used in the web form).
 * Returns 0 on success, -1 for an unknown key or invalid value. */
int  config_set(lc_config_t *c, const char *key, const char *value);
/* Checks settings that depend on each other. Returns NULL if they are
 * consistent, otherwise a message for the user. */
const char *config_check(const lc_config_t *c);

#endif
