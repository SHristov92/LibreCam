/*
 * Camera for the BK7252N HAL: DVP sensor -> on-chip YUV buffer -> hardware
 * JPEG encoder -> GDMA nodes -> frame buffers.
 *
 * Sensor detection and init follow the stock firmwares: every sensor the A13
 * mini's firmware knows (docs/device-spec.md section 4, in its probe order,
 * stock_sensor_tables.h), then the ones only the rear view camera's firmware
 * knows (docs/device-spec-rearview.md section 4, stock_sensor_tables_rearview.h),
 * with their I2C addresses, ID registers and init tables. Boards with
 * BOARD_STOCK_TABLES_REARVIEW use the rear view firmware's tuning for the
 * sensors both know. Encoder and DMA handling mirrors the SDK's
 * camera_intf.c, under our own names so both can be linked.
 */
#include "lc/board.h"
#include "lc/hal.h"

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
#include "uart_pub.h"
#include "video_transfer.h"
#include "wlan_cli_pub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Not every extracted table is used (day/night, SIV121, the other
 * firmware's tuning). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "stock_sensor_tables.h"
#include "stock_sensor_tables_rearview.h"
#pragma GCC diagnostic pop

#define NODE_LEN          1472
#define NODE_COUNT        4
#define EOF_TIMER_CHANNEL 5     /* same high-res timer the SDK uses */
#define EOF_TIMER_MS      2
#define I2C_RETRIES       6
#define PROBE_PASSES      3

/* The sensor's SCCB bus: the hardware I2C1 block (GPIO20/21), or, on
 * boards with BOARD_CAM_SCCB_SOFT, SCCB driven in software on
 * BOARD_CAM_SCL_PIN/BOARD_CAM_SDA_PIN. The rear view camera's sensor is on
 * GPIO0/1, where the SDK only has its interrupt-per-byte I2C2 driver: under
 * our JPEG and Wi-Fi interrupt load it hit SCL timeouts, lost transfers and
 * reported success for transfers it never made (garbled registers, hangs). */
#if defined(BOARD_CAM_SCCB_SOFT) && BOARD_CAM_SCCB_SOFT
#define CAM_SOFT          1
#define CAM_BUS_NAME      "SCCB"
#else
#define CAM_SOFT          0
#define CAM_BUS_NAME      "I2C1"
#endif

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
    uint8_t wake_reg, wake_val;         /* written before the ID read; 0 = none */
    uint8_t reg16;                      /* 16-bit register addresses (HM1055) */
} sensor_t;

#define T(t) (t), (uint16_t)(sizeof(t) / sizeof((t)[0]))

/* Sensors both stock firmwares know: the A13 mini's tuning, or the rear
 * view camera's on boards that ask for it. */
#ifdef BOARD_STOCK_TABLES_REARVIEW
#define TT(a13, rearview) T(rearview)
#else
#define TT(a13, rearview) T(a13)
#endif

/* Probe order from the stock firmware (0x9af6c). GC2145, GC0328C and GC0311
 * also have size-specific sub-tables, applied in sensor_set_size(). */
