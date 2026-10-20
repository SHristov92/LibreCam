#ifndef LC_APP_H
#define LC_APP_H

#ifndef LC_FW_VERSION
#define LC_FW_VERSION "0.1.0-dev"
#endif

/* Runs the application. Platform code calls hal_init() first. Never returns. */
void lc_main(void);

/* Last battery reading in mV (-1 = unknown), refreshed by lc_main. */
extern volatile int g_vbat_mv;
/* 1 when g_vbat_mv is below the board's BOARD_VBAT_LOW_MV threshold. */
int lc_low_battery(void);

#endif
