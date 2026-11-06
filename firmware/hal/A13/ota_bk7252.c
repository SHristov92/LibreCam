/*
 * Firmware update for the BK7252N HAL.
 *
 * The bootloader (Beken bootloader_bk7252n 1.0.14, the stock one) embeds
 * RT-Thread OTA: at boot it looks for an RT-Thread "RBL" package in the FAL
 * "download" partition (raw flash 0x132000, 0xAE000 bytes), decrypts it
 * (AES-256-CBC, the SDK's default key), inflates it (gzip) into the app
 * partition, verifies it and erases the package. Its strings say it refuses
 * unencrypted packages and CRC-wrapped images, and the package's app must be
 * linked at 0x10000 - all of which build.sh's packaging satisfies.
 *
 * We only have to put a valid package there. The 96-byte header goes in
 * last, after the body's CRC32 was checked from a flash read-back, so a
 * partial or corrupt upload never looks valid to the bootloader.
 */
#include "lc/hal.h"

#include "include.h"
#include "drv_model_pub.h"
#include "flash_pub.h"
#include "BkDriverFlash.h"

#include <stdio.h>
#include <string.h>

#define DL_ADDR     0x132000u
#define DL_SIZE     0xAE000u
#define SECTOR      0x1000u
#define HDR_SIZE    96u

/* struct rt_ota_rbl_hdr (RT-Thread OTA 0.2), little endian */
#define HDR_MAGIC        0          /* "RBL\0" */
#define HDR_ALGO         4          /* low byte: crypt, next: compression */
#define HDR_NAME         12         /* char[16], partition: "app" */
#define HDR_VERSION      28         /* char[24] */
#define HDR_CRC_BODY     76
#define HDR_SIZE_RAW     84
#define HDR_SIZE_PKG     88
#define HDR_CRC_HDR      92         /* crc32 of bytes 0..91 */

static DD_HANDLE g_fl = DD_HANDLE_UNVALID;
static uint8_t g_hdr[HDR_SIZE];
static uint32_t g_size, g_pos, g_erased_to;
static int g_active;

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    uint32_t i, j, c;

    if (!table[1]) {
        for (i = 0; i < 256; i++) {
            for (c = i, j = 0; j < 8; j++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void flash_open(void)
{
    UINT32 st;

    if (g_fl == DD_HANDLE_UNVALID)
        g_fl = ddev_open(FLASH_DEV_NAME, &st, 0);
}

static void erase_sector(uint32_t addr)
{
    ddev_control(g_fl, CMD_FLASH_ERASE_SECTOR, &addr);
}

/* Erases sectors so that [DL_ADDR, DL_ADDR + end) is writable. */
static void erase_to(uint32_t end)
{
    while (g_erased_to < end) {
        erase_sector(DL_ADDR + g_erased_to);
        g_erased_to += SECTOR;
    }
}

const char *hal_ota_format(void)
{
    return "rbl";
}

int hal_ota_begin(uint32_t size, const char **err)
{
    if (size <= HDR_SIZE || size > DL_SIZE) {
        *err = size > DL_SIZE ? "package larger than the update partition" : "file too small";
        return -1;
    }
    flash_open();
    if (g_fl == DD_HANDLE_UNVALID) {
        *err = "flash unavailable";
        return -1;
    }
    g_size = size;
    g_pos = 0;
    g_erased_to = 0;
    g_active = 1;
    bk_flash_enable_security(FLASH_PROTECT_NONE);
    /* The first sector holds the header: erased now, written at finish. */
    erase_to(SECTOR);
    printf("[ota] receiving %u bytes\n", (unsigned)size);
    return 0;
}

static int check_header(const char **err)
{
    if (memcmp(g_hdr + HDR_MAGIC, "RBL", 4)) {
        *err = "not an RBL update package";
        return -1;
    }
    if (crc32_update(0, g_hdr, HDR_CRC_HDR) != rd32(g_hdr + HDR_CRC_HDR)) {
        *err = "package header is corrupt";
        return -1;
    }
    if (strncmp((const char *)g_hdr + HDR_NAME, "app", 16)) {
        *err = "package is not for the app partition";
        return -1;
    }
    if ((g_hdr[HDR_ALGO] & 0xFF) == 0) {
        *err = "package is not encrypted; the bootloader refuses those";
        return -1;
    }
    if (rd32(g_hdr + HDR_SIZE_PKG) + HDR_SIZE != g_size) {
        *err = "file size does not match the package header";
        return -1;
    }
    if (rd32(g_hdr + HDR_SIZE_RAW) > 0x110000u) {
        *err = "firmware larger than the app partition";
        return -1;
    }
    return 0;
}

int hal_ota_write(const void *data, size_t len, const char **err)
{
    const uint8_t *p = data;

    if (!g_active) {
        *err = "no update in progress";
        return -1;
    }
    if (g_pos + len > g_size) {
        *err = "more data than announced";
        return -1;
    }
    /* Header bytes are kept in RAM until finish. */
    while (len && g_pos < HDR_SIZE) {
        g_hdr[g_pos++] = *p++;
        len--;
        if (g_pos == HDR_SIZE && check_header(err) != 0) {
            hal_ota_abort();
            return -1;
        }
    }
    if (len) {
        erase_to((g_pos + (uint32_t)len + SECTOR - 1) & ~(SECTOR - 1));
        ddev_write(g_fl, (char *)p, (UINT32)len, DL_ADDR + g_pos);
        g_pos += (uint32_t)len;
    }
    return 0;
}

int hal_ota_finish(const char **err)
{
    uint8_t buf[512];
    uint32_t off, crc = 0;

    if (!g_active || g_pos != g_size) {
        *err = "upload incomplete";
        hal_ota_abort();
        return -1;
    }
    for (off = HDR_SIZE; off < g_size; off += sizeof(buf)) {
        uint32_t n = g_size - off < sizeof(buf) ? g_size - off : sizeof(buf);
        ddev_read(g_fl, (char *)buf, n, DL_ADDR + off);
        crc = crc32_update(crc, buf, n);
    }
    if (crc != rd32(g_hdr + HDR_CRC_BODY)) {
        printf("[ota] body crc %08x, header says %08x\n", (unsigned)crc,
               (unsigned)rd32(g_hdr + HDR_CRC_BODY));
        *err = "flash verify failed (CRC mismatch)";
        hal_ota_abort();
        return -1;
    }
    ddev_write(g_fl, (char *)g_hdr, HDR_SIZE, DL_ADDR);
    ddev_read(g_fl, (char *)buf, HDR_SIZE, DL_ADDR);
    if (memcmp(buf, g_hdr, HDR_SIZE)) {
        *err = "flash verify failed (header)";
        hal_ota_abort();
        return -1;
    }
    bk_flash_enable_security(FLASH_UNPROTECT_LAST_BLOCK);
    g_active = 0;
    printf("[ota] package %.24s verified; the bootloader installs it on restart\n",
           (const char *)g_hdr + HDR_VERSION);
    return 0;
}

void hal_ota_abort(void)
{
    if (g_fl != DD_HANDLE_UNVALID) {
        bk_flash_enable_security(FLASH_PROTECT_NONE);
        erase_sector(DL_ADDR);          /* no header = nothing to install */
        bk_flash_enable_security(FLASH_UNPROTECT_LAST_BLOCK);
    }
    g_active = 0;
}
