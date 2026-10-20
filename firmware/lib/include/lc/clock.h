/*
 * Wall clock. The camera has no RTC battery, so time comes from SNTP once
 * on a network with internet access, or from the web UI (the browser posts
 * its clock on load, which also works on the setup access point).
 */
#ifndef LC_CLOCK_H
#define LC_CLOCK_H

#include <stdint.h>

typedef struct {
    int year, month, day, hour, min, sec;   /* month 1..12 */
} lc_tm_t;

/* Sets UTC (seconds since 1970) and the local offset in minutes east of UTC. */
void     lc_time_set(uint32_t utc, int tz_min);
void     lc_time_set_tz(int tz_min);
/* 1 once the clock was set since boot. */
int      lc_time_valid(void);
/* Current UTC seconds, 0 if unknown. */
uint32_t lc_time_utc(void);
/* Local time; returns -1 (and fills nothing) if the clock isn't set. */
int      lc_time_local(lc_tm_t *tm);
/* FatFs timestamp (local time), or 2025-01-01 when unknown. */
uint32_t lc_fattime(void);

/* Queries pool.ntp.org once; blocks up to ~3 s. Returns 0 on success. */
int      lc_sntp_sync(void);

/* Breaks UTC seconds + offset into a calendar date (exposed for tests). */
void     lc_time_breakdown(uint32_t t, lc_tm_t *tm);

#endif
