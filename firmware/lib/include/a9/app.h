#ifndef A9_APP_H
#define A9_APP_H

#ifndef A9_FW_VERSION
#define A9_FW_VERSION "0.1.0-dev"
#endif

/* Runs the application. Platform code calls hal_init() first. Never returns. */
void a9_main(void);

/* Last battery reading in mV (-1 = unknown), refreshed by a9_main. */
extern volatile int g_vbat_mv;
/* 1 when g_vbat_mv is below the board's BOARD_VBAT_LOW_MV threshold. */
int a9_low_battery(void);

#endif
