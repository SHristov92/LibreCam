/*
 * Wall clock. The camera has no RTC battery, so time comes from SNTP once
 * on a network with internet access, or from the web UI (the browser posts
 * its clock on load, which also works on the setup access point).
 */
#ifndef A9_CLOCK_H
#define A9_CLOCK_H

#include <stdint.h>

typedef struct {
    int year, month, day, hour, min, sec;   /* month 1..12 */
} a9_tm_t;

/* Sets UTC (seconds since 1970) and the local offset in minutes east of UTC. */
void     a9_time_set(uint32_t utc, int tz_min);
void     a9_time_set_tz(int tz_min);
/* 1 once the clock was set since boot. */
int      a9_time_valid(void);
/* Current UTC seconds, 0 if unknown. */
uint32_t a9_time_utc(void);
/* Local time; returns -1 (and fills nothing) if the clock isn't set. */
int      a9_time_local(a9_tm_t *tm);
/* FatFs timestamp (local time), or 2025-01-01 when unknown. */
uint32_t a9_fattime(void);

/* Queries pool.ntp.org once; blocks up to ~3 s. Returns 0 on success. */
int      a9_sntp_sync(void);

/* Breaks UTC seconds + offset into a calendar date (exposed for tests). */
void     a9_time_breakdown(uint32_t t, a9_tm_t *tm);

#endif
