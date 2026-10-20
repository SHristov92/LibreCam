#include "lc/audio.h"
#include "lc/hal.h"

#include <stdio.h>
#include <string.h>

#define RING_PKTS 25   /* 500 ms */

static uint8_t g_ring[RING_PKTS][AUDIO_PKT_SAMPLES];
static volatile uint32_t g_head;   /* packets written; packet n (from 1) is in slot (n-1) % RING_PKTS */
static hal_mutex_t *g_lock;
static volatile int g_running;

/* ITU-T G.711 A-law, as in the reference linear2alaw(). */
uint8_t audio_alaw_encode(int16_t pcm)
{
    static const int16_t seg_end[8] = { 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF };
    int val = pcm >> 3;
    uint8_t mask, aval;
    int seg;

    if (val >= 0) {
        mask = 0xD5;
    } else {
        mask = 0x55;
        val = -val - 1;
    }
    for (seg = 0; seg < 8 && val > seg_end[seg]; seg++)
        ;
    if (seg >= 8)
        return (uint8_t)(0x7F ^ mask);
    aval = (uint8_t)(seg << 4);
    aval |= (uint8_t)((seg < 2 ? val >> 1 : val >> seg) & 0x0F);
    return (uint8_t)(aval ^ mask);
}

static void audio_task(void *arg)
{
    int16_t pcm[AUDIO_PKT_SAMPLES];
    size_t have = 0;

    (void)arg;
    for (;;) {
        int n = hal_audio_read(pcm + have, AUDIO_PKT_SAMPLES - have, 100);
        uint8_t *slot;
        size_t i;

        if (n < 0) {
            hal_sleep_ms(100);
            continue;
        }
        have += (size_t)n;
        if (have < AUDIO_PKT_SAMPLES)
            continue;
        have = 0;
        slot = g_ring[g_head % RING_PKTS];
        hal_mutex_lock(g_lock);
        for (i = 0; i < AUDIO_PKT_SAMPLES; i++)
            slot[i] = audio_alaw_encode(pcm[i]);
        g_head++;
        hal_mutex_unlock(g_lock);
    }
}

int audio_start(void)
{
    if (g_running)
        return 0;
    if (hal_audio_start(AUDIO_RATE) != 0)
        return -1;
    g_lock = hal_mutex_create();
    if (!g_lock || hal_task_start("audio", audio_task, NULL, 2048, 7) != 0)
        return -1;
    g_running = 1;
    return 0;
}

int audio_available(void)
{
    return g_running;
}

int audio_get_packet(uint8_t *buf, uint32_t *seq, uint32_t *rtp_ts)
{
    uint32_t head, next;

    if (!g_running)
        return 0;
    hal_mutex_lock(g_lock);
    head = g_head;
    next = *seq ? *seq + 1 : head;          /* fresh readers start at the newest */
    if (head >= RING_PKTS && next <= head - RING_PKTS)
        next = head - RING_PKTS + 1;        /* fell behind: skip to the oldest kept */
    if (next == 0 || next > head) {
        hal_mutex_unlock(g_lock);
        return 0;
    }
    memcpy(buf, g_ring[(next - 1) % RING_PKTS], AUDIO_PKT_SAMPLES);
    hal_mutex_unlock(g_lock);
    *seq = next;
    *rtp_ts = (next - 1) * AUDIO_PKT_SAMPLES;
    return AUDIO_PKT_SAMPLES;
}
