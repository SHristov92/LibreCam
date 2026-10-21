#include "lc/mdns.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define T_A    1
#define T_PTR  12
#define T_TXT  16
#define T_SRV  33
#define T_ANY  255

#define CLASS_IN    1
#define CACHE_FLUSH 0x8000    /* unique records: replace what caches hold */

#define TTL_HOST    120
#define TTL_SERVICE 4500
#define TTL_LEGACY  10

/* Records we can send, as bits in a set. */
enum {
    R_A        = 1 << 0,
    R_HTTP_PTR = 1 << 1, R_HTTP_SRV = 1 << 2, R_HTTP_TXT = 1 << 3,
    R_RTSP_PTR = 1 << 4, R_RTSP_SRV = 1 << 5, R_RTSP_TXT = 1 << 6,
    R_ENUM_HTTP = 1 << 7, R_ENUM_RTSP = 1 << 8,
    R_ALL = (1 << 9) - 1,
};

static const char *const g_svc[2] = { "_http._tcp.local", "_rtsp._tcp.local" };
#define ENUM_NAME "_services._dns-sd._udp.local"

typedef struct {
    uint8_t *buf;
    size_t   len, cap;
    int      err;
} writer_t;

static void put(writer_t *w, const void *p, size_t n)
{
    if (w->err || w->len + n > w->cap) {
        w->err = 1;
        return;
    }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void put16(writer_t *w, unsigned v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    put(w, b, 2);
}

static void put32(writer_t *w, uint32_t v)
{
    put16(w, v >> 16);
    put16(w, v & 0xFFFF);
}

/* Writes a dotted name as DNS labels (no compression). */
static void put_name(writer_t *w, const char *name)
{
    while (*name) {
        size_t l = strcspn(name, ".");
        uint8_t lb = (uint8_t)(l > 63 ? 63 : l);

        put(w, &lb, 1);
        put(w, name, lb);
        name += l;
        if (*name == '.')
            name++;
    }
    put(w, "", 1);
}

static void put_rr_head(writer_t *w, const char *name, unsigned type, unsigned cls, uint32_t ttl)
{
    put_name(w, name);
    put16(w, type);
    put16(w, cls);
    put32(w, ttl);
}

/* Patches the 16-bit length at `at` to cover everything written since. */
static void end_rdata(writer_t *w, size_t at)
{
    size_t n = w->len - at - 2;

    if (!w->err) {
        w->buf[at] = (uint8_t)(n >> 8);
        w->buf[at + 1] = (uint8_t)n;
    }
}

static void names(const mdns_info_t *info, char *host, char inst[2][96])
{
    int i;

    snprintf(host, 80, "%s.local", info->host);
    for (i = 0; i < 2; i++)
        snprintf(inst[i], 96, "%s.%s", info->host, g_svc[i]);
}

/* Writes one record; ttl 0 picks the record's normal TTL. */
static void put_record(writer_t *w, int rec, const mdns_info_t *info, uint32_t ttl)
{
    char host[80], inst[2][96], txt[48];
    int svc = (rec & (R_RTSP_PTR | R_RTSP_SRV | R_RTSP_TXT | R_ENUM_RTSP)) ? 1 : 0;
    size_t at;
    uint8_t len;

    names(info, host, inst);
    switch (rec) {
    case R_A:
        put_rr_head(w, host, T_A, CLASS_IN | CACHE_FLUSH, ttl ? ttl : TTL_HOST);
        put16(w, 4);
        put(w, info->ip, 4);
        break;
    case R_HTTP_PTR:
    case R_RTSP_PTR:
        put_rr_head(w, g_svc[svc], T_PTR, CLASS_IN, ttl ? ttl : TTL_SERVICE);
        at = w->len;
        put16(w, 0);
        put_name(w, inst[svc]);
        end_rdata(w, at);
        break;
    case R_HTTP_SRV:
    case R_RTSP_SRV:
        put_rr_head(w, inst[svc], T_SRV, CLASS_IN | CACHE_FLUSH, ttl ? ttl : TTL_HOST);
        at = w->len;
        put16(w, 0);
        put16(w, 0);                    /* priority */
        put16(w, 0);                    /* weight */
        put16(w, svc ? info->rtsp_port : info->http_port);
        put_name(w, host);
        end_rdata(w, at);
        break;
    case R_HTTP_TXT:
    case R_RTSP_TXT:
        put_rr_head(w, inst[svc], T_TXT, CLASS_IN | CACHE_FLUSH, ttl ? ttl : TTL_SERVICE);
        snprintf(txt, sizeof(txt), "path=%s", svc ? info->rtsp_path : "/");
        len = (uint8_t)strlen(txt);
        put16(w, len + 1u);
        put(w, &len, 1);
        put(w, txt, len);
        break;
    case R_ENUM_HTTP:
    case R_ENUM_RTSP:
        put_rr_head(w, ENUM_NAME, T_PTR, CLASS_IN, ttl ? ttl : TTL_SERVICE);
        at = w->len;
        put16(w, 0);
        put_name(w, g_svc[svc]);
        end_rdata(w, at);
        break;
    }
}

/* Writes the records in `set`; returns how many. */
static int put_set(writer_t *w, int set, const mdns_info_t *info, uint32_t ttl)
{
    int bit, n = 0;

    for (bit = 1; bit <= R_ALL; bit <<= 1)
        if (set & bit) {
            put_record(w, bit, info, ttl);
            n++;
        }
    return n;
}

/* Reads a possibly compressed name at *off into a dotted string. */
static int read_name(const uint8_t *q, size_t qlen, size_t *off, char *out, size_t cap)
{
    size_t p = *off, o = 0;
    int jumped = 0, hops = 0;

    for (;;) {
        uint8_t l;

        if (p >= qlen)
            return -1;
        l = q[p];
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= qlen || ++hops > 8)
                return -1;
            if (!jumped)
                *off = p + 2;
            jumped = 1;
            p = (size_t)((l & 0x3F) << 8 | q[p + 1]);
            continue;
        }
        if (l & 0xC0)
            return -1;
        p++;
        if (!l)
            break;
        if (p + l > qlen || o + l + 2 > cap)
            return -1;
        if (o)
            out[o++] = '.';
        memcpy(out + o, q + p, l);
        o += l;
        p += l;
    }
    out[o] = 0;
    if (!jumped)
        *off = p;
    return 0;
}

