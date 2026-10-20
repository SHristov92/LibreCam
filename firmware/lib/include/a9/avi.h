/*
 * Minimal AVI (RIFF) writer: one MJPEG video stream and, optionally, one
 * G.711 A-law audio stream. Chunks are appended as they come; the index is
 * spooled to a side file and the headers are patched with the real frame
 * rate and lengths when the clip is closed.
 */
#ifndef A9_AVI_H
#define A9_AVI_H

#include <stddef.h>
#include <stdint.h>

#include "a9/hal.h"

#define AVI_IDX_BUF 128     /* index entries buffered before one write to the side file */

typedef struct {
    hal_file_t *f, *idx;
    uint8_t     ibuf[AVI_IDX_BUF * 16];
    uint32_t    ibuf_n;
    char        idx_path[64];
    uint32_t    movi_pos;       /* file offset of the 'movi' fourcc */
    uint32_t    pos;            /* current end of file */
    uint32_t    frames, audio_bytes, idx_entries, max_frame;
    uint32_t    t_start, t_last;
    uint16_t    width, height;
    int         audio;
    int         error;
} avi_t;

int  avi_open(avi_t *a, const char *path, uint16_t width, uint16_t height, int audio);
int  avi_add_video(avi_t *a, const uint8_t *jpeg, size_t len, uint32_t ts_ms);
int  avi_add_audio(avi_t *a, const uint8_t *alaw, size_t len);
/* Writes the index, fixes up the headers and closes. Returns 0 on success. */
int  avi_close(avi_t *a);

#endif
