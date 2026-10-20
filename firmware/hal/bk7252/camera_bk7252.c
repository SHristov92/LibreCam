/*
 * Camera for the BK7252N HAL: DVP sensor -> on-chip YUV buffer -> hardware
 * JPEG encoder -> GDMA nodes -> frame buffers.
 *
 * Sensor detection and init follow the stock firmware (docs/device-spec.md
 * section 4): the same probe order, I2C addresses, ID registers and init
 * tables (stock_sensor_tables.h). Encoder and DMA handling mirrors the SDK's
 * camera_intf.c, under our own names so both can be linked.
 */
#include "a9/board.h"
#include "a9/hal.h"

#include "include.h"
#include "arm_arch.h"
#include "rtos_pub.h"
#include "mem_pub.h"
#include "drv_model_pub.h"
#include "general_dma_pub.h"
#include "bk_timer_pub.h"
#include "i2c_pub.h"
#include "jpeg_encoder_pub.h"
#include "gpio_pub.h"
#include "video_transfer.h"

#include <stdio.h>
#include <string.h>

/* Not every extracted table is used (day/night, SIV121). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "stock_sensor_tables.h"
#pragma GCC diagnostic pop

#define NODE_LEN          1472
#define NODE_COUNT        4
#define EOF_TIMER_CHANNEL 5     /* same high-res timer the SDK uses */
#define EOF_TIMER_MS      2
#define I2C_RETRIES       6
#define PROBE_PASSES      3

extern size_t xPortGetFreeHeapSize(void);

typedef const uint8_t reg_pair_t[2];

typedef struct {
    const char *name;
    uint8_t addr;                       /* 7-bit SCCB address */
    uint8_t id_reg, id_val;             /* first ID byte */
    int16_t id2_reg;                    /* second ID byte, -1 if none */
    uint8_t id2_val, id2_mask;
    reg_pair_t *init;
    uint16_t init_n;
    uint8_t vga_only;                   /* sensor table outputs one size */
    uint16_t width, height;             /* that size */
} sensor_t;

#define T(t) (t), (uint16_t)(sizeof(t) / sizeof((t)[0]))

/* Probe order from the stock firmware (0x9af6c). GC2145, GC0328C and GC0311
 * also have size-specific sub-tables, applied in sensor_set_size(). */
static const sensor_t g_sensors[] = {
    { "GC2145",  0x3C, 0xF0, 0x21, 0xF1, 0x45, 0xFF, T(gc2145_init),       0, 640, 480 },
    { "OV7740",  0x21, 0x0A, 0x77, 0x0B, 0x40, 0xFD, T(ov7740_init),       1, 640, 480 },
    { "SP0A39",  0x21, 0x00, 0x0A, 0x01, 0x39, 0xFF, T(sp0a39_init),       1, 640, 480 },
    { "SP0A20",  0x21, 0x02, 0x2B, -1, 0, 0,          T(sp0a20_init),       1, 640, 480 },
    { "SP0A28",  0x3D, 0x02, 0xA2, -1, 0, 0,          T(sp0a28_init),       1, 640, 480 },
    { "SP0828",  0x18, 0x02, 0x0C, -1, 0, 0,          T(sp0828_init),       1, 240, 320 },
    { "SP0A19",  0x21, 0x02, 0xA6, -1, 0, 0,          T(sp0a19_init),       1, 640, 480 },
    { "BF20A6",  0x6E, 0xFC, 0x20, -1, 0, 0,          T(bf20a6_init),       1, 640, 480 },
    { "GC0307",  0x21, 0x00, 0x99, -1, 0, 0,          T(gc0307_init),       1, 640, 480 },
    { "GC0308",  0x21, 0x00, 0x9B, -1, 0, 0,          T(gc0308_init),       1, 640, 480 },
    { "GC0312",  0x21, 0xF0, 0xB3, -1, 0, 0,          T(gc0310_gc0312_init),1, 640, 480 },
    { "GC0310",  0x21, 0xF0, 0xA3, -1, 0, 0,          T(gc0310_gc0312_init),1, 640, 480 },
    { "GC0311",  0x33, 0xF0, 0xBB, -1, 0, 0,          T(gc0311_init),       0, 640, 480 },
    { "GC0328C", 0x21, 0xF0, 0x9D, -1, 0, 0,          T(gc0328c_init),      0, 640, 480 },
    { "GC0309",  0x21, 0x00, 0xA0, -1, 0, 0,          T(gc0309_init),       1, 640, 480 },
    { "GC0329C", 0x31, 0xFB, 0x62, -1, 0, 0,          T(gc0329c_init),      1, 640, 480 },
};

