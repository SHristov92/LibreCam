/*
 * Small HTTP/1.0 server for the configuration UI and JSON API.
 *
 *   GET  /                 web UI (web/index.html, gzipped and embedded at build time)
 *   GET  /api/status       device state as JSON (mdns_host: the camera's .local name)
 *   GET  /api/config       current settings as JSON (passwords omitted)
 *   POST /api/config       form-encoded settings; saved, then the device reboots
 *   POST /api/image        form-encoded image settings, applied at once;
 *                          saved too when save=1
 *   GET  /stream[?fps=n]   live view: MJPEG (multipart/x-mixed-replace)
 *   POST /api/reboot
 *   POST /api/factory-reset
 *   GET  /snapshot.jpg     latest camera frame
 *   GET  /api/storage      SD card state
 *   GET  /api/files?dir=/DCIM   files in /DCIM, /TIMELAPSE or /VIDEO
 *   GET  /sd/<dir>/<file>  download a file from the card
 *   POST /api/files/delete path=<file>
 *   POST /api/snapshot     save the current frame to the card
 *   POST /api/record       action=start|stop a clip
 *   POST /api/time         utc=<unix seconds>&tz=<minutes east of UTC>
 *   POST /api/ota          firmware update: the raw package as the body
 *                          (Content-Length required); streamed to flash, then
 *                          the camera restarts and the bootloader installs it
 *
 * Connections are served one at a time from a single task; every response
 * closes the connection, which keeps RAM use flat. /stream connections are
 * handed to a task of their own.
 */
#define _DEFAULT_SOURCE   /* strtok_r under -std=c99 (newlib, glibc) */
#include "lc/app.h"
#include "lc/board.h"
#include "lc/clock.h"
#include "lc/config.h"
#include "lc/hal.h"
#include "lc/image.h"
#include "lc/mdns.h"
#include "lc/recorder.h"
#include "lc/servers.h"
#include "lc/web_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lc/net.h"

#define REQ_MAX    4096
#define BOUNDARY   "lcframe"

volatile int g_reboot_requested;

static int g_listen = -1;
static uint8_t *g_snap;
static hal_mutex_t *g_stream_lock;
static int g_stream_clients;

