/*
 * TXW81x (Taixin TXW817/TXW818, C-SKY CK803) implementation of the HAL on
 * Taixin's SDK as published in OpenTXW81X (see README.md for the pinned
 * commit). The SDK runs AliOS Rhino behind its OSAL (os_* calls) and lwIP.
 *
 * The camera lives in camera_txw81x.c; audio, SD card and OTA are stubs in
 * stubs_txw81x.c until a board has been dumped and tested.
 *
 * Wi-Fi follows the SDK's own demo (project/main.c in OpenTXW81X): the lower
 * MAC and both interfaces are set up once, and switching between AP and
 * station moves the single lwIP netif "w0" between the two net devices.
 */
#include "lc/app.h"
#include "lc/board.h"
#include "lc/hal.h"

#include "sys_config.h"
#include "typesdef.h"
#include "list.h"
#include "dev.h"
#include "devid.h"
#include "osal/string.h"
#include "osal/mutex.h"
#include "osal/task.h"
#include "osal/sleep.h"
#include "osal/time.h"
#include "osal/work.h"
#include "hal/gpio.h"
#include "hal/spi_nor.h"
#include "hal/netdev.h"
#include "lib/common/common.h"
#include "lib/common/sysevt.h"
#include "lib/syscfg/syscfg.h"
#include "lib/lmac/lmac.h"
#include "lib/skb/skbpool.h"
#include "lib/net/dhcpd/dhcpd.h"
#include "lib/umac/ieee80211.h"
#include "lib/umac/umac.h"
#include "lib/heap/sysheap.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/tcpip.h"
#include "lwip/dns.h"
#include "netif/ethernetif.h"
#include "lib/net/eloop/eloop.h"
#include "dhcpd_eloop/dhcpd_eloop.h"
#include "chip/txw81x/sysctrl.h"
#include "chip/txw81x/misc.h"
#include "chip/txw81x/io_function.h"
#include "syscfg.h"
#include "project_config.h"

#include <stdio.h>
#include <string.h>

/* Symbols the SDK's libraries expect from the application (project/main.c
 * defines them in the vendor demo, which we don't build). */
struct system_status sys_status;
uint8_t qc_mode;

/* The minilibc has no strtok_r (declared in compat/string.h). */
char *strtok_r(char *str, const char *delim, char **saveptr)
{
    char *end;

    if (!str)
        str = *saveptr;
    str += strspn(str, delim);
    if (!*str) {
        *saveptr = str;
        return NULL;
    }
    end = str + strcspn(str, delim);
    if (*end)
        *end++ = 0;
    *saveptr = end;
    return str;
}

/* ---- System ---------------------------------------------------------- */

static void lc_flash_init(void);
static void lc_mac_init(void);
static void lc_gpio_init(void);

void hal_init(void)
{
    lc_flash_init();
    lc_mac_init();
    lc_gpio_init();
}

uint32_t hal_millis(void)
{
    return (uint32_t)os_jiffies_to_msecs(os_jiffies());
}

void hal_sleep_ms(uint32_t ms)
{
    os_sleep_ms((int)ms);
}

void hal_reboot(void)
{
    mcu_reset();
}

/* TODO(txw81x): the SDK has a deep sleep with GPIO wake (lowPower_app);
 * until a board is tested, power down by restarting instead. */
void hal_deep_sleep(void)
{
#if BOARD_LED_PIN >= 0
    gpio_set_val(BOARD_LED_PIN, BOARD_LED_ACTIVE_LOW);
#endif
    printf("[lc] deep sleep not supported on txw81x yet, restarting\n");
    os_sleep_ms(50);
    mcu_reset();
}

void hal_get_mac(uint8_t mac[6])
{
    memcpy(mac, sys_cfgs.mac, 6);
}

/* One heap (272 KB SRAM on the TXW817); boards with PSRAM could put frames
 * there later. */
void *hal_malloc_large(size_t size)
{
    return os_malloc(size);
}

struct task_start {
    hal_task_fn fn;
    void *arg;
};

static void task_tramp(void *p)
{
    struct task_start ts = *(struct task_start *)p;

    os_free(p);
    ts.fn(ts.arg);
    /* Rhino tasks may simply return; the OSAL wrapper deletes them. */
}

