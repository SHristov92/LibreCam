/*
 * BK7252N implementation of the HAL on Beken's FreeRTOS SDK
 * (beken_freertos_sdk_release, CFG_SOC_NAME == SOC_BK7252N).
 *
 * The camera lives in camera_bk7252.c. Hardware facts used here (pins, flash
 * map, battery scale) come from the stock firmware; see docs/device-spec.md.
 */
#include "lc/app.h"
#include "lc/board.h"
#include "lc/hal.h"

#include "include.h"
#include "rtos_pub.h"
#include "mem_pub.h"
#include "drv_model_pub.h"
#include "flash_pub.h"
#include "BkDriverFlash.h"
#include "gpio_pub.h"
#include "saradc_pub.h"
#include "wlan_ui_pub.h"
#include "rw_msg_pub.h"
#include "param_config.h"
#include "sys_ctrl_pub.h"
#include "icu_pub.h"
#include "manual_ps_pub.h"
#include "pseudo_random_pub.h"

#include <stdio.h>
#include <string.h>

/* ---- System ---------------------------------------------------------- */

static void lc_kv_init(void);
static void lc_gpio_init(void);

/* The IO LDO (VDDRAM pin) supplies the camera sensor and the SD card IO.
 * Stock firmware turns it on first thing in main (0x810bc -> 0x91db0(4)):
 * trim 7 (the 3.5 V step, i.e. full on from a 3.3 V input), then block
 * enable bit 18. Without it the unpowered sensor holds SCL/SDA low. */
static void io_ldo_on(void)
{
    UINT32 p = 7;

    sddev_control(SCTRL_DEV_NAME, CMD_QSPI_VDDRAM_VOLTAGE, &p);
    p = PCLK_POSI_VDDRAM;
    sddev_control(SCTRL_DEV_NAME, CMD_SCTRL_BLK_ENABLE, &p);
}

static void lc_mac_init(void);

void hal_init(void)
{
    io_ldo_on();
    lc_kv_init();
    lc_mac_init();
    lc_gpio_init();
}

uint32_t hal_millis(void)
{
    return rtos_get_time();
}

void hal_sleep_ms(uint32_t ms)
{
    rtos_delay_milliseconds(ms);
}

void hal_reboot(void)
{
    bk_reboot();
}

void hal_deep_sleep(void)
{
#ifdef BOARD_WAKE_PIN_MASK
    UINT32 p = PCLK_POSI_VDDRAM;
    PS_DEEP_CTRL_PARAM ds;

#if BOARD_LED_PIN >= 0
    bk_gpio_output(BOARD_LED_PIN, BOARD_LED_ACTIVE_LOW);
#endif
#ifdef BOARD_IR_LED_PIN
    bk_gpio_output(BOARD_IR_LED_PIN, 0);
#endif
#ifdef BOARD_SPK_AMP_PIN
    bk_gpio_output(BOARD_SPK_AMP_PIN, 0);
#endif
    /* Camera sensor and SD card run from the IO LDO. */
    sddev_control(SCTRL_DEV_NAME, CMD_SCTRL_BLK_DISABLE, &p);
    printf("[lc] deep sleep; press a button to wake\n");
    rtos_delay_milliseconds(50);            /* let the UART drain */
    /* Filled in full, as the stock firmware does (0x86308): the SDK's
     * bk_enter_deep_sleep() wrapper leaves the BK7252N-only edge/level
     * selection and the low-power clock source as stack garbage, so whether
     * a button could wake the chip depended on the build and the camera.
     * Like stock: wake while a button holds its pin low (level, not edge),
     * the internal RC oscillator as the sleep clock, the IR LED held off.
     * A wake is a cold boot. */
    memset(&ds, 0, sizeof(ds));
    ds.wake_up_way = PS_DEEP_WAKEUP_GPIO;
    ds.gpio_index_map = BOARD_WAKE_PIN_MASK;
    ds.gpio_edge_map = BOARD_WAKE_PIN_MASK;     /* 1 = low / falling */
    ds.gpio_edge_sel_map = 0;                   /* 0 = level */
    ds.lpo_32k_src = LPO_SELECT_ROSC;
#ifdef BOARD_IR_LED_PIN
    ds.gpio_stay_lo_map = 1u << BOARD_IR_LED_PIN;
#endif
    bk_enter_deep_sleep_mode(&ds);
#endif
    bk_reboot();                            /* no wake source: restart instead */
}