static DD_HANDLE g_i2c = DD_HANDLE_UNVALID, g_ejpeg = DD_HANDLE_UNVALID;
static I2C_OP_ST g_op;
static const sensor_t *g_sensor;

static DJPEG_DESC_ST g_ej;
static uint8_t g_rxbuf[NODE_LEN * NODE_COUNT];

/* Frame buffers: the ISR assembles into g_asm and swaps it with g_latest at
 * end of frame unless a reader is copying g_latest. */
static uint8_t *g_asm, *g_latest;
static volatile uint32_t g_asm_len, g_latest_len, g_latest_ts, g_seq;
static volatile int g_asm_bad, g_readers;
static beken_semaphore_t g_frame_sem;
static volatile uint32_t g_frames, g_dropped;

/* ---- SCCB ------------------------------------------------------------ */

static int sccb_write(uint8_t reg, uint8_t val)
{
    int i;

    g_op.op_addr = reg;
    g_op.addr_width = ADDR_WIDTH_8;
    for (i = 0; i < I2C_RETRIES; i++)
        if (ddev_write(g_i2c, (char *)&val, 1, (UINT32)&g_op) == 0)
            return 0;
    return -1;
}

static int sccb_read(uint8_t reg, uint8_t *val)
{
    int i;

    g_op.op_addr = reg;
    g_op.addr_width = ADDR_WIDTH_8;
    for (i = 0; i < I2C_RETRIES; i++)
        if (ddev_read(g_i2c, (char *)val, 1, (UINT32)&g_op) == 0)
            return 0;
    return -1;
}

static int sccb_write_table(reg_pair_t *t, uint16_t n)
{
    uint16_t i;
    int fails = 0;

    for (i = 0; i < n; i++)
        if (sccb_write(t[i][0], t[i][1]) != 0)
            fails++;
    return fails;
}

/* ---- Sensor ---------------------------------------------------------- */

static const sensor_t *sensor_probe(void)
{
    int pass;
    size_t i;
    uint8_t v;

    for (pass = 0; pass < PROBE_PASSES; pass++) {
        for (i = 0; i < sizeof(g_sensors) / sizeof(g_sensors[0]); i++) {
            const sensor_t *s = &g_sensors[i];

            g_op.salve_id = s->addr;
            if (sccb_read(s->id_reg, &v) != 0 || v != s->id_val)
                continue;
            if (s->id2_reg >= 0 &&
                (sccb_read((uint8_t)s->id2_reg, &v) != 0 || (v & s->id2_mask) != s->id2_val))
                continue;
            return s;
        }
        rtos_delay_milliseconds(100);
    }
    return NULL;
}

/* Applies the stock size sub-tables; returns the size the sensor outputs. */
static hal_resolution_t sensor_set_size(hal_resolution_t want)
{
    const sensor_t *s = g_sensor;

    if (!strcmp(s->name, "GC2145")) {
        if (want == HAL_RES_HD) {
            sccb_write_table(T(gc2145_mode2));
            return HAL_RES_HD;
        }
        sccb_write_table(T(gc2145_mode1));
        return HAL_RES_VGA;
    }
    if (!strcmp(s->name, "GC0328C")) {
        if (want == HAL_RES_QVGA) {
            sccb_write_table(T(gc0328c_qvga));
            return HAL_RES_QVGA;
        }
        sccb_write_table(T(gc0328c_vga));
        return HAL_RES_VGA;
    }
    if (!strcmp(s->name, "GC0311")) {
        if (want == HAL_RES_QVGA) {
            sccb_write_table(T(gc0311_qvga));
            return HAL_RES_QVGA;
        }
        sccb_write_table(T(gc0311_vga));
        return HAL_RES_VGA;
    }
    return HAL_RES_VGA;   /* vga_only sensors: fixed native size */
}

