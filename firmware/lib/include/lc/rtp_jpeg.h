/*
 * RTP payload format for JPEG (RFC 2435).
 */
#ifndef LC_RTP_JPEG_H
#define LC_RTP_JPEG_H

#include <stddef.h>
#include <stdint.h>

#define RTP_PT_JPEG       26
#define RTP_MAX_PAYLOAD   1400
/* Room for the 4-byte RTSP interleaved prefix in front of the RTP header. */
#define RTP_PREFIX        4
#define RTP_PACKET_CAP    (RTP_PREFIX + 12 + 8 + 4 + 4 + 128 + RTP_MAX_PAYLOAD)

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  type;          /* 0 = 4:2:2, 1 = 4:2:0, +64 if restart markers */
    uint16_t restart_interval;
    uint8_t  qtables[128];  /* luma + chroma, 8-bit precision */
    uint16_t qtables_len;   /* always 128 */
    const uint8_t *scan;    /* entropy-coded data after SOS */
    size_t   scan_len;
} jpeg_info_t;

/* Parses a baseline JPEG. Returns 0 on success, -1 if the frame cannot be
 * carried by RFC 2435 (progressive, 12-bit, too large, odd sampling...). */
int jpeg_parse(const uint8_t *data, size_t len, jpeg_info_t *out);

typedef struct {
    uint16_t seq;
    uint32_t ssrc;
} rtp_state_t;

/* Called for each packet. pkt points at RTP_PREFIX bytes of free space
 * followed by the RTP packet of length len (prefix not included). */
typedef int (*rtp_send_fn)(void *ctx, uint8_t *pkt, size_t len);

/* Packetizes one JPEG frame. Returns 0, or -1 if parsing or sending failed. */
int rtp_jpeg_send_frame(rtp_state_t *st, const uint8_t *jpeg, size_t len,
                        uint32_t rtp_ts, rtp_send_fn send, void *ctx);

#endif
