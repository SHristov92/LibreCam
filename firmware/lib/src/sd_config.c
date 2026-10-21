/* Wi-Fi setup from ap.txt / wifi.txt on the SD card; see lc/sd_config.h. */
#include "lc/sd_config.h"

#include "lc/hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_MAX 2048

/* Friendly names in the files -> config_set() keys. */
static const char *const g_ap_keys[][2] = {
    { "ssid", "ap_ssid" }, { "password", "ap_pass" }, { "channel", "ap_channel" },
};
static const char *const g_wifi_keys[][2] = {
    { "ssid", "sta_ssid" }, { "password", "sta_pass" }, { "ip", "ip_addr" },
    { "mask", "ip_mask" }, { "gateway", "ip_gw" }, { "dns", "ip_dns" },
};

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

int sd_config_parse(lc_config_t *c, char *text, int ap, const char **err, int *line)
{
    lc_config_t n = *c;
    char *p = text, *next;
    int ln = 0, have_ssid = 0;
    size_t i, nkeys = ap ? sizeof(g_ap_keys) / sizeof(g_ap_keys[0])
                         : sizeof(g_wifi_keys) / sizeof(g_wifi_keys[0]);

    /* The file describes the whole mode: anything it leaves out is unset. */
    if (ap) {
        n.ap_pass[0] = 0;
        n.sta_ssid[0] = 0;              /* stay in AP mode */
        n.sta_pass[0] = 0;
    } else {
        n.sta_pass[0] = 0;
        n.ip_static = 0;
        n.ip_dns[0] = 0;
    }
    if (!strncmp(p, "\xEF\xBB\xBF", 3)) /* UTF-8 BOM from Notepad */
        p += 3;
    for (; p; p = next) {
        char *key, *val, *eq;
        const char *cfg_key = NULL;

        ln++;
        next = strchr(p, '\n');
        if (next)
            *next++ = 0;
        key = trim(p);
        if (!*key || *key == '#')
            continue;
        eq = strchr(key, '=');
        if (!eq) {
            *err = "expected key = value";
            *line = ln;
            return -1;
        }
        *eq = 0;
        key = trim(key);
        val = trim(eq + 1);
        if (*val == '"' && strlen(val) >= 2 && val[strlen(val) - 1] == '"') {
            val[strlen(val) - 1] = 0;
            val++;
        } else if (*val != '"') {
            char *hash = strstr(val, " #");     /* trailing comment */
            if (hash)
                *hash = 0, val = trim(val);
        }
        for (i = 0; i < nkeys; i++)
            if (!strcmp(key, ap ? g_ap_keys[i][0] : g_wifi_keys[i][0]))
                cfg_key = ap ? g_ap_keys[i][1] : g_wifi_keys[i][1];
        if (!ap && cfg_key && !strcmp(cfg_key, "ip_addr")) {
            n.ip_static = *val != 0;
            if (!*val)
                continue;
        }
        if (cfg_key && strstr(cfg_key, "ssid"))
            have_ssid = *val != 0;
        if (config_set(&n, cfg_key ? cfg_key : key, val) != 0) {
            static char msg[64];

            snprintf(msg, sizeof(msg), cfg_key ? "invalid %.24s%s" : "unknown setting or invalid value: %.24s",
                     key, cfg_key && !strcmp(key, "password") && ap ? " (8 to 63 characters, or empty)" : "");
            *err = msg;
            *line = ln;
            return -1;
        }
    }
    *line = 0;
    if (!have_ssid) {
        *err = "ssid is missing";
        return -1;
    }
    if (!ap && n.ip_static && !n.ip_mask[0])
        config_set(&n, "ip_mask", "255.255.255.0");
    if ((*err = config_check(&n)) != NULL)
        return -1;
    *c = n;
    return 0;
}

static void write_error(const char *path, const char *err, int line)
{
    char epath[32], msg[160];
    hal_file_t *f;
    int len;

    snprintf(epath, sizeof(epath), "%.*s-error.txt", (int)(strlen(path) - 4), path);
    len = line ? snprintf(msg, sizeof(msg), "%s, line %d: %s\r\n", path + 1, line, err)
               : snprintf(msg, sizeof(msg), "%s: %s\r\n", path + 1, err);
    printf("[sdcfg] %s", msg);
    f = hal_file_open(epath, 'w');
    if (f) {
        hal_file_write(f, msg, (size_t)len);
        hal_file_close(f);
    }
}

static int apply_file(const char *path, int ap)
{
    char epath[32];
    hal_file_t *f = hal_file_open(path, 'r');
    char *text;
    const char *err;
    int len, line;

    if (!f)
        return 0;
    text = malloc(FILE_MAX + 1);
    len = text ? hal_file_read(f, text, FILE_MAX + 1) : -1;
    hal_file_close(f);
    if (len < 0 || len > FILE_MAX) {
        free(text);
        write_error(path, len > FILE_MAX ? "file too large" : "can't read it", 0);
        return 0;
    }
    text[len] = 0;
    if (sd_config_parse(&g_config, text, ap, &err, &line) != 0) {
        free(text);
        write_error(path, err, line);
        return 0;
    }
    free(text);
    hal_file_remove(path);
    snprintf(epath, sizeof(epath), "%.*s-error.txt", (int)(strlen(path) - 4), path);
    hal_file_remove(epath);
    printf("[sdcfg] applied %s: %s \"%s\"\n", path + 1, ap ? "access point" : "network",
           ap ? g_config.ap_ssid : g_config.sta_ssid);
    return 1;
}

int sd_config_apply(void)
{
    int n;

    if (!hal_storage_poll())
        return 0;
    n = apply_file(SD_CONFIG_AP, 1);
    n += apply_file(SD_CONFIG_WIFI, 0);
    if (n && config_save() != 0)
        printf("[sdcfg] saving the settings failed\n");
    return n;
}