int hal_task_start(const char *name, hal_task_fn fn, void *arg,
                   uint32_t stack_bytes, int priority)
{
    struct task_start *ts = os_malloc(sizeof(*ts));
    uint32 prio;

    if (!ts)
        return -1;
    ts->fn = fn;
    ts->arg = arg;
    /* OSAL: higher number = higher priority; the SDK's Wi-Fi and lwIP tasks
     * run at NORMAL and above. Keep ours in BELOW_NORMAL..NORMAL. */
    if (priority < 1)
        priority = 1;
    if (priority > 10)
        priority = 10;
    prio = OS_TASK_PRIORITY_BELOW_NORMAL + (uint32)(priority - 1) *
           (OS_TASK_PRIORITY_NORMAL - OS_TASK_PRIORITY_BELOW_NORMAL) / 9;
    if (!os_task_create(name, task_tramp, ts, prio, 0, NULL, stack_bytes)) {
        os_free(ts);
        return -1;
    }
    return 0;
}

struct hal_mutex {
    struct os_mutex m;
};

hal_mutex_t *hal_mutex_create(void)
{
    hal_mutex_t *m = os_malloc(sizeof(*m));

    if (m && os_mutex_init(&m->m) != 0) {
        os_free(m);
        m = NULL;
    }
    return m;
}

void hal_mutex_lock(hal_mutex_t *m)   { os_mutex_lock(&m->m, osWaitForever); }
void hal_mutex_unlock(hal_mutex_t *m) { os_mutex_unlock(&m->m); }

/* ---- Settings flash ---------------------------------------------------
 *
 * lib/src/kv_flash.c keeps the settings in one 4 KB sector. Where depends on
 * the board's flash layout, which comes from the vendor parameter block, so
 * board_config.h names the address (BOARD_KV_ADDR). Without one we take the
 * sector just below the SDK's own config sectors (syscfg), which sit at the
 * end of the flash. Check this against a dump before trusting it.
 */
static struct spi_nor_flash *g_flash;
static uint32_t g_kv_addr;

static void lc_flash_init(void)
{
    struct syscfg_info info;

    os_memset(&info, 0, sizeof(info));
    syscfg_info_get(&info);
    g_flash = info.flash1;
    if (g_flash)
        spi_nor_open(g_flash);
#ifdef BOARD_KV_ADDR
    g_kv_addr = BOARD_KV_ADDR;
#else
    {
        uint32_t low = info.addr1;

        if (info.addr2 && info.addr2 < low)
            low = info.addr2;
        g_kv_addr = low >= HAL_KV_FLASH_SIZE ? low - HAL_KV_FLASH_SIZE : 0;
    }
#endif
    printf("[lc] settings sector at 0x%06x (syscfg 0x%06x/0x%06x)\n",
           (unsigned)g_kv_addr, (unsigned)info.addr1, (unsigned)info.addr2);
}

int hal_kv_flash_read(uint32_t off, void *buf, size_t len)
{
    if (!g_flash || !g_kv_addr || off + len > HAL_KV_FLASH_SIZE)
        return -1;
    spi_nor_read(g_flash, g_kv_addr + off, buf, len);
    return 0;
}

int hal_kv_flash_write(uint32_t off, const void *buf, size_t len)
{
    if (!g_flash || !g_kv_addr || off + len > HAL_KV_FLASH_SIZE)
        return -1;
    spi_nor_write(g_flash, g_kv_addr + off, (uint8 *)buf, len);
    return 0;
}

int hal_kv_flash_erase(void)
{
    if (!g_flash || !g_kv_addr)
        return -1;
    spi_nor_sector_erase(g_flash, g_kv_addr);
    return 0;
}

/* ---- MAC address ------------------------------------------------------
 *
 * syscfg_set_default_val() (called from main) reads the MAC from efuse and
 * falls back to a random one on every boot. Keep a random one stable by
 * saving it in our settings. */
static void lc_mac_init(void)
{
    uint8_t *mac = sys_cfgs.mac, saved[6];
    const char *from = "efuse";
    uint8_t efuse[6];

    sysctrl_efuse_mac_addr_calc(efuse);
    if (IS_ZERO_ADDR(efuse)) {
        if (hal_kv_get("mac", saved, 6) == 6) {
            memcpy(mac, saved, 6);
            from = "settings";
        } else {
            mac[0] = (mac[0] & 0xFC) | 0x02;   /* locally administered, unicast */
            hal_kv_set("mac", mac, 6);
            from = "random, saved";
        }
    }
    printf("[lc] MAC %02X:%02X:%02X:%02X:%02X:%02X (%s)\n", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5], from);
}

