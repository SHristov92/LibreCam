#include "a9/rtp_jpeg.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

int jpeg_parse(const uint8_t *d, size_t len, jpeg_info_t *out)
{
    size_t i = 2;
    int have_sof = 0;
    uint8_t qt_present = 0, tq_luma = 0, tq_chroma = 0;
    uint8_t tables[4][64];

    memset(out, 0, sizeof(*out));
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return -1;

    while (i + 4 <= len) {
        uint8_t marker;
        uint16_t seg;

        if (d[i] != 0xFF)
            return -1;
        marker = d[i + 1];
        if (marker == 0xFF) {          /* fill byte */
            i++;
            continue;
        }
        seg = rd16(d + i + 2);
        if (seg < 2 || i + 2 + seg > len)
            return -1;

        switch (marker) {
        case 0xDB: { /* DQT: may hold several tables */
            size_t p = i + 4, end = i + 2 + seg;
            while (p < end) {
                uint8_t pq = d[p] >> 4, tq = d[p] & 0x0F;
                if (pq != 0 || tq > 3 || p + 65 > end)
                    return -1;
                memcpy(tables[tq], d + p + 1, 64);
                qt_present |= (uint8_t)(1u << tq);
                p += 65;
            }
            break;
        }
        case 0xC0: { /* SOF0, baseline */
            const uint8_t *s = d + i + 4;
            if (seg < 17 || s[0] != 8 || s[5] != 3)
                return -1;
            out->height = rd16(s + 1);
            out->width = rd16(s + 3);
            /* Luma sampling decides the type; chroma must be 1x1. */
            if (s[7] == 0x21)
                out->type = 0;
            else if (s[7] == 0x22)
                out->type = 1;
            else
                return -1;
            if (s[10] != 0x11 || s[13] != 0x11 || s[11] != s[14] ||
                s[8] > 3 || s[11] > 3)
                return -1;
            tq_luma = s[8];
            tq_chroma = s[11];
            have_sof = 1;
            break;
        }
        case 0xC1: case 0xC2: case 0xC3: /* extended / progressive / lossless */
            return -1;
        case 0xDD: /* DRI */
            out->restart_interval = rd16(d + i + 4);
            break;
        case 0xDA: { /* SOS: entropy-coded data follows the header */
            size_t start = i + 2 + seg, end = len;
            if (!have_sof || !(qt_present & (1u << tq_luma)) ||
                !(qt_present & (1u << tq_chroma)))
                return -1;
            /* RFC 2435 always carries luma then chroma, even when the
             * encoder shares one table between them. */
            memcpy(out->qtables, tables[tq_luma], 64);
            memcpy(out->qtables + 64, tables[tq_chroma], 64);
            out->qtables_len = 128;
            while (end >= start + 2 && !(d[end - 2] == 0xFF && d[end - 1] == 0xD9))
                end--;
            if (end < start + 2)
                return -1;
            out->scan = d + start;
            out->scan_len = end - 2 - start;
            if (out->restart_interval)
                out->type |= 64;
            if (out->width == 0 || out->height == 0 ||
                out->width > 2040 || out->height > 2040)
                return -1;
            return 0;
        }
        default:
            break;
        }
        i += 2 + seg;
    }
    return -1;
}

int rtp_jpeg_send_frame(rtp_state_t *st, const uint8_t *jpeg, size_t len,
                        uint32_t rtp_ts, rtp_send_fn send, void *ctx)
{
    jpeg_info_t ji;
    uint8_t pkt[RTP_PACKET_CAP];
    size_t offset = 0;

    if (jpeg_parse(jpeg, len, &ji) != 0)
        return -1;

    while (offset < ji.scan_len) {
        uint8_t *rtp = pkt + RTP_PREFIX;
        uint8_t *p = rtp + 12;
        size_t chunk, room;
        int last;

        /* JPEG main header */
        *p++ = 0;                               /* type-specific */
        *p++ = (uint8_t)(offset >> 16);
        *p++ = (uint8_t)(offset >> 8);
        *p++ = (uint8_t)offset;
        *p++ = ji.type;
        *p++ = 255;                             /* Q: tables in-band */
        *p++ = (uint8_t)(ji.width / 8);
        *p++ = (uint8_t)(ji.height / 8);

        if (ji.type & 64) {                     /* restart marker header */
            *p++ = (uint8_t)(ji.restart_interval >> 8);
            *p++ = (uint8_t)ji.restart_interval;
            *p++ = 0xFF;                        /* F=1 L=1, count 0x3FFF */
            *p++ = 0xFF;
        }
        if (offset == 0) {                      /* quantization table header */
            *p++ = 0;
            *p++ = 0;                           /* 8-bit precision */
            *p++ = (uint8_t)(ji.qtables_len >> 8);
            *p++ = (uint8_t)ji.qtables_len;
            memcpy(p, ji.qtables, ji.qtables_len);
            p += ji.qtables_len;
        }

        room = RTP_MAX_PAYLOAD - (size_t)(p - (rtp + 12));
        chunk = ji.scan_len - offset;
        if (chunk > room)
            chunk = room;
        last = (offset + chunk == ji.scan_len);
        memcpy(p, ji.scan + offset, chunk);
        p += chunk;

        rtp[0] = 0x80;
        rtp[1] = (uint8_t)(RTP_PT_JPEG | (last ? 0x80 : 0));
        rtp[2] = (uint8_t)(st->seq >> 8);
        rtp[3] = (uint8_t)st->seq;
        rtp[4] = (uint8_t)(rtp_ts >> 24);
        rtp[5] = (uint8_t)(rtp_ts >> 16);
        rtp[6] = (uint8_t)(rtp_ts >> 8);
        rtp[7] = (uint8_t)rtp_ts;
        rtp[8] = (uint8_t)(st->ssrc >> 24);
        rtp[9] = (uint8_t)(st->ssrc >> 16);
        rtp[10] = (uint8_t)(st->ssrc >> 8);
        rtp[11] = (uint8_t)st->ssrc;
        st->seq++;

        if (send(ctx, pkt, (size_t)(p - rtp)) != 0)
            return -1;
        offset += chunk;
    }
    return 0;
}
