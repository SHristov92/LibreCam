/* kv_flash.c against a RAM "flash" sector that behaves like NOR flash:
 * erase sets 0xFF, writes can only clear bits. */
#include <stdio.h>
#include <string.h>

#include "lc/hal.h"

static uint8_t g_sector[HAL_KV_FLASH_SIZE];
static int fails;

int hal_kv_flash_read(uint32_t off, void *buf, size_t len)
{
    if (off + len > sizeof(g_sector))
        return -1;
    memcpy(buf, g_sector + off, len);
    return 0;
}

int hal_kv_flash_write(uint32_t off, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t i;

    if (off + len > sizeof(g_sector))
        return -1;
    for (i = 0; i < len; i++)
        g_sector[off + i] &= p[i];
    return 0;
}

int hal_kv_flash_erase(void)
{
    memset(g_sector, 0xFF, sizeof(g_sector));
    return 0;
}

/* Single-threaded test: the lock only has to exist. */
struct hal_mutex { int dummy; };
static hal_mutex_t g_mutex;
hal_mutex_t *hal_mutex_create(void) { return &g_mutex; }
void hal_mutex_lock(hal_mutex_t *m) { (void)m; }
void hal_mutex_unlock(hal_mutex_t *m) { (void)m; }

static void expect(const char *what, int ok)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        fails++;
    }
}

/* A sector as the BK7252 firmware wrote it before the store moved into the
 * lib: {magic "VK9A", len, sum} then {klen, vlen lo, vlen hi, key, value}. */
static void write_legacy_sector(void)
{
    static const uint8_t rec[] = { 3, 2, 0, 'a', 'b', 'c', 0x12, 0x34 };
    uint32_t hdr[3], s = 0x9e3779b9u;
    size_t i;

    for (i = 0; i < sizeof(rec); i++)
        s = (s << 5) + s + rec[i];
    hdr[0] = 0x41394b56u;
    hdr[1] = sizeof(rec);
    hdr[2] = s;
    memset(g_sector, 0xFF, sizeof(g_sector));
    memcpy(g_sector, hdr, sizeof(hdr));
    memcpy(g_sector + sizeof(hdr), rec, sizeof(rec));
}

int main(void)
{
    uint8_t buf[64], big[HAL_KV_FLASH_SIZE];
    int n;

    hal_kv_flash_erase();
    expect("empty store has no key", hal_kv_get("x", buf, sizeof(buf)) == -1);

    expect("set a", hal_kv_set("a", "hello", 5) == 0);
    expect("set b", hal_kv_set("b", "12", 2) == 0);
    n = hal_kv_get("a", buf, sizeof(buf));
    expect("get a", n == 5 && !memcmp(buf, "hello", 5));

    expect("replace a", hal_kv_set("a", "hi", 2) == 0);
    n = hal_kv_get("a", buf, sizeof(buf));
    expect("get replaced a", n == 2 && !memcmp(buf, "hi", 2));
    n = hal_kv_get("b", buf, sizeof(buf));
    expect("b survives a's replacement", n == 2 && !memcmp(buf, "12", 2));

    n = hal_kv_get("a", buf, 1);
    expect("short buffer truncates", n == 1 && buf[0] == 'h');

    memset(big, 0x5A, sizeof(big));
    expect("oversized value refused", hal_kv_set("big", big, sizeof(big)) == -1);
    n = hal_kv_get("b", buf, sizeof(buf));
    expect("refused set leaves the store alone", n == 2);

    g_sector[20] ^= 0x01;   /* flip a bit in the records */
    expect("corrupt store reads as empty", hal_kv_get("b", buf, sizeof(buf)) == -1);

    expect("erase all", hal_kv_erase_all() == 0);
    expect("nothing after erase", hal_kv_get("a", buf, sizeof(buf)) == -1);

    write_legacy_sector();
    n = hal_kv_get("abc", buf, sizeof(buf));
    expect("reads the BK7252 format", n == 2 && buf[0] == 0x12 && buf[1] == 0x34);

    if (fails)
        return 1;
    printf("kv_flash: all passed\n");
    return 0;
}