/* ---- Wi-Fi ----------------------------------------------------------- */

#define AP_IP   0x0104A8C0u   /* 192.168.4.1, network byte order */
#define AP_MASK 0x00FFFFFFu

static volatile hal_wifi_state_t g_state = HAL_WIFI_OFF;
static int g_wifi_ready;
static struct netdev *g_ndev;        /* device "w0" is attached to, if any */
static int g_static_ip;

static int32 lc_wifi_event(uint8 ifidx, uint16 evt, uint32 param1, uint32 param2)
{
    (void)ifidx;
    (void)evt;
    (void)param1;
    (void)param2;
    return RET_OK;
}

static sysevt_hdl_res lc_sysevt_wifi(uint32 event_id, uint32 data, uint32 priv)
{
    (void)data;
    (void)priv;
    if (sys_cfgs.wifi_mode != WIFI_MODE_STA || g_state == HAL_WIFI_OFF)
        return SYSEVT_CONTINUE;
    switch (event_id & 0xffff) {
    case SYSEVT_WIFI_CONNECTTED:
        if (g_static_ip)
            g_state = HAL_WIFI_STA_CONNECTED;
        else
            lwip_netif_set_dhcp2("w0", 1);
        break;
    case SYSEVT_WIFI_WRONG_KEY:
        g_state = HAL_WIFI_STA_FAILED;
        break;
    case SYSEVT_WIFI_DISCONNECT:
    case SYSEVT_WIFI_UNPAIR:
        if (g_state != HAL_WIFI_STA_FAILED)
            g_state = HAL_WIFI_STA_CONNECTING;
        break;
    default:
        break;
    }
    return SYSEVT_CONTINUE;
}

static sysevt_hdl_res lc_sysevt_net(uint32 event_id, uint32 data, uint32 priv)
{
    (void)data;
    (void)priv;
    if (event_id == SYS_EVENT(SYS_EVENT_NETWORK, SYSEVT_LWIP_DHCPC_DONE) &&
        sys_cfgs.wifi_mode == WIFI_MODE_STA && g_state == HAL_WIFI_STA_CONNECTING)
        g_state = HAL_WIFI_STA_CONNECTED;
    return SYSEVT_CONTINUE;
}

/* Lower MAC tuning, as in the SDK demo's sys_wifi_parameter_init(); the
 * values come from project_config.h. */
static void wifi_params(void *ops)
{
    lmac_set_rf_pwr_level(ops, WIFI_RF_PWR_LEVEL);
#if WIFI_FEM_CHIP != LMAC_FEM_NONE
    lmac_set_fem(ops, WIFI_FEM_CHIP);
#endif
    lmac_set_rts(ops, WIFI_RTS_THRESHOLD);
    lmac_set_retry_cnt(ops, WIFI_TX_MAX_RETRY, WIFI_RTS_MAX_RETRY);
    lmac_set_txpower(ops, WIFI_TX_MAX_POWER);
    lmac_set_supp_rate(ops, WIFI_TX_SUPP_RATE);
    lmac_set_max_sta_cnt(ops, WIFI_MAX_STA_CNT);
    lmac_set_mcast_dup_txcnt(ops, WIFI_MULICAST_RETRY);
    lmac_set_max_ps_frame(ops, WIFI_MAX_PS_CNT);
    lmac_set_tx_duty_cycle(ops, WIFI_TX_DUTY_CYCLE);
#if WIFI_PREVENT_PS_MODE_EN
    lmac_set_prevent_ps_mode(ops, WIFI_PREVENT_PS_MODE_EN);
#endif
    lmac_set_temperature_compesate_en(ops, WIFI_TEMPERATURE_COMPESATE_EN);
    lmac_set_freq_offset_track_mode(ops, WIFI_FREQ_OFFSET_TRACK_MODE);
}