static int sensor_init(void)
{
    int fails;

    g_op.salve_id = g_sensor->addr;
    fails = sccb_write_table(g_sensor->init, g_sensor->init_n);
    /* Post-init writes the stock firmware does (0x9bbb0). */
    if (!strcmp(g_sensor->name, "SP0A39") || !strcmp(g_sensor->name, "SP0828"))
        sccb_write(0xFD, 0x00);
    if (fails)
        printf("[cam] %s: %d of %u init writes failed\n", g_sensor->name, fails,
               (unsigned)g_sensor->init_n);
    return fails > g_sensor->init_n / 4 ? -1 : 0;
}

/* ---- Power and bus checks -------------------------------------------- */

static int pin_read(int pin, int pullup)
{
    if (pullup)
        bk_gpio_config_input_pup(pin);
    else
        bk_gpio_config_input(pin);
    rtos_delay_milliseconds(2);
    return bk_gpio_input(pin) ? 1 : 0;
}

static void cam_power(int on)
{
#ifdef BOARD_CAM_PWR_PIN_A
    bk_gpio_config_output(BOARD_CAM_PWR_PIN_A);
    bk_gpio_output(BOARD_CAM_PWR_PIN_A, on);
#endif
#ifdef BOARD_CAM_PWR_PIN_B
    bk_gpio_config_output(BOARD_CAM_PWR_PIN_B);
    bk_gpio_output(BOARD_CAM_PWR_PIN_B, on);
#endif
    (void)on;
}

/* With the sensor unpowered its ESD diodes hold SCL/SDA low, and the I2C1
 * controller then raises SCL-timeout interrupts without end (the SDK's ISR
 * prints "i" each time and starves the system). Only open I2C on a bus
 * whose lines idle high. */
static int i2c_bus_idle_high(void)
{
#if defined(BOARD_CAM_SCL_PIN) && defined(BOARD_CAM_SDA_PIN)
    int scl = pin_read(BOARD_CAM_SCL_PIN, 1), sda = pin_read(BOARD_CAM_SDA_PIN, 1);

    printf("[cam] I2C idle levels with pull-ups: SCL(GPIO%d)=%d SDA(GPIO%d)=%d\n",
           BOARD_CAM_SCL_PIN, scl, BOARD_CAM_SDA_PIN, sda);
    return scl && sda;
#else
    return 1;
#endif
}

/* ---- Encoder callbacks (interrupt context) --------------------------- */

static void frame_done(void)
{
    uint32_t len = g_asm_len;

    if (!g_asm_bad && len > 4 && g_asm[0] == 0xFF && g_asm[1] == 0xD8 && !g_readers) {
        uint8_t *t;
        uint32_t i;

        /* The encoder appends a 4-byte size after EOI; cut at the last EOI. */
        for (i = len; i >= 2 && i + 16 >= len; i--) {
            if (g_asm[i - 2] == 0xFF && g_asm[i - 1] == 0xD9) {
                len = i;
                break;
            }
        }
        t = g_latest;
        g_latest = g_asm;
        g_asm = t;
        g_latest_len = len;
        g_latest_ts = rtos_get_time();
        g_seq++;
        g_frames++;
        rtos_set_semaphore(&g_frame_sem);
    } else {
        g_dropped++;
    }
    g_asm_len = 0;
    g_asm_bad = 0;
}

static void on_node(void *ptr, UINT32 len, UINT32 is_eof, UINT32 frame_len)
{
    (void)frame_len;
    if (g_asm_len + len <= A9_FRAME_BUF) {
        os_memcpy(g_asm + g_asm_len, ptr, len);
        g_asm_len += len;
    } else {
        g_asm_bad = 1;
    }
    if (is_eof)
        frame_done();
}

