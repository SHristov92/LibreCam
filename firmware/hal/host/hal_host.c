/*
 * Linux host implementation of the HAL, for developing and testing the
 * application without hardware.
 *
 *   LC_FRAMES_DIR  directory of .jpg files played in a loop as the camera
 *                  (default: ./test_frames, see make_test_frames.sh)
 *   LC_KV_DIR      directory used as the flash key/value store (./lc_kv)
 *   LC_SD_DIR      directory standing in for the SD card (./sd)
 *   LC_BUTTON_FILE power button is "pressed" while this file exists
 *   LC_SD_SIZE_MB  pretend card size; free space = size - files stored
 */
#include "lc/hal.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static struct timespec g_t0;
static hal_wifi_state_t g_wifi = HAL_WIFI_OFF;

/* ---- System ---------------------------------------------------------- */

void hal_init(void)
{
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
}

uint32_t hal_millis(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec - g_t0.tv_sec) * 1000 + (t.tv_nsec - g_t0.tv_nsec) / 1000000);
}

void hal_sleep_ms(uint32_t ms)
{
    usleep(ms * 1000);
}

void hal_reboot(void)
{
    printf("[host] reboot requested, exiting\n");
    fflush(stdout);
    exit(0);
}

void hal_deep_sleep(void)
{
    printf("[host] deep sleep requested; exiting\n");
    fflush(stdout);
    exit(0);
}

void hal_get_mac(uint8_t mac[6])
{
    static const uint8_t m[6] = { 0x02, 0x00, 0x00, 0xa9, 0x12, 0x34 };
    memcpy(mac, m, 6);
}

struct task_start {
    hal_task_fn fn;
    void *arg;
};

static void *task_tramp(void *p)
{
    struct task_start ts = *(struct task_start *)p;
    free(p);
    ts.fn(ts.arg);
    return NULL;
}

int hal_task_start(const char *name, hal_task_fn fn, void *arg,
                   uint32_t stack_bytes, int priority)
{
    pthread_t t;
    pthread_attr_t a;
    struct task_start *ts = malloc(sizeof(*ts));
    int rc;

    (void)name;
    (void)priority;
    if (!ts)
        return -1;
    ts->fn = fn;
    ts->arg = arg;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    /* Keep host stacks small-ish so stack overruns show up in testing. */
    pthread_attr_setstacksize(&a, stack_bytes < 65536 ? 65536 : stack_bytes);
    rc = pthread_create(&t, &a, task_tramp, ts);
    pthread_attr_destroy(&a);
    if (rc) {
        free(ts);
        return -1;
    }
    return 0;
}

struct hal_mutex {
    pthread_mutex_t m;
};

hal_mutex_t *hal_mutex_create(void)
{
    hal_mutex_t *m = malloc(sizeof(*m));
    if (m)
        pthread_mutex_init(&m->m, NULL);
    return m;
}

void hal_mutex_lock(hal_mutex_t *m)   { pthread_mutex_lock(&m->m); }
void hal_mutex_unlock(hal_mutex_t *m) { pthread_mutex_unlock(&m->m); }

/* ---- Key/value storage: one file per key ----------------------------- */

static void kv_path(const char *key, char *out, size_t cap)
{
    const char *dir = getenv("LC_KV_DIR");
    snprintf(out, cap, "%s/%s", dir ? dir : "lc_kv", key);
}

int hal_kv_get(const char *key, void *buf, size_t len)
{
    char p[256];
    FILE *f;
    size_t n;

    kv_path(key, p, sizeof(p));
    if (!(f = fopen(p, "rb")))
        return -1;
    n = fread(buf, 1, len, f);
    fclose(f);
    return (int)n;
}

