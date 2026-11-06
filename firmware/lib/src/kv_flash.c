/*
 * Key/value settings in one raw 4 KB flash sector, for ports without a file
 * system. The port supplies hal_kv_flash_read/write/erase (lc/hal.h) for the
 * sector it reserves; this file implements hal_kv_get/set/erase_all on top.
 *
 * Layout: header, then packed records {u8 key_len, u16 val_len, key, value},
 * terminated by key_len 0xFF (erased flash). The whole sector is rewritten on
 * every set; the app writes rarely. This is the format the BK7252 port has
 * used since its first release, so settings survive the move to this file.
 */
#include "lc/hal.h"

#include <stdlib.h>
#include <string.h>

#define KV_MAGIC   0x41394b56u   /* "VK9A" */

struct kv_hdr {
    uint32_t magic;
    uint32_t len;     /* bytes of records after the header */
    uint32_t sum;     /* checksum of the records */
};

static hal_mutex_t *g_kv_lock;

static uint32_t kv_sum(const uint8_t *p, uint32_t n)
{
    uint32_t s = 0x9e3779b9u;

    while (n--)
        s = (s << 5) + s + *p++;
    return s;
}

/* The first call comes from hal_init(), before any other task runs. */
static void kv_lock(void)
{
    if (!g_kv_lock)
        g_kv_lock = hal_mutex_create();
    if (g_kv_lock)
        hal_mutex_lock(g_kv_lock);
}

static void kv_unlock(void)
{
    if (g_kv_lock)
        hal_mutex_unlock(g_kv_lock);
}

/* Loads the record area into a fresh HAL_KV_FLASH_SIZE buffer. Returns its
 * length, 0 for an empty/invalid sector, -1 on allocation failure. */
static int kv_load(uint8_t **out)
{
    struct kv_hdr h;
    uint8_t *buf = malloc(HAL_KV_FLASH_SIZE);

    *out = buf;
    if (!buf)
        return -1;
    if (hal_kv_flash_read(0, &h, sizeof(h)) != 0)
        return 0;
    if (h.magic != KV_MAGIC || h.len > HAL_KV_FLASH_SIZE - sizeof(h))
        return 0;
    if (hal_kv_flash_read(sizeof(h), buf, h.len) != 0)
        return 0;
    return kv_sum(buf, h.len) == h.sum ? (int)h.len : 0;
}

static int kv_find(const uint8_t *buf, int len, const char *key, int *rec, int *rec_len)
{
    size_t klen = strlen(key);
    int off = 0;

    while (off + 3 <= len && buf[off] != 0xFF) {
        int kl = buf[off], vl = buf[off + 1] | (buf[off + 2] << 8);
        int total = 3 + kl + vl;

        if (off + total > len)
            break;
        if ((size_t)kl == klen && !memcmp(buf + off + 3, key, klen)) {
            *rec = off;
            *rec_len = total;
            return 1;
        }
        off += total;
    }
    return 0;
}

/* Header last, so a write cut short leaves an invalid (empty) store rather
 * than a valid header over half-written records. */
static int kv_store(const uint8_t *buf, uint32_t len)
{
    struct kv_hdr h = { KV_MAGIC, len, kv_sum(buf, len) };
    struct kv_hdr check;

    if (hal_kv_flash_erase() != 0 ||
        hal_kv_flash_write(sizeof(h), buf, len) != 0 ||
        hal_kv_flash_write(0, &h, sizeof(h)) != 0 ||
        hal_kv_flash_read(0, &check, sizeof(check)) != 0)
        return -1;
    return memcmp(&check, &h, sizeof(h)) ? -1 : 0;
}

int hal_kv_get(const char *key, void *val, size_t cap)
{
    uint8_t *buf;
    int len, rec, rec_len, ret = -1;

    kv_lock();
    len = kv_load(&buf);
    if (len > 0 && kv_find(buf, len, key, &rec, &rec_len)) {
        int kl = buf[rec], vl = buf[rec + 1] | (buf[rec + 2] << 8);
        size_t n = (size_t)vl < cap ? (size_t)vl : cap;

        memcpy(val, buf + rec + 3 + kl, n);
        ret = (int)n;
    }
    free(buf);
    kv_unlock();
    return ret;
}

int hal_kv_set(const char *key, const void *val, size_t len)
{
    size_t klen = strlen(key);
    uint8_t *buf;
    int used, rec, rec_len, ret = -1;

    if (klen == 0 || klen > 64 || len > 0xFFFF)
        return -1;
    kv_lock();
    used = kv_load(&buf);
    if (used < 0)
        goto out;
    if (kv_find(buf, used, key, &rec, &rec_len)) {
        memmove(buf + rec, buf + rec + rec_len, used - rec - rec_len);
        used -= rec_len;
    }
    if ((size_t)used + 3 + klen + len > HAL_KV_FLASH_SIZE - sizeof(struct kv_hdr))
        goto out;
    buf[used] = (uint8_t)klen;
    buf[used + 1] = len & 0xFF;
    buf[used + 2] = len >> 8;
    memcpy(buf + used + 3, key, klen);
    memcpy(buf + used + 3 + klen, val, len);
    used += 3 + klen + len;
    ret = kv_store(buf, used);
out:
    free(buf);
    kv_unlock();
    return ret;
}

int hal_kv_erase_all(void)
{
    int ret;

    kv_lock();
    ret = hal_kv_flash_erase();
    kv_unlock();
    return ret;
}
