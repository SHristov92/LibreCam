/*
 * BSD sockets. Each HAL provides hal_net.h, which pulls in its socket API
 * (lwIP, libc, ...) and defines closesocket() if the platform lacks it.
 */
#ifndef A9_NET_H
#define A9_NET_H

#include <stdint.h>

#include "hal_net.h"

/* Creates a listening TCP socket on all interfaces. Returns fd or -1. */
int net_listen_tcp(uint16_t port, int backlog);

#endif
