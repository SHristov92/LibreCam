#include "a9/recorder.h"
#include "a9/audio.h"
#include "a9/avi.h"
#include "a9/board.h"
#include "a9/clock.h"
#include "a9/config.h"
#include "a9/hal.h"
#include "a9/rtp_jpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MS          20
#define LONG_PRESS_MS    1000
#define MIN_FREE_BYTES   (64ull << 20)

static volatile int g_mounted, g_snap_req, g_snap_done, g_snap_ok;
static volatile int g_clip_req, g_clip_stop, g_recording;
static volatile uint32_t g_rec_start;
static char g_snap_path[64], g_last[64];
static const char *volatile g_active;   /* basename of the clip being written */
static uint64_t g_total, g_free;
static uint32_t g_space_at;

/* ---- Names and space ----------------------------------------------------- */

static int file_exists(const char *path)
{
    hal_file_t *f = hal_file_open(path, 'r');
    if (f)
        hal_file_close(f);
    return f != NULL;
}

/* <dir>/<prefix>_YYYYMMDD_HHMMSS.<ext>, or <prefix>_UP<uptime-s> before the
 * clock is known; _2, _3... on collision. */
static void make_name(char *out, size_t cap, const char *dir, const char *prefix, const char *ext)
{
    char base[48];
    a9_tm_t t;
    int i;

    if (a9_time_local(&t) == 0)
        snprintf(base, sizeof(base), "%s_%04d%02d%02d_%02d%02d%02d", prefix,
                 t.year, t.month, t.day, t.hour, t.min, t.sec);
    else
        snprintf(base, sizeof(base), "%s_UP%06u", prefix, (unsigned)(hal_millis() / 1000));
    snprintf(out, cap, "%s/%s.%s", dir, base, ext);
    for (i = 2; i < 100 && file_exists(out); i++)
        snprintf(out, cap, "%s/%s_%d.%s", dir, base, i, ext);
}

struct oldest {
    char name[64];
    const char *skip;           /* file being written, never deleted */
};

static int find_oldest(const char *name, uint32_t size, int is_dir, void *ctx)
{
    struct oldest *o = ctx;
    size_t l = strlen(name);

    (void)size;
    if (is_dir || l >= sizeof(o->name) || (l > 4 && !strcmp(name + l - 4, ".idx")))
        return 0;
    if (o->skip && !strcmp(name, o->skip))
        return 0;
    if (!o->name[0] || strcmp(name, o->name) < 0)
        snprintf(o->name, sizeof(o->name), "%s", name);
    return 0;
}

static void refresh_space(void)
{
    if (hal_storage_space(&g_total, &g_free) != 0)
        g_total = g_free = 0;
    g_space_at = hal_millis();
}

/* Deletes oldest files until there is room for `need` bytes plus the reserve. */
static void ensure_space(uint64_t need, const char *active)
{
    static const char *const order[] = { REC_DIR_TIMELAPSE, REC_DIR_VIDEO, REC_DIR_PHOTO };
    uint64_t reserve;
    size_t d;
    int guard;

    if (!active)
        active = g_active;
    refresh_space();
    reserve = g_total / 20 > MIN_FREE_BYTES ? g_total / 20 : MIN_FREE_BYTES;
    for (d = 0; d < 3; d++) {
        for (guard = 0; guard < 200 && g_free < need + reserve; guard++) {
            struct oldest o;
            char path[96];

            memset(&o, 0, sizeof(o));
            o.skip = active;
            hal_dir_list(order[d], find_oldest, &o);
            if (!o.name[0])
                break;
            snprintf(path, sizeof(path), "%s/%s", order[d], o.name);
            printf("[rec] card low, deleting %s\n", path);
            if (hal_file_remove(path) != 0)
                break;
            refresh_space();
        }
    }
}

/* ---- Stills ---------------------------------------------------------------- */

static int save_still(const char *dir, const char *prefix, char *path, size_t cap)
{
    uint8_t *buf = hal_malloc_large(A9_FRAME_BUF);
    uint32_t seq = 0, ts;
    hal_file_t *f;
    int len, ok = -1;

    if (!buf)
        return -1;
    len = hal_camera_get_frame(buf, A9_FRAME_BUF, &seq, &ts, 1000);
    if (len > 0) {
        ensure_space((uint64_t)len, NULL);
        make_name(path, cap, dir, prefix, "jpg");
        f = hal_file_open(path, 'w');
        if (f) {
            ok = hal_file_write(f, buf, (size_t)len) == len ? 0 : -1;
            if (hal_file_close(f) != 0)
                ok = -1;
            if (ok != 0)
                hal_file_remove(path);
        }
    }
    free(buf);
    if (ok == 0)
        snprintf(g_last, sizeof(g_last), "%s", path);
    else
        printf("[rec] could not save %s still\n", prefix);
    return ok;
}