int hal_kv_set(const char *key, const void *buf, size_t len)
{
    char p[256];
    FILE *f;
    const char *dir = getenv("LC_KV_DIR");

    mkdir(dir ? dir : "lc_kv", 0755);
    kv_path(key, p, sizeof(p));
    if (!(f = fopen(p, "wb")))
        return -1;
    if (fwrite(buf, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    return fclose(f) ? -1 : 0;
}

int hal_kv_erase_all(void)
{
    char p[256];
    kv_path("a9cfg", p, sizeof(p));
    unlink(p);
    return 0;
}

/* ---- Wi-Fi: simulated, the host's own network is used --------------- */

int hal_wifi_start_ap(const char *ssid, const char *password, uint8_t channel)
{
    printf("[host] AP \"%s\" ch%u %s (simulated)\n", ssid, channel,
           password[0] ? "WPA2" : "open");
    g_wifi = HAL_WIFI_AP;
    return 0;
}

int hal_wifi_start_sta(const char *ssid, const char *password)
{
    (void)password;
    printf("[host] joining \"%s\" (simulated)\n", ssid);
    g_wifi = HAL_WIFI_STA_CONNECTED;
    return 0;
}

void hal_wifi_stop(void)               { g_wifi = HAL_WIFI_OFF; }
hal_wifi_state_t hal_wifi_state(void)  { return g_wifi; }
int  hal_wifi_rssi(void)               { return -55; }

void hal_wifi_ip(char *buf, size_t len)
{
    const char *ip = getenv("LC_HOST_IP");
    snprintf(buf, len, "%s", ip ? ip : "127.0.0.1");
}

/* ---- Camera: replays JPEG files -------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t   len;
} jpeg_file_t;

static jpeg_file_t *g_frames;
static size_t g_nframes;
static uint8_t g_fps = 15;

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int hal_camera_start(const hal_camera_cfg_t *cfg)
{
    const char *dir = getenv("LC_FRAMES_DIR");
    DIR *d;
    struct dirent *e;
    char *names[1024];
    size_t n = 0, i;

    if (!dir)
        dir = "test_frames";
    g_fps = cfg->fps ? cfg->fps : 15;
    if (!(d = opendir(dir))) {
        printf("[host] no frames directory %s\n", dir);
        return -1;
    }
    while ((e = readdir(d)) && n < 1024) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".jpg"))
            names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof(names[0]), cmp_names);

    g_frames = calloc(n ? n : 1, sizeof(*g_frames));
    for (i = 0; i < n; i++) {
        char p[512];
        FILE *f;
        long sz;
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        free(names[i]);
        if (!(f = fopen(p, "rb")))
            continue;
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        g_frames[g_nframes].data = malloc((size_t)sz);
        if (fread(g_frames[g_nframes].data, 1, (size_t)sz, f) == (size_t)sz)
            g_frames[g_nframes++].len = (size_t)sz;
        fclose(f);
    }
    printf("[host] camera: %zu frames from %s at %u fps\n", g_nframes, dir, g_fps);
    return g_nframes ? 0 : -1;
}

void hal_camera_stop(void) {}

void hal_camera_info(hal_camera_info_t *info)
{
    /* Test frames come in whatever size they were made; report all sizes. */
    info->sensor = "test frames";
    info->width = info->height = 0;
    info->resolutions = (1u << HAL_RES_QVGA) | (1u << HAL_RES_VGA) | (1u << HAL_RES_HD);
}

int hal_camera_get_frame(uint8_t *buf, size_t cap, uint32_t *last_seq,
                         uint32_t *ts_ms, uint32_t timeout_ms)
{
    uint32_t start = hal_millis();
    const jpeg_file_t *f;
    uint32_t seq;

    if (!g_nframes)
        return -1;
    for (;;) {
        seq = hal_millis() * g_fps / 1000 + 1;
        if (seq > *last_seq)
            break;
        if (hal_millis() - start >= timeout_ms)
            return 0;
        hal_sleep_ms(5);
    }
    f = &g_frames[seq % g_nframes];
    if (f->len > cap)
        return -(int)f->len;
    memcpy(buf, f->data, f->len);
    *last_seq = seq;
    *ts_ms = seq * 1000 / g_fps;
    return (int)f->len;
}

/* ---- GPIO ------------------------------------------------------------ */

void hal_led_set(int on)       { (void)on; }
/* LC_BUTTON_FILE=<path>: the power button counts as pressed while that file exists. */
int hal_button_pressed(void)
{
    const char *f = getenv("LC_BUTTON_FILE");
    struct stat st;
    return f && stat(f, &st) == 0;
}

void *hal_malloc_large(size_t size) { return malloc(size); }

/* Microphone: a 440 Hz triangle tone, paced by the clock. */
static uint32_t g_audio_rate, g_audio_t0, g_audio_sent;

int hal_audio_start(uint32_t rate)
{
    g_audio_rate = rate;
    g_audio_t0 = hal_millis();
    g_audio_sent = 0;
    return 0;
}

int hal_audio_read(int16_t *buf, size_t samples, uint32_t timeout_ms)
{
    uint32_t due, n, i;

    if (!g_audio_rate)
        return -1;
    due = (uint32_t)((uint64_t)(hal_millis() - g_audio_t0) * g_audio_rate / 1000);
    if (due <= g_audio_sent) {
        hal_sleep_ms(timeout_ms < 10 ? timeout_ms : 10);
        return 0;
    }
    n = due - g_audio_sent;
    if (n > samples)
        n = (uint32_t)samples;
    for (i = 0; i < n; i++) {
        uint32_t ph = (uint32_t)(((uint64_t)(g_audio_sent + i) * 440 * 65536 / g_audio_rate) & 0xFFFF);
        int tri = ph < 0x8000 ? (int)ph * 2 - 0x8000 : 0x17FFF - (int)ph * 2;
        buf[i] = (int16_t)(tri / 4);
    }
    g_audio_sent += n;
    return (int)n;
}

/* LC_VBAT_MV=<mV> simulates a battery reading; unset means "no sensor". */
int hal_battery_mv(void)
{
    const char *v = getenv("LC_VBAT_MV");
    return v ? atoi(v) : -1;
}

int hal_user_button_pressed(void) { return 0; }

/* ---- Storage: a directory standing in for the SD card ---------------- */
/* LC_SD_DIR (default ./sd) is the card root; the card is "inserted" while
 * that directory exists. */

struct hal_file {
    FILE *fp;
};

static const char *sd_root(void)
{
    const char *d = getenv("LC_SD_DIR");
    return d ? d : "sd";
}

static void sd_path(char *out, size_t cap, const char *path)
{
    snprintf(out, cap, "%s%s%s", sd_root(), path[0] == '/' ? "" : "/", path);
}

int hal_storage_poll(void)
{
    struct stat st;
    return stat(sd_root(), &st) == 0 && S_ISDIR(st.st_mode);
}

static uint64_t du(const char *dir, int depth)
{
    char p[1024];
    struct dirent *e;
    struct stat st;
    uint64_t sum = 0;
    DIR *d = opendir(dir);

    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (stat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode) && depth > 0)
            sum += du(p, depth - 1);
        else if (S_ISREG(st.st_mode))
            sum += (uint64_t)st.st_size;
    }
    closedir(d);
    return sum;
}

