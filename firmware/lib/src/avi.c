#include "a9/avi.h"
#include "a9/audio.h"

#include <stdio.h>
#include <string.h>

/* hdrl with both streams: RIFF + LIST hdrl + avih + video strl + audio strl + LIST movi. */
#define HDR_SIZE (12 + 12 + 64 + (12 + 64 + 48) + (12 + 64 + 26) + 12)

static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void cc(uint8_t *p, const char *s) { memcpy(p, s, 4); }

/* Builds the header. Rates are placeholders until avi_close() knows them. */
static size_t build_header(const avi_t *a, uint8_t *h, uint32_t riff_size, uint32_t movi_size,
                           uint32_t usec_per_frame, uint32_t rate_num, uint32_t rate_den)
{
    uint8_t *p = h;
    uint32_t hdrl_size = 4 + 64 + (12 + 64 + 48) + (a->audio ? 12 + 64 + 26 : 0);

    memset(h, 0, HDR_SIZE);
    cc(p, "RIFF"); le32(p + 4, riff_size); cc(p + 8, "AVI "); p += 12;
    cc(p, "LIST"); le32(p + 4, hdrl_size); cc(p + 8, "hdrl"); p += 12;
    /* avih */
    cc(p, "avih"); le32(p + 4, 56); p += 8;
    le32(p + 0, usec_per_frame);
    le32(p + 4, 0);                 /* max bytes/s, informative */
    le32(p + 12, 0x10);             /* AVIF_HASINDEX */
    le32(p + 16, a->frames);
    le32(p + 24, a->audio ? 2 : 1);
    le32(p + 28, a->max_frame);
    le32(p + 32, a->width);
    le32(p + 36, a->height);
    p += 56;
    /* video stream */
    cc(p, "LIST"); le32(p + 4, 4 + 64 + 48); cc(p + 8, "strl"); p += 12;
    cc(p, "strh"); le32(p + 4, 56); p += 8;
    cc(p, "vids"); cc(p + 4, "MJPG");
    le32(p + 20, rate_den);         /* dwScale */
    le32(p + 24, rate_num);         /* dwRate: fps = rate / scale */
    le32(p + 32, a->frames);        /* dwLength */
    le32(p + 36, a->max_frame);
    le32(p + 40, 0xFFFFFFFF);       /* quality: default */
    le16(p + 52, a->width);         /* rcFrame right/bottom */
    le16(p + 54, a->height);
    p += 56;
    cc(p, "strf"); le32(p + 4, 40); p += 8;
    le32(p + 0, 40);                /* BITMAPINFOHEADER */
    le32(p + 4, a->width);
    le32(p + 8, a->height);
    le16(p + 12, 1);
    le16(p + 14, 24);
    cc(p + 16, "MJPG");
    le32(p + 20, (uint32_t)a->width * a->height * 3);
    p += 40;
    if (a->audio) {
        cc(p, "LIST"); le32(p + 4, 4 + 64 + 26); cc(p + 8, "strl"); p += 12;
        cc(p, "strh"); le32(p + 4, 56); p += 8;
        cc(p, "auds");
        le32(p + 20, 1);            /* dwScale: one 8-bit sample */
        le32(p + 24, AUDIO_RATE);   /* dwRate: samples/s */
        le32(p + 32, a->audio_bytes);
        le32(p + 36, 800);          /* suggested buffer */
        le32(p + 40, 0xFFFFFFFF);
        le32(p + 44, 1);            /* dwSampleSize */
        p += 56;
        cc(p, "strf"); le32(p + 4, 18); p += 8;
        le16(p + 0, 6);             /* WAVE_FORMAT_ALAW */
        le16(p + 2, 1);             /* mono */
        le32(p + 4, AUDIO_RATE);
        le32(p + 8, AUDIO_RATE);    /* bytes/s */
        le16(p + 12, 1);            /* block align */
        le16(p + 14, 8);            /* bits/sample */
        le16(p + 16, 0);            /* cbSize */
        p += 18;
    }
    cc(p, "LIST"); le32(p + 4, movi_size); cc(p + 8, "movi"); p += 12;
    return (size_t)(p - h);
}

