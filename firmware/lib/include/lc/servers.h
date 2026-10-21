#ifndef LC_SERVERS_H
#define LC_SERVERS_H

#include <stdint.h>


int  rtsp_server_start(uint16_t port);
int  rtsp_active_clients(void);

int  http_server_start(uint16_t port);
/* Browsers watching the live view (/stream). */
int  http_stream_clients(void);

/* Set by the HTTP server when a config change needs a reboot to apply. */
extern volatile int g_reboot_requested;

#endif