void hal_get_mac(uint8_t mac[6])
{
    wifi_get_mac_address((char *)mac, CONFIG_ROLE_STA);
}

/* The BK7252N has two heaps: SRAM (os_malloc, also used by Wi-Fi and lwIP
 * for every packet) and a 256 KB bank at 0x2000000 (psram_malloc). Frames
 * go to the second one; os_free/free() returns blocks to either heap. */
void *hal_malloc_large(size_t size)
{
    void *p = psram_malloc(size);
    return p ? p : os_malloc(size);
}

struct task_start {
    hal_task_fn fn;
    void *arg;
};

static void task_tramp(beken_thread_arg_t p)
{
    struct task_start ts = *(struct task_start *)p;

    os_free(p);
    ts.fn(ts.arg);
    rtos_delete_thread(NULL);
}

int hal_task_start(const char *name, hal_task_fn fn, void *arg,
                   uint32_t stack_bytes, int priority)
{
    struct task_start *ts = os_malloc(sizeof(*ts));
    int bk_prio;

    if (!ts)
        return -1;
    ts->fn = fn;
    ts->arg = arg;
    /* Beken: lower number = higher priority; the Wi-Fi core runs at 1..3 and
     * the SDK's own application threads at 6..7. Keep ours below Wi-Fi. */
    bk_prio = 11 - priority;
    if (bk_prio < 4)
        bk_prio = 4;
    if (bk_prio > 9)
        bk_prio = 9;
    if (rtos_create_thread(NULL, bk_prio, name, task_tramp, stack_bytes, ts) != kNoErr) {
        os_free(ts);
        return -1;
    }
    return 0;
}

struct hal_mutex {
    beken_mutex_t m;
};

hal_mutex_t *hal_mutex_create(void)
{
    hal_mutex_t *m = os_malloc(sizeof(*m));

    if (m && rtos_init_mutex(&m->m) != kNoErr) {
        os_free(m);
        m = NULL;
    }
    return m;
}

void hal_mutex_lock(hal_mutex_t *m)   { rtos_lock_mutex(&m->m); }
void hal_mutex_unlock(hal_mutex_t *m) { rtos_unlock_mutex(&m->m); }

/* ---- Key/value storage ------------------------------------------------
 *
 * One 4 KB sector at KV_ADDR, free in both the stock and the SDK layouts
 * (stock vendor config ends at 0x1F9000, the SDK's last partition is the
 * BLE bonding sector at 0x1F0000). Layout: header, then packed records
 * {u8 key_len, u16 val_len, key, value}, terminated by key_len 0xFF.
 * The whole sector is rewritten on every set; the app writes rarely.
 */
#define KV_ADDR    0x1FD000
#define KV_SIZE    0x1000
#define KV_MAGIC   0x41394b56u   /* "VK9A" */

struct kv_hdr {
    uint32_t magic;
    uint32_t len;     /* bytes of records after the header */
    uint32_t sum;     /* simple additive checksum of the records */
};

static DD_HANDLE g_flash = DD_HANDLE_UNVALID;
static beken_mutex_t g_kv_lock;

static uint32_t kv_sum(const uint8_t *p, uint32_t n)
{
    uint32_t s = 0x9e3779b9u;

    while (n--)
        s = (s << 5) + s + *p++;
    return s;
}

static void lc_kv_init(void)
{
    UINT32 st;

    g_flash = ddev_open(FLASH_DEV_NAME, &st, 0);
    rtos_init_mutex(&g_kv_lock);
}

/* ---- MAC address ------------------------------------------------------
 *
 * The SDK takes the MAC from the RF calibration area and, if it isn't there,
 * uses one built-in default (C8:47:8C:42:88:48) - so every camera that keeps
 * its MAC elsewhere ends up with the same address, AP name and host name.
 * Before Wi-Fi starts, replace that default with the camera's own MAC from
 * the stock config, or else a random one made once and kept in our settings. */

