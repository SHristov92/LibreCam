/*
 * Minimal RTSP server (RFC 2326) streaming MJPEG over RTP (RFC 2435) as
 * track0 and, when a microphone is available, G.711 A-law audio as track1.
 *
 * Supports OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN and GET_PARAMETER, with
 * RTP over UDP (unicast) or interleaved in the RTSP TCP connection. One task
 * per client; A9_RTSP_MAX_CLIENTS bounds RAM use since each client owns a frame buffer.
 */
#include "a9/audio.h"
#include "a9/board.h"
#include "a9/config.h"
#include "a9/hal.h"
#include "a9/rtp_jpeg.h"
#include "a9/servers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "a9/net.h"

#define REQ_BUF          2048
#define SESSION_TIMEOUT  60000

#define TRACK_VIDEO 0
#define TRACK_AUDIO 1
#define TRACKS      2

typedef struct {
    int       setup;
    int       tcp;              /* interleaved */
    uint8_t   channel;
    int       udp_sock;
    struct sockaddr_in udp_dst;
    uint16_t  server_port;
    rtp_state_t rtp;
} rtsp_track_t;

typedef struct rtsp_client {
    int       sock;
    struct sockaddr_in peer;
    int       playing;
    uint32_t  session;
    uint32_t  last_activity;
    rtsp_track_t track[TRACKS];
    char      req[REQ_BUF + 1];
    size_t    req_len;
} rtsp_client_t;

typedef struct {
    rtsp_client_t *c;
    rtsp_track_t  *t;
} rtp_dest_t;

static int g_listen = -1;
static volatile int g_clients;
static hal_mutex_t *g_lock;

int rtsp_active_clients(void)
{
    return g_clients;
}

static int send_all(int s, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        int n = send(s, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int rtp_send(void *ctx, uint8_t *pkt, size_t len)
{
    rtp_dest_t *d = ctx;

    if (d->t->tcp) {
        pkt[0] = '$';
        pkt[1] = d->t->channel;
        pkt[2] = (uint8_t)(len >> 8);
        pkt[3] = (uint8_t)len;
        return send_all(d->c->sock, pkt, len + RTP_PREFIX);
    }
    return sendto(d->t->udp_sock, pkt + RTP_PREFIX, len, 0,
                  (struct sockaddr *)&d->t->udp_dst, sizeof(d->t->udp_dst)) < 0 ? -1 : 0;
}

/* Sends every audio packet captured since the last call. */
static int send_audio(rtsp_client_t *c, uint32_t *seq)
{
    uint8_t pkt[RTP_PREFIX + 12 + AUDIO_PKT_SAMPLES];
    rtsp_track_t *t = &c->track[TRACK_AUDIO];
    rtp_dest_t d = { c, t };
    uint32_t ts;
    int n;

    while ((n = audio_get_packet(pkt + RTP_PREFIX + 12, seq, &ts)) > 0) {
        uint8_t *rtp = pkt + RTP_PREFIX;

        rtp[0] = 0x80;
        rtp[1] = AUDIO_RTP_PT;
        rtp[2] = (uint8_t)(t->rtp.seq >> 8);
        rtp[3] = (uint8_t)t->rtp.seq;
        rtp[4] = (uint8_t)(ts >> 24);
        rtp[5] = (uint8_t)(ts >> 16);
        rtp[6] = (uint8_t)(ts >> 8);
        rtp[7] = (uint8_t)ts;
        rtp[8] = (uint8_t)(t->rtp.ssrc >> 24);
        rtp[9] = (uint8_t)(t->rtp.ssrc >> 16);
        rtp[10] = (uint8_t)(t->rtp.ssrc >> 8);
        rtp[11] = (uint8_t)t->rtp.ssrc;
        t->rtp.seq++;
        if (rtp_send(&d, pkt, 12 + (size_t)n) != 0)
            return -1;
    }
    return 0;
}

/* Returns a pointer to the value of header `name`, terminated at CR. */
static const char *header(const char *req, const char *name, char *out, size_t cap)
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

static void reply(rtsp_client_t *c, int code, const char *reason,
                  const char *cseq, const char *extra, const char *body)
{
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
                     "RTSP/1.0 %d %s\r\nCSeq: %s\r\nServer: a9-firmware\r\n%s",
                     code, reason, cseq ? cseq : "0", extra ? extra : "");
    if (c->session && code == 200)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "Session: %08X;timeout=%d\r\n",
                      (unsigned)c->session, SESSION_TIMEOUT / 1000);
    if (body)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "Content-Type: application/sdp\r\nContent-Length: %u\r\n",
                      (unsigned)strlen(body));
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\r\n%s", body ? body : "");
    send_all(c->sock, buf, (size_t)n);
}

