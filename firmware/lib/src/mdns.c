/* mDNS responder task: the socket side of mdns_msg.c. */
#define _DEFAULT_SOURCE   /* struct ip_mreq under -std=c99 (glibc) */
#include "lc/config.h"
#include "lc/hal.h"
#include "lc/mdns.h"
#include "lc/net.h"

#include <stdio.h>
#include <string.h>

#define MDNS_GROUP "224.0.0.251"

static int parse_ip(const char *s, uint8_t ip[4])
{
    unsigned a, b, c, d;

    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return -1;
    ip[0] = (uint8_t)a;
    ip[1] = (uint8_t)b;
    ip[2] = (uint8_t)c;
    ip[3] = (uint8_t)d;
    return (a | b | c | d) ? 0 : -1;
}

static void mdns_task(void *arg)
{
    static uint8_t in[MDNS_MSG_MAX], out[MDNS_MSG_MAX];
    struct sockaddr_in me, group, from;
    struct ip_mreq mreq;
    struct timeval tv = { 1, 0 };
    unsigned char ttl = 255;
    int one = 1, s, announce = 0;
    uint8_t last_ip[4] = { 0 };
    char host[64], ipstr[16];
    mdns_info_t info;

    (void)arg;
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        printf("[mdns] no socket\n");
        return;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port = htons(MDNS_PORT);
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&me, sizeof(me)) < 0 ||
        setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        printf("[mdns] can't listen on port %d\n", MDNS_PORT);
        closesocket(s);
        return;
    }
    /* Responders drop mDNS packets sent with any other IP TTL. */
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&group, 0, sizeof(group));
    group.sin_family = AF_INET;
    group.sin_port = htons(MDNS_PORT);
    group.sin_addr.s_addr = inet_addr(MDNS_GROUP);

    mdns_hostname(g_config.device_name, host, sizeof(host));
    info.host = host;
    info.http_port = g_config.http_port;
    info.rtsp_port = g_config.rtsp_port;
    info.rtsp_path = g_config.rtsp_path;
    printf("[mdns] http://%s.local/\n", host);

    for (;;) {
        socklen_t fl = sizeof(from);
        int n;

        /* (Re)announce twice, a second apart, whenever the address changes. */
        hal_wifi_ip(ipstr, sizeof(ipstr));
        if (parse_ip(ipstr, info.ip) == 0 && memcmp(info.ip, last_ip, 4)) {
            memcpy(last_ip, info.ip, 4);
            announce = 2;
        }
        if (announce && last_ip[0]) {
            size_t len = mdns_announce(&info, out, sizeof(out));
            if (len)
                sendto(s, out, len, 0, (struct sockaddr *)&group, sizeof(group));
            announce--;
        }

        n = recvfrom(s, in, sizeof(in), 0, (struct sockaddr *)&from, &fl);
        if (n > 0 && last_ip[0]) {
            int legacy = ntohs(from.sin_port) != MDNS_PORT;
            size_t len = mdns_reply(in, (size_t)n, &info, legacy, out, sizeof(out));

            if (len)
                sendto(s, out, len, 0, legacy ? (struct sockaddr *)&from : (struct sockaddr *)&group,
                       legacy ? fl : sizeof(group));
        }
    }
}

int mdns_start(void)
{
    return hal_task_start("mdns", mdns_task, NULL, 3072, 3);
}
