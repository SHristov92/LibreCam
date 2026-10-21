/*
 * SD card recording: snapshots (web UI or a short press of the user button)
 * and AVI clips (MJPEG + A-law audio) of config.clip_s seconds (web UI or
 * holding the user button for a second). Files go to /DCIM and /VIDEO; when
 * the card runs low the oldest clips go first, then snapshots.
 */
#ifndef LC_RECORDER_H
#define LC_RECORDER_H

#include <stddef.h>
#include <stdint.h>

#define REC_DIR_PHOTO     "/DCIM"
#define REC_DIR_VIDEO     "/VIDEO"

typedef struct {
    int      mounted;
    uint64_t total_bytes, free_bytes;
    int      recording;
    uint32_t rec_elapsed_s;
    char     last_file[64];     /* most recent file written */
} recorder_status_t;

int  recorder_start(void);
/* Saves the current frame to /DCIM; waits up to 3 s. Returns 0 and the
 * path written, or -1 (no card, no camera, write error). */
int  recorder_snapshot(char *path, size_t cap);
int  recorder_clip_start(void);
void recorder_clip_stop(void);
int  recorder_is_recording(void);
/* hal_millis() when the last photo was saved (0 = none yet). */
uint32_t recorder_photo_time(void);
/* Finishes a clip in progress and stops all SD writes; waits up to
 * timeout_ms. Used before deep sleep. */
void recorder_shutdown(uint32_t timeout_ms);
void recorder_status(recorder_status_t *st);

#endif