/* ---- Clips ------------------------------------------------------------------- */

#define AUDIO_BATCH 25        /* up to 500 ms of audio per chunk */

typedef struct {
    avi_t    avi;
    uint8_t *frame;
    uint32_t seq, audio_seq;
    uint32_t log_at, slowest_ms;
    uint8_t  audio[AUDIO_BATCH * AUDIO_PKT_SAMPLES];
    char     path[64];
    const char *name;           /* path basename, protected from cleanup */
} clip_t;

static clip_t g_clip;           /* ~6 KB: kept off the task stack */

static int clip_begin(clip_t *c)
{
    jpeg_info_t ji;
    uint32_t ts;
    int len;

    memset(c, 0, sizeof(*c));
    c->frame = hal_malloc_large(A9_FRAME_BUF);
    if (!c->frame) {
        printf("[rec] out of memory for clip\n");
        return -1;
    }
    /* The first frame gives the real size (whatever the sensor settled on). */
    len = hal_camera_get_frame(c->frame, A9_FRAME_BUF, &c->seq, &ts, 1000);
    if (len <= 0 || jpeg_parse(c->frame, (size_t)len, &ji) != 0) {
        printf("[rec] no camera frame, no clip\n");
        free(c->frame);
        return -1;
    }
    /* Rough size: ~35 KB VGA frames at the configured rate, plus audio. */
    ensure_space((uint64_t)g_config.clip_s * (g_config.camera.fps * 35000u + AUDIO_RATE), NULL);
    make_name(c->path, sizeof(c->path), REC_DIR_VIDEO, "VID", "avi");
    c->name = strrchr(c->path, '/') + 1;
    if (avi_open(&c->avi, c->path, ji.width, ji.height, audio_available()) != 0 ||
        avi_add_video(&c->avi, c->frame, (size_t)len, ts) != 0) {
        printf("[rec] cannot create %s\n", c->path);
        free(c->frame);
        return -1;
    }
    g_rec_start = hal_millis();
    c->log_at = g_rec_start;
    g_active = c->name;
    g_recording = 1;
    printf("[rec] recording %s for %u s\n", c->path, g_config.clip_s);
    return 0;
}

/* Writes whatever arrived since the last call: the pending audio as one
 * chunk, then the newest frame. Returns -1 on a write error. */
static int clip_step(clip_t *c)
{
    uint32_t ts, t0, dt, now;
    size_t alen = 0;
    int len;

    while (c->avi.audio && alen + AUDIO_PKT_SAMPLES <= sizeof(c->audio) &&
           audio_get_packet(c->audio + alen, &c->audio_seq, &ts) > 0)
        alen += AUDIO_PKT_SAMPLES;
    t0 = hal_millis();
    if (alen && avi_add_audio(&c->avi, c->audio, alen) != 0)
        return -1;
    dt = hal_millis() - t0;
    len = hal_camera_get_frame(c->frame, A9_FRAME_BUF, &c->seq, &ts, TICK_MS);
    if (len > 0) {
        t0 = hal_millis();
        if (avi_add_video(&c->avi, c->frame, (size_t)len, ts) != 0)
            return -1;
        dt += hal_millis() - t0;
    }
    now = hal_millis();
    if (dt > c->slowest_ms)
        c->slowest_ms = dt;
    if (now - c->log_at >= 5000) {
        c->log_at = now;
        printf("[rec] %u s: %u frames, %u KB, slowest write %u ms\n",
               (unsigned)((now - g_rec_start) / 1000), (unsigned)c->avi.frames,
               (unsigned)(c->avi.pos >> 10), (unsigned)c->slowest_ms);
    }
    return 0;
}

static void clip_end(clip_t *c)
{
    uint32_t t0 = hal_millis();
    int ok;

    printf("[rec] stopping %s\n", c->path);
    ok = avi_close(&c->avi) == 0;
    printf("[rec] %s %s: %u frames, %u s (closing took %u ms)\n", ok ? "saved" : "FAILED",
           c->path, (unsigned)c->avi.frames, (unsigned)((t0 - g_rec_start) / 1000),
           (unsigned)(hal_millis() - t0));
    if (ok)
        snprintf(g_last, sizeof(g_last), "%s", c->path);
    free(c->frame);
    g_active = NULL;
    g_recording = 0;
}