#define SDK_DEFAULT_MAC "\xC8\x47\x8C\x42\x88\x48"

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Parses 12 hex digits, optionally separated by ':' or '-'. */
static int parse_mac(const char *s, uint8_t mac[6])
{
    int i;

    for (i = 0; i < 6; i++) {
        int hi = hexval(s[0]), lo = hi < 0 ? -1 : hexval(s[1]);

        if (lo < 0)
            return -1;
        mac[i] = (uint8_t)(hi << 4 | lo);
        s += 2;
        if (i < 5 && (*s == ':' || *s == '-'))
            s++;
    }
    return 0;
}

static int vendor_mac(uint8_t mac[6])
{
#ifdef BOARD_VENDOR_MAC_JSON
    static const char key[] = "\"MAC_ADDR\":\"";
    char buf[257], *p;

    ddev_read(g_flash, buf, sizeof(buf) - 1, BOARD_VENDOR_MAC_JSON);
    buf[sizeof(buf) - 1] = 0;
    p = strstr(buf, key);
    if (p && parse_mac(p + sizeof(key) - 1, mac) == 0 && !(mac[0] & 1) &&
        memcmp(mac, "\0\0\0\0\0\0", 6) && memcmp(mac, "\xff\xff\xff\xff\xff\xff", 6))
        return 0;
#endif
    return -1;
}

static void lc_mac_init(void)
{
    uint8_t mac[6];
    const char *from;

    wifi_get_mac_address((char *)mac, CONFIG_ROLE_STA);    /* SDK's own lookup */
    if (memcmp(mac, SDK_DEFAULT_MAC, 6)) {
        from = "RF calibration";
    } else if (vendor_mac(mac) == 0) {
        from = "stock config";
    } else if (hal_kv_get("mac", mac, 6) == 6) {
        from = "settings";
    } else {
        /* The RF calibration sector differs from chip to chip: mix it in. */
        uint8_t cal[256];
        uint32_t r = prandom_get() ^ rtos_get_time(), i;

        ddev_read(g_flash, (char *)cal, sizeof(cal), 0x1E0000);
        for (i = 0; i < sizeof(cal); i++)
            r = (r << 5) + r + cal[i];

        /* Locally administered, unicast; the rest random. */
        mac[0] = 0x02;
        mac[1] = (uint8_t)(prandom_get() >> 8);
        mac[2] = (uint8_t)r;
        mac[3] = (uint8_t)(r >> 8);
        mac[4] = (uint8_t)(r >> 16);
        mac[5] = (uint8_t)(r >> 24);
        hal_kv_set("mac", mac, 6);
        from = "random, saved";
    }
    memcpy(system_mac, mac, 6);
    printf("[lc] MAC %02X:%02X:%02X:%02X:%02X:%02X (%s)\n", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5], from);
}

/* Loads the record area into a fresh KV_SIZE buffer. Returns its length,
 * 0 for an empty/invalid sector, -1 on allocation failure. */
static int kv_load(uint8_t **out)
{
    struct kv_hdr h;
    uint8_t *buf = os_malloc(KV_SIZE);

    *out = buf;
    if (!buf)
        return -1;
    ddev_read(g_flash, (char *)&h, sizeof(h), KV_ADDR);
    if (h.magic != KV_MAGIC || h.len > KV_SIZE - sizeof(h))
        return 0;
    ddev_read(g_flash, (char *)buf, h.len, KV_ADDR + sizeof(h));
    return kv_sum(buf, h.len) == h.sum ? (int)h.len : 0;
}

static int kv_find(const uint8_t *buf, int len, const char *key, int *rec, int *rec_len)
{
    size_t klen = strlen(key);
    int off = 0;

    while (off + 3 <= len && buf[off] != 0xFF) {
        int kl = buf[off], vl = buf[off + 1] | (buf[off + 2] << 8);
        int total = 3 + kl + vl;

        if (off + total > len)
            break;
        if ((size_t)kl == klen && !memcmp(buf + off + 3, key, klen)) {
            *rec = off;
            *rec_len = total;
            return 1;
        }
        off += total;
    }
    return 0;
}

