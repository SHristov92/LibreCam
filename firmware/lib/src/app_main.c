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
#include "lc/image.h"
#include "lc/led.h"
#include "lc/recorder.h"
#include "lc/servers.h"

#include <stdio.h>

#define STA_CONNECT_TIMEOUT_MS 20000
#define STA_LOST_FALLBACK_MS   60000
#define SLEEP_HOLD_MS          3000    /* power button: release after 3 s = deep sleep */
#define FACTORY_RESET_HOLD_MS  10000   /* release after 10 s = factory reset */
#define UI_TICK_MS             25
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

/* ---- Power button and status LED ---------------------------------------- */

enum { GESTURE_NONE, GESTURE_SLEEP, GESTURE_RESET };

static void go_to_sleep(void)
{
    printf("[lc] power button: going to deep sleep\n");
    recorder_shutdown(5000);        /* a clip in progress is finished first */
    hal_camera_stop();
    hal_deep_sleep();
}

static led_state_t led_state(uint32_t now, int gesture, uint32_t *t)
{
    uint32_t photo = recorder_photo_time();

    *t = now;
    /* Button feedback shows even with the LED disabled in settings. */
    if (gesture == GESTURE_RESET)
        return LED_RESET_ARMED;
    if (gesture == GESTURE_SLEEP)
        return LED_SLEEP_ARMED;
    if (!g_config.led)
        return LED_OFF;
    if (photo && now - photo < led_oneshot_ms(LED_PHOTO)) {
        *t = now - photo;
        return LED_PHOTO;
    }
    if (recorder_is_recording())
        return LED_RECORDING;
    if (lc_low_battery())
        return LED_LOW_BATTERY;
    switch (hal_wifi_state()) {
    case HAL_WIFI_AP:            return LED_AP;
    case HAL_WIFI_STA_CONNECTED: return LED_CONNECTED;
    default:                     return LED_CONNECTING;
    }
}

/* Power button: hold 3 s (LED goes dark) and release to sleep; hold 10 s
 * (LED flickers) and release to factory reset. A press still held from
 * waking the camera is ignored until the button is released once. */
static void ui_task(void *arg)
{
    uint32_t pressed_since = 0, t;
    int seen_up = 0, gesture = GESTURE_NONE;

    (void)arg;
    for (;;) {
        uint32_t now = hal_millis();
        int pressed = hal_button_pressed();

        if (!pressed)
            seen_up = 1;
        if (pressed && seen_up) {
            uint32_t held;

            if (!pressed_since)
                pressed_since = now ? now : 1;
            held = now - pressed_since;
            gesture = held >= FACTORY_RESET_HOLD_MS ? GESTURE_RESET :
                      held >= SLEEP_HOLD_MS ? GESTURE_SLEEP : GESTURE_NONE;
        } else if (pressed_since) {
            pressed_since = 0;
            if (gesture == GESTURE_RESET) {
                printf("[lc] power button: factory reset\n");
                config_factory_reset();
                g_reboot_requested = 1;
            } else if (gesture == GESTURE_SLEEP) {
                hal_led_set(0);
                go_to_sleep();
            }
            gesture = GESTURE_NONE;
        }
        hal_led_set(led_level(led_state(now, gesture, &t), t));
        hal_sleep_ms(UI_TICK_MS);
    }
}

/* Joins the configured network; returns 0 once an IP is assigned. */
static int start_sta(void)
{
    uint32_t t0 = hal_millis();
    hal_ip_cfg_t ip = { g_config.ip_addr, g_config.ip_mask, g_config.ip_gw,
                        g_config.ip_dns[0] ? g_config.ip_dns : g_config.ip_gw };

    printf("[lc] joining \"%s\"%s%s\n", g_config.sta_ssid,
           g_config.ip_static ? " as " : "", g_config.ip_static ? g_config.ip_addr : "");
    if (hal_wifi_start_sta(g_config.sta_ssid, g_config.sta_pass,
                           g_config.ip_static ? &ip : NULL) != 0)
        return -1;
    while (hal_millis() - t0 < STA_CONNECT_TIMEOUT_MS) {
        hal_wifi_state_t st = hal_wifi_state();
        if (st == HAL_WIFI_STA_CONNECTED)
            return 0;
        if (st == HAL_WIFI_STA_FAILED)
            break;
        hal_sleep_ms(50);
    }
    hal_wifi_stop();
    return -1;
}

void lc_main(void)
{
    uint32_t lost_since = 0, vbat_at = 0;
    char ip[16];

    printf("[lc] firmware %s, board %s (%s)\n", LC_FW_VERSION, BOARD_NAME, BOARD_MCU);
    g_vbat_mv = hal_battery_mv();
    printf("[lc] battery %d mV%s\n", g_vbat_mv, lc_low_battery() ? " (low)" : "");
    config_load();
    lc_time_set_tz(g_config.tz_min);
    hal_task_start("ui", ui_task, NULL, 3072, 6);

    if (!g_config.sta_ssid[0] || start_sta() != 0)
        start_ap();
    hal_wifi_ip(ip, sizeof(ip));
    printf("[lc] ip %s\n", ip);

    if (hal_camera_start(&g_config.camera) != 0)
        printf("[lc] camera init failed; web UI still available\n");
    else
        image_apply_all(&g_config);
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