static const sensor_t g_sensors[] = {
    { "GC2145",  0x3C, 0xF0, 0x21, 0xF1, 0x45, 0xFF, T(gc2145_init),       0, 640, 480 },
    { "OV7740",  0x21, 0x0A, 0x77, 0x0B, 0x40, 0xFD, T(ov7740_init),       1, 640, 480 },
    { "SP0A39",  0x21, 0x00, 0x0A, 0x01, 0x39, 0xFF, T(sp0a39_init),       1, 640, 480 },
    { "SP0A20",  0x21, 0x02, 0x2B, -1, 0, 0,          T(sp0a20_init),       1, 640, 480 },
    { "SP0A28",  0x3D, 0x02, 0xA2, -1, 0, 0,          T(sp0a28_init),       1, 640, 480 },
    { "SP0828",  0x18, 0x02, 0x0C, -1, 0, 0,          T(sp0828_init),       1, 240, 320 },
    { "SP0A19",  0x21, 0x02, 0xA6, -1, 0, 0,          TT(sp0a19_init, sp0a19_rv_init),       1, 640, 480 },
    { "BF20A6",  0x6E, 0xFC, 0x20, -1, 0, 0,          TT(bf20a6_init, bf20a6_rv_init),       1, 640, 480 },
    { "GC0307",  0x21, 0x00, 0x99, -1, 0, 0,          T(gc0307_init),       1, 640, 480 },
    { "GC0308",  0x21, 0x00, 0x9B, -1, 0, 0,          TT(gc0308_init, gc0308_rv_init),       1, 640, 480 },
    { "GC0312",  0x21, 0xF0, 0xB3, -1, 0, 0,          TT(gc0310_gc0312_init, gc0312_rv_init), 1, 640, 480 },
    { "GC0310",  0x21, 0xF0, 0xA3, -1, 0, 0,          T(gc0310_gc0312_init),1, 640, 480 },
    { "GC0311",  0x33, 0xF0, 0xBB, -1, 0, 0,          TT(gc0311_init, gc0311_rv_init),       0, 640, 480 },
    { "GC0328C", 0x21, 0xF0, 0x9D, -1, 0, 0,          TT(gc0328c_init, gc0328c_rv_init),     0, 640, 480 },
    { "GC0309",  0x21, 0x00, 0xA0, -1, 0, 0,          TT(gc0309_init, gc0309_rv_init),       1, 640, 480 },
    { "GC0329C", 0x31, 0xFB, 0x62, -1, 0, 0,          T(gc0329c_init),      1, 640, 480 },
    /* Only in the rear view camera's firmware (its probe 0x45784, init
     * 0x45244). All boot at VGA there. */
    { "GC0329",  0x31, 0x00, 0xC0, -1, 0, 0,          T(gc0329_init),       1, 640, 480, 0xFC, 0x16 },
    { "HI704",   0x3C, 0x04, 0x96, -1, 0, 0,          T(hi704_init),        1, 640, 480 },
    { "OV7670",  0x21, 0x0A, 0x76, -1, 0, 0,          T(ov7670_init),       1, 640, 480 },
    { "OV7673",  0x33, 0x01, 0x0C, -1, 0, 0,          T(ov7673_init),       1, 640, 480 },
    /* The stock firmware loads SP0A19's table for the SP0A38. */
    { "SP0A38",  0x21, 0x01, 0x0A, -1, 0, 0,          T(sp0a19_rv_init),    1, 640, 480 },
    { "SP2518",  0x30, 0x02, 0x53, -1, 0, 0,          T(sp2518_init),       1, 640, 480 },
    { "SC030",   0x68, 0xF8, 0x46, -1, 0, 0,          T(sc030_init),        1, 640, 480 },
    /* PAS6329 and PAS6375 share ID 0x63; stock writes both tables (sensor_init). */
    { "PAS63xx", 0x40, 0x00, 0x63, -1, 0, 0,          T(pas6375_init),      1, 640, 480 },
    { "BF2013",  0x6E, 0x00, 0x63, -1, 0, 0,          T(bf2013_init),       1, 640, 480 },
    { "BF3A03",  0x6E, 0xFC, 0x3A, -1, 0, 0,          T(bf3a03_init),       1, 640, 480 },
    /* 16-bit registers; its table is written in sensor_init(). */
    { "HM1055",  0x24, 0x01, 0x09, -1, 0, 0,          NULL, 0,              1, 640, 480, 0, 0, 1 },
};

#if !CAM_SOFT
static DD_HANDLE g_i2c = DD_HANDLE_UNVALID;
#endif
static DD_HANDLE g_ejpeg = DD_HANDLE_UNVALID;
static int g_bus_up;                    /* SCCB usable (sensor found or probing) */
static I2C_OP_ST g_op;
static beken_mutex_t g_sccb_lock;   /* image controls vs. the camreg command */
static const sensor_t *g_sensor;
static uint16_t g_width, g_height;

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