static int kv_store(const uint8_t *buf, uint32_t len)
{
    struct kv_hdr h = { KV_MAGIC, len, kv_sum(buf, len) };
    UINT32 addr = KV_ADDR;
    uint8_t check[sizeof(h)];

    bk_flash_enable_security(FLASH_PROTECT_NONE);
    ddev_control(g_flash, CMD_FLASH_ERASE_SECTOR, &addr);
    ddev_write(g_flash, (char *)buf, len, KV_ADDR + sizeof(h));
    ddev_write(g_flash, (char *)&h, sizeof(h), KV_ADDR);
    bk_flash_enable_security(FLASH_UNPROTECT_LAST_BLOCK);
    ddev_read(g_flash, (char *)check, sizeof(check), KV_ADDR);
    return memcmp(check, &h, sizeof(h)) ? -1 : 0;
}

int hal_kv_get(const char *key, void *val, size_t cap)
{
    uint8_t *buf;
    int len, rec, rec_len, ret = -1;

    rtos_lock_mutex(&g_kv_lock);
    len = kv_load(&buf);
    if (len > 0 && kv_find(buf, len, key, &rec, &rec_len)) {
        int kl = buf[rec], vl = buf[rec + 1] | (buf[rec + 2] << 8);
        size_t n = (size_t)vl < cap ? (size_t)vl : cap;

        memcpy(val, buf + rec + 3 + kl, n);
        ret = (int)n;
    }
    if (buf)
        os_free(buf);
    rtos_unlock_mutex(&g_kv_lock);
    return ret;
}

int hal_kv_set(const char *key, const void *val, size_t len)
{
    size_t klen = strlen(key);
    uint8_t *buf;
    int used, rec, rec_len, ret = -1;

    if (klen == 0 || klen > 64 || len > 0xFFFF)
        return -1;
    rtos_lock_mutex(&g_kv_lock);
    used = kv_load(&buf);
    if (used < 0)
        goto out;
    if (kv_find(buf, used, key, &rec, &rec_len)) {
        memmove(buf + rec, buf + rec + rec_len, used - rec - rec_len);
        used -= rec_len;
    }
    if ((size_t)used + 3 + klen + len > KV_SIZE - sizeof(struct kv_hdr))
        goto out;
    buf[used] = (uint8_t)klen;
    buf[used + 1] = len & 0xFF;
    buf[used + 2] = len >> 8;
    memcpy(buf + used + 3, key, klen);
    memcpy(buf + used + 3 + klen, val, len);
    used += 3 + klen + len;
    ret = kv_store(buf, used);
out:
    if (buf)
        os_free(buf);
    rtos_unlock_mutex(&g_kv_lock);
    return ret;
}

int hal_kv_erase_all(void)
{
    UINT32 addr = KV_ADDR;

    rtos_lock_mutex(&g_kv_lock);
    bk_flash_enable_security(FLASH_PROTECT_NONE);
    ddev_control(g_flash, CMD_FLASH_ERASE_SECTOR, &addr);
    bk_flash_enable_security(FLASH_UNPROTECT_LAST_BLOCK);
    rtos_unlock_mutex(&g_kv_lock);
    return 0;
}

/* ---- Wi-Fi ----------------------------------------------------------- */

#define AP_IP   "192.168.4.1"
#define AP_MASK "255.255.255.0"

static volatile int g_ap_up, g_sta_started;

