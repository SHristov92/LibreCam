/*
 * BSD sockets come from lwIP on the camera and from libc on the host.
 */
#ifndef A9_NET_COMPAT_H
#define A9_NET_COMPAT_H

#include <stdint.h>

#if defined(A9_PLATFORM_BK7252)
#include <sys/socket.h>      /* RT-Thread SAL, backed by lwIP */
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
/* closesocket() is provided by SAL/lwIP. */
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket(s) close(s)
#endif

/* Creates a listening TCP socket on all interfaces. Returns fd or -1. */
int net_listen_tcp(uint16_t port, int backlog);

#endif
