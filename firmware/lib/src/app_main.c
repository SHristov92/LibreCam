/*
 * Application entry: brings up Wi-Fi (station, falling back to a setup
 * access point), the camera, and the HTTP and RTSP servers, then supervises.
 */
#include "a9/app.h"
#include "a9/audio.h"
#include "a9/board.h"
#include "a9/config.h"
#include "a9/hal.h"
#include "a9/servers.h"

#include <stdio.h>

#define STA_CONNECT_TIMEOUT_MS 20000
#define STA_LOST_FALLBACK_MS   60000
#define FACTORY_RESET_HOLD_MS  5000
#define VBAT_SAMPLE_MS         10000

volatile int g_vbat_mv = -1;

int a9_low_battery(void)
{
    return BOARD_VBAT_LOW_MV >= 0 && g_vbat_mv >= 0 && g_vbat_mv < BOARD_VBAT_LOW_MV;
}

static void sample_battery(void)
{
    int was_low = a9_low_battery();

    g_vbat_mv = hal_battery_mv();
    if (a9_low_battery() != was_low)
        printf("[a9] battery %d mV%s\n", g_vbat_mv, a9_low_battery() ? " (low)" : "");
}

static void start_ap(void)
{
    printf("[a9] starting setup AP \"%s\"\n", g_config.ap_ssid);
    hal_wifi_start_ap(g_config.ap_ssid, g_config.ap_pass, g_config.ap_channel);
}

/* Joins the configured network; returns 0 once an IP is assigned. */
static int start_sta(void)
{
    uint32_t t0 = hal_millis();

    printf("[a9] joining \"%s\"\n", g_config.sta_ssid);
    if (hal_wifi_start_sta(g_config.sta_ssid, g_config.sta_pass) != 0)
        return -1;
    while (hal_millis() - t0 < STA_CONNECT_TIMEOUT_MS) {
        hal_wifi_state_t st = hal_wifi_state();
        if (st == HAL_WIFI_STA_CONNECTED)
            return 0;
        if (st == HAL_WIFI_STA_FAILED)
            break;
        hal_led_set((hal_millis() / 250) & 1);
        hal_sleep_ms(50);
    }
    hal_wifi_stop();
    return -1;
}

void a9_main(void)
{
    uint32_t pressed_since = 0, lost_since = 0, vbat_at = 0;
    char ip[16];

    printf("[a9] firmware %s, board %s (%s)\n", A9_FW_VERSION, BOARD_NAME, BOARD_MCU);
    g_vbat_mv = hal_battery_mv();
    printf("[a9] battery %d mV%s\n", g_vbat_mv, a9_low_battery() ? " (low)" : "");
    config_load();

    if (!g_config.sta_ssid[0] || start_sta() != 0)
        start_ap();
    hal_wifi_ip(ip, sizeof(ip));
    printf("[a9] ip %s\n", ip);

    if (hal_camera_start(&g_config.camera) != 0)
        printf("[a9] camera init failed; web UI still available\n");
    if (audio_start() != 0)
        printf("[a9] microphone unavailable; streaming video only\n");
    if (http_server_start(g_config.http_port) != 0)
        printf("[a9] http server failed on port %u\n", g_config.http_port);
    if (rtsp_server_start(g_config.rtsp_port) != 0)
        printf("[a9] rtsp server failed on port %u\n", g_config.rtsp_port);
    printf("[a9] web http://%s:%u/  stream rtsp://%s:%u%s\n", ip, g_config.http_port,
           ip, g_config.rtsp_port, g_config.rtsp_path);

    for (;;) {
        uint32_t now = hal_millis();
        hal_wifi_state_t st = hal_wifi_state();

        if (now - vbat_at >= VBAT_SAMPLE_MS) {
            vbat_at = now;
            sample_battery();
        }

        /* LED: slow blink in setup AP mode, solid when on the network. */
        hal_led_set(st == HAL_WIFI_AP ? (now / 1000) & 1 : st == HAL_WIFI_STA_CONNECTED);

        /* Holding the button for 5 s erases the settings. */
        if (hal_button_pressed()) {
            if (!pressed_since)
                pressed_since = now ? now : 1;
            else if (now - pressed_since > FACTORY_RESET_HOLD_MS) {
                printf("[a9] factory reset\n");
                config_factory_reset();
                g_reboot_requested = 1;
            }
        } else {
            pressed_since = 0;
        }

        /* If the home network disappears for a while, reboot so the
         * boot sequence can fall back to the setup AP. */
        if (g_config.sta_ssid[0] && st != HAL_WIFI_AP && st != HAL_WIFI_STA_CONNECTED) {
            if (!lost_since)
                lost_since = now ? now : 1;
            else if (now - lost_since > STA_LOST_FALLBACK_MS)
                g_reboot_requested = 1;
        } else {
            lost_since = 0;
        }

        if (g_reboot_requested) {
            hal_sleep_ms(500); /* let the HTTP response flush */
            hal_reboot();
        }
        hal_sleep_ms(100);
    }
}