int hal_wifi_start_ap(const char *ssid, const char *password, uint8_t channel)
{
    network_InitTypeDef_st cfg;

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s", ssid);
    snprintf(cfg.wifi_key, sizeof(cfg.wifi_key), "%s", password ? password : "");
    cfg.wifi_mode = BK_SOFT_AP;
    cfg.dhcp_mode = DHCP_SERVER;
    cfg.wifi_retry_interval = 100;
    snprintf(cfg.local_ip_addr, sizeof(cfg.local_ip_addr), AP_IP);
    snprintf(cfg.net_mask, sizeof(cfg.net_mask), AP_MASK);
    snprintf(cfg.gateway_ip_addr, sizeof(cfg.gateway_ip_addr), AP_IP);
    snprintf(cfg.dns_server_ip_addr, sizeof(cfg.dns_server_ip_addr), AP_IP);
    if (channel >= 1 && channel <= 13)
        bk_wlan_ap_set_default_channel(channel);
    if (bk_wlan_start(&cfg) != kNoErr)
        return -1;
    g_ap_up = 1;
    return 0;
}

/* From the SDK's lwIP port (net.c). */
extern void ip_address_set(int iface, int dhcp, char *ip, char *mask, char *gw, char *dns);

static char *or_any(const char *addr)
{
    return (char *)(addr && addr[0] ? addr : "0.0.0.0");
}

int hal_wifi_start_sta(const char *ssid, const char *password, const hal_ip_cfg_t *ip)
{
    network_InitTypeDef_st cfg;

    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s", ssid);
    snprintf(cfg.wifi_key, sizeof(cfg.wifi_key), "%s", password ? password : "");
    cfg.wifi_mode = BK_STATION;
    cfg.dhcp_mode = DHCP_CLIENT;
    cfg.wifi_retry_interval = 100;
#if CFG_STA_AUTO_RECONNECT
    cfg.auto_reconnect_count = 0;      /* keep retrying after a drop */
    cfg.auto_reconnect_timeout = 0;
    cfg.disable_auto_reconnect_after_disconnect = 0;
#endif
    if (bk_wlan_start(&cfg) != kNoErr)
        return -1;
    /* Without CFG_WLAN_FAST_CONNECT_STATIC_IP, bk_wlan_start_sta() forces
     * DHCP. The address is applied only once the link comes up, so set the
     * static one now, while the connection is still being made. lwIP then
     * reports RW_EVT_STA_GOT_IP as soon as the link is up. */
    if (ip)
        ip_address_set(BK_STATION, DHCP_DISABLE, or_any(ip->ip), or_any(ip->mask),
                       or_any(ip->gw), or_any(ip->dns));
    g_sta_started = 1;
    return 0;
}

void hal_wifi_stop(void)
{
    if (g_sta_started)
        bk_wlan_stop(BK_STATION);
    if (g_ap_up)
        bk_wlan_stop(BK_SOFT_AP);
    g_sta_started = g_ap_up = 0;
}

hal_wifi_state_t hal_wifi_state(void)
{
    if (g_ap_up)
        return HAL_WIFI_AP;
    if (!g_sta_started)
        return HAL_WIFI_OFF;
    switch (mhdr_get_station_status()) {
    case RW_EVT_STA_GOT_IP:
        return HAL_WIFI_STA_CONNECTED;
    case RW_EVT_STA_PASSWORD_WRONG:
    case RW_EVT_STA_NO_AP_FOUND:
    case RW_EVT_STA_AUTH_FAILED:
    case RW_EVT_STA_ASSOC_FAILED:
        return HAL_WIFI_STA_FAILED;
    default:
        return HAL_WIFI_STA_CONNECTING;
    }
}

void hal_wifi_ip(char *buf, size_t len)
{
    IPStatusTypedef ip;

    memset(&ip, 0, sizeof(ip));
    if (g_ap_up)
        bk_wlan_get_ip_status(&ip, BK_SOFT_AP);
    else if (g_sta_started)
        bk_wlan_get_ip_status(&ip, BK_STATION);
    snprintf(buf, len, "%s", ip.ip[0] ? ip.ip : "0.0.0.0");
}

static beken_semaphore_t g_scan_done;

/* The SDK's wlan_sta_scan_result()/wlan_ap_scan_result() are one-line
 * wrappers around wpa_ctrl_request(). A strong reference to any of them from
 * this early object makes the linker pull libsupplicant members in a
 * different order, and two of its crypto objects then clash (duplicate
 * sha256_vector). A weak reference pulls nothing and binds to the copy the
 * SDK links anyway. The command values are wpa_ctrl_cmd_t's
 * (src/common/wpa_ctrl.h), as the wrappers pass them. */