/* Card size from LC_SD_SIZE_MB (default 8192); free = size - stored bytes. */
int hal_storage_space(uint64_t *total, uint64_t *free_bytes)
{
    const char *sz = getenv("LC_SD_SIZE_MB");
    uint64_t used;

    if (!hal_storage_poll())
        return -1;
    *total = (uint64_t)(sz ? strtoull(sz, NULL, 10) : 8192) << 20;
    used = du(sd_root(), 2);
    *free_bytes = used < *total ? *total - used : 0;
    return 0;
}

hal_file_t *hal_file_open(const char *path, char mode)
{
    char p[512];
    hal_file_t *f;
    FILE *fp;

    sd_path(p, sizeof(p), path);
    fp = fopen(p, mode == 'w' ? "w+b" : "rb");
    if (!fp)
        return NULL;
    f = malloc(sizeof(*f));
    if (!f) {
        fclose(fp);
        return NULL;
    }
    f->fp = fp;
    return f;
}

int hal_file_read(hal_file_t *f, void *buf, size_t len)
{
    size_t n = fread(buf, 1, len, f->fp);
    return ferror(f->fp) ? -1 : (int)n;
}

int hal_file_write(hal_file_t *f, const void *buf, size_t len)
{
    return fwrite(buf, 1, len, f->fp) == len ? (int)len : -1;
}

int hal_file_seek(hal_file_t *f, uint32_t pos)
{
    return fseek(f->fp, (long)pos, SEEK_SET);
}

uint32_t hal_file_size(hal_file_t *f)
{
    long cur = ftell(f->fp), end;
    fseek(f->fp, 0, SEEK_END);
    end = ftell(f->fp);
    fseek(f->fp, cur, SEEK_SET);
    return (uint32_t)end;
}

int hal_file_close(hal_file_t *f)
{
    int r = fclose(f->fp);
    free(f);
    return r;
}

int hal_file_remove(const char *path)
{
    char p[512];
    sd_path(p, sizeof(p), path);
    return remove(p);
}

int hal_mkdir(const char *path)
{
    char p[512];
    struct stat st;
    sd_path(p, sizeof(p), path);
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
        return 0;
    return mkdir(p, 0755);
}

int hal_dir_list(const char *path, hal_dir_cb cb, void *ctx)
{
    char p[512], fp[1024];
    struct dirent *e;
    struct stat st;
    DIR *d;

    sd_path(p, sizeof(p), path);
    d = opendir(p);
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(fp, sizeof(fp), "%s/%s", p, e->d_name);
        if (stat(fp, &st) != 0)
            continue;
        if (cb(e->d_name, (uint32_t)st.st_size, S_ISDIR(st.st_mode), ctx))
            break;
    }
    closedir(d);
    return 0;
}