/* ---- Task ------------------------------------------------------------------- */

static void recorder_task(void *arg)
{
    uint32_t last_poll = 0, pressed_at = 0, next_tl = 0;
    int long_done = 0;
    clip_t *clip = &g_clip;

    (void)arg;
    for (;;) {
        uint32_t now = hal_millis();

        if (now - last_poll >= 1000 || !last_poll) {
            int m = hal_storage_poll();
            last_poll = now ? now : 1;
            if (m && !g_mounted) {
                hal_mkdir(REC_DIR_PHOTO);
                hal_mkdir(REC_DIR_TIMELAPSE);
                hal_mkdir(REC_DIR_VIDEO);
                refresh_space();
            }
            if (!m && g_recording) {
                printf("[rec] card gone while recording\n");
                clip_end(clip);
            }
            g_mounted = m;
            if (m && now - g_space_at > 30000)
                refresh_space();
        }

        /* User button: short press = snapshot, hold 1 s = start/stop clip. */
        if (hal_user_button_pressed()) {
            if (!pressed_at)
                pressed_at = now ? now : 1;
            else if (!long_done && now - pressed_at >= LONG_PRESS_MS) {
                long_done = 1;
                if (g_recording)
                    g_clip_stop = 1;
                else
                    g_clip_req = 1;
            }
        } else {
            if (pressed_at && !long_done)
                g_snap_req = 2;          /* 2 = from the button, nobody waits */
            pressed_at = 0;
            long_done = 0;
        }

        if (g_snap_req) {
            int from_button = g_snap_req == 2;
            char path[64];
            int ok = g_mounted ? save_still(REC_DIR_PHOTO, "IMG", path, sizeof(path)) : -1;

            if (ok == 0)
                printf("[rec] snapshot %s\n", path);
            if (!from_button) {
                snprintf(g_snap_path, sizeof(g_snap_path), "%s", ok == 0 ? path : "");
                g_snap_ok = ok == 0;
                g_snap_done = 1;
            }
            g_snap_req = 0;
        }

        if (g_config.timelapse_s && g_mounted && (int32_t)(now - next_tl) >= 0) {
            char path[64];
            next_tl = now + g_config.timelapse_s * 1000u;
            save_still(REC_DIR_TIMELAPSE, "TL", path, sizeof(path));
        }

        if (g_clip_req) {
            g_clip_req = 0;
            g_clip_stop = 0;
            if (!g_recording && g_mounted)
                clip_begin(clip);
        }
        if (g_recording) {
            int err = clip_step(clip);
            if (err || g_clip_stop || hal_millis() - g_rec_start >= g_config.clip_s * 1000u) {
                if (err)
                    printf("[rec] write error, ending clip\n");
                g_clip_stop = 0;
                clip_end(clip);
                refresh_space();
            }
        } else {
            hal_sleep_ms(TICK_MS);
        }
    }
}

int recorder_start(void)
{
    return hal_task_start("recorder", recorder_task, NULL, 6144, 4);
}

int recorder_snapshot(char *path, size_t cap)
{
    int i;

    if (!g_mounted)
        return -1;
    g_snap_done = 0;
    g_snap_req = 1;
    for (i = 0; i < 150 && !g_snap_done; i++)
        hal_sleep_ms(20);
    if (!g_snap_done || !g_snap_ok)
        return -1;
    snprintf(path, cap, "%s", g_snap_path);
    return 0;
}

int recorder_clip_start(void)
{
    if (!g_mounted)
        return -1;
    g_clip_req = 1;
    return 0;
}

void recorder_clip_stop(void)
{
    if (g_recording)
        g_clip_stop = 1;
}

int recorder_is_recording(void)
{
    return g_recording;
}

void recorder_status(recorder_status_t *st)
{
    st->mounted = g_mounted;
    st->total_bytes = g_mounted ? g_total : 0;
    st->free_bytes = g_mounted ? g_free : 0;
    st->recording = g_recording;
    st->rec_elapsed_s = g_recording ? (hal_millis() - g_rec_start) / 1000 : 0;
    snprintf(st->last_file, sizeof(st->last_file), "%s", g_last);
}