static int send_all(int s, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        int n = send(s, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void respond(int s, int code, const char *reason, const char *type,
                    const void *body, size_t len, const char *extra)
{
    char hdr[384];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n%s\r\n",
                     code, reason, type, (unsigned)len, extra ? extra : "");
    if (send_all(s, hdr, (size_t)n) == 0 && len)
        send_all(s, body, len);
}

static void respond_text(int s, int code, const char *reason, const char *msg)
{
    respond(s, code, reason, "text/plain", msg, strlen(msg), NULL);
}

static void respond_json(int s, const char *json)
{
    respond(s, 200, "OK", "application/json", json, strlen(json), NULL);
}

/* Escapes a string for inclusion in JSON. */
static void json_str(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (; *in && o + 7 < cap; in++) {
        unsigned char ch = (unsigned char)*in;
        if (ch == '"' || ch == '\\') {
            out[o++] = '\\';
            out[o++] = (char)ch;
        } else if (ch < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", ch);
        } else {
            out[o++] = (char)ch;
        }
    }
    out[o] = 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* In-place application/x-www-form-urlencoded decoding. */
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            *o++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

/* Copies the url-decoded value of `key` from a form/query string. */
static int form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    const char *p = body;

    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t n = strcspn(v, "&");
            if (n >= cap)
                return -1;
            memcpy(out, v, n);
            out[n] = 0;
            url_decode(out);
            return 0;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return -1;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    int bits = 0, acc = 0;
    for (; *in && *in != '='; in++) {
        int v = b64val(*in);
        if (v < 0)
            return -1;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o + 1 >= cap)
                return -1;
            out[o++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[o] = 0;
    return (int)o;
}

static const char *find_header(const char *req, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    const char *p = req;
    while ((p = strstr(p, "\r\n")) != NULL) {
        p += 2;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            size_t i = 0;
            while (*v == ' ')
                v++;
            while (v[i] && v[i] != '\r' && i + 1 < cap) {
                out[i] = v[i];
                i++;
            }
            out[i] = 0;
            return out;
        }
    }
    return NULL;
}

/* Basic auth with user "admin" when an admin password is configured. */
static int authorized(const char *req)
{
    char hv[128], dec[96], expect[64];
    if (!g_config.admin_pass[0])
        return 1;
    if (!find_header(req, "Authorization", hv, sizeof(hv)) ||
        strncasecmp(hv, "Basic ", 6) ||
        b64_decode(hv + 6, dec, sizeof(dec)) < 0)
        return 0;
    snprintf(expect, sizeof(expect), "admin:%s", g_config.admin_pass);
    return !strcmp(dec, expect);
}

int http_stream_clients(void)
{
    return g_stream_clients;
}

static void handle_status(int s)
{
    char json[1200], ip[16], name[64], res[24] = "", ctrls[160], host[64];
    hal_camera_info_t cam;
    int i, n = 0;
    hal_wifi_state_t st = hal_wifi_state();
    const char *mode = st == HAL_WIFI_AP ? "ap" :
                       st == HAL_WIFI_STA_CONNECTED ? "sta" :
                       st == HAL_WIFI_STA_CONNECTING ? "connecting" : "offline";

    hal_wifi_ip(ip, sizeof(ip));
    json_str(name, sizeof(name), g_config.device_name);
    mdns_hostname(g_config.device_name, host, sizeof(host));
    hal_camera_info(&cam);
    image_supported_json(ctrls, sizeof(ctrls));
    for (i = 0; i <= HAL_RES_HD; i++)
        if (cam.resolutions & (1u << i))
            n += snprintf(res + n, sizeof(res) - (size_t)n, "%s%d", n ? "," : "", i);
    snprintf(json, sizeof(json),
             "{\"device_name\":\"%s\",\"mode\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"uptime_s\":%u,\"rtsp_url\":\"rtsp://%s:%u%s\",\"rtsp_clients\":%d,"
             "\"firmware\":\"%s\",\"board\":\"%s\",\"mcu\":\"%s\","
             "\"vbat_mv\":%d,\"low_battery\":%s,"
             "\"sensor\":\"%s\",\"stream_width\":%u,\"stream_height\":%u,\"resolutions\":[%s],"
             "\"controls\":%s,\"web_viewers\":%d,\"ip_static\":%s,\"mdns_host\":\"%s.local\","
             "\"ota\":%s%s%s}",
             name, mode, ip, st == HAL_WIFI_STA_CONNECTED ? hal_wifi_rssi() : 0,
             (unsigned)(hal_millis() / 1000), ip, g_config.rtsp_port,
             g_config.rtsp_path, rtsp_active_clients(), LC_FW_VERSION,
             BOARD_NAME, BOARD_MCU, g_vbat_mv, lc_low_battery() ? "true" : "false",
             cam.sensor ? cam.sensor : "", cam.width, cam.height, res, ctrls,
             http_stream_clients(), g_config.ip_static ? "true" : "false", host,
             hal_ota_format() ? "\"" : "", hal_ota_format() ? hal_ota_format() : "null",
             hal_ota_format() ? "\"" : "");
    respond_json(s, json);
}

static void handle_get_config(int s)
{
    char json[1300], name[64], ssid[72], apssid[72], path[48];
    const lc_config_t *c = &g_config;
    const lc_image_t *im = &c->image;

    json_str(name, sizeof(name), c->device_name);
    json_str(ssid, sizeof(ssid), c->sta_ssid);
    json_str(apssid, sizeof(apssid), c->ap_ssid);
    json_str(path, sizeof(path), c->rtsp_path);
    snprintf(json, sizeof(json),
             "{\"device_name\":\"%s\",\"sta_ssid\":\"%s\",\"sta_pass_set\":%s,"
             "\"ap_ssid\":\"%s\",\"ap_pass_set\":%s,\"ap_channel\":%u,"
             "\"admin_pass_set\":%s,\"http_port\":%u,\"rtsp_port\":%u,"
             "\"rtsp_path\":\"%s\",\"resolution\":%d,\"fps\":%u,\"quality\":%u,"
             "\"flip\":%u,\"timelapse_s\":%u,\"clip_s\":%u,\"led\":%u,\"tz_min\":%d,"
             "\"ip_static\":%u,\"ip_addr\":\"%s\",\"ip_mask\":\"%s\",\"ip_gw\":\"%s\","
             "\"ip_dns\":\"%s\",\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,"
             "\"ae\":%u,\"ae_level\":%d,\"exposure\":%u,\"gain\":%u,\"awb\":%u,"
             "\"flicker\":%u,\"night\":%u}",
             name, ssid, c->sta_pass[0] ? "true" : "false", apssid,
             c->ap_pass[0] ? "true" : "false", c->ap_channel,
             c->admin_pass[0] ? "true" : "false", c->http_port, c->rtsp_port,
             path, (int)c->camera.resolution, c->camera.fps, c->camera.quality,
             c->camera.flip, c->timelapse_s, c->clip_s, c->led, c->tz_min,
             c->ip_static, c->ip_addr, c->ip_mask, c->ip_gw, c->ip_dns,
             im->brightness, im->contrast, im->saturation, im->ae, im->ae_level,
             im->exposure, im->gain, im->awb, im->flicker, im->night);
    respond_json(s, json);
}

static int is_password_key(const char *k)
{
    return !strcmp(k, "sta_pass") || !strcmp(k, "ap_pass") || !strcmp(k, "admin_pass");
}

static void handle_post_config(int s, char *body)
{
    lc_config_t next = g_config;
    char *save = NULL, *pair, err[128];
    const char *bad;

    for (pair = strtok_r(body, "&", &save); pair; pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq)
            continue;
        *eq = 0;
        url_decode(pair);
        url_decode(eq + 1);
        /* Empty password fields mean "keep the current one"; a field named
         * clear_<name> explicitly removes it. */
        if (is_password_key(pair) && !eq[1])
            continue;
        if (!strncmp(pair, "clear_", 6) && is_password_key(pair + 6)) {
            config_set(&next, pair + 6, "");
            continue;
        }
        if (config_set(&next, pair, eq + 1) != 0) {
            snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"invalid %.40s\"}", pair);
            respond(s, 400, "Bad Request", "application/json", err, strlen(err), NULL);
            return;
        }
    }
    if (next.camera.resolution != g_config.camera.resolution) {
        hal_camera_info_t cam;

        hal_camera_info(&cam);
        if (cam.resolutions && !(cam.resolutions & (1u << next.camera.resolution))) {
            snprintf(err, sizeof(err),
                     "{\"ok\":false,\"error\":\"%.24s does not support that resolution\"}",
                     cam.sensor ? cam.sensor : "camera");
            respond(s, 400, "Bad Request", "application/json", err, strlen(err), NULL);
            return;
        }
    }
    if ((bad = config_check(&next)) != NULL) {
        snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"%.100s\"}", bad);
        respond(s, 400, "Bad Request", "application/json", err, strlen(err), NULL);
        return;
    }
    g_config = next;
    if (config_save() != 0) {
        respond_text(s, 500, "Internal Server Error", "save failed");
        return;
    }
    respond_json(s, "{\"ok\":true,\"rebooting\":true}");
    g_reboot_requested = 1;
}

