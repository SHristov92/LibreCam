#ifndef A9_APP_H
#define A9_APP_H

#ifndef A9_FW_VERSION
#define A9_FW_VERSION "0.1.0-dev"
#endif

/* Runs the application. Platform code calls hal_init() first. Never returns. */
void a9_main(void);

#endif