/* Checks that the request URL addresses our stream. Returns the track it
 * names (TRACK_VIDEO for the bare stream URL or /track0, TRACK_AUDIO for
 * /track1), or -1. */
static int url_track(const char *url)
{
    const char *path = strstr(url, "://");
    size_t pl = strlen(g_config.rtsp_path);

    path = path ? strchr(path + 3, '/') : url;
    if (!path || strncmp(path, g_config.rtsp_path, pl))
        return -1;
    path += pl;
    if (!*path || !strcmp(path, "/") || !strcmp(path, "/track0"))
        return TRACK_VIDEO;
    if (!strcmp(path, "/track1") && audio_available())
        return TRACK_AUDIO;
    return -1;
}

static int setup_udp(rtsp_client_t *c, rtsp_track_t *t, uint16_t client_port)
{
    struct sockaddr_in a;
    socklen_t al = sizeof(a);

    if (t->udp_sock < 0)
        t->udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (t->udp_sock < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = 0;
    if (bind(t->udp_sock, (struct sockaddr *)&a, sizeof(a)) < 0 ||
        getsockname(t->udp_sock, (struct sockaddr *)&a, &al) < 0) {
        closesocket(t->udp_sock);
        t->udp_sock = -1;
        return -1;
    }
    t->server_port = ntohs(a.sin_port);
    t->udp_dst = c->peer;
    t->udp_dst.sin_port = htons(client_port);
    return 0;
}

/* Handles one complete request. Returns -1 to close the connection. */
static int handle_request(rtsp_client_t *c)
{
    char method[16], url[256], cseq[16], transport[256], extra[384];
    const char *cs;

    if (sscanf(c->req, "%15s %255s", method, url) != 2)
        return -1;
    cs = header(c->req, "CSeq", cseq, sizeof(cseq));

    if (!strcmp(method, "OPTIONS")) {
        reply(c, 200, "OK", cs,
              "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n", NULL);
    } else if (!strcmp(method, "DESCRIBE")) {
        char sdp[640], ip[16];
        int n;
        if (url_track(url) != TRACK_VIDEO) {
            reply(c, 404, "Not Found", cs, NULL, NULL);
            return 0;
        }
        hal_wifi_ip(ip, sizeof(ip));
        n = snprintf(sdp, sizeof(sdp),
                     "v=0\r\no=- %u 1 IN IP4 %s\r\ns=%s\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
                     "a=control:*\r\nm=video 0 RTP/AVP 26\r\na=rtpmap:26 JPEG/90000\r\n"
                     "a=framerate:%u\r\na=control:track0\r\n",
                     (unsigned)hal_millis(), ip, g_config.device_name,
                     (unsigned)g_config.camera.fps);
        if (audio_available())
            snprintf(sdp + n, sizeof(sdp) - (size_t)n,
                     "m=audio 0 RTP/AVP %d\r\na=rtpmap:%d PCMA/%d\r\na=control:track1\r\n",
                     AUDIO_RTP_PT, AUDIO_RTP_PT, AUDIO_RATE);
        snprintf(extra, sizeof(extra), "Content-Base: %s/\r\n", url);
        reply(c, 200, "OK", cs, extra, sdp);
    } else if (!strcmp(method, "SETUP")) {
        const char *il, *cp;
        int tr = url_track(url);
        rtsp_track_t *t;
        if (tr < 0) {
            reply(c, 404, "Not Found", cs, NULL, NULL);
            return 0;
        }
        t = &c->track[tr];
        if (!header(c->req, "Transport", transport, sizeof(transport))) {
            reply(c, 461, "Unsupported Transport", cs, NULL, NULL);
            return 0;
        }
        if (!c->session)
            c->session = ((uint32_t)rand() << 16) ^ (uint32_t)rand() ^ hal_millis();
        if (strstr(transport, "RTP/AVP/TCP")) {
            il = strstr(transport, "interleaved=");
            t->tcp = 1;
            t->channel = il ? (uint8_t)atoi(il + 12) : (uint8_t)(2 * tr);
            snprintf(extra, sizeof(extra),
                     "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n",
                     t->channel, t->channel + 1);
        } else if ((cp = strstr(transport, "client_port=")) != NULL) {
            int port = atoi(cp + 12);
            if (port <= 0 || port > 65535 || setup_udp(c, t, (uint16_t)port) != 0) {
                reply(c, 461, "Unsupported Transport", cs, NULL, NULL);
                return 0;
            }
            t->tcp = 0;
            snprintf(extra, sizeof(extra),
                     "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%u-%u\r\n",
                     port, port + 1, t->server_port, t->server_port + 1);
        } else {
            reply(c, 461, "Unsupported Transport", cs, NULL, NULL);
            return 0;
        }
        t->setup = 1;
        reply(c, 200, "OK", cs, extra, NULL);
    } else if (!strcmp(method, "PLAY")) {
        if (!c->session) {
            reply(c, 455, "Method Not Valid in This State", cs, NULL, NULL);
            return 0;
        }
        c->playing = 1;
        reply(c, 200, "OK", cs, "Range: npt=0.000-\r\n", NULL);
    } else if (!strcmp(method, "TEARDOWN")) {
        reply(c, 200, "OK", cs, NULL, NULL);
        return -1;
    } else if (!strcmp(method, "GET_PARAMETER") || !strcmp(method, "SET_PARAMETER")) {
        reply(c, 200, "OK", cs, NULL, NULL);
    } else {
        reply(c, 501, "Not Implemented", cs, NULL, NULL);
    }
    return 0;
}

/* Consumes buffered input: interleaved RTCP from the client and requests. */
static int process_input(rtsp_client_t *c)
{
    for (;;) {
        char *end;
        size_t used;

        if (c->req_len >= 4 && c->req[0] == '$') {
            size_t pl = 4 + (((uint8_t)c->req[2] << 8) | (uint8_t)c->req[3]);
            if (c->req_len < pl)
                return 0;
            memmove(c->req, c->req + pl, c->req_len - pl);
            c->req_len -= pl;
            continue;
        }
        c->req[c->req_len] = 0;
        end = strstr(c->req, "\r\n\r\n");
        if (!end)
            return c->req_len >= REQ_BUF ? -1 : 0;
        /* Requests we handle carry no body; skip any declared one. */
        used = (size_t)(end - c->req) + 4;
        {
            char cl[16];
            *end = 0;
            if (header(c->req, "Content-Length", cl, sizeof(cl)))
                used += (size_t)atoi(cl);
            *end = '\r';
        }
        if (used > c->req_len)
            return 0;
        if (handle_request(c) != 0)
            return -1;
        memmove(c->req, c->req + used, c->req_len - used);
        c->req_len -= used;
    }
}

static int uses_tcp(const rtsp_client_t *c)
{
    return (c->track[TRACK_VIDEO].setup && c->track[TRACK_VIDEO].tcp) ||
           (c->track[TRACK_AUDIO].setup && c->track[TRACK_AUDIO].tcp);
}

static void client_task(void *arg)
{
    rtsp_client_t *c = arg;
    uint8_t *frame = hal_malloc_large(A9_FRAME_BUF);
    uint32_t last_seq = 0, audio_seq = 0;
    int i;

    for (i = 0; i < TRACKS; i++) {
        c->track[i].rtp.ssrc = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
        c->track[i].rtp.seq = (uint16_t)rand();
    }
    c->last_activity = hal_millis();

    while (frame) {
        fd_set rf;
        struct timeval tv = { 0, c->playing ? 0 : 200000 };

        FD_ZERO(&rf);
        FD_SET(c->sock, &rf);
        if (select(c->sock + 1, &rf, NULL, NULL, &tv) > 0) {
            int n = recv(c->sock, c->req + c->req_len, REQ_BUF - c->req_len, 0);
            if (n <= 0)
                break;
            c->req_len += (size_t)n;
            c->last_activity = hal_millis();
            if (process_input(c) != 0)
                break;
        }

        if (c->playing) {
            rtsp_track_t *v = &c->track[TRACK_VIDEO];
            rtp_dest_t vd = { c, v };
            uint32_t ts_ms;
            int len;

            if (c->track[TRACK_AUDIO].setup && send_audio(c, &audio_seq) != 0 &&
                c->track[TRACK_AUDIO].tcp)
                break;
            /* Short waits keep audio packets flowing between frames. */
            len = v->setup ? hal_camera_get_frame(frame, A9_FRAME_BUF, &last_seq, &ts_ms, 20) : 0;
            if (len > 0 &&
                rtp_jpeg_send_frame(&v->rtp, frame, (size_t)len, ts_ms * 90u,
                                    rtp_send, &vd) != 0 && v->tcp)
                break;
            if (!v->setup)
                hal_sleep_ms(20);
            /* UDP clients keep the session alive with RTSP keepalives
             * (GET_PARAMETER/OPTIONS) or RTCP on the TCP connection. */
        }
        if (hal_millis() - c->last_activity > SESSION_TIMEOUT && !uses_tcp(c))
            break;
    }

    free(frame);
    for (i = 0; i < TRACKS; i++)
        if (c->track[i].udp_sock >= 0)
            closesocket(c->track[i].udp_sock);
    closesocket(c->sock);
    free(c);
    hal_mutex_lock(g_lock);
    g_clients--;
    hal_mutex_unlock(g_lock);
}

static void accept_task(void *arg)
{
    (void)arg;
    for (;;) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        int s = accept(g_listen, (struct sockaddr *)&peer, &pl);
        rtsp_client_t *c;
        struct timeval snd_to = { 5, 0 };
        int one = 1;

        if (s < 0) {
            hal_sleep_ms(100);
            continue;
        }
        hal_mutex_lock(g_lock);
        if (g_clients >= A9_RTSP_MAX_CLIENTS) {
            hal_mutex_unlock(g_lock);
            send(s, "RTSP/1.0 453 Not Enough Bandwidth\r\n\r\n", 37, 0);
            closesocket(s);
            continue;
        }
        g_clients++;
        hal_mutex_unlock(g_lock);

        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        c = calloc(1, sizeof(*c));
        if (c) {
            c->sock = s;
            c->peer = peer;
            c->track[TRACK_VIDEO].udp_sock = -1;
            c->track[TRACK_AUDIO].udp_sock = -1;
        }
        if (!c || hal_task_start("rtsp_cli", client_task, c, 8192, 5) != 0) {
            free(c);
            closesocket(s);
            hal_mutex_lock(g_lock);
            g_clients--;
            hal_mutex_unlock(g_lock);
        }
    }
}

int rtsp_server_start(uint16_t port)
{
    g_lock = hal_mutex_create();
    g_listen = net_listen_tcp(port, A9_RTSP_MAX_CLIENTS);
    if (g_listen < 0)
        return -1;
    return hal_task_start("rtsp", accept_task, NULL, 4096, 5);
}