static void handle_snapshot(int s)
{
    uint32_t seq = 0, ts;
    int len;
    /* Boards short on RAM free the buffer after each request. */
    uint8_t *snap = LC_SNAPSHOT_KEEP_BUF ? g_snap : NULL;

    if (!snap)
        snap = hal_malloc_large(LC_FRAME_BUF);
    len = snap ? hal_camera_get_frame(snap, LC_FRAME_BUF, &seq, &ts, 2000) : -1;
    if (len <= 0)
        respond_text(s, 503, "Service Unavailable", snap ? "no frame" : "out of memory");
    else
        respond(s, 200, "OK", "image/jpeg", snap, (size_t)len, NULL);
    if (LC_SNAPSHOT_KEEP_BUF)
        g_snap = snap;
    else
        free(snap);
}

/* Image settings apply at once, so sliders can be dragged while watching
 * the live view; the UI sends save=1 when a slider is released, which
 * keeps flash writes to one per adjustment. */
static void handle_image(int s, char *body)
{
    char *save = NULL, *pair, err[96];
    int store = 0;

    for (pair = strtok_r(body, "&", &save); pair; pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq)
            continue;
        *eq = 0;
        url_decode(pair);
        url_decode(eq + 1);
        if (!strcmp(pair, "save")) {
            store = !strcmp(eq + 1, "1");
            continue;
        }
        if (image_set(&g_config, pair, eq + 1) != 0) {
            snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"invalid %.40s\"}", pair);
            respond(s, 400, "Bad Request", "application/json", err, strlen(err), NULL);
            return;
        }
    }
    if (store && config_save() != 0) {
        respond_text(s, 500, "Internal Server Error", "save failed");
        return;
    }
    respond_json(s, "{\"ok\":true}");
}

