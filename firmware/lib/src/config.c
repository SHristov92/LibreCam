#include "lc/config.h"
#include "lc/board.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Key and magic keep the project's original "A9" name so settings saved
 * by earlier builds still load. */
#define CFG_KEY     "a9cfg"
#define CFG_MAGIC   0x41394346u /* "A9CF" */
#define CFG_VERSION 3
/* Size of lc_config_t in version 1 (it ended with the camera settings)
 * and version 2 (it ended with tz_min). */
#define CFG_V1_SIZE (offsetof(lc_config_t, camera) + sizeof(hal_camera_cfg_t))
#define CFG_V2_SIZE offsetof(lc_config_t, ip_static)

typedef struct {
    uint32_t    magic;
    uint32_t    version;
    lc_config_t cfg;
} stored_config_t;

lc_config_t g_config;

static void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

void config_defaults(lc_config_t *c)
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
    c->ip_static = 0;
    copy_str(c->ip_mask, sizeof(c->ip_mask), "255.255.255.0");
    c->image.ae = 1;
    c->image.exposure = 40;
    c->image.gain = 20;
    c->image.awb = HAL_AWB_AUTO;
    c->image.flicker = 50;
}

/* The project was called "A9" before LibreCam: names still matching the old
 * defaults ("A9-Setup-XXXX", "a9-xxxxxx") follow the rename. */
static void rename_old_defaults(lc_config_t *c)
{
    char tmp[40];

    if (!strncmp(c->ap_ssid, "A9-Setup-", 9) && strlen(c->ap_ssid) == 13) {
        snprintf(tmp, sizeof(tmp), BOARD_AP_SSID_PREFIX "-%s", c->ap_ssid + 9);
        copy_str(c->ap_ssid, sizeof(c->ap_ssid), tmp);
    }
    if (!strncmp(c->device_name, "a9-", 3) && strlen(c->device_name) == 9) {
        snprintf(tmp, sizeof(tmp), BOARD_HOSTNAME_PREFIX "-%s", c->device_name + 3);
        copy_str(c->device_name, sizeof(c->device_name), tmp);
    }
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
    } else if (s.version == 2 && n >= (int)(offsetof(stored_config_t, cfg) + CFG_V2_SIZE)) {
        /* Older layouts: keep their fields, defaults for the newer ones. */
        memcpy(&g_config, &s.cfg, CFG_V2_SIZE);
    } else if (s.version == 1 && n >= (int)(offsetof(stored_config_t, cfg) + CFG_V1_SIZE)) {
        memcpy(&g_config, &s.cfg, CFG_V1_SIZE);
    }
    rename_old_defaults(&g_config);
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

static int parse_int(const char *v, long min, long max, long *out)
{
    char *end;
    long n;

    if (!v || !*v)
        return -1;
    n = strtol(v, &end, 10);
    if (*end || n < min || n > max)
        return -1;
    *out = n;
    return 0;
}

/* Accepts a dotted IPv4 address ("192.168.1.20"); the empty string too
 * when allow_empty. */
static int ipv4_ok(const char *v, int allow_empty)
{
    int parts = 0;

    if (!*v)
        return allow_empty;
    while (parts < 4) {
        unsigned n = 0;
        int digits = 0;

        while (*v >= '0' && *v <= '9' && digits < 4) {
            n = n * 10 + (unsigned)(*v++ - '0');
            digits++;
        }
        if (!digits || digits > 3 || n > 255)
            return 0;
        parts++;
        if (parts < 4 && *v++ != '.')
            return 0;
    }
    return !*v;
}

int config_set(lc_config_t *c, const char *key, const char *value)
{
    unsigned long n;
    long i;

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

#define IP_FIELD(name)                                                       \
    if (!strcmp(key, #name)) {                                               \
        if (!ipv4_ok(value, 1))                                              \
            return -1;                                                       \
        copy_str(c->name, sizeof(c->name), value);                           \
        return 0;                                                            \
    }
    IP_FIELD(ip_addr)
    IP_FIELD(ip_mask)
    IP_FIELD(ip_gw)
    IP_FIELD(ip_dns)
#undef IP_FIELD
    if (!strcmp(key, "ip_static") && !parse_uint(value, 0, 1, &n)) {
        c->ip_static = (uint8_t)n;
        return 0;
    }

#define IMAGE_LEVEL(name)                                                    \
    if (!strcmp(key, #name) && !parse_int(value, -4, 4, &i)) {               \
        c->image.name = (int8_t)i;                                           \
        return 0;                                                            \
    }
    IMAGE_LEVEL(brightness)
    IMAGE_LEVEL(contrast)
    IMAGE_LEVEL(saturation)
    IMAGE_LEVEL(ae_level)
#undef IMAGE_LEVEL
    if (!strcmp(key, "ae") && !parse_uint(value, 0, 1, &n)) {
        c->image.ae = (uint8_t)n;
        return 0;
    }
    if (!strcmp(key, "exposure") && !parse_uint(value, 1, 100, &n)) {
        c->image.exposure = (uint8_t)n;
        return 0;
    }
    if (!strcmp(key, "gain") && !parse_uint(value, 0, 100, &n)) {
        c->image.gain = (uint8_t)n;
        return 0;
    }
    if (!strcmp(key, "awb") && !parse_uint(value, 0, HAL_AWB_INCANDESCENT, &n)) {
        c->image.awb = (uint8_t)n;
        return 0;
    }
    if (!strcmp(key, "flicker") && !parse_uint(value, 50, 60, &n) && (n == 50 || n == 60)) {
        c->image.flicker = (uint8_t)n;
        return 0;
    }
    if (!strcmp(key, "night") && !parse_uint(value, 0, 1, &n)) {
        c->image.night = (uint8_t)n;
        return 0;
    }
    if (!strcmp(key, "tz_min") && !parse_int(value, -14 * 60, 14 * 60, &i)) {
        c->tz_min = (int16_t)i;
        return 0;
    }
    return -1;
}

const char *config_check(const lc_config_t *c)
{
    if (c->ip_static && (!c->ip_addr[0] || !c->ip_mask[0]))
        return "a static address needs an IP address and a netmask";
    return NULL;
}
