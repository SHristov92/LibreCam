/* mDNS messages: answers to the queries browsers, avahi and DNS-SD send. */
#include <stdio.h>
#include <string.h>

#include "lc/mdns.h"

static int fails;

static void expect(const char *what, long got, long want)
{
    if (got != want) {
        printf("%s: got %ld, want %ld\n", what, got, want);
        fails++;
    }
}

static const mdns_info_t g_info = { "porch-cam", { 192, 168, 1, 143 }, 80, 554, "/live" };

/* Builds a query for one name and type; id and question as given. */
static size_t query(uint8_t *q, uint16_t id, const char *name, unsigned type)
{
    size_t n = 12;

    memset(q, 0, 12);
    q[0] = (uint8_t)(id >> 8);
    q[1] = (uint8_t)id;
    q[5] = 1;
    while (*name) {
        size_t l = strcspn(name, ".");
        q[n++] = (uint8_t)l;
        memcpy(q + n, name, l);
        n += l;
        name += l + (name[l] == '.');
    }
    q[n++] = 0;
    q[n++] = (uint8_t)(type >> 8);
    q[n++] = (uint8_t)type;
    q[n++] = 0;
    q[n++] = 1;
    return n;
}

static unsigned u16(const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); }

/* Finds the A record's address: the 4 bytes after type 1, class, TTL, len 4. */
static const uint8_t *find_a(const uint8_t *m, size_t n)
{
    size_t i;

    for (i = 0; i + 14 <= n; i++)
        if (u16(m + i) == 1 && (u16(m + i + 2) & 0x7FFF) == 1 && u16(m + i + 8) == 4)
            return m + i + 10;
    return NULL;
}

static int contains(const uint8_t *m, size_t n, const char *s)
{
    size_t l = strlen(s), i;

    for (i = 0; i + l <= n; i++)
        if (!memcmp(m + i, s, l))
            return 1;
    return 0;
}

int main(void)
{
    uint8_t q[256], r[MDNS_MSG_MAX];
    const uint8_t *a;
    char host[64];
    size_t n, ql;

    /* Host lookup, as from another responder (port 5353). */
    ql = query(q, 0, "porch-cam.local", 1);
    n = mdns_reply(q, ql, &g_info, 0, r, sizeof(r));
    expect("A: answered", n > 0, 1);
    expect("A: flags", r[2], 0x84);
    expect("A: no question echoed", u16(r + 4), 0);
    expect("A: one answer", u16(r + 6), 1);
    a = find_a(r, n);
    expect("A: address", a && !memcmp(a, "\xc0\xa8\x01\x8f", 4), 1);

    /* Case doesn't matter. */
    ql = query(q, 0, "Porch-Cam.LOCAL", 1);
    expect("A: case", mdns_reply(q, ql, &g_info, 0, r, sizeof(r)) > 0, 1);

    /* Other names and types get nothing. */
    ql = query(q, 0, "other.local", 1);
    expect("other host", (long)mdns_reply(q, ql, &g_info, 0, r, sizeof(r)), 0);
    ql = query(q, 0, "porch-cam.local", 28);      /* AAAA */
    expect("AAAA", (long)mdns_reply(q, ql, &g_info, 0, r, sizeof(r)), 0);

    /* Legacy unicast (getaddrinfo, dig -p 5353): id and question echoed. */
    ql = query(q, 0x1234, "porch-cam.local", 1);
    n = mdns_reply(q, ql, &g_info, 1, r, sizeof(r));
    expect("legacy: id", u16(r), 0x1234);
    expect("legacy: question", u16(r + 4), 1);
    expect("legacy: question bytes", !memcmp(r + 12, q + 12, ql - 12), 1);
    expect("legacy: answer", u16(r + 6), 1);

    /* Service browsing: PTR answer, SRV/TXT/A as additional records. */
    ql = query(q, 0, "_http._tcp.local", 12);
    n = mdns_reply(q, ql, &g_info, 0, r, sizeof(r));
    expect("http PTR: answers", u16(r + 6), 1);
    expect("http PTR: additional", u16(r + 10), 3);
    expect("http PTR: instance", contains(r, n, "\x09porch-cam\x05_http"), 1);
    ql = query(q, 0, "_rtsp._tcp.local", 12);
    n = mdns_reply(q, ql, &g_info, 0, r, sizeof(r));
    expect("rtsp TXT path", contains(r, n, "\x0apath=/live"), 1);
    expect("rtsp SRV port", contains(r, n, "\x00\x00\x00\x00\x02\x2a"), 1);
    ql = query(q, 0, "_services._dns-sd._udp.local", 12);
    expect("service enumeration", (mdns_reply(q, ql, &g_info, 0, r, sizeof(r)), u16(r + 6)), 2);

    /* A compressed name (pointer back into the question). */
    ql = query(q, 0, "porch-cam.local", 1);
    q[5] = 2;
    q[ql++] = 0xC0; q[ql++] = 12; q[ql++] = 0; q[ql++] = 1; q[ql++] = 0; q[ql++] = 1;
    expect("compressed: answered once", (mdns_reply(q, ql, &g_info, 0, r, sizeof(r)), u16(r + 6)), 1);
    /* A pointer loop is rejected, not followed forever. */
    q[ql - 6] = 0xC0; q[ql - 5] = (uint8_t)(ql - 6);
    expect("pointer loop", (long)mdns_reply(q, ql, &g_info, 0, r, sizeof(r)), 1 > 2);

    /* Responses are ignored. */
    ql = query(q, 0, "porch-cam.local", 1);
    q[2] = 0x84;
    expect("response ignored", (long)mdns_reply(q, ql, &g_info, 0, r, sizeof(r)), 0);

    /* Announcement: all nine records. */
    n = mdns_announce(&g_info, r, sizeof(r));
    expect("announce: records", u16(r + 6), 9);
    expect("announce: fits", n > 0 && n <= MDNS_MSG_MAX, 1);
    printf("announcement: %u bytes\n", (unsigned)n);

    mdns_hostname("Porch Cam #2", host, sizeof(host));
    expect("hostname", strcmp(host, "porch-cam-2"), 0);
    mdns_hostname("librecam-a91234", host, sizeof(host));
    expect("hostname default", strcmp(host, "librecam-a91234"), 0);
    mdns_hostname("!!!", host, sizeof(host));
    expect("hostname empty", strcmp(host, "librecam"), 0);

    if (fails)
        printf("%d failures\n", fails);
    return fails != 0;
}
