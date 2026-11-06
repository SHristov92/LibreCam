/* Socket API on the TXW81x: lwIP 2.x BSD sockets from Taixin's SDK. */
#ifndef LC_HAL_NET_H
#define LC_HAL_NET_H

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

#ifndef closesocket
#define closesocket(s) lwip_close(s)
#endif

#endif
