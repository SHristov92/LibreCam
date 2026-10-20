#ifndef A9_SERVERS_H
#define A9_SERVERS_H

#include <stdint.h>

/* Largest JPEG frame accepted; each RTSP client and the snapshot handler
 * allocate one buffer of this size, so tune it to the target's RAM. */
#ifndef FRAME_BUF
#define FRAME_BUF (96 * 1024)
#endif

int  rtsp_server_start(uint16_t port);
int  rtsp_active_clients(void);

int  http_server_start(uint16_t port);

/* Set by the HTTP server when a config change needs a reboot to apply. */
extern volatile int g_reboot_requested;

#endif