/* ---- Live view ------------------------------------------------------------
 *
 * Browsers can't play RTSP, but they show an MJPEG stream (multipart/x-mixed-
 * replace) in a plain <img>. It carries the same JPEG frames as the RTSP
 * stream, so it costs no encoding, only a frame buffer and the bandwidth.
 * Each viewer gets a task. Past LC_HTTP_STREAM_MAX_CLIENTS viewers get a 503
 * and the web UI falls back to polling /snapshot.jpg. A closed or reloaded
 * tab frees its slot on the next frame, when the send fails. */

typedef struct {
    int      sock;
    uint32_t gap_ms;    /* minimum time between frames, 0 = every frame */
} stream_arg_t;

static void stream_release(void)
{
    hal_mutex_lock(g_stream_lock);
    g_stream_clients--;
    hal_mutex_unlock(g_stream_lock);
}

static void stream_task(void *arg)
{
    static const char head[] =
        "HTTP/1.0 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=" BOUNDARY
        "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    stream_arg_t a = *(stream_arg_t *)arg;
    uint8_t *frame = hal_malloc_large(LC_FRAME_BUF);
    uint32_t seq = 0, ts, sent_ts = 0;
    char hdr[96];
    int len, n, sent = 0;

    free(arg);
    if (!frame)
        respond_text(a.sock, 503, "Service Unavailable", "out of memory");
    else if (send_all(a.sock, head, sizeof(head) - 1) == 0) {
        for (;;) {
            len = hal_camera_get_frame(frame, LC_FRAME_BUF, &seq, &ts, 1000);
            if (len == -1)
                break;                  /* no camera */
            if (len < 0) {
                hal_sleep_ms(20);       /* frame too big: wait for the next */
                continue;
            }
            if (len == 0 || (sent && ts - sent_ts < a.gap_ms))
                continue;
            n = snprintf(hdr, sizeof(hdr), "--" BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
                         "Content-Length: %d\r\n\r\n", len);
            if (send_all(a.sock, hdr, (size_t)n) != 0 ||
                send_all(a.sock, frame, (size_t)len) != 0 ||
                send_all(a.sock, "\r\n", 2) != 0)
                break;
            sent = 1;
            sent_ts = ts;
        }
    }
    stream_release();
    free(frame);
    closesocket(a.sock);
}

/* Returns 1 when the socket was handed to a stream task. */
static int handle_stream(int s, const char *query)
{
    stream_arg_t *a;
    char v[8];
    unsigned long fps = 0;
    int full;

    hal_mutex_lock(g_stream_lock);
    full = g_stream_clients >= LC_HTTP_STREAM_MAX_CLIENTS;
    if (!full)
        g_stream_clients++;
    hal_mutex_unlock(g_stream_lock);
    if (full) {
        respond_text(s, 503, "Service Unavailable", "too many viewers");
        return 0;
    }
    if (query && form_get(query, "fps", v, sizeof(v)) == 0)
        fps = strtoul(v, NULL, 10);
    a = malloc(sizeof(*a));
    if (a) {
        a->sock = s;
        a->gap_ms = fps ? 1000 / fps : 0;
    }
    if (!a || hal_task_start("mjpeg", stream_task, a, 3072, 5) != 0) {
        free(a);
        stream_release();
        respond_text(s, 503, "Service Unavailable", "out of memory");
        return 0;
    }
    return 1;
}

/* ---- Storage ------------------------------------------------------------ */

/* Accepts only /DCIM/x, /TIMELAPSE/x, /VIDEO/x (or the bare folder when
 * dir_ok) with no traversal. */
