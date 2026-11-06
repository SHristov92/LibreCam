/*
 * Parts of the HAL the TXW81x port doesn't implement yet. Each reports
 * "not available" the way lc/hal.h allows, so the app runs without them:
 * no microphone track in RTSP, no SD card features, no web firmware update.
 *
 * TODO(txw81x), once a board is dumped and on the bench:
 *  - audio: the SDK's audio_adc (sdk/app/audio_app) gives 8 kHz PCM
 *  - SD card: sdhost + the SDK's FatFs (SDH_EN/FS_EN in project_config.h),
 *    or third_party/fatfs on the sdhost block driver as on the BK7252
 *  - OTA: the SDK's libota_write_fw() writes the second image slot; the
 *    package is the "_compress" image from makecode
 */
#include "lc/hal.h"

#include <stddef.h>

int hal_audio_start(uint32_t rate)
{
    (void)rate;
    return -1;
}

int hal_audio_read(int16_t *buf, size_t samples, uint32_t timeout_ms)
{
    (void)buf;
    (void)samples;
    (void)timeout_ms;
    return -1;
}

int hal_storage_poll(void)
{
    return 0;
}

int hal_storage_space(uint64_t *total, uint64_t *free_bytes)
{
    (void)total;
    (void)free_bytes;
    return -1;
}

hal_file_t *hal_file_open(const char *path, char mode)
{
    (void)path;
    (void)mode;
    return NULL;
}

int hal_file_read(hal_file_t *f, void *buf, size_t len)
{
    (void)f;
    (void)buf;
    (void)len;
    return -1;
}

int hal_file_write(hal_file_t *f, const void *buf, size_t len)
{
    (void)f;
    (void)buf;
    (void)len;
    return -1;
}

int hal_file_seek(hal_file_t *f, uint32_t pos)
{
    (void)f;
    (void)pos;
    return -1;
}

uint32_t hal_file_size(hal_file_t *f)
{
    (void)f;
    return 0;
}

int hal_file_close(hal_file_t *f)
{
    (void)f;
    return -1;
}

int hal_file_remove(const char *path)
{
    (void)path;
    return -1;
}

int hal_mkdir(const char *path)
{
    (void)path;
    return -1;
}

int hal_dir_list(const char *path, hal_dir_cb cb, void *ctx)
{
    (void)path;
    (void)cb;
    (void)ctx;
    return -1;
}

const char *hal_ota_format(void)
{
    return NULL;
}

int hal_ota_begin(uint32_t size, const char **err)
{
    (void)size;
    *err = "no firmware update on this board yet";
    return -1;
}

int hal_ota_write(const void *data, size_t len, const char **err)
{
    (void)data;
    (void)len;
    *err = "no firmware update on this board yet";
    return -1;
}

int hal_ota_finish(const char **err)
{
    *err = "no firmware update on this board yet";
    return -1;
}

void hal_ota_abort(void)
{
}
