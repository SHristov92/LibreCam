#include "lc/clock.h"
#include "lc/hal.h"

#include <stdio.h>
#include <string.h>

#include "lc/net.h"

#define NTP_HOST      "pool.ntp.org"
#define NTP_UNIX_DIFF 2208988800u   /* seconds from 1900 to 1970 */

static volatile uint32_t g_utc_at_set, g_ms_at_set;
static volatile int g_valid, g_tz_min;

void lc_time_set(uint32_t utc, int tz_min)
{
    g_ms_at_set = hal_millis();
    g_utc_at_set = utc;
    g_tz_min = tz_min;
    g_valid = 1;
}

void lc_time_set_tz(int tz_min)
{
    g_tz_min = tz_min;
}

int lc_time_valid(void)
{
    return g_valid;
}

uint32_t lc_time_utc(void)
{
    if (!g_valid)
        return 0;
    return g_utc_at_set + (hal_millis() - g_ms_at_set) / 1000;
}

/* Days since 1970-01-01 to civil date (Howard Hinnant's algorithm). */
void lc_time_breakdown(uint32_t t, lc_tm_t *tm)
{
    int64_t days = (int64_t)(t / 86400), z, era, doe, yoe, doy, mp;
    uint32_t secs = t % 86400;

    z = days + 719468;
    era = z / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    tm->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    tm->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    tm->year = (int)(yoe + era * 400 + (tm->month <= 2));
    tm->hour = (int)(secs / 3600);
    tm->min = (int)(secs / 60 % 60);
    tm->sec = (int)(secs % 60);
}

int lc_time_local(lc_tm_t *tm)
{
    if (!g_valid)
        return -1;
    lc_time_breakdown((uint32_t)((int64_t)lc_time_utc() + g_tz_min * 60), tm);
    return 0;
}

uint32_t lc_fattime(void)
{
    lc_tm_t t;

    if (lc_time_local(&t) != 0 || t.year < 1980)
        return ((uint32_t)(2025 - 1980) << 25) | (1u << 21) | (1u << 16);
    return ((uint32_t)(t.year - 1980) << 25) | ((uint32_t)t.month << 21) |
           ((uint32_t)t.day << 16) | ((uint32_t)t.hour << 11) |
           ((uint32_t)t.min << 5) | ((uint32_t)t.sec >> 1);
}

int lc_sntp_sync(void)
{
    struct hostent *he = gethostbyname(NTP_HOST);
    struct sockaddr_in a;
    struct timeval tv = { 3, 0 };
    uint8_t pkt[48];
    int s, n;

    if (!he || !he->h_addr_list[0])
        return -1;
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(123);
    memcpy(&a.sin_addr, he->h_addr_list[0], sizeof(a.sin_addr));
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B;                               /* LI 0, version 3, client */
    n = sendto(s, (const char *)pkt, sizeof(pkt), 0, (struct sockaddr *)&a, sizeof(a));
    if (n == (int)sizeof(pkt))
        n = recv(s, (char *)pkt, sizeof(pkt), 0);
    closesocket(s);
    if (n < 48 || (pkt[0] & 0x07) != 4)          /* mode 4 = server */
        return -1;
    {
        uint32_t secs = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
                        ((uint32_t)pkt[42] << 8) | pkt[43];
        if (secs < NTP_UNIX_DIFF)
            return -1;
        lc_time_set(secs - NTP_UNIX_DIFF, g_tz_min);
    }
    return 0;
}
