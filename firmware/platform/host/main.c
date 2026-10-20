/*
 * Host entry point. Ports default to 8080/8554 so no root is needed;
 * override with A9_HTTP_PORT / A9_RTSP_PORT.
 */
#include "app.h"
#include "config.h"
#include "hal.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    const char *http = getenv("A9_HTTP_PORT");
    const char *rtsp = getenv("A9_RTSP_PORT");

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);   /* lwIP has no SIGPIPE; match it */
    hal_init();
    config_load();
    config_set(&g_config, "http_port", http ? http : "8080");
    config_set(&g_config, "rtsp_port", rtsp ? rtsp : "8554");
    config_save();
    a9_main();
    return 0;
}