static int wifi_core_init(void)
{
    struct lmac_init_param lparam;
    struct ieee80211_initparam param;
    void *ops, *rxbuf;

    if (g_wifi_ready)
        return 0;
#if DCDC_EN
    pmu_dcdc_open();
#endif
    rxbuf = os_malloc(WIFI_RX_BUFF_SIZE);
    if (!rxbuf)
        return -1;
    lparam.rxbuf = (uint32_t)rxbuf;
    lparam.rxbuf_size = WIFI_RX_BUFF_SIZE;
    ops = lmac_bgn_init(&lparam);
#ifdef CONFIG_SLEEP
    {
        void *bgn_dsleep_init(void *ops);
        bgn_dsleep_init(ops);
    }
#endif
    lmac_set_aggcnt(ops, 0);
    lmac_set_rx_aggcnt(ops, 0);

    os_memset(&param, 0, sizeof(param));
    param.vif_maxcnt = 2;
    param.sta_maxcnt = 2;
    param.bss_maxcnt = 2;
    param.bss_lifetime = 300;
    param.no_rxtask = 1;
    param.evt_cb = lc_wifi_event;
    ieee80211_init(&param);
    ieee80211_support_txw80x(ops);
    wifi_params(ops);
    ieee80211_iface_create_ap(WIFI_MODE_AP, IEEE80211_BAND_2GHZ);
    ieee80211_iface_create_sta(WIFI_MODE_STA, IEEE80211_BAND_2GHZ);
    g_wifi_ready = 1;
    return 0;
}

extern u8_t netif_num;

static void netif_detach(void)
{
    if (!g_ndev)
        return;
    lwip_netif_set_dhcp(g_ndev, 0);
    os_sleep_ms(1);
    lwip_netif_remove(g_ndev);
    netif_num--;                       /* as the SDK demo's close_wifi() */
    g_ndev = NULL;
}

static void netif_attach(struct netdev *ndev, uint32_t ip, uint32_t mask, uint32_t gw)
{
    ip_addr_t a, m, g;

    a.addr = ip;
    m.addr = mask;
    g.addr = gw;
    lwip_netif_add(ndev, "w0", &a, &m, &g);
    lwip_netif_remove_register(ndev);
    lwip_netif_set_default(ndev);
    g_ndev = ndev;
}

static void copy_cfg_str(void *dst, size_t cap, const char *src)
{
    snprintf((char *)dst, cap, "%s", src ? src : "");
}

int hal_wifi_start_ap(const char *ssid, const char *password, uint8_t channel)
{
    struct netdev *ndev;
    struct dhcpd_param dp;
    int secure = password && password[0];

    if (wifi_core_init() != 0)
        return -1;
    hal_wifi_stop();
    sys_cfgs.wifi_mode = WIFI_MODE_AP;
    sys_cfgs.channel = channel >= 1 && channel <= 13 ? channel : 6;
    copy_cfg_str(sys_cfgs.ssid, sizeof(sys_cfgs.ssid), ssid);
    copy_cfg_str(sys_cfgs.passwd, sizeof(sys_cfgs.passwd), password);
    sys_cfgs.key_mgmt = secure ? WPA_KEY_MGMT_PSK : WPA_KEY_MGMT_NONE;
    if (secure)
        wpa_passphrase(sys_cfgs.ssid, sys_cfgs.passwd, sys_cfgs.psk);
    wificfg_flush(WIFI_MODE_AP);

    ndev = (struct netdev *)dev_get(HG_WIFI1_DEVID);
    if (!ndev)
        return -1;
    netif_attach(ndev, AP_IP, AP_MASK, AP_IP);
    os_memset(&dp, 0, sizeof(dp));
    dp.start_ip = 0x6404A8C0u;          /* 192.168.4.100 */
    dp.end_ip = 0xC804A8C0u;            /* 192.168.4.200 */
    dp.netmask = AP_MASK;
    dp.router = AP_IP;
    dp.dns1 = AP_IP;
    dp.lease_time = 3600;
    if (dhcpd_start_eloop("w0", &dp))
        printf("[lc] dhcpd start failed\n");
    if (ieee80211_iface_start(WIFI_MODE_AP) != 0)
        return -1;
    g_state = HAL_WIFI_AP;
    return 0;
}

static uint32_t parse_ip(const char *s)
{
    ip4_addr_t a;

    if (!s || !s[0] || !ip4addr_aton(s, &a))
        return 0;
    return a.addr;
}