#if CAM_SOFT
/* Open drain on the GPIO config registers: 0x3C releases the line (input
 * with pull-up, the board's pull-ups hold it high), 0x00 drives it low;
 * bit 0 reads the pin. Slow on purpose; the master sets the pace, so being
 * interrupted mid-transfer only stretches a clock phase. */
#define GPIO_CFG(pin)   ((volatile uint32_t *)(0x0802800 + (pin) * 4))
#define LINE_RELEASE    0x3C
#define LINE_LOW        0x00
#define SCCB_HALF_BIT   200         /* busy-loop iterations: a few us, well under 400 kHz */

static void sccb_delay(void)
{
    volatile int i;

    for (i = 0; i < SCCB_HALF_BIT; i++)
        ;
}

static void scl(int high) { *GPIO_CFG(BOARD_CAM_SCL_PIN) = high ? LINE_RELEASE : LINE_LOW; sccb_delay(); }
static void sda(int high) { *GPIO_CFG(BOARD_CAM_SDA_PIN) = high ? LINE_RELEASE : LINE_LOW; sccb_delay(); }
static int sda_in(void)   { return *GPIO_CFG(BOARD_CAM_SDA_PIN) & 1; }

static void sccb_start(void)
{
    sda(1);
    scl(1);
    sda(0);
    scl(0);
}

static void sccb_stop(void)
{
    sda(0);
    scl(1);
    sda(1);
}

/* Returns 0 if the sensor acknowledged the byte. */
static int sccb_tx(uint8_t b)
{
    int i, nack;

    for (i = 7; i >= 0; i--) {
        sda((b >> i) & 1);
        scl(1);
        scl(0);
    }
    sda(1);
    scl(1);
    nack = sda_in();
    scl(0);
    return nack ? -1 : 0;
}

/* One byte, answered with NACK (SCCB reads are single bytes). */
static uint8_t sccb_rx(void)
{
    uint8_t b = 0;
    int i;

    sda(1);
    for (i = 0; i < 8; i++) {
        scl(1);
        b = (uint8_t)(b << 1 | sda_in());
        scl(0);
    }
    scl(1);
    scl(0);
    return b;
}

/* Register address of 1 or 2 bytes, high byte first. SCCB style: a read
 * is a write of the address, STOP, then a separate read transaction. */
static int sccb_xfer(uint16_t reg, int reg16, uint8_t *val, int write)
{
    uint8_t a = (uint8_t)(g_op.salve_id << 1);
    int r;

    sccb_start();
    r = sccb_tx(a);
    if (!r && reg16)
        r = sccb_tx((uint8_t)(reg >> 8));
    if (!r)
        r = sccb_tx((uint8_t)reg);
    if (!r && write)
        r = sccb_tx(*val);
    sccb_stop();
    if (r || write)
        return r;
    sccb_start();
    r = sccb_tx(a | 1);
    if (!r)
        *val = sccb_rx();
    sccb_stop();
    return r;
}

static int bus_xfer(uint16_t reg, int reg16, uint8_t *val, int write)
{
    int i;

    for (i = 0; i < I2C_RETRIES; i++)
        if (sccb_xfer(reg, reg16, val, write) == 0)
            return 0;
    return -1;
}

static int bus_open(void)
{
    sda(1);
    scl(1);
    sccb_stop();
    g_bus_up = 1;
    return 0;
}

static void bus_close(void)
{
    g_bus_up = 0;
}
#else
static int bus_xfer(uint16_t reg, int reg16, uint8_t *val, int write)
{
    int i;

    g_op.op_addr = reg;
    g_op.addr_width = reg16 ? ADDR_WIDTH_16 : ADDR_WIDTH_8;
    for (i = 0; i < I2C_RETRIES; i++)
        if ((write ? ddev_write(g_i2c, (char *)val, 1, (UINT32)&g_op) :
                     ddev_read(g_i2c, (char *)val, 1, (UINT32)&g_op)) == 0)
            return 0;
    return -1;
}

