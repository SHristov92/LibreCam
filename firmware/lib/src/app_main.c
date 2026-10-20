/*
 * Application entry: brings up Wi-Fi (station, falling back to a setup
 * access point), the camera, and the HTTP and RTSP servers, then supervises.
 */
#include "lc/app.h"
#include "lc/audio.h"
#include "lc/board.h"
#include "lc/clock.h"
#include "lc/config.h"
#include "lc/hal.h"
#include "lc/recorder.h"
#include "lc/servers.h"

#include <stdio.h>

#define STA_CONNECT_TIMEOUT_MS 20000
#define STA_LOST_FALLBACK_MS   60000
#define FACTORY_RESET_HOLD_MS  5000
#define VBAT_SAMPLE_MS         10000

volatile int g_vbat_mv = -1;

int lc_low_battery(void)
{
    return BOARD_VBAT_LOW_MV >= 0 && g_vbat_mv >= 0 && g_vbat_mv < BOARD_VBAT_LOW_MV;
}

/* Keeps the clock right while on a network with internet access. */
static void sntp_task(void *arg)
{
    uint32_t synced_at = 0;

    (void)arg;
    for (;;) {
        if (hal_wifi_state() == HAL_WIFI_STA_CONNECTED &&
            (!synced_at || hal_millis() - synced_at > 6u * 3600 * 1000)) {
            if (lc_sntp_sync() == 0) {
                lc_tm_t t;
                synced_at = hal_millis();
                lc_time_local(&t);
                printf("[lc] time %04d-%02d-%02d %02d:%02d:%02d (SNTP)\n",
                       t.year, t.month, t.day, t.hour, t.min, t.sec);
            }
        }
        hal_sleep_ms(synced_at ? 60000 : 10000);
    }
}

static void sample_battery(void)
{
    int was_low = lc_low_battery();

    g_vbat_mv = hal_battery_mv();
    if (lc_low_battery() != was_low)
        printf("[lc] battery %d mV%s\n", g_vbat_mv, lc_low_battery() ? " (low)" : "");
}

static void start_ap(void)
{
    printf("[lc] starting setup AP \"%s\"\n", g_config.ap_ssid);
    hal_wifi_start_ap(g_config.ap_ssid, g_config.ap_pass, g_config.ap_channel);
}

/* Joins the configured network; returns 0 once an IP is assigned. */
static int start_sta(void)
{
    uint32_t t0 = hal_millis();

    printf("[lc] joining \"%s\"\n", g_config.sta_ssid);
    if (hal_wifi_start_sta(g_config.sta_ssid, g_config.sta_pass) != 0)
        return -1;
    while (hal_millis() - t0 < STA_CONNECT_TIMEOUT_MS) {
        hal_wifi_state_t st = hal_wifi_state();
        if (st == HAL_WIFI_STA_CONNECTED)
            return 0;
        if (st == HAL_WIFI_STA_FAILED)
            break;
        hal_led_set(g_config.led && ((hal_millis() / 250) & 1));
        hal_sleep_ms(50);
    }
    hal_wifi_stop();
    return -1;
}

void lc_main(void)
{
    uint32_t pressed_since = 0, lost_since = 0, vbat_at = 0;
    char ip[16];

    printf("[lc] firmware %s, board %s (%s)\n", LC_FW_VERSION, BOARD_NAME, BOARD_MCU);
    g_vbat_mv = hal_battery_mv();
    printf("[lc] battery %d mV%s\n", g_vbat_mv, lc_low_battery() ? " (low)" : "");
    config_load();
    lc_time_set_tz(g_config.tz_min);

    if (!g_config.sta_ssid[0] || start_sta() != 0)
        start_ap();
    hal_wifi_ip(ip, sizeof(ip));
    printf("[lc] ip %s\n", ip);

    if (hal_camera_start(&g_config.camera) != 0)
        printf("[lc] camera init failed; web UI still available\n");
    if (audio_start() != 0)
        printf("[lc] microphone unavailable; streaming video only\n");
    if (recorder_start() != 0)
        printf("[lc] recorder task failed to start\n");
    hal_task_start("sntp", sntp_task, NULL, 3072, 2);
    if (http_server_start(g_config.http_port) != 0)
        printf("[lc] http server failed on port %u\n", g_config.http_port);
    if (rtsp_server_start(g_config.rtsp_port) != 0)
        printf("[lc] rtsp server failed on port %u\n", g_config.rtsp_port);
    printf("[lc] web http://%s:%u/  stream rtsp://%s:%u%s\n", ip, g_config.http_port,
           ip, g_config.rtsp_port, g_config.rtsp_path);

    for (;;) {
        uint32_t now = hal_millis();
        hal_wifi_state_t st = hal_wifi_state();

        if (now - vbat_at >= VBAT_SAMPLE_MS) {
            vbat_at = now;
            sample_battery();
        }

        /* LED: fast blink while recording to SD, slow blink in setup AP
         * mode, solid when on the network; off when disabled in settings. */
        if (!g_config.led)
            hal_led_set(0);
        else if (recorder_is_recording())
            hal_led_set((now / 250) & 1);
        else
            hal_led_set(st == HAL_WIFI_AP ? (now / 1000) & 1 : st == HAL_WIFI_STA_CONNECTED);

        /* Holding the button for 5 s erases the settings. */
        if (hal_button_pressed()) {
            if (!pressed_since)
                pressed_since = now ? now : 1;
            else if (now - pressed_since > FACTORY_RESET_HOLD_MS) {
                printf("[lc] factory reset\n");
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