static void on_dma_rx(UINT32 dma)
{
    UINT16 read = g_ej.rx_read_len;

    (void)dma;
    if (g_ejpeg == DD_HANDLE_UNVALID)
        return;
    on_node(g_ej.rxbuf + read, g_ej.node_len, 0, 0);
    read += g_ej.node_len;
    if (read >= g_ej.rxbuf_len)
        read = 0;
    g_ej.rx_read_len = read;
}

/* End-of-frame IRQ fires before DMA has flushed the last partial node, so
 * like the SDK we wait EOF_TIMER_MS, then drain it and restart DMA. */
static void on_eof_timer(UINT8 param)
{
    GDMA_CFG_ST en;
    UINT16 read = g_ej.rx_read_len;
    UINT32 ch = g_ej.dma_channel;
    int left, got;
    UINT32 frame_len;

    (void)param;
    if (g_ejpeg == DD_HANDLE_UNVALID)
        return;
    left = sddev_control(GDMA_DEV_NAME, CMD_GDMA_GET_LEFT_LEN, (void *)ch);
    got = (int)g_ej.node_len - left;
    frame_len = ddev_control(g_ejpeg, EJPEG_CMD_GET_FRAME_LEN, NULL);
    if (got > 0)
        on_node(g_ej.rxbuf + read, (UINT32)got, 1, frame_len);
    else
        frame_done();

    en.channel = ch;
    en.param = 0;
    sddev_control(GDMA_DEV_NAME, CMD_GDMA_SET_DMA_ENABLE, &en);
    g_ej.rx_read_len = 0;
    en.param = 1;
    sddev_control(GDMA_DEV_NAME, CMD_GDMA_SET_DMA_ENABLE, &en);

    ch = EOF_TIMER_CHANNEL;
    sddev_control(TIMER_DEV_NAME, CMD_TIMER_UNIT_DISABLE, &ch);
}

static void on_end_frame(void)
{
    timer_param_t p;

    if (g_ejpeg == DD_HANDLE_UNVALID)
        return;
    p.channel = EOF_TIMER_CHANNEL;
    p.div = 1;
    p.period = EOF_TIMER_MS;
    p.t_Int_Handler = on_eof_timer;
    sddev_control(TIMER_DEV_NAME, CMD_TIMER_INIT_PARAM, &p);
}

/* ---- HAL ------------------------------------------------------------- */

static uint32_t fps_type(uint8_t fps)
{
    return fps <= 5 ? TYPE_5FPS : fps <= 10 ? TYPE_10FPS : fps <= 20 ? TYPE_20FPS : TYPE_30FPS;
}

