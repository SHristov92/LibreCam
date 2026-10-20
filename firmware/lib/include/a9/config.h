#ifndef A9_CONFIG_H
#define A9_CONFIG_H

#include <stdint.h>
#include "a9/hal.h"

#define CFG_STR_LEN 64

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
    uint16_t timelapse_s;   /* SD timelapse interval, 0 = off */
    uint16_t clip_s;        /* length of a recorded clip */
    uint8_t  led;           /* status LED indication on/off */
    int16_t  tz_min;        /* local time offset from UTC, minutes */
} a9_config_t;

extern a9_config_t g_config;

void config_defaults(a9_config_t *c);
/* Loads from flash, falling back to defaults. */
void config_load(void);
int  config_save(void);
void config_factory_reset(void);

/* Sets one field from its string key (as used in the web form).
 * Returns 0 on success, -1 for an unknown key or invalid value. */
int  config_set(a9_config_t *c, const char *key, const char *value);

#endif