extern int wpa_ctrl_request(int cmd, void *data) __attribute__((weak));
#define WPA_CTRL_CMD_STA_SCAN_RESULTS     0x04
#define WPA_CTRL_CMD_STA_BSS_FLUSH        0x12
#define WPA_CTRL_CMD_AP_SCAN_RESULTS      0x72
#define WPA_CTRL_CMD_AP_SCAN_RESULTS_FLUSH 0x73

static void scan_done_cb(void *ctxt, unsigned char vif_idx)
{
    (void)ctxt;
    (void)vif_idx;
    if (g_scan_done)
        rtos_set_semaphore(&g_scan_done);
}

int hal_wifi_scan(hal_wifi_ap_t *aps, int max)
{
    static beken_mutex_t lock;
    ScanResult_adv r = {0};
    int i, n, ret;

    if (!lock && rtos_init_mutex(&lock) != kNoErr)
        return -1;
    if (!g_scan_done) {
        if (rtos_init_semaphore(&g_scan_done, 1) != kNoErr)
            return -1;
        bk_wlan_scan_ap_reg_cb(scan_done_cb);
    }
    rtos_lock_mutex(&lock);
    while (rtos_get_semaphore(&g_scan_done, 0) == kNoErr)
        ;                               /* drop a stale completion */
    /* Uses the AP interface while the setup AP is up, so clients stay on. */
    bk_wlan_start_scan();
    if (rtos_get_semaphore(&g_scan_done, 8000) != kNoErr)
        printf("[wifi] scan timed out\n");
    /* CFG_WPA_CTRL_IFACE: the results live in hostapd/wpa_supplicant, not in
     * the driver's table (bk_wlan_get_scan_ap_result stays empty). */
    if (!wpa_ctrl_request) {
        ret = -1;
    } else if (bk_wlan_ap_is_up() > 0) {
        ret = wpa_ctrl_request(WPA_CTRL_CMD_AP_SCAN_RESULTS, &r);
        wpa_ctrl_request(WPA_CTRL_CMD_AP_SCAN_RESULTS_FLUSH, 0);
    } else {
        ret = wpa_ctrl_request(WPA_CTRL_CMD_STA_SCAN_RESULTS, &r);
        wpa_ctrl_request(WPA_CTRL_CMD_STA_BSS_FLUSH, 0);
    }
    n = 0;
    if (ret == 0 && r.ApList) {
        for (i = 0; i < r.ApNum && n < max; i++, n++) {
            memcpy(aps[n].ssid, r.ApList[i].ssid, 32);
            aps[n].ssid[32] = 0;
            aps[n].rssi = (int8_t)r.ApList[i].ApPower;
            aps[n].channel = (uint8_t)r.ApList[i].channel;
            aps[n].secure = r.ApList[i].security != BK_SECURITY_TYPE_NONE;
        }
    }
    if (r.ApList)
        os_free(r.ApList);
    printf("[wifi] scan: %d networks (ret %d)\n", n, ret);
    rtos_unlock_mutex(&lock);
    return ret == 0 ? n : -1;
}

int hal_wifi_rssi(void)
{
    LinkStatusTypeDef ls;

    memset(&ls, 0, sizeof(ls));
    if (bk_wlan_get_link_status(&ls) != kNoErr)
        return 0;
    return ls.wifi_strength;
}

/* ---- GPIO ------------------------------------------------------------ */