static int bus_open(void)
{
    UINT32 st, io_out = 1;

    g_i2c = ddev_open(I2C1_DEV_NAME, &st, 0);
    if (g_i2c == DD_HANDLE_UNVALID)
        return -1;
    ddev_control(g_i2c, I2C1_CMD_SET_IO_OUTPUT, &io_out);
    g_bus_up = 1;
    return 0;
}

static void bus_close(void)
{
    g_bus_up = 0;
    ddev_close(g_i2c);
    g_i2c = DD_HANDLE_UNVALID;
}
#endif

static int sccb_write(uint8_t reg, uint8_t val)  { return bus_xfer(reg, 0, &val, 1); }
static int sccb_read(uint8_t reg, uint8_t *val)  { return bus_xfer(reg, 0, val, 0); }
static int sccb_write16(uint16_t reg, uint8_t val) { return bus_xfer(reg, 1, &val, 1); }
static int sccb_read16(uint16_t reg, uint8_t *val) { return bus_xfer(reg, 1, val, 0); }

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
            if (s->wake_reg) {          /* stock writes it twice */
                sccb_write(s->wake_reg, s->wake_val);
                sccb_write(s->wake_reg, s->wake_val);
            }
            if ((s->reg16 ? sccb_read16(s->id_reg, &v) : sccb_read(s->id_reg, &v)) != 0 ||
                v != s->id_val)
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
    /* The init tables already output VGA. Stock firmware writes the VGA crop
     * sub-tables only on a runtime size change, never at init; doing so here
     * shifted the GC0328C image sideways, so only QVGA gets a sub-table. */
    if (!strcmp(s->name, "GC0328C")) {
        if (want == HAL_RES_QVGA) {
            sccb_write_table(TT(gc0328c_qvga, gc0328c_rv_qvga));
            return HAL_RES_QVGA;
        }
        return HAL_RES_VGA;
    }
    if (!strcmp(s->name, "GC0311")) {
        if (want == HAL_RES_QVGA) {
            sccb_write_table(TT(gc0311_qvga, gc0311_rv_qvga));
            return HAL_RES_QVGA;
        }
        return HAL_RES_VGA;
    }
    return HAL_RES_VGA;   /* vga_only sensors: fixed native size */
}

