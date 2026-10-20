/* Socket API for the Linux host build. */
#ifndef A9_HAL_NET_H
#define A9_HAL_NET_H

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#define closesocket(s) close(s)

#endif