static size_t finish(writer_t *w, uint16_t id, unsigned qd, unsigned an, unsigned ar)
{
    if (w->err || w->len < 12)
        return 0;
    w->buf[0] = (uint8_t)(id >> 8);
    w->buf[1] = (uint8_t)id;
    w->buf[2] = 0x84;               /* response, authoritative */
    w->buf[3] = 0;
    w->buf[4] = (uint8_t)(qd >> 8);
    w->buf[5] = (uint8_t)qd;
    w->buf[6] = (uint8_t)(an >> 8);
    w->buf[7] = (uint8_t)an;
    w->buf[8] = w->buf[9] = 0;
    w->buf[10] = (uint8_t)(ar >> 8);
    w->buf[11] = (uint8_t)ar;
    return w->len;
}

size_t mdns_reply(const uint8_t *q, size_t qlen, const mdns_info_t *info, int legacy,
                  uint8_t *out, size_t cap)
{
    writer_t w = { out, 12, cap, cap < 12 };
    char name[256], host[80], inst[2][96];
    size_t off = 12, qend;
    unsigned qd, i, an, ar;
    int answers = 0, extra = 0, s;

    if (qlen < 12 || (q[2] & 0x80) || (q[2] & 0x78))   /* responses, non-queries */
        return 0;
    qd = (unsigned)(q[4] << 8 | q[5]);
    names(info, host, inst);
    for (i = 0; i < qd; i++) {
        unsigned type;

        if (read_name(q, qlen, &off, name, sizeof(name)) != 0 || off + 4 > qlen)
            return 0;
        type = (unsigned)(q[off] << 8 | q[off + 1]);
        off += 4;
#define WANT(t) (type == (t) || type == T_ANY)
        if (!strcasecmp(name, host) && WANT(T_A))
            answers |= R_A;
        for (s = 0; s < 2; s++) {
            int ptr = s ? R_RTSP_PTR : R_HTTP_PTR, srv = s ? R_RTSP_SRV : R_HTTP_SRV,
                txt = s ? R_RTSP_TXT : R_HTTP_TXT;

            if (!strcasecmp(name, g_svc[s]) && WANT(T_PTR)) {
                answers |= ptr;
                extra |= srv | txt | R_A;
            }
            if (!strcasecmp(name, inst[s])) {
                if (WANT(T_SRV)) {
                    answers |= srv;
                    extra |= R_A;
                }
                if (WANT(T_TXT))
                    answers |= txt;
            }
        }
        if (!strcasecmp(name, ENUM_NAME) && WANT(T_PTR))
            answers |= R_ENUM_HTTP | R_ENUM_RTSP;
#undef WANT
    }
    if (!answers)
        return 0;
    qend = off;
    if (legacy)
        put(&w, q + 12, qend - 12);
    an = (unsigned)put_set(&w, answers, info, legacy ? TTL_LEGACY : 0);
    ar = (unsigned)put_set(&w, extra & ~answers, info, legacy ? TTL_LEGACY : 0);
    return finish(&w, legacy ? (uint16_t)(q[0] << 8 | q[1]) : 0, legacy ? qd : 0, an, ar);
}

size_t mdns_announce(const mdns_info_t *info, uint8_t *out, size_t cap)
{
    writer_t w = { out, 12, cap, cap < 12 };
    unsigned an = (unsigned)put_set(&w, R_ALL, info, 0);

    return finish(&w, 0, 0, an, 0);
}

void mdns_hostname(const char *device_name, char *out, size_t cap)
{
    size_t o = 0;
    int dash = 0;

    for (; *device_name && o + 1 < cap && o < 63; device_name++) {
        char c = *device_name;

        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[o++] = c;
            dash = 0;
        } else if (o && !dash) {
            out[o++] = '-';      /* spaces, '_', '.' and the rest become one '-' */
            dash = 1;
        }
    }
    while (o && out[o - 1] == '-')
        o--;
    out[o] = 0;
    if (!o)
        snprintf(out, cap, "librecam");
}
