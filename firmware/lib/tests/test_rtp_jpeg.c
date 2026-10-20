/* Checks JPEG parsing and RFC 2435 packetization on a sample frame. */
#include "lc/rtp_jpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct {
    int      packets;
    size_t   next_offset;
    size_t   payload_total;
    int      saw_marker;
    uint16_t last_seq;
    const jpeg_info_t *ji;
} ctx_t;

static int on_packet(void *p, uint8_t *pkt, size_t len)
{
    ctx_t *c = p;
    const uint8_t *rtp = pkt + RTP_PREFIX, *j = rtp + 12;
    size_t off = ((size_t)j[1] << 16) | ((size_t)j[2] << 8) | j[3];
    size_t hdr = 12 + 8;
    uint16_t seq = (uint16_t)((rtp[2] << 8) | rtp[3]);

    CHECK(len <= RTP_PACKET_CAP - RTP_PREFIX);
    CHECK(rtp[0] == 0x80);
    CHECK((rtp[1] & 0x7F) == RTP_PT_JPEG);
    CHECK(!c->saw_marker);
    if (c->packets)
        CHECK(seq == (uint16_t)(c->last_seq + 1));
    c->last_seq = seq;
    CHECK(off == c->next_offset);
    CHECK(j[4] == c->ji->type && j[5] == 255);
    CHECK(j[6] * 8 == c->ji->width && j[7] * 8 == c->ji->height);
    if (off == 0) {
        size_t ql = ((size_t)j[10] << 8) | j[11];
        CHECK(ql == c->ji->qtables_len);
        CHECK(!memcmp(j + 12, c->ji->qtables, ql));
        hdr += 4 + ql;
    }
    CHECK(!memcmp(rtp + hdr, c->ji->scan + off, len - hdr));
    c->next_offset += len - hdr;
    c->payload_total += len - hdr;
    c->saw_marker = rtp[1] & 0x80;
    c->packets++;
    return 0;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "tests/data/frame.jpg", "rb");
    static uint8_t buf[1 << 20];
    size_t len;
    jpeg_info_t ji;
    rtp_state_t st = { 65530, 0x1234 };
    ctx_t c = { 0 };

    CHECK(f);
    len = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    CHECK(jpeg_parse(buf, len, &ji) == 0);
    CHECK(ji.width == 320 && ji.height == 240);
    CHECK(ji.type == 1);              /* 4:2:0 */
    CHECK(ji.qtables_len == 128);
    CHECK(ji.scan_len > 0 && ji.scan + ji.scan_len + 2 <= buf + len);

    c.ji = &ji;
    CHECK(rtp_jpeg_send_frame(&st, buf, len, 9000, on_packet, &c) == 0);
    CHECK(c.saw_marker);
    CHECK(c.payload_total == ji.scan_len);
    CHECK(c.packets > 1);             /* exercises fragmentation + seq wrap */

    buf[1] = 0;                       /* not a JPEG any more */
    CHECK(jpeg_parse(buf, len, &ji) != 0);

    printf("ok: %d packets, %zu scan bytes\n", c.packets, c.payload_total);
    return 0;
}