static int sd_path_ok(const char *p, int dir_ok)
{
    static const char *const dirs[] = { REC_DIR_PHOTO, REC_DIR_TIMELAPSE, REC_DIR_VIDEO };
    size_t i;

    if (strstr(p, "..") || strchr(p, '\\') || strlen(p) > 96)
        return 0;
    for (i = 0; i < 3; i++) {
        size_t l = strlen(dirs[i]);
        if (!strncmp(p, dirs[i], l) &&
            ((dir_ok && !p[l]) || (p[l] == '/' && p[l + 1] && !strchr(p + l + 1, '/'))))
            return 1;
    }
    return 0;
}

static void handle_storage(int s)
{
    char json[320], last[96];
    recorder_status_t st;

    recorder_status(&st);
    json_str(last, sizeof(last), st.last_file);
    snprintf(json, sizeof(json),
             "{\"mounted\":%s,\"total_mb\":%u,\"free_mb\":%u,\"recording\":%s,"
             "\"rec_elapsed_s\":%u,\"last_file\":\"%s\",\"time_set\":%s}",
             st.mounted ? "true" : "false", (unsigned)(st.total_bytes >> 20),
             (unsigned)(st.free_bytes >> 20), st.recording ? "true" : "false",
             (unsigned)st.rec_elapsed_s, last, lc_time_valid() ? "true" : "false");
    respond_json(s, json);
}

struct list_ctx {
    int sock, n, err;
};

static int list_entry(const char *name, uint32_t size, int is_dir, void *ctx)
{
    struct list_ctx *c = ctx;
    char item[200], esc[140];
    size_t l = strlen(name);

    if (is_dir || (l > 4 && !strcmp(name + l - 4, ".idx")))
        return 0;
    json_str(esc, sizeof(esc), name);
    snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"size\":%u}",
             c->n ? "," : "", esc, (unsigned)size);
    if (send_all(c->sock, item, strlen(item)) != 0) {
        c->err = 1;
        return 1;
    }
    return ++c->n >= 2000;
}

static void handle_files(int s, const char *query)
{
    static const char hdr[] = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                              "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    char dir[64];
    struct list_ctx c = { s, 0, 0 };

    if (!query || form_get(query, "dir", dir, sizeof(dir)) || !sd_path_ok(dir, 1)) {
        respond_text(s, 400, "Bad Request", "dir must be /DCIM, /TIMELAPSE or /VIDEO");
        return;
    }
    if (send_all(s, hdr, sizeof(hdr) - 1) != 0)
        return;
    send_all(s, "{\"files\":[", 10);
    hal_dir_list(dir, list_entry, &c);
    if (!c.err)
        send_all(s, "]}", 2);
}

static void handle_download(int s, const char *url_path)
{
    char path[128], hdr[256];
    const char *ext;
    hal_file_t *f;
    uint8_t *buf;
    uint32_t left;
    int n;

    snprintf(path, sizeof(path), "%s", url_path + 3);        /* strip "/sd" */
    url_decode(path);
    if (!sd_path_ok(path, 0) || !(f = hal_file_open(path, 'r'))) {
        respond_text(s, 404, "Not Found", "no such file");
        return;
    }
    buf = malloc(4096);
    if (!buf) {
        hal_file_close(f);
        respond_text(s, 503, "Service Unavailable", "out of memory");
        return;
    }
    ext = strrchr(path, '.');
    left = hal_file_size(f);
    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                 "Content-Disposition: %s; filename=\"%s\"\r\nConnection: close\r\n\r\n",
                 ext && !strcmp(ext, ".jpg") ? "image/jpeg" : "video/x-msvideo", (unsigned)left,
                 ext && !strcmp(ext, ".jpg") ? "inline" : "attachment", strrchr(path, '/') + 1);
    if (send_all(s, hdr, (size_t)n) == 0) {
        while (left && (n = hal_file_read(f, buf, 4096)) > 0) {
            if (send_all(s, buf, (size_t)n) != 0)
                break;
            left -= (uint32_t)n;
        }
    }
    free(buf);
    hal_file_close(f);
}

static void handle_delete(int s, const char *body)
{
    char path[128];

    if (form_get(body, "path", path, sizeof(path)) || !sd_path_ok(path, 0)) {
        respond_text(s, 400, "Bad Request", "bad path");
        return;
    }
    if (hal_file_remove(path) != 0) {
        respond_text(s, 404, "Not Found", "could not delete");
        return;
    }
    respond_json(s, "{\"ok\":true}");
}

