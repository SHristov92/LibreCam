#include "a9/config.h"
#include "a9/board.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFG_KEY     "a9cfg"
#define CFG_MAGIC   0x41394346u /* "A9CF" */
#define CFG_VERSION 2
/* Size of a9_config_t in version 1 (it ended with the camera settings). */
#define CFG_V1_SIZE (offsetof(a9_config_t, camera) + sizeof(hal_camera_cfg_t))

typedef struct {
    uint32_t    magic;
    uint32_t    version;
    a9_config_t cfg;
} stored_config_t;

a9_config_t g_config;

static void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

void config_defaults(a9_config_t *c)
{
    uint8_t mac[6];

    memset(c, 0, sizeof(*c));
    hal_get_mac(mac);
    snprintf(c->device_name, sizeof(c->device_name), BOARD_HOSTNAME_PREFIX "-%02x%02x%02x",
             mac[3], mac[4], mac[5]);
    snprintf(c->ap_ssid, sizeof(c->ap_ssid), BOARD_AP_SSID_PREFIX "-%02X%02X", mac[4], mac[5]);
    c->ap_channel = 6;
    c->http_port = 80;
    c->rtsp_port = 554;
    copy_str(c->rtsp_path, sizeof(c->rtsp_path), "/live");
    c->camera.resolution = BOARD_DEFAULT_RESOLUTION;
    c->camera.fps = BOARD_DEFAULT_FPS;
    c->camera.quality = 70;
    c->camera.flip = 0;
    c->timelapse_s = 0;
    c->clip_s = 30;
    c->led = 1;
    c->tz_min = 0;
}

void config_load(void)
{
    stored_config_t s;
    int n = hal_kv_get(CFG_KEY, &s, sizeof(s));

    config_defaults(&g_config);
    if (n < (int)offsetof(stored_config_t, cfg) || s.magic != CFG_MAGIC)
        return;
    if (s.version == CFG_VERSION && n == (int)sizeof(s)) {
        g_config = s.cfg;
    } else if (s.version == 1 && n >= (int)(offsetof(stored_config_t, cfg) + CFG_V1_SIZE)) {
        /* Older layout: keep its fields, defaults for the newer ones. */
        memcpy(&g_config, &s.cfg, CFG_V1_SIZE);
    }
}

int config_save(void)
{
    stored_config_t s;

    s.magic = CFG_MAGIC;
    s.version = CFG_VERSION;
    s.cfg = g_config;
    return hal_kv_set(CFG_KEY, &s, sizeof(s));
}

void config_factory_reset(void)
{
    hal_kv_erase_all();
    config_defaults(&g_config);
}

static int parse_uint(const char *v, unsigned long min, unsigned long max,
                      unsigned long *out)
{
    char *end;
    unsigned long n;

    if (!v || !*v)
        return -1;
    n = strtoul(v, &end, 10);
    if (*end || n < min || n > max)
        return -1;
    *out = n;
    return 0;
}

int config_set(a9_config_t *c, const char *key, const char *value)
{
    unsigned long n;

#define STR_FIELD(name)                                      \
    if (!strcmp(key, #name)) {                               \
        if (strlen(value) >= sizeof(c->name))                \
            return -1;                                       \
        copy_str(c->name, sizeof(c->name), value);           \
        return 0;                                            \
    }
    STR_FIELD(device_name)
    STR_FIELD(sta_ssid)
    STR_FIELD(sta_pass)
    STR_FIELD(ap_ssid)
    STR_FIELD(admin_pass)
#undef STR_FIELD

    if (!strcmp(key, "ap_pass")) {
        /* WPA2 needs 8..63 characters; empty means open. */
        size_t l = strlen(value);
        if (l != 0 && (l < 8 || l > 63))
            return -1;
        copy_str(c->ap_pass, sizeof(c->ap_pass), value);
        return 0;
    }
    if (!strcmp(key, "rtsp_path")) {
        if (value[0] != '/' || strlen(value) >= sizeof(c->rtsp_path))
            return -1;
        copy_str(c->rtsp_path, sizeof(c->rtsp_path), value);
        return 0;
    }
    if (!strcmp(key, "ap_channel") && !parse_uint(value, 1, 13, &n)) {
        c->ap_channel = (uint8_t)n;
        return 0;
    }
    if (!strcmp(key, "http_port") && !parse_uint(value, 1, 65535, &n)) {
        c->http_port = (uint16_t)n;
        return 0;
    }
    if (!strcmp(key, "rtsp_port") && !parse_uint(value, 1, 65535, &n)) {
        c->rtsp_port = (uint16_t)n;
        return 0;
    }
    if (!strcmp(key, "resolution") && !parse_uint(value, 0, HAL_RES_HD, &n)) {
        c->camera.resolution = (hal_resolution_t)n;
        return 0;
    }
    if (!strcmp(key, "fps") && !parse_uint(value, 1, 30, &n)) {
        c->camera.fps = (uint8_t)n;
        return 0;
    }
    if (!strcmp(key, "quality") && !parse_uint(value, 1, 100, &n)) {
        c->camera.quality = (uint8_t)n;
        return 0;
    }
    if (!strcmp(key, "flip") && !parse_uint(value, 0, 3, &n)) {
        c->camera.flip = (uint8_t)n;
        return 0;
    }
    if (!strcmp(key, "timelapse_s") && !parse_uint(value, 0, 65535, &n)) {
        c->timelapse_s = (uint16_t)n;
        return 0;
    }
    if (!strcmp(key, "clip_s") && !parse_uint(value, 5, 600, &n)) {
        c->clip_s = (uint16_t)n;
        return 0;
    }
    if (!strcmp(key, "led") && !parse_uint(value, 0, 1, &n)) {
        c->led = (uint8_t)n;
        return 0;
    }
    if (!strcmp(key, "tz_min")) {
        char *end;
        long v = strtol(value, &end, 10);
        if (!*value || *end || v < -14 * 60 || v > 14 * 60)
            return -1;
        c->tz_min = (int16_t)v;
        return 0;
    }
    return -1;
}