int hal_camera_start(const hal_camera_cfg_t *cfg)
{
    UINT32 st, io_out = 1, ppi;
    hal_resolution_t res;
    uint16_t w, h;

    if (g_ejpeg != DD_HANDLE_UNVALID)
        return 0;
    if (!g_asm) {
        g_asm = os_malloc(A9_FRAME_BUF);
        g_latest = os_malloc(A9_FRAME_BUF);
        if (!g_asm || !g_latest || rtos_init_semaphore(&g_frame_sem, 1) != kNoErr) {
            printf("[cam] out of memory for frame buffers\n");
            return -1;
        }
    }

    /* Log the raw pin levels once: they tell us how the board straps them. */
    printf("[cam] pins before power-up (no pull): GPIO20=%d GPIO21=%d GPIO22=%d GPIO23=%d\n",
           pin_read(20, 0), pin_read(21, 0), pin_read(22, 0), pin_read(23, 0));
    cam_power(1);
    rtos_delay_milliseconds(50);

    /* Power the JPEG/DVP block and start MCLK so the sensor answers on SCCB. */
    camera_power_on();
    rtos_delay_milliseconds(20);

    if (!i2c_bus_idle_high()) {
        printf("[cam] I2C1 lines held low: sensor not powered or not on GPIO20/21; "
               "camera disabled\n");
        return -1;
    }

    g_i2c = ddev_open(I2C1_DEV_NAME, &st, 0);
    if (g_i2c == DD_HANDLE_UNVALID) {
        printf("[cam] i2c1 open failed\n");
        return -1;
    }
    ddev_control(g_i2c, I2C1_CMD_SET_IO_OUTPUT, &io_out);

    g_sensor = sensor_probe();
    if (!g_sensor) {
        printf("[cam] no known sensor answered on I2C1\n");
        goto fail;
    }
    printf("[cam] sensor %s at 0x%02x\n", g_sensor->name, g_sensor->addr);
    if (sensor_init() != 0)
        goto fail;
    res = sensor_set_size(cfg->resolution);
    if (g_sensor->vga_only) {
        w = g_sensor->width;
        h = g_sensor->height;
    } else {
        w = res == HAL_RES_HD ? 1280 : res == HAL_RES_QVGA ? 320 : 640;
        h = res == HAL_RES_HD ? 720 : res == HAL_RES_QVGA ? 240 : 480;
    }
    ppi = res == HAL_RES_HD ? VGA_1280_720 : res == HAL_RES_QVGA ? QVGA_320_240 : VGA_640_480;
    if (res != cfg->resolution)
        printf("[cam] %s can't do the requested size, using %ux%u\n", g_sensor->name, w, h);

    memset(&g_ej, 0, sizeof(g_ej));
    g_ej.rxbuf = g_rxbuf;
    g_ej.rxbuf_len = sizeof(g_rxbuf);
    g_ej.node_len = NODE_LEN;
    CMPARAM_SET_PPI(g_ej.sener_cfg, ppi);
    CMPARAM_SET_FPS(g_ej.sener_cfg, fps_type(cfg->fps));
    g_ej.x_pixel = w / 8;
    g_ej.y_pixel = h / 8;
    g_ej.node_full_handler = on_node;
    g_ej.end_frame_handler = on_end_frame;
    g_ej.dma_rx_handler = on_dma_rx;
    g_ej.dma_channel = GDMA_CHANNEL_4;
    g_asm_len = 0;
    g_asm_bad = 1;   /* the first frame may start mid-stream */

    g_ejpeg = ddev_open(EJPEG_DEV_NAME, &st, (UINT32)&g_ej);
    if (g_ejpeg == DD_HANDLE_UNVALID) {
        printf("[cam] jpeg encoder open failed\n");
        goto fail;
    }
    printf("[cam] streaming %ux%u JPEG, free heap %u bytes\n", w, h,
           (unsigned)xPortGetFreeHeapSize());
    return 0;

fail:
    ddev_close(g_i2c);
    g_i2c = DD_HANDLE_UNVALID;
    return -1;
}

void hal_camera_stop(void)
{
    DD_HANDLE j = g_ejpeg;
    GLOBAL_INT_DECLARATION();

    if (j == DD_HANDLE_UNVALID)
        return;
    GLOBAL_INT_DISABLE();
    g_ejpeg = DD_HANDLE_UNVALID;
    GLOBAL_INT_RESTORE();
    ddev_close(j);
    ddev_close(g_i2c);
    g_i2c = DD_HANDLE_UNVALID;
}

int hal_camera_get_frame(uint8_t *buf, size_t cap, uint32_t *last_seq,
                         uint32_t *ts_ms, uint32_t timeout_ms)
{
    uint32_t t0 = rtos_get_time();
    GLOBAL_INT_DECLARATION();

    if (!g_latest)
        return -1;
    for (;;) {
        uint32_t seq, len, ts, waited;

        GLOBAL_INT_DISABLE();
        seq = g_seq;
        len = g_latest_len;
        ts = g_latest_ts;
        if (seq != *last_seq && len)
            g_readers++;
        GLOBAL_INT_RESTORE();

        if (seq != *last_seq && len) {
            int ret;

            if (len > cap) {
                ret = -(int)len;
            } else {
                memcpy(buf, g_latest, len);
                *last_seq = seq;
                *ts_ms = ts;
                ret = (int)len;
            }
            GLOBAL_INT_DISABLE();
            g_readers--;
            GLOBAL_INT_RESTORE();
            return ret;
        }
        waited = rtos_get_time() - t0;
        if (waited >= timeout_ms)
            return 0;
        rtos_get_semaphore(&g_frame_sem, timeout_ms - waited < 20 ? timeout_ms - waited : 20);
    }
}