static void handle_take_snapshot(int s)
{
    char path[64], json[128];

    if (recorder_snapshot(path, sizeof(path)) != 0) {
        respond_text(s, 503, "Service Unavailable", "no SD card or no camera frame");
        return;
    }
    snprintf(json, sizeof(json), "{\"ok\":true,\"path\":\"%s\"}", path);
    respond_json(s, json);
}

static void handle_record(int s, const char *body)
{
    char action[8];

    if (form_get(body, "action", action, sizeof(action)) != 0) {
        respond_text(s, 400, "Bad Request", "action=start|stop");
        return;
    }
    if (!strcmp(action, "stop")) {
        recorder_clip_stop();
    } else if (!strcmp(action, "start")) {
        if (recorder_clip_start() != 0) {
            respond_text(s, 503, "Service Unavailable", "no SD card");
            return;
        }
    } else {
        respond_text(s, 400, "Bad Request", "action=start|stop");
        return;
    }
    respond_json(s, "{\"ok\":true}");
}

/* The web UI posts the browser's clock on load: good enough to name files
 * on the setup AP, where SNTP can't reach the internet. */
static void handle_time(int s, const char *body)
{
    char utc[16], tz[8];
    long tzv = g_config.tz_min;

    if (form_get(body, "tz", tz, sizeof(tz)) == 0) {
        tzv = strtol(tz, NULL, 10);
        if (tzv >= -14 * 60 && tzv <= 14 * 60 && tzv != g_config.tz_min) {
            g_config.tz_min = (int16_t)tzv;
            config_save();
        }
        lc_time_set_tz(g_config.tz_min);
    }
    if (!lc_time_valid() && form_get(body, "utc", utc, sizeof(utc)) == 0) {
        unsigned long t = strtoul(utc, NULL, 10);
        if (t > 1700000000ul)
            lc_time_set((uint32_t)t, g_config.tz_min);
    }
    respond_json(s, lc_time_valid() ? "{\"ok\":true,\"time_set\":true}"
                                    : "{\"ok\":true,\"time_set\":false}");
}

/* Streams the request body into the OTA HAL. `pre` holds body bytes that
 * arrived with the headers; `buf` (REQ_MAX bytes) is free for the rest. */
static void handle_ota(int s, char *buf, const char *pre, size_t pre_len, size_t total)
{
    const char *err = "update failed";
    char json[200], esc[120];
    size_t done;

    if (!hal_ota_format()) {
        respond_text(s, 501, "Not Implemented", "this board has no firmware update");
        return;
    }
    if (!total) {
        respond_text(s, 411, "Length Required", "Content-Length required");
        return;
    }
    if (hal_ota_begin((uint32_t)total, &err) != 0)
        goto fail;
    if (pre_len > total)
        pre_len = total;
    if (pre_len && hal_ota_write(pre, pre_len, &err) != 0)
        goto fail;
    for (done = pre_len; done < total;) {
        size_t want = total - done < REQ_MAX ? total - done : REQ_MAX;
        int n = recv(s, buf, want, 0);

        if (n <= 0) {
            err = "upload interrupted";
            goto fail;
        }
        if (hal_ota_write(buf, (size_t)n, &err) != 0)
            goto fail;
        done += (size_t)n;
    }
    if (hal_ota_finish(&err) != 0)
        goto fail;
    printf("[lc] firmware update received; restarting to install it\n");
    respond_json(s, "{\"ok\":true,\"rebooting\":true}");
    g_reboot_requested = 1;
    return;
fail:
    hal_ota_abort();
    printf("[lc] firmware update rejected: %s\n", err);
    json_str(esc, sizeof(esc), err);
    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", esc);
    respond(s, 400, "Bad Request", "application/json", json, strlen(json), NULL);
}

