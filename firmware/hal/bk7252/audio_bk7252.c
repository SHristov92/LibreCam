/*
 * Microphone for the BK7252N HAL: the on-chip audio ADC in mono MIC mode,
 * read from the driver's ring buffer (ISR mode, as the SDK's audio_intf).
 * Stock firmware records at 8 kHz with the ADC volume at its maximum (124).
 */
#include "a9/hal.h"

#include "include.h"
#include "rtos_pub.h"
#include "drv_model_pub.h"
#include "audio_pub.h"

#include <stdio.h>

#define ADC_RING_BYTES  4096   /* 256 ms at 8 kHz */
#define ADC_VOLUME      124

static DD_HANDLE g_adc = DD_HANDLE_UNVALID;
static AUD_ADC_CFG_ST g_cfg;
static uint8_t g_ring[ADC_RING_BYTES];

int hal_audio_start(uint32_t rate)
{
    UINT32 st, vol = ADC_VOLUME;

    if (g_adc != DD_HANDLE_UNVALID)
        return 0;
    g_cfg.buf = g_ring;
    g_cfg.buf_len = sizeof(g_ring);
    g_cfg.freq = (UINT16)rate;
    g_cfg.channels = 1;
    g_cfg.mode = 0;                                    /* ISR mode, microphone */
    g_cfg.linein_detect_pin = AUD_ADC_LINEIN_DETECT_PIN;
    g_adc = ddev_open(AUD_ADC_DEV_NAME, &st, (UINT32)&g_cfg);
    if (g_adc == DD_HANDLE_UNVALID) {
        printf("[mic] aud_adc open failed\n");
        return -1;
    }
    ddev_control(g_adc, AUD_ADC_CMD_SET_VOLUME, &vol);
    ddev_control(g_adc, AUD_ADC_CMD_PLAY, NULL);
    printf("[mic] capturing %u Hz mono\n", (unsigned)rate);
    return 0;
}

int hal_audio_read(int16_t *buf, size_t samples, uint32_t timeout_ms)
{
    uint32_t t0 = rtos_get_time();

    if (g_adc == DD_HANDLE_UNVALID)
        return -1;
    for (;;) {
        UINT32 fill = ddev_control(g_adc, AUD_ADC_CMD_GET_FILL_BUF_SIZE, NULL) & ~1u;

        if (fill) {
            if (fill > samples * 2)
                fill = (UINT32)samples * 2;
            ddev_read(g_adc, (char *)buf, fill, 0);
            return (int)(fill / 2);
        }
        if (rtos_get_time() - t0 >= timeout_ms)
            return 0;
        rtos_delay_milliseconds(5);
    }
}
