/*
 * Small HTTP/1.0 server for the configuration UI and JSON API.
 *
 *   GET  /                 web UI (web/index.html, embedded at build time)
 *   GET  /api/status       device state as JSON
 *   GET  /api/config       current settings as JSON (passwords omitted)
 *   POST /api/config       form-encoded settings; saved, then the device reboots
 *   POST /api/reboot
 *   POST /api/factory-reset
 *   GET  /snapshot.jpg     latest camera frame
 *
 * Connections are served one at a time from a single task; every response
 * closes the connection, which keeps RAM use flat.
 */
#include "a9/app.h"
#include "a9/board.h"
#include "a9/config.h"
#include "a9/hal.h"
#include "a9/servers.h"
#include "a9/web_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "a9/net.h"

#define REQ_MAX    4096

volatile int g_reboot_requested;

static int g_listen = -1;
static uint8_t *g_snap;

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

static void handle_status(int s)
{
    char json[640], ip[16], name[64];
    hal_wifi_state_t st = hal_wifi_state();
    const char *mode = st == HAL_WIFI_AP ? "ap" :
                       st == HAL_WIFI_STA_CONNECTED ? "sta" :
                       st == HAL_WIFI_STA_CONNECTING ? "connecting" : "offline";

    hal_wifi_ip(ip, sizeof(ip));
    json_str(name, sizeof(name), g_config.device_name);
    snprintf(json, sizeof(json),
             "{\"device_name\":\"%s\",\"mode\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"uptime_s\":%u,\"rtsp_url\":\"rtsp://%s:%u%s\",\"rtsp_clients\":%d,"
             "\"firmware\":\"%s\",\"board\":\"%s\",\"mcu\":\"%s\"}",
             name, mode, ip, st == HAL_WIFI_STA_CONNECTED ? hal_wifi_rssi() : 0,
             (unsigned)(hal_millis() / 1000), ip, g_config.rtsp_port,
             g_config.rtsp_path, rtsp_active_clients(), A9_FW_VERSION,
             BOARD_NAME, BOARD_MCU);
    respond_json(s, json);
}

static void handle_get_config(int s)
{
    char json[768], name[64], ssid[72], apssid[72], path[48];
    const a9_config_t *c = &g_config;

    json_str(name, sizeof(name), c->device_name);
    json_str(ssid, sizeof(ssid), c->sta_ssid);
    json_str(apssid, sizeof(apssid), c->ap_ssid);
    json_str(path, sizeof(path), c->rtsp_path);
    snprintf(json, sizeof(json),
             "{\"device_name\":\"%s\",\"sta_ssid\":\"%s\",\"sta_pass_set\":%s,"
             "\"ap_ssid\":\"%s\",\"ap_pass_set\":%s,\"ap_channel\":%u,"
             "\"admin_pass_set\":%s,\"http_port\":%u,\"rtsp_port\":%u,"
             "\"rtsp_path\":\"%s\",\"resolution\":%d,\"fps\":%u,\"quality\":%u,"
             "\"flip\":%u}",
             name, ssid, c->sta_pass[0] ? "true" : "false", apssid,
             c->ap_pass[0] ? "true" : "false", c->ap_channel,
             c->admin_pass[0] ? "true" : "false", c->http_port, c->rtsp_port,
             path, (int)c->camera.resolution, c->camera.fps, c->camera.quality,
             c->camera.flip);
    respond_json(s, json);
}

static int is_password_key(const char *k)
{
    return !strcmp(k, "sta_pass") || !strcmp(k, "ap_pass") || !strcmp(k, "admin_pass");
}

static void handle_post_config(int s, char *body)
{
    a9_config_t next = g_config;
    char *save = NULL, *pair, err[96];

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

    if (!g_snap)
        g_snap = malloc(A9_FRAME_BUF);
    len = g_snap ? hal_camera_get_frame(g_snap, A9_FRAME_BUF, &seq, &ts, 2000) : -1;
    if (len <= 0) {
        respond_text(s, 503, "Service Unavailable", "no frame");
        return;
    }
    respond(s, 200, "OK", "image/jpeg", g_snap, (size_t)len, NULL);
}

static void handle_connection(int s)
{
    char *req = malloc(REQ_MAX + 1);
    size_t got = 0;
    char *hdr_end = NULL, method[8], path[128], cl[16];
    size_t body_len = 0;

    if (!req)
        return;
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
    hdr_end[4 + body_len] = 0;

    if (!authorized(req)) {
        respond(s, 401, "Unauthorized", "text/plain", "auth required", 13,
                "WWW-Authenticate: Basic realm=\"A9 camera\"\r\n");
        goto out;
    }

    if (!strcmp(method, "GET") && (!strcmp(path, "/") || !strcmp(path, "/index.html"))) {
        respond(s, 200, "OK", "text/html; charset=utf-8", web_ui_html, web_ui_html_len, NULL);
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
    } else if (!strcmp(method, "GET") && !strncmp(path, "/snapshot.jpg", 13)) {
        handle_snapshot(s);
    } else {
        respond_text(s, 404, "Not Found", "not found");
    }
out:
    free(req);
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
        handle_connection(s);
        closesocket(s);
    }
}

int http_server_start(uint16_t port)
{
    g_listen = net_listen_tcp(port, 4);
    if (g_listen < 0)
        return -1;
    return hal_task_start("http", http_task, NULL, 6144, 6);
}