/* Returns 1 when the socket was handed on and must stay open. */
static int handle_connection(int s)
{
    char *req = malloc(REQ_MAX + 1);
    size_t got = 0;
    char *hdr_end = NULL, method[8], path[128], cl[16];
    size_t body_len = 0;
    int kept = 0, ota = 0;

    if (!req)
        return 0;
    /* Read headers, then the body announced by Content-Length. */
    while (got < REQ_MAX) {
        int n = recv(s, req + got, REQ_MAX - got, 0);
        if (n <= 0)
            goto out;
        got += (size_t)n;
        req[got] = 0;
        if (!hdr_end && (hdr_end = strstr(req, "\r\n\r\n")) != NULL) {
            *hdr_end = 0;
            if (find_header(req, "Content-Length", cl, sizeof(cl)))
                body_len = (size_t)strtoul(cl, NULL, 10);
            /* Firmware uploads are far larger than REQ_MAX: streamed below. */
            if (!strncmp(req, "POST /api/ota ", 14)) {
                ota = 1;
                break;
            }
            if (body_len > REQ_MAX - (size_t)(hdr_end + 4 - req)) {
                respond_text(s, 413, "Payload Too Large", "too large");
                goto out;
            }
        }
        if (hdr_end && got >= (size_t)(hdr_end + 4 - req) + body_len)
            break;
    }
    if (!hdr_end || sscanf(req, "%7s %127s", method, path) != 2) {
        respond_text(s, 400, "Bad Request", "bad request");
        goto out;
    }
    if (!ota)
        hdr_end[4 + body_len] = 0;

    if (!authorized(req)) {
        respond(s, 401, "Unauthorized", "text/plain", "auth required", 13,
                "WWW-Authenticate: Basic realm=\"LibreCam\"\r\n");
        goto out;
    }
    if (ota) {
        /* The body bytes that came with the headers are copied out first,
         * since handle_ota() reuses the request buffer. */
        size_t hdr_len = (size_t)(hdr_end + 4 - req), pre_len = got - hdr_len;
        char *pre = malloc(pre_len ? pre_len : 1);

        if (!pre) {
            respond_text(s, 503, "Service Unavailable", "out of memory");
            goto out;
        }
        memcpy(pre, req + hdr_len, pre_len);
        handle_ota(s, req, pre, pre_len, body_len);
        free(pre);
        goto out;
    }

    if (!strcmp(method, "GET") && (!strcmp(path, "/") || !strcmp(path, "/index.html"))) {
        respond(s, 200, "OK", "text/html; charset=utf-8", web_ui_html, web_ui_html_len,
                "Content-Encoding: gzip\r\n");
    } else if (!strcmp(method, "GET") && !strcmp(path, "/api/status")) {
        handle_status(s);
    } else if (!strcmp(method, "GET") && !strcmp(path, "/api/config")) {
        handle_get_config(s);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/config")) {
        handle_post_config(s, hdr_end + 4);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/reboot")) {
        respond_json(s, "{\"ok\":true}");
        g_reboot_requested = 1;
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/factory-reset")) {
        config_factory_reset();
        respond_json(s, "{\"ok\":true}");
        g_reboot_requested = 1;
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/image")) {
        handle_image(s, hdr_end + 4);
    } else if (!strcmp(method, "GET") && (!strcmp(path, "/stream") || !strncmp(path, "/stream?", 8))) {
        kept = handle_stream(s, path[7] ? path + 8 : NULL);
    } else if (!strcmp(method, "GET") && !strncmp(path, "/snapshot.jpg", 13)) {
        handle_snapshot(s);
    } else if (!strcmp(method, "GET") && !strcmp(path, "/api/storage")) {
        handle_storage(s);
    } else if (!strcmp(method, "GET") && !strncmp(path, "/api/files?", 11)) {
        handle_files(s, path + 11);
    } else if (!strcmp(method, "GET") && !strncmp(path, "/sd/", 4)) {
        handle_download(s, path);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/files/delete")) {
        handle_delete(s, hdr_end + 4);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/snapshot")) {
        handle_take_snapshot(s);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/record")) {
        handle_record(s, hdr_end + 4);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/time")) {
        handle_time(s, hdr_end + 4);
    } else {
        respond_text(s, 404, "Not Found", "not found");
    }
out:
    free(req);
    return kept;
}

static void http_task(void *arg)
{
    (void)arg;
    for (;;) {
        struct timeval to = { 5, 0 };
        int s = accept(g_listen, NULL, NULL);
        if (s < 0) {
            hal_sleep_ms(100);
            continue;
        }
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
        if (!handle_connection(s))
            closesocket(s);
    }
}

int http_server_start(uint16_t port)
{
    g_stream_lock = hal_mutex_create();
    g_listen = net_listen_tcp(port, 4);
    if (!g_stream_lock || g_listen < 0)
        return -1;
    return hal_task_start("http", http_task, NULL, 6144, 6);
}
