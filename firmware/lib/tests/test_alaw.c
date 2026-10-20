/* Checks the G.711 A-law encoder against reference values (ITU-T G.711,
 * the classic linear2alaw()) and that it is monotonic over the full range. */
#include <stdio.h>

#include "lc/audio.h"

/* audio.c needs the HAL only for capture; the encoder is pure. */
#include "../src/audio.c"
int hal_audio_start(uint32_t rate) { (void)rate; return -1; }
int hal_audio_read(int16_t *b, size_t n, uint32_t t) { (void)b; (void)n; (void)t; return -1; }
hal_mutex_t *hal_mutex_create(void) { return NULL; }
void hal_mutex_lock(hal_mutex_t *m) { (void)m; }
void hal_mutex_unlock(hal_mutex_t *m) { (void)m; }
int hal_task_start(const char *n, hal_task_fn f, void *a, uint32_t s, int p)
{ (void)n; (void)f; (void)a; (void)s; (void)p; return -1; }
void hal_sleep_ms(uint32_t ms) { (void)ms; }

/* Decoded A-law values must rise with the input. */
static int alaw_decode(uint8_t a)
{
    int t, seg;

    a ^= 0x55;
    t = (a & 0x0F) << 4;
    seg = (a & 0x70) >> 4;
    if (seg == 0)
        t += 8;
    else if (seg == 1)
        t += 0x108;
    else
        t = (t + 0x108) << (seg - 1);
    return (a & 0x80) ? t : -t;
}

int main(void)
{
    static const struct { int16_t in; uint8_t out; } ref[] = {
        { 0, 0xD5 }, { -1, 0x55 }, { 8, 0xD5 }, { 16, 0xD4 },
        { 32767, 0xAA }, { -32768, 0x2A }, { 1000, 0xFA }, { -1000, 0x7A },
    };
    int fails = 0, prev = -1000000;
    size_t i;
    long v;

    for (i = 0; i < sizeof(ref) / sizeof(ref[0]); i++) {
        uint8_t got = audio_alaw_encode(ref[i].in);
        if (got != ref[i].out) {
            printf("alaw(%d) = 0x%02X, want 0x%02X\n", ref[i].in, got, ref[i].out);
            fails++;
        }
    }
    for (v = -32768; v <= 32767; v++) {
        int d = alaw_decode(audio_alaw_encode((int16_t)v));
        if (d < prev) {
            printf("not monotonic at %ld\n", v);
            fails++;
            break;
        }
        prev = d;
    }
    printf("%s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
