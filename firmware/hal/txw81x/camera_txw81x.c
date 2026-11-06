/*
 * Camera on the TXW81x: DVP sensor -> VPP -> hardware JPEG, all driven by
 * the SDK (lib/video/dvp). The SDK detects the sensor itself (csi_cfg tries
 * every driver in csi_v2.c: GC0328C, GC0329, SP0A19, SP0828, BF3A03, ...).
 *
 * The JPEG encoder publishes each frame on the SDK's stream framework as a
 * list of fixed-size nodes. A pump task subscribes to the "rtp-jpeg" stream
 * (the one the vendor RTSP server, which we don't run, would read), copies
 * each frame into one of two contiguous buffers and frees the nodes at once,
 * so the encoder never runs out of node memory while a slow client reads.
 *
 * TODO(txw81x): resolution/quality from hal_camera_cfg_t, image controls,
 * flip. The SDK sizes the output from the sensor driver and adapts the JPEG
 * quantization to the node memory on its own.
 */
#include "lc/board.h"
#include "lc/hal.h"

#include "sys_config.h"
#include "typesdef.h"
#include "tx_platform.h"
#include "list.h"
#include "dev.h"
#include "devid.h"
#include "osal/string.h"
#include "osal/sleep.h"
#include "osal/task.h"
#include "osal/time.h"
#include "osal/mutex.h"
#include "hal/gpio.h"
#include "chip/txw81x/pmu.h"
#include "custom_mem/custom_mem.h"
#include "stream_frame.h"
#include "stream_define.h"
#include "jpgdef.h"
#include "lib/video/dvp/jpeg/jpg.h"
#include "lib/video/dvp/cmos_sensor/csi.h"
#include "utlist.h"
#include "project_config.h"

#include <stdio.h>
#include <string.h>

/* Node memory for the JPEG encoder (custom_mem). The OpenBeken port uses
 * 60 KB; a VGA frame at the SDK's adaptive quality fits comfortably. */
#ifndef BOARD_JPEG_NODE_MEM
#define BOARD_JPEG_NODE_MEM (60 * 1024)
#endif

extern SNSER snser;
bool csi_cfg(void);
bool csi_open(void);

static struct os_mutex g_lock;
static uint8_t *g_buf[2];
static uint32_t g_len[2], g_ts[2];
static volatile uint32_t g_seq;
static volatile int g_cur = -1;        /* buffer holding the newest frame */
static int g_started;
static uint16_t g_w, g_h;

/* Powers the sensor rail (VCAM) as the vendor demo does. */
static int vcam_on(void)
{
#if VCAM_EN
    pmu_vcam_dis();
    os_sleep_ms(1);
    pmu_set_vcam_vol(VCAM_VOL_2V80);
    pmu_vcam_lc_en();
    pmu_vcam_oc_detect_dis();
    pmu_vcam_oc_int_dis();
    pmu_vcam_discharge_dis();
    pmu_vcam_pg_dis();
    pmu_vcam_en();
    os_sleep_ms(1);
    pmu_vcam_lc_dis();
    pmu_vcam_oc_detect_en();
    if (PMU_VCAM_OC_PENDING)
        return -1;
    pmu_vcam_oc_detect_dis();
    pmu_vcam_oc_int_dis();
    pmu_lvd_oe_en();
#endif
    return 0;
}

static int stream_op(stream *s, void *priv, int opcode)
{
    (void)priv;
    if (opcode == STREAM_OPEN_EXIT)
        enable_stream(s, 1);
    return 0;
}

/* Reads width and height from the SOF0 marker. */
static void jpeg_size(const uint8_t *p, uint32_t len)
{
    uint32_t i = 2;

    while (i + 9 < len && p[i] == 0xFF) {
        uint8_t m = p[i + 1];
        uint32_t seg = (uint32_t)p[i + 2] << 8 | p[i + 3];

        if (m == 0xC0) {
            g_h = (uint16_t)(p[i + 5] << 8 | p[i + 6]);
            g_w = (uint16_t)(p[i + 7] << 8 | p[i + 8]);
            return;
        }
        if (m == 0xDA)
            return;
        i += 2 + seg;
    }
}

/* Copies one frame's nodes into dst. Returns its length, or 0 if it doesn't
 * fit. */
static uint32_t copy_frame(struct data_structure *f, uint8_t *dst, uint32_t cap)
{
    struct stream_jpeg_data_s *head = (struct stream_jpeg_data_s *)GET_DATA_BUF(f);
    struct stream_jpeg_data_s *el, *tmp;
    uint32_t total = GET_DATA_LEN(f), node_len = GET_NODE_LEN(f), off = 0;

    if (total > cap)
        return 0;
    LL_FOREACH_SAFE(head, el, tmp) {
        uint32_t n;

        if (el == head)                /* the list head carries no data */
            continue;
        n = total - off < node_len ? total - off : node_len;
        if (!n)
            break;
        memcpy(dst + off, (uint8_t *)GET_JPG_SELF_BUF(f, el->data), n);
        off += n;
    }
    return off;
}