int hal_wifi_start_sta(const char *ssid, const char *password, const hal_ip_cfg_t *ip)
{
    struct netdev *ndev;
    int secure = password && password[0];

    if (wifi_core_init() != 0)
        return -1;
    hal_wifi_stop();
    sys_cfgs.wifi_mode = WIFI_MODE_STA;
    sys_cfgs.station_channel = 0;      /* no saved BSS: scan every channel */
    copy_cfg_str(sys_cfgs.ssid, sizeof(sys_cfgs.ssid), ssid);
    copy_cfg_str(sys_cfgs.passwd, sizeof(sys_cfgs.passwd), password);
    sys_cfgs.key_mgmt = secure ? WPA_KEY_MGMT_PSK : WPA_KEY_MGMT_NONE;
    if (secure)
        wpa_passphrase(sys_cfgs.ssid, sys_cfgs.passwd, sys_cfgs.psk);
    wificfg_flush(WIFI_MODE_STA);

    ndev = (struct netdev *)dev_get(HG_WIFI0_DEVID);
    if (!ndev)
        return -1;
    g_static_ip = ip != NULL;
    sys_cfgs.dhcpc_en = !g_static_ip;
    if (g_static_ip) {
        netif_attach(ndev, parse_ip(ip->ip), parse_ip(ip->mask), parse_ip(ip->gw));
        if (parse_ip(ip->dns)) {
            ip_addr_t d;

            d.addr = parse_ip(ip->dns);
            dns_setserver(0, &d);
        }
    } else {
        netif_attach(ndev, 0, 0, 0);
    }
    g_state = HAL_WIFI_STA_CONNECTING;
    if (ieee80211_iface_start(WIFI_MODE_STA) != 0) {
        g_state = HAL_WIFI_STA_FAILED;
        return -1;
    }
    return 0;
}

void hal_wifi_stop(void)
{
    if (!g_wifi_ready)
        return;
    if (g_state == HAL_WIFI_AP)
        dhcpd_stop_eloop("w0");
    ieee80211_iface_stop(WIFI_MODE_AP);
    ieee80211_iface_stop(WIFI_MODE_STA);
    netif_detach();
    g_state = HAL_WIFI_OFF;
}

hal_wifi_state_t hal_wifi_state(void)
{
    return g_state;
}

void hal_wifi_ip(char *buf, size_t len)
{
    ip_addr_t ip;

    if (!g_ndev) {
        snprintf(buf, len, "0.0.0.0");
        return;
    }
    ip = lwip_netif_get_ip2("w0");
    snprintf(buf, len, "%s", ip4addr_ntoa(&ip));
}

int hal_wifi_rssi(void)
{
    struct ieee80211_stainfo st;

    if (g_state != HAL_WIFI_STA_CONNECTED)
        return 0;
    os_memset(&st, 0, sizeof(st));
    if (ieee80211_conf_get_stainfo(WIFI_MODE_STA, 0, NULL, &st) != 0)
        return 0;
    return st.rssi;
}

#define SCAN_MAX_BSS 32

int hal_wifi_scan(hal_wifi_ap_t *aps, int max)
{
    struct ieee80211_scandata sp;
    struct hgic_bss_info *bss;
    uint8 ifidx;
    int i, n, out = 0;

    if (wifi_core_init() != 0)
        return -1;
    bss = os_malloc(SCAN_MAX_BSS * sizeof(*bss));
    if (!bss)
        return -1;
    ifidx = g_state == HAL_WIFI_AP ? WIFI_MODE_AP : WIFI_MODE_STA;
    os_memset(&sp, 0, sizeof(sp));
    sp.chan_bitmap = 0x1FFF;           /* channels 1..13 */
    sp.scan_time = 100;                /* ms per channel */
    sp.scan_cnt = 1;
    sp.flush = 1;
    ieee80211_scan(ifidx, 1, &sp);
    os_sleep_ms(13 * 100 + 200);
    n = ieee80211_get_bsslist(bss, SCAN_MAX_BSS, 1);
    for (i = 0; i < n && out < max; i++) {
        uint32_t f = bss[i].freq;

        if (f > 10000)                 /* some SDK builds report 100 kHz units */
            f /= 10;
        copy_cfg_str(aps[out].ssid, sizeof(aps[out].ssid), (const char *)bss[i].ssid);
        aps[out].ssid[32] = 0;
        aps[out].rssi = bss[i].signal;
        aps[out].channel = f >= 2412 && f <= 2472 ? (uint8_t)((f - 2407) / 5) :
                           f == 2484 ? 14 : 0;
        aps[out].secure = bss[i].encrypt != 0;
        out++;
    }
    os_free(bss);
    return n < 0 ? -1 : out;
}

