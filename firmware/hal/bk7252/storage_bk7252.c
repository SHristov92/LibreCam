/*
 * SD card storage for the BK7252N HAL: our FatFs copy (third_party/fatfs,
 * symbols prefixed lcff_) on the SDK's SDIO block driver. The card detect
 * switch is on BOARD_SD_DETECT_PIN (0 = card inserted); the card and its IO
 * run from the IO LDO that hal_init() turns on.
 *
 * FatFs is built non-reentrant, so every call here takes g_fs_lock. hal_dir_list
 * callbacks run with the lock held and must not call back into storage.
 */
#include "lc/board.h"
#include "lc/clock.h"
#include "lc/hal.h"

#include "include.h"
#include "rtos_pub.h"
#include "mem_pub.h"
#include "gpio_pub.h"
#include "sd_card.h"

#include "../../third_party/fatfs/ff.h"      /* not the SDK's ff.h */
#include "../../third_party/fatfs/diskio.h"

#include <stdio.h>
#include <string.h>

struct hal_file {
    FIL fil;
};

static FATFS g_fs;
static beken_mutex_t g_fs_lock;
static int g_lock_ready, g_mounted, g_sd_inited;
static uint8_t g_bounce[512] __attribute__((aligned(4)));

static void lock(void)
{
    if (!g_lock_ready) {
        rtos_init_mutex(&g_fs_lock);
        g_lock_ready = 1;
    }
    rtos_lock_mutex(&g_fs_lock);
}

static void unlock(void)
{
    rtos_unlock_mutex(&g_fs_lock);
}

/* ---- FatFs disk glue -------------------------------------------------- */

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv == 0 && g_sd_inited ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0)
        return STA_NOINIT;
    if (!g_sd_inited && bk_sd_card_init() == BK_OK)
        g_sd_inited = 1;
    return g_sd_inited ? 0 : STA_NOINIT;
}

/* The SDIO DMA wants word-aligned buffers; FatFs may hand us any address. */
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !g_sd_inited)
        return RES_NOTRDY;
    if (((uintptr_t)buff & 3) == 0)
        return bk_sd_card_read_blocks(buff, (uint32_t)sector, count) == BK_OK ? RES_OK : RES_ERROR;
    for (; count; count--, sector++, buff += 512) {
        if (bk_sd_card_read_blocks(g_bounce, (uint32_t)sector, 1) != BK_OK)
            return RES_ERROR;
        memcpy(buff, g_bounce, 512);
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !g_sd_inited)
        return RES_NOTRDY;
    if (((uintptr_t)buff & 3) == 0)
        return bk_sd_card_write_blocks(buff, (uint32_t)sector, count) == BK_OK ? RES_OK : RES_ERROR;
    for (; count; count--, sector++, buff += 512) {
        memcpy(g_bounce, buff, 512);
        if (bk_sd_card_write_blocks(g_bounce, (uint32_t)sector, 1) != BK_OK)
            return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0 || !g_sd_inited)
        return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        return bk_sd_card_rw_sync() == BK_OK ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = bk_sd_card_get_card_size();
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

DWORD get_fattime(void)
{
    return lc_fattime();
}

/* ---- HAL ---------------------------------------------------------------- */

static int card_present(void)
{
#ifdef BOARD_SD_DETECT_PIN
    static int cfg;

    if (!cfg) {
        bk_gpio_config_input_pup(BOARD_SD_DETECT_PIN);
        cfg = 1;
    }
    return bk_gpio_input(BOARD_SD_DETECT_PIN) == 0;
#else
    return 1;
#endif
}

int hal_storage_poll(void)
{
    int present = card_present();

    lock();
    if (present && !g_mounted) {
        FRESULT r = f_mount(&g_fs, "", 1);

        if (r == FR_OK) {
            g_mounted = 1;
            printf("[sd] card mounted\n");
        } else {
            static int warned;
            if (!warned++)
                printf("[sd] mount failed (FatFs error %d)\n", (int)r);
            if (g_sd_inited) {
                bk_sd_card_deinit();
                g_sd_inited = 0;
            }
        }
    } else if (!present && g_mounted) {
        f_mount(NULL, "", 0);
        if (g_sd_inited)
            bk_sd_card_deinit();
        g_sd_inited = 0;
        g_mounted = 0;
        printf("[sd] card removed\n");
    }
    present = g_mounted;
    unlock();
    return present;
}

int hal_storage_space(uint64_t *total, uint64_t *free_bytes)
{
    FATFS *fs;
    DWORD nfree;
    int ret = -1;

    lock();
    if (g_mounted && f_getfree("", &nfree, &fs) == FR_OK) {
        *total = (uint64_t)(fs->n_fatent - 2) * fs->csize * 512u;
        *free_bytes = (uint64_t)nfree * fs->csize * 512u;
        ret = 0;
    }
    unlock();
    return ret;
}

hal_file_t *hal_file_open(const char *path, char mode)
{
    hal_file_t *f = os_malloc(sizeof(*f));
    BYTE m = mode == 'w' ? (FA_WRITE | FA_CREATE_ALWAYS) : FA_READ;

    if (!f)
        return NULL;
    lock();
    if (!g_mounted || f_open(&f->fil, path, m) != FR_OK) {
        os_free(f);
        f = NULL;
    }
    unlock();
    return f;
}

int hal_file_read(hal_file_t *f, void *buf, size_t len)
{
    UINT n = 0;
    FRESULT r;

    lock();
    r = f_read(&f->fil, buf, (UINT)len, &n);
    unlock();
    return r == FR_OK ? (int)n : -1;
}

int hal_file_write(hal_file_t *f, const void *buf, size_t len)
{
    UINT n = 0;
    FRESULT r;

    lock();
    r = f_write(&f->fil, buf, (UINT)len, &n);
    unlock();
    return r == FR_OK && n == len ? (int)n : -1;
}

int hal_file_seek(hal_file_t *f, uint32_t pos)
{
    FRESULT r;

    lock();
    r = f_lseek(&f->fil, pos);
    unlock();
    return r == FR_OK ? 0 : -1;
}

uint32_t hal_file_size(hal_file_t *f)
{
    return (uint32_t)f_size(&f->fil);
}

int hal_file_close(hal_file_t *f)
{
    FRESULT r;

    lock();
    r = f_close(&f->fil);
    unlock();
    os_free(f);
    return r == FR_OK ? 0 : -1;
}

int hal_file_remove(const char *path)
{
    FRESULT r;

    lock();
    r = g_mounted ? f_unlink(path) : FR_NOT_READY;
    unlock();
    return r == FR_OK ? 0 : -1;
}

int hal_mkdir(const char *path)
{
    FRESULT r;

    lock();
    r = g_mounted ? f_mkdir(path) : FR_NOT_READY;
    unlock();
    return r == FR_OK || r == FR_EXIST ? 0 : -1;
}

int hal_dir_list(const char *path, hal_dir_cb cb, void *ctx)
{
    DIR dir;
    FILINFO fno;
    int ret = -1;

    lock();
    if (g_mounted && f_opendir(&dir, path) == FR_OK) {
        ret = 0;
        while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
            if (cb(fno.fname, (uint32_t)fno.fsize, (fno.fattrib & AM_DIR) != 0, ctx))
                break;
        }
        f_closedir(&dir);
    }
    unlock();
    return ret;
}