static int sensor_init(void)
{
    int fails;

    g_op.salve_id = g_sensor->addr;
    if (!strcmp(g_sensor->name, "HM1055")) {
        size_t i;

        fails = 0;
        for (i = 0; i < sizeof(hm1055_init) / sizeof(hm1055_init[0]); i++)
            if (sccb_write16(hm1055_init[i].reg, hm1055_init[i].val) != 0)
                fails++;
        if (fails)
            printf("[cam] HM1055: %d of %u init writes failed\n", fails,
                   (unsigned)(sizeof(hm1055_init) / sizeof(hm1055_init[0])));
        return fails > (int)(sizeof(hm1055_init) / sizeof(hm1055_init[0])) / 4 ? -1 : 0;
    }
    /* PAS6329 and PAS6375 report the same ID: the stock firmware writes
     * the PAS6329's three register pages (selected with EF) and then the
     * PAS6375 table, and so do we. Only the PAS6375 table counts below. */
    if (!strcmp(g_sensor->name, "PAS63xx")) {
        sccb_write(0xEF, 0x00);
        sccb_write_table(T(pas6329_page0));
        sccb_write(0xEF, 0x01);
        sccb_write_table(T(pas6329_page1));
        sccb_write(0xEF, 0x02);
        sccb_write_table(T(pas6329_page2));
        sccb_write(0xEF, 0x00);
    }
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

/* With the sensor unpowered its ESD diodes hold SCL/SDA low, and the I2C
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
    if (g_asm_len + len <= LC_FRAME_BUF) {
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

/* ---- Shell: camreg <reg> [value] ------------------------------------- */

/* Reads or writes a register of the detected sensor (hex arguments), for
 * tuning on the running camera, e.g. "camreg fe 0" then "camreg 53 8". */
static void camreg_cmd(char *out, int out_len, int argc, char **argv)
{
    uint8_t reg, val;

    (void)out;
    (void)out_len;
    if (!g_sensor || !g_bus_up) {
        printf("no camera sensor\n");
        return;
    }
    if (argc < 2 || argc > 3) {
        printf("usage: camreg <reg> [value]   (hex, sensor %s at 0x%02x)\n",
               g_sensor->name, g_sensor->addr);
        return;
    }
    reg = (uint8_t)strtoul(argv[1], NULL, 16);
    rtos_lock_mutex(&g_sccb_lock);
    g_op.salve_id = g_sensor->addr;
    if (argc == 3 && sccb_write(reg, (uint8_t)strtoul(argv[2], NULL, 16)) != 0)
        printf("write 0x%02x failed\n", reg);
    else if (sccb_read(reg, &val) != 0)
        printf("read 0x%02x failed\n", reg);
    else
        printf("0x%02x = 0x%02x\n", reg, val);
    rtos_unlock_mutex(&g_sccb_lock);
}

static const struct cli_command g_camreg_cmd = {
    "camreg", "camreg <reg> [value]: read/write camera sensor register (hex)", camreg_cmd
};

/* ---- Image controls --------------------------------------------------- */

/* GalaxyCore GC0328C and GC0311, after GalaxyCore's reference driver: both
 * have the same ISP registers, with different stock values. Registers are
 * on page 0 unless noted; 0xFE selects the page and is put back to 0 after
 * each write. */
#define GC_PAGE        0xFE
#define GC_EXP_H       0x03    /* exposure, sensor lines [11:8] */
#define GC_EXP_L       0x04    /*                        [7:0]  */
#define GC_FLIP        0x17    /* bit0 mirror, bit1 upside down */
#define GC_AWB_EN      0x42    /* bit1: auto white balance */
#define GC_AEC_EN      0x4F    /* bit0: auto exposure */
#define GC_GLOBAL_GAIN 0x70
#define GC_AWB_R       0x77    /* white balance gains while AWB is off */
#define GC_AWB_G       0x78
#define GC_AWB_B       0x79
#define GC_SAT_CB      0xD1    /* stock night mode writes 0 */
#define GC_SAT_CR      0xD2
#define GC_CONTRAST    0xD3
#define GC_LUMA_OFFSET 0xD5    /* brightness, signed */
#define GC1_EXP_STEP   0x29    /* page 1: anti-flicker exposure step [11:0] */
#define GC1_EXP_LEVEL  0x2B    /* page 1: four exposure limits, 2 bytes each */
#define GC1_AEC_TARGET 0x13    /* page 1: target luma */

/* The values in the init table in use, which the controls are relative to:
 * a control at 0 gives the stock picture. The flip control keeps the
 * table's orientation as "no flip". Anti-flicker: lines per mains
 * half-cycle (10 ms at 50 Hz, 8.3 ms at 60 Hz), and the four exposure
 * limits as multiples of the 50 Hz step. Registers a table leaves alone
 * are at the sensor's reset value (D3 = 0x40, D5 = 0). */
typedef struct {
    const char *sensor;
    uint8_t flip, sat_cb, sat_cr, contrast, luma_offset, ae_target, global_gain;
    uint8_t awb[3];                     /* R, G, B gains AWB starts from */
    uint16_t step_50hz, step_60hz;
    uint8_t levels[4];
} gc_params_t;

static const gc_params_t g_gc_params[] = {
#ifdef BOARD_STOCK_TABLES_REARVIEW
    /* gc0328c_rv_init (0xffdf2) */
    { "GC0328C", 0x14, 0x24, 0x24, 0x40, 0x00, 0x55, 0x45, { 0x5A, 0x40, 0x56 },
      131, 109, { 5, 5, 5, 5 } },
    /* gc0311_rv_init (0xfeb80): limits 0x300/0x300/0x3C0/0xA40 */
    { "GC0311",  0x14, 0x28, 0x28, 0x44, 0xFE, 0x4A, 0x50, { 0x70, 0x40, 0x50 },
      96, 80, { 8, 8, 10, 27 } },
#else
    /* gc0328c_init (0x16de6e) */
    { "GC0328C", 0x14, 0x22, 0x21, 0x40, 0x00, 0x45, 0x45, { 0x5A, 0x40, 0x56 },
      131, 109, { 5, 5, 5, 5 } },
    /* gc0311_init (0x16d258): mirrored and flipped; limits 0x2A0/0x300/0x3C0/0x540 */
    { "GC0311",  0x17, 0x28, 0x28, 0x40, 0x00, 0x48, 0x50, { 0x70, 0x40, 0x50 },
      96, 80, { 7, 8, 10, 14 } },
#endif
};

static const gc_params_t *gc_params(void)
{
    size_t i;

    for (i = 0; g_sensor && i < sizeof(g_gc_params) / sizeof(g_gc_params[0]); i++)
        if (!strcmp(g_sensor->name, g_gc_params[i].sensor))
            return &g_gc_params[i];
    return NULL;
}

static struct {
    int ae, exposure, gain;
} g_manual = { 1, 40, 20 };

static int gc_write(uint8_t page, uint8_t reg, uint8_t val)
{
    int r = 0;

    if (page)
        r |= sccb_write(GC_PAGE, page);
    r |= sccb_write(reg, val);
    if (page)
        r |= sccb_write(GC_PAGE, 0);
    return r;
}

static int gc_update(uint8_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t v;

    if (sccb_read(reg, &v) != 0)
        return -1;
    return sccb_write(reg, (uint8_t)((v & ~mask) | (bits & mask)));
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Manual exposure: 1..100 maps to 1..1000 lines, finer at the short end.
 * Manual gain: 0..100 maps to 1x..4x global gain (0x40 = 1x). */
static int gc_write_manual(void)
{
    int lines = clamp(g_manual.exposure * g_manual.exposure / 10, 1, 1000);
    int gain = 0x40 + g_manual.gain * (0xFF - 0x40) / 100;

    return gc_write(0, GC_EXP_H, (uint8_t)(lines >> 8)) |
           gc_write(0, GC_EXP_L, (uint8_t)lines) |
           gc_write(0, GC_GLOBAL_GAIN, (uint8_t)gain);
}

static int gc_set(const gc_params_t *p, hal_ctrl_t ctrl, int v)
{
    static const uint8_t awb[][3] = {        /* R, G, B gains */
        { 0 },                               /* auto: from the stock values */
        { 0x74, 0x52, 0x40 },                /* daylight */
        { 0x8C, 0x50, 0x40 },                /* cloudy */
        { 0x40, 0x42, 0x50 },                /* fluorescent */
        { 0x48, 0x40, 0x5C },                /* incandescent */
    };
    const uint8_t *gains;
    int r, i, step;

    switch (ctrl) {
    case HAL_CTRL_BRIGHTNESS:
        return gc_write(0, GC_LUMA_OFFSET, (uint8_t)(p->luma_offset + clamp(v, -4, 4) * 0x10));
    case HAL_CTRL_CONTRAST:
        return gc_write(0, GC_CONTRAST, (uint8_t)(p->contrast + clamp(v, -4, 4) * 8));
    case HAL_CTRL_SATURATION:
        v = clamp(v, -4, 4) + 4;             /* 0 = grey .. 8 = double */
        return gc_write(0, GC_SAT_CB, (uint8_t)(p->sat_cb * v / 4)) |
               gc_write(0, GC_SAT_CR, (uint8_t)(p->sat_cr * v / 4));
    case HAL_CTRL_AE_LEVEL:
        return gc_write(1, GC1_AEC_TARGET, (uint8_t)(p->ae_target + clamp(v, -4, 4) * 8));
    case HAL_CTRL_AE:
        g_manual.ae = !!v;
        if (g_manual.ae)
            return gc_write(0, GC_GLOBAL_GAIN, p->global_gain) | gc_update(GC_AEC_EN, 0x01, 0x01);
        return gc_update(GC_AEC_EN, 0x01, 0x00) | gc_write_manual();
    case HAL_CTRL_EXPOSURE:
        g_manual.exposure = clamp(v, 1, 100);
        return g_manual.ae ? 0 : gc_write_manual();
    case HAL_CTRL_GAIN:
        g_manual.gain = clamp(v, 0, 100);
        return g_manual.ae ? 0 : gc_write_manual();
    case HAL_CTRL_AWB:
        if (v < HAL_AWB_AUTO || v > HAL_AWB_INCANDESCENT)
            return -1;
        gains = v == HAL_AWB_AUTO ? p->awb : awb[v];
        r = gc_update(GC_AWB_EN, 0x02, 0x00);
        r |= gc_write(0, GC_AWB_R, gains[0]) | gc_write(0, GC_AWB_G, gains[1]) |
             gc_write(0, GC_AWB_B, gains[2]);
        if (v == HAL_AWB_AUTO)
            r |= gc_update(GC_AWB_EN, 0x02, 0x02);
        return r;
    case HAL_CTRL_FLICKER:
        step = v == 60 ? p->step_60hz : p->step_50hz;
        r = sccb_write(GC_PAGE, 1);
        r |= sccb_write(GC1_EXP_STEP, (uint8_t)(step >> 8));
        r |= sccb_write(GC1_EXP_STEP + 1, (uint8_t)step);
        for (i = 0; i < 4; i++) {
            /* The same exposure time in 60 Hz steps: 6 for every 5. */
            int n = v == 60 ? (p->levels[i] * 6 + 2) / 5 : p->levels[i];
            int level = step * n;
            r |= sccb_write((uint8_t)(GC1_EXP_LEVEL + 2 * i), (uint8_t)(level >> 8));
            r |= sccb_write((uint8_t)(GC1_EXP_LEVEL + 2 * i + 1), (uint8_t)level);
        }
        r |= sccb_write(GC_PAGE, 0);
        return r;
    case HAL_CTRL_FLIP:
        return gc_write(0, GC_FLIP, (uint8_t)(p->flip ^ (v & 3)));
    default:
        return -1;
    }
}

uint32_t hal_camera_ctrls(void)
{
    uint32_t c = 0;

    if (gc_params() && g_bus_up)
        c = (1u << HAL_CTRL_IR) - 1;         /* everything up to the IR LEDs */
#ifdef BOARD_IR_LED_PIN
    c |= 1u << HAL_CTRL_IR;
#endif
    return c;
}

int hal_camera_set_ctrl(hal_ctrl_t ctrl, int value)
{
    int r;

    if (!(hal_camera_ctrls() & (1u << ctrl)))
        return -1;
    if (ctrl == HAL_CTRL_IR) {
#ifdef BOARD_IR_LED_PIN
        bk_gpio_output(BOARD_IR_LED_PIN, value ? 1 : 0);
#endif
        return 0;
    }
    rtos_lock_mutex(&g_sccb_lock);
    g_op.salve_id = g_sensor->addr;
    r = gc_set(gc_params(), ctrl, value);
    rtos_unlock_mutex(&g_sccb_lock);
    if (r)
        printf("[cam] control %d = %d: register write failed\n", (int)ctrl, value);
    return r ? -1 : 0;
}

/* ---- HAL ------------------------------------------------------------- */

static uint32_t fps_type(uint8_t fps)
{
    return fps <= 5 ? TYPE_5FPS : fps <= 10 ? TYPE_10FPS : fps <= 20 ? TYPE_20FPS : TYPE_30FPS;
}

int hal_camera_start(const hal_camera_cfg_t *cfg)
{
    UINT32 st, ppi;
    hal_resolution_t res;
    uint16_t w, h;

    if (g_ejpeg != DD_HANDLE_UNVALID)
        return 0;
    if (!g_asm) {
        g_asm = hal_malloc_large(LC_FRAME_BUF);
        g_latest = hal_malloc_large(LC_FRAME_BUF);
        if (!g_asm || !g_latest || rtos_init_semaphore(&g_frame_sem, 1) != kNoErr ||
            rtos_init_mutex(&g_sccb_lock) != kNoErr) {
            printf("[cam] out of memory for frame buffers\n");
            return -1;
        }
    }

#if CAM_SOFT && (BOARD_CAM_SCL_PIN <= 1 || BOARD_CAM_SDA_PIN <= 1)
    /* GPIO0/1 are also UART2, which the SDK opens at boot with its RX
     * interrupt on: SCCB traffic would fill its FIFO ("uart rx fifo full"
     * from the ISR). The console is UART1, so take UART2 down for good. */
    {
        static int uart2_down;

        if (!uart2_down) {
            uart2_exit();
            uart2_down = 1;
        }
    }
#endif
#if defined(BOARD_CAM_SCL_PIN) && defined(BOARD_CAM_SDA_PIN)
    /* Log the raw pin levels once: they tell us how the board straps them. */
    printf("[cam] SCCB pins before power-up (no pull): SCL(GPIO%d)=%d SDA(GPIO%d)=%d\n",
           BOARD_CAM_SCL_PIN, pin_read(BOARD_CAM_SCL_PIN, 0),
           BOARD_CAM_SDA_PIN, pin_read(BOARD_CAM_SDA_PIN, 0));
#endif

    /* Power the JPEG/DVP block and start MCLK so the sensor answers on SCCB. */
    camera_power_on();
    rtos_delay_milliseconds(20);

    if (!i2c_bus_idle_high()) {
        printf("[cam] " CAM_BUS_NAME " lines held low: sensor not powered or not on "
               "this bus; camera disabled\n");
        return -1;
    }

    if (bus_open() != 0) {
        printf("[cam] " CAM_BUS_NAME " open failed\n");
        return -1;
    }

    g_sensor = sensor_probe();
    if (!g_sensor) {
        printf("[cam] no known sensor answered on " CAM_BUS_NAME "\n");
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
    g_width = w;
    g_height = h;
    cli_register_command(&g_camreg_cmd);
    printf("[cam] streaming %ux%u JPEG, free heap %u bytes\n", w, h,
           (unsigned)xPortGetFreeHeapSize());
    return 0;

fail:
    bus_close();
    return -1;
}

void hal_camera_info(hal_camera_info_t *info)
{
    const sensor_t *s = g_sensor;

    info->sensor = s ? s->name : NULL;
    info->width = g_ejpeg != DD_HANDLE_UNVALID ? g_width : 0;
    info->height = g_ejpeg != DD_HANDLE_UNVALID ? g_height : 0;
    if (!s)
        info->resolutions = 0;
    else if (!strcmp(s->name, "GC2145"))
        info->resolutions = (1u << HAL_RES_VGA) | (1u << HAL_RES_HD);
    else if (!strcmp(s->name, "GC0328C") || !strcmp(s->name, "GC0311"))
        info->resolutions = (1u << HAL_RES_QVGA) | (1u << HAL_RES_VGA);
    else if (s->width == 640 && s->height == 480)
        info->resolutions = 1u << HAL_RES_VGA;
    else
        info->resolutions = 0;   /* SP0828: 240x320, none of the standard sizes */
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
    bus_close();
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
