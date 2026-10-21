/*
 * Minimal mDNS responder (RFC 6762) with DNS-SD (RFC 6763): answers for
 * <host>.local and advertises the web UI (_http._tcp) and the stream
 * (_rtsp._tcp), so browsers, phones and tools like Home Assistant find the
 * camera by name. The message code is pure, for unit tests; mdns_start()
 * runs it on a socket.
 */
#ifndef LC_MDNS_H
#define LC_MDNS_H

#include <stddef.h>
#include <stdint.h>

#define MDNS_PORT     5353
#define MDNS_MSG_MAX  1024

typedef struct {
    const char *host;        /* DNS label without ".local" */
    uint8_t     ip[4];
    uint16_t    http_port;
    uint16_t    rtsp_port;
    const char *rtsp_path;
} mdns_info_t;

/* Turns a device name into a host label: lowercase letters, digits and
 * '-', at most 63 characters. Falls back to "librecam" if nothing is left. */
void   mdns_hostname(const char *device_name, char *out, size_t cap);

/* Builds the answer to the query q. legacy: the query came from a port
 * other than 5353 (a plain DNS resolver), which wants its id and question
 * echoed and short TTLs. Returns the length, 0 if there is nothing to say. */
size_t mdns_reply(const uint8_t *q, size_t qlen, const mdns_info_t *info, int legacy,
                  uint8_t *out, size_t cap);

/* Builds an unsolicited announcement of every record. */
size_t mdns_announce(const mdns_info_t *info, uint8_t *out, size_t cap);

/* Starts the responder task; it follows the current IP address and the
 * configured device name. Returns 0 on success. */
int    mdns_start(void);

#endif