static int put(avi_t *a, const void *buf, size_t len)
{
    if (a->error || hal_file_write(a->f, buf, len) != (int)len) {
        a->error = 1;
        return -1;
    }
    a->pos += (uint32_t)len;
    return 0;
}

static int add_chunk(avi_t *a, const char *id, const uint8_t *data, size_t len, int key)
{
    uint8_t hdr[8], ent[16], pad = 0;

    cc(hdr, id);
    le32(hdr + 4, (uint32_t)len);
    cc(ent, id);
    le32(ent + 4, key ? 0x10 : 0);              /* AVIIF_KEYFRAME */
    le32(ent + 8, a->pos - a->movi_pos);        /* offset from 'movi' */
    le32(ent + 12, (uint32_t)len);
    if (put(a, hdr, 8) || put(a, data, len) || ((len & 1) && put(a, &pad, 1)))
        return -1;
    if (hal_file_write(a->idx, ent, 16) != 16) {
        a->error = 1;
        return -1;
    }
    a->idx_entries++;
    return 0;
}

int avi_open(avi_t *a, const char *path, uint16_t width, uint16_t height, int audio)
{
    uint8_t h[HDR_SIZE];
    size_t n;

    memset(a, 0, sizeof(*a));
    a->width = width;
    a->height = height;
    a->audio = audio;
    snprintf(a->idx_path, sizeof(a->idx_path), "%s.idx", path);
    a->f = hal_file_open(path, 'w');
    if (!a->f)
        return -1;
    a->idx = hal_file_open(a->idx_path, 'w');
    if (!a->idx) {
        hal_file_close(a->f);
        hal_file_remove(path);
        return -1;
    }
    n = build_header(a, h, 0, 0, 0, 15, 1);
    if (put(a, h, n))
        return -1;
    a->movi_pos = a->pos - 4;
    return 0;
}

int avi_add_video(avi_t *a, const uint8_t *jpeg, size_t len, uint32_t ts_ms)
{
    if (!a->frames)
        a->t_start = ts_ms;
    a->t_last = ts_ms;
    if (len > a->max_frame)
        a->max_frame = (uint32_t)len;
    if (add_chunk(a, "00dc", jpeg, len, 1))
        return -1;
    a->frames++;
    return 0;
}

int avi_add_audio(avi_t *a, const uint8_t *alaw, size_t len)
{
    if (!a->audio || add_chunk(a, "01wb", alaw, len, 1))
        return -1;
    a->audio_bytes += (uint32_t)len;
    return 0;
}

int avi_close(avi_t *a)
{
    uint8_t h[HDR_SIZE], buf[512], hdr[8];
    uint32_t dur_ms = a->t_last - a->t_start, idx_size = a->idx_entries * 16;
    uint32_t movi_size, usec, num, den;
    size_t n;
    int r;

    /* Frame rate from the measured span: frames-1 intervals over dur_ms. */
    if (a->frames > 1 && dur_ms) {
        num = (a->frames - 1) * 1000u;
        den = dur_ms;
        usec = (uint32_t)((uint64_t)dur_ms * 1000u / (a->frames - 1));
    } else {
        num = 15; den = 1; usec = 66667;
    }
    movi_size = a->pos - a->movi_pos;

    /* Append idx1 from the spooled index. */
    hal_file_close(a->idx);
    a->idx = hal_file_open(a->idx_path, 'r');
    cc(hdr, "idx1");
    le32(hdr + 4, idx_size);
    put(a, hdr, 8);
    while (a->idx && !a->error && (r = hal_file_read(a->idx, buf, sizeof(buf))) > 0)
        put(a, buf, (size_t)r);
    if (a->idx)
        hal_file_close(a->idx);
    hal_file_remove(a->idx_path);

    n = build_header(a, h, a->pos - 8, movi_size, usec, num, den);
    if (hal_file_seek(a->f, 0) != 0 || hal_file_write(a->f, h, n) != (int)n)
        a->error = 1;
    if (hal_file_close(a->f) != 0)
        a->error = 1;
    return a->error ? -1 : 0;
}