static void pump_task(void *arg)
{
    stream *s;

    (void)arg;
    s = open_stream(R_RTP_JPEG, 0, 2, stream_op, NULL);
    if (!s) {
        printf("[cam] can't subscribe to the JPEG stream\n");
        return;
    }
    for (;;) {
        struct data_structure *f = GET_FRAME(s);
        int next;
        uint32_t len, total;

        if (!f) {
            os_sleep_ms(5);
            continue;
        }
        total = GET_DATA_LEN(f);
        next = g_cur == 0 ? 1 : 0;
        os_mutex_lock(&g_lock, osWaitForever);
        len = copy_frame(f, g_buf[next], LC_FRAME_BUF);
        if (len) {
            g_len[next] = len;
            g_ts[next] = hal_millis();
            if (!g_w)
                jpeg_size(g_buf[next], len);
            g_cur = next;
            g_seq++;
        }
        os_mutex_unlock(&g_lock);
        DEL_FRAME(f);
        if (!len)
            printf("[cam] frame of %u bytes dropped (LC_FRAME_BUF %u)\n",
                   (unsigned)total, (unsigned)LC_FRAME_BUF);
    }
}

int hal_camera_start(const hal_camera_cfg_t *cfg)
{
    void *node_mem;

    (void)cfg;
    if (g_started)
        return 0;
    os_mutex_init(&g_lock);
    g_buf[0] = hal_malloc_large(LC_FRAME_BUF);
    g_buf[1] = hal_malloc_large(LC_FRAME_BUF);
    node_mem = os_malloc(BOARD_JPEG_NODE_MEM);
    if (!g_buf[0] || !g_buf[1] || !node_mem) {
        printf("[cam] out of memory\n");
        return -1;
    }
    if (vcam_on() != 0)
        printf("[cam] VCAM over-current\n");
    custom_mem_init(node_mem, BOARD_JPEG_NODE_MEM);
    stream_work_queue_start();
    jpg_cfg(HG_JPG0_DEVID, VPP_DATA0);
    if (!csi_cfg()) {
        printf("[cam] no sensor found\n");
        return -1;
    }
    csi_open();
    if (hal_task_start("cam_pump", pump_task, NULL, 2048, 8) != 0)
        return -1;
    g_started = 1;
    return 0;
}

void hal_camera_info(hal_camera_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->sensor = g_started && snser.p_sensor_cmd ? "DVP sensor" : NULL;
    info->width = g_w;
    info->height = g_h;
    info->resolutions = g_w == 640 ? 1u << HAL_RES_VGA :
                        g_w == 320 ? 1u << HAL_RES_QVGA :
                        g_w == 1280 ? 1u << HAL_RES_HD : 0;
}

/* The SDK has no clean stop for the DVP/JPEG pipeline; leave it running. */
void hal_camera_stop(void)
{
}

uint32_t hal_camera_ctrls(void)
{
#if defined(BOARD_IR_LED_PIN) && BOARD_IR_LED_PIN >= 0
    return 1u << HAL_CTRL_IR;
#else
    return 0;
#endif
}

int hal_camera_set_ctrl(hal_ctrl_t ctrl, int value)
{
#if defined(BOARD_IR_LED_PIN) && BOARD_IR_LED_PIN >= 0
    if (ctrl == HAL_CTRL_IR) {
        gpio_set_val(BOARD_IR_LED_PIN, value ? 1 : 0);
        return 0;
    }
#endif
    (void)ctrl;
    (void)value;
    return -1;
}

int hal_camera_get_frame(uint8_t *buf, size_t cap, uint32_t *last_seq,
                         uint32_t *ts_ms, uint32_t timeout_ms)
{
    uint32_t start = hal_millis();

    if (!g_started)
        return -1;
    while (g_seq == *last_seq || g_cur < 0) {
        if (hal_millis() - start >= timeout_ms)
            return 0;
        os_sleep_ms(5);
    }
    os_mutex_lock(&g_lock, osWaitForever);
    {
        int cur = g_cur;
        uint32_t len = g_len[cur];

        if (len > cap) {
            os_mutex_unlock(&g_lock);
            return -(int)len;
        }
        memcpy(buf, g_buf[cur], len);
        *last_seq = g_seq;
        if (ts_ms)
            *ts_ms = g_ts[cur];
        os_mutex_unlock(&g_lock);
        return (int)len;
    }
}