static void lc_gpio_init(void)
{
#if BOARD_LED_PIN >= 0
    bk_gpio_config_output(BOARD_LED_PIN);
    bk_gpio_output(BOARD_LED_PIN, BOARD_LED_ACTIVE_LOW);
#endif
#if BOARD_BUTTON_PIN >= 0
    bk_gpio_config_input_pup(BOARD_BUTTON_PIN);
#endif
#ifdef BOARD_USER_BUTTON_PIN
    bk_gpio_config_input_pup(BOARD_USER_BUTTON_PIN);
#endif
#ifdef BOARD_IR_LED_PIN
    bk_gpio_config_output(BOARD_IR_LED_PIN);    /* IR illuminator off */
    bk_gpio_output(BOARD_IR_LED_PIN, 0);
#endif
#ifdef BOARD_SPK_AMP_PIN
    bk_gpio_config_output(BOARD_SPK_AMP_PIN);   /* speaker amplifier off */
    bk_gpio_output(BOARD_SPK_AMP_PIN, 0);
#endif
}

void hal_led_set(int on)
{
#if BOARD_LED_PIN >= 0
    bk_gpio_output(BOARD_LED_PIN, (on ? 1 : 0) ^ BOARD_LED_ACTIVE_LOW);
#else
    (void)on;
#endif
}

int hal_user_button_pressed(void)
{
#ifdef BOARD_USER_BUTTON_PIN
    return !bk_gpio_input(BOARD_USER_BUTTON_PIN);   /* active low */
#else
    return 0;
#endif
}

int hal_button_pressed(void)
{
#if BOARD_BUTTON_PIN >= 0
    return (bk_gpio_input(BOARD_BUTTON_PIN) ? 1 : 0) ^ BOARD_BUTTON_ACTIVE_LOW;
#else
    return 0;
#endif
}

/* ---- Battery --------------------------------------------------------- */

/* SARADC channel 0 senses VBAT. The stock firmware scales the ADC voltage
 * by 3 (internal divider), which reads ~3300 mV on a 3.3 V supply. */
#define VBAT_ADC_CHANNEL  0
#define VBAT_SCALE        3
#define VBAT_SAMPLES      32

int hal_battery_mv(void)
{
    saradc_desc_t d;
    UINT16 data[VBAT_SAMPLES];
    DD_HANDLE h;
    UINT32 st, sum = 0, t0;
    int i;

    if (saradc_check_busy())
        return g_vbat_mv;
    memset(&d, 0, sizeof(d));
    d.channel = VBAT_ADC_CHANNEL;
    d.data_buff_size = VBAT_SAMPLES;
    d.mode = (ADC_CONFIG_MODE_CONTINUE << 0) | (ADC_CONFIG_MODE_4CLK_DELAY << 2);
    d.pre_div = 0x10;
    d.samp_rate = 0x20;
    d.pData = data;
    h = ddev_open(SARADC_DEV_NAME, &st, (UINT32)&d);
    if (h == DD_HANDLE_UNVALID)
        return -1;
    t0 = rtos_get_time();
    /* The driver stops one short of data_buff_size. */
    while (d.current_sample_data_cnt < d.data_buff_size - 1 && rtos_get_time() - t0 < 200)
        rtos_delay_milliseconds(1);
    ddev_close(h);
    if (d.current_sample_data_cnt < d.data_buff_size - 1) {
        printf("[lc] vbat: only %u of %u ADC samples\n",
               (unsigned)d.current_sample_data_cnt, (unsigned)d.data_buff_size);
        return -1;
    }
    /* Skip the first samples while the ADC settles. */
    for (i = VBAT_SAMPLES / 2; i < VBAT_SAMPLES - 1; i++)
        sum += data[i];
    return (int)(saradc_calculate((UINT16)(sum / (VBAT_SAMPLES / 2 - 1))) * 1000.0f * VBAT_SCALE);
}

/* ---- Entry ----------------------------------------------------------- */

extern size_t xPortGetFreeHeapSize(void);

static void lc_thread(beken_thread_arg_t arg)
{
    (void)arg;
    hal_init();
    printf("[lc] free heap %u bytes\n", (unsigned)xPortGetFreeHeapSize());
    lc_main();
}

/* Pulled in by the linker with -u lc_bk_entry (see sdk.mk); the SDK calls
 * user_main() from its app thread once the Wi-Fi stack is up. */
void lc_bk_entry(void) {}

void user_main(void)
{
    rtos_create_thread(NULL, 6, "librecam", lc_thread, 6144, NULL);
}
