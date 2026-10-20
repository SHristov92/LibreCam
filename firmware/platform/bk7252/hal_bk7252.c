/*
 * BK7252 / BK7252N implementation of the HAL, for Beken's RT-Thread based SDK.
 *
 * STATUS: not yet compiled or run on hardware. The RT-Thread parts (tasks,
 * mutexes, ticks, EasyFlash) use standard RT-Thread APIs. The Wi-Fi, camera
 * and GPIO parts are marked TODO: their exact function names differ between
 * SDK drops, so they need to be filled in against the SDK actually used
 * (see platform/bk7252/README.md).
 */
#include "hal.h"
#include "app.h"

#include <rtthread.h>
#include <string.h>
#include <stdio.h>
#include <easyflash.h>

/* ---- System ---------------------------------------------------------- */

void hal_init(void)
{
    easyflash_init();
}

uint32_t hal_millis(void)
{
    return (uint32_t)(rt_tick_get() * 1000 / RT_TICK_PER_SECOND);
}

void hal_sleep_ms(uint32_t ms)
{
    rt_thread_mdelay(ms);
}

void hal_reboot(void)
{
    rt_hw_cpu_reset();
}

void hal_get_mac(uint8_t mac[6])
{
    /* TODO: read the station MAC from the SDK (wifi_get_mac_address() in
     * Beken SDKs; check the name in the SDK drop in use). */
    static const uint8_t fallback[6] = { 0xc8, 0x47, 0x8c, 0x00, 0x00, 0x01 };
    memcpy(mac, fallback, 6);
}

struct task_start {
    hal_task_fn fn;
    void *arg;
};

static void task_tramp(void *p)
{
    struct task_start ts = *(struct task_start *)p;
    rt_free(p);
    ts.fn(ts.arg);
}

int hal_task_start(const char *name, hal_task_fn fn, void *arg,
                   uint32_t stack_bytes, int priority)
{
    struct task_start *ts = rt_malloc(sizeof(*ts));
    rt_thread_t t;

    if (!ts)
        return -1;
    ts->fn = fn;
    ts->arg = arg;
    /* RT-Thread: lower number = higher priority. Map the app's 0..10 scale
     * into the middle of the range, below the Wi-Fi/lwIP threads. */
    t = rt_thread_create(name, task_tramp, ts, stack_bytes,
                         RT_THREAD_PRIORITY_MAX / 2 + (10 - priority), 10);
    if (!t) {
        rt_free(ts);
        return -1;
    }
    return rt_thread_startup(t) == RT_EOK ? 0 : -1;
}

struct hal_mutex {
    rt_mutex_t m;
};

hal_mutex_t *hal_mutex_create(void)
{
    hal_mutex_t *m = rt_malloc(sizeof(*m));
    if (m)
        m->m = rt_mutex_create("a9", RT_IPC_FLAG_PRIO);
    return m;
}

void hal_mutex_lock(hal_mutex_t *m)   { rt_mutex_take(m->m, RT_WAITING_FOREVER); }
void hal_mutex_unlock(hal_mutex_t *m) { rt_mutex_release(m->m); }

/* ---- Key/value storage: EasyFlash ENV -------------------------------- */

int hal_kv_get(const char *key, void *buf, size_t len)
{
    size_t saved = 0;
    size_t n = ef_get_env_blob(key, buf, len, &saved);
    return saved ? (int)n : -1;
}

int hal_kv_set(const char *key, const void *buf, size_t len)
{
    return ef_set_env_blob(key, buf, len) == EF_NO_ERR ? 0 : -1;
}

int hal_kv_erase_all(void)
{
    return ef_del_env("a9cfg") == EF_NO_ERR ? 0 : -1;
}

/* ---- Wi-Fi ----------------------------------------------------------- */

static hal_wifi_state_t g_wifi = HAL_WIFI_OFF;

int hal_wifi_start_ap(const char *ssid, const char *password, uint8_t channel)
{
    /* TODO: start a soft-AP with DHCP server on 192.168.4.1/24.
     * Beken SDKs: fill network_InitTypeDef_st with wifi_mode = SOFT_AP,
     * local_ip_addr/net_mask/gateway_ip_addr, dhcp_mode = DHCP_SERVER and
     * call bk_wlan_start(). */
    (void)ssid; (void)password; (void)channel;
    g_wifi = HAL_WIFI_AP;
    return -1;
}

int hal_wifi_start_sta(const char *ssid, const char *password)
{
    /* TODO: station mode with DHCP client. Beken SDKs: network_InitTypeDef_st
     * with wifi_mode = STATION, dhcp_mode = DHCP_CLIENT, then bk_wlan_start().
     * Register a status callback to drive g_wifi (connected/failed). */
    (void)ssid; (void)password;
    g_wifi = HAL_WIFI_STA_CONNECTING;
    return -1;
}

void hal_wifi_stop(void)
{
    /* TODO: bk_wlan_stop() for the active interface. */
    g_wifi = HAL_WIFI_OFF;
}

hal_wifi_state_t hal_wifi_state(void)
{
    return g_wifi;
}

void hal_wifi_ip(char *buf, size_t len)
{
    /* TODO: read the interface address from lwIP (netif_default->ip_addr). */
    snprintf(buf, len, "%s", g_wifi == HAL_WIFI_AP ? "192.168.4.1" : "0.0.0.0");
}

int hal_wifi_rssi(void)
{
    /* TODO: bk_wlan_get_link_status() -> wifi_strength. */
    return 0;
}

/* ---- Camera ---------------------------------------------------------- */

/*
 * TODO: the BK7252 camera path is DVP sensor -> hardware JPEG encoder ->
 * DMA into a frame buffer. Beken's SDK exposes this through its video
 * transfer / camera interface module. The implementation should:
 *   1. probe the sensor over I2C (seen on A9 boards: GC0308, GC0328,
 *      GC0311, HI704 and similar VGA parts),
 *   2. configure resolution, fps and JPEG quality from cfg,
 *   3. keep the newest complete JPEG in a double buffer guarded by a mutex,
 *      bumping a sequence counter per frame,
 *   4. have hal_camera_get_frame() copy it out (see platform/host for the
 *      expected semantics).
 */
int hal_camera_start(const hal_camera_cfg_t *cfg)
{
    (void)cfg;
    return -1;
}

void hal_camera_stop(void) {}

int hal_camera_get_frame(uint8_t *buf, size_t cap, uint32_t *last_seq,
                         uint32_t *ts_ms, uint32_t timeout_ms)
{
    (void)buf; (void)cap; (void)last_seq; (void)ts_ms;
    hal_sleep_ms(timeout_ms);
    return 0;
}

/* ---- GPIO ------------------------------------------------------------ */

/* TODO: pin numbers come from the board; trace them on Spas's PCB. */
void hal_led_set(int on)      { (void)on; }
int  hal_button_pressed(void) { return 0; }

/* ---- Entry ----------------------------------------------------------- */

static void a9_thread(void *arg)
{
    (void)arg;
    hal_init();
    a9_main();
}

int a9_app_start(void)
{
    rt_thread_t t = rt_thread_create("a9", a9_thread, RT_NULL, 4096,
                                     RT_THREAD_PRIORITY_MAX / 2, 10);
    return t ? rt_thread_startup(t) : -1;
}
INIT_APP_EXPORT(a9_app_start);
