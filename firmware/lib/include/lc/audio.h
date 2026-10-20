/*
 * Microphone audio for streaming: 8 kHz mono G.711 A-law (RTP payload
 * type 8, "PCMA") in 20 ms packets, kept in a small ring that any number
 * of consumers read at their own pace.
 */
#ifndef LC_AUDIO_H
#define LC_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#define AUDIO_RATE          8000
#define AUDIO_PKT_SAMPLES   160          /* 20 ms */
#define AUDIO_RTP_PT        8            /* PCMA */

/* Starts capture and the encoder task. Returns 0 on success; safe to call
 * once. Without a working microphone audio_available() stays 0. */
int audio_start(void);
int audio_available(void);

/* Copies the next packet after *seq into buf (AUDIO_PKT_SAMPLES bytes).
 * Readers that fell behind skip ahead to the oldest packet still held.
 * Returns the byte count, or 0 if nothing new. *rtp_ts gets the packet's
 * timestamp in 8 kHz units. Start with *seq = 0 to get the newest packet. */
int audio_get_packet(uint8_t *buf, uint32_t *seq, uint32_t *rtp_ts);

uint8_t audio_alaw_encode(int16_t pcm);

#endif
