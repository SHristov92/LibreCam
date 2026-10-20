#include "net_compat.h"

#include <string.h>

int net_listen_tcp(uint16_t port, int backlog)
{
    struct sockaddr_in a;
    int one = 1;
    int s = socket(AF_INET, SOCK_STREAM, 0);

    if (s < 0)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, backlog) < 0) {
        closesocket(s);
        return -1;
    }
    return s;
}