/* ---- Board GPIO ------------------------------------------------------ */

static void pin_output(int pin)
{
    gpio_set_dir(pin, GPIO_DIR_OUTPUT);
    gpio_set_mode(pin, GPIO_PULL_NONE, GPIO_PULL_LEVEL_NONE);
    gpio_iomap_output(pin, GPIO_IOMAP_OUTPUT);
}

static void pin_input(int pin, int active_low)
{
    gpio_set_dir(pin, GPIO_DIR_INPUT);
    gpio_set_mode(pin, active_low ? GPIO_PULL_UP : GPIO_PULL_DOWN, GPIO_PULL_LEVEL_4_7K);
    gpio_iomap_input(pin, GPIO_IOMAP_INPUT);
}

static void lc_gpio_init(void)
{
#if BOARD_LED_PIN >= 0
    pin_output(BOARD_LED_PIN);
    gpio_set_val(BOARD_LED_PIN, BOARD_LED_ACTIVE_LOW);
#endif
#if BOARD_BUTTON_PIN >= 0
    pin_input(BOARD_BUTTON_PIN, BOARD_BUTTON_ACTIVE_LOW);
#endif
#if defined(BOARD_USER_BUTTON_PIN) && BOARD_USER_BUTTON_PIN >= 0
    pin_input(BOARD_USER_BUTTON_PIN, 1);
#endif
#if defined(BOARD_IR_LED_PIN) && BOARD_IR_LED_PIN >= 0
    pin_output(BOARD_IR_LED_PIN);
    gpio_set_val(BOARD_IR_LED_PIN, 0);
#endif
    (void)pin_output;
    (void)pin_input;
}

void hal_led_set(int on)
{
#if BOARD_LED_PIN >= 0
    gpio_set_val(BOARD_LED_PIN, on ? !BOARD_LED_ACTIVE_LOW : BOARD_LED_ACTIVE_LOW);
#else
    (void)on;
#endif
}

int hal_button_pressed(void)
{
#if BOARD_BUTTON_PIN >= 0
    return gpio_get_val(BOARD_BUTTON_PIN) == (BOARD_BUTTON_ACTIVE_LOW ? 0 : 1);
#else
    return 0;
#endif
}

int hal_user_button_pressed(void)
{
#if defined(BOARD_USER_BUTTON_PIN) && BOARD_USER_BUTTON_PIN >= 0
    return gpio_get_val(BOARD_USER_BUTTON_PIN) == 0;
#else
    return 0;
#endif
}

/* TODO(txw81x): the ADC can read VBAT; needs the board's divider. */
int hal_battery_mv(void)
{
    return -1;
}

/* ---- Entry ----------------------------------------------------------- */

static void lc_task(void *arg)
{
    (void)arg;
    hal_init();
    printf("[lc] free heap %u bytes\n", (unsigned)sysheap_freesize(&sram_heap));
    lc_main();
}

/* Called by the SDK (chip/txw81x/system.c) once the kernel runs. Sets up
 * what the vendor demo's main() does before its application starts, then
 * hands over to LibreCam. */
int main(void)
{
    void *skb_pool = os_malloc(10 * 1024);

    if (!skb_pool) {
        printf("[lc] no memory for the packet pool\n");
        return -1;
    }
    skbpool_init((uint32_t)skb_pool, 10 * 1024, 80, 0);
    syscfg_set_default_val();
    sys_event_init(32);
    sys_event_take(SYS_EVENT(SYS_EVENT_WIFI, 0), lc_sysevt_wifi, 0);
    sys_event_take(SYS_EVENT(SYS_EVENT_NETWORK, 0), lc_sysevt_net, 0);
    tcpip_init(NULL, NULL);
    eloop_init();
    os_task_create("eloop_run", user_eloop_run, NULL, OS_TASK_PRIORITY_NORMAL, 0, NULL, 2048);
    os_task_create("librecam", lc_task, NULL, OS_TASK_PRIORITY_BELOW_NORMAL, 0, NULL, 8192);
    return 0;
}
