#ifndef A9_SERVERS_H
#define A9_SERVERS_H

#include <stdint.h>


int  rtsp_server_start(uint16_t port);
int  rtsp_active_clients(void);

int  http_server_start(uint16_t port);

/* Set by the HTTP server when a config change needs a reboot to apply. */
extern volatile int g_reboot_requested;

#endif
