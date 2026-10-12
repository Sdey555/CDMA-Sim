#define _POSIX_C_SOURCE 200809L
#include "udp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int udp_make_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        perror("socket");
    return fd;
}

int udp_bind_port(int fd, int port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return -1;
    }
    return 0;
}

int udp_make_address(const char *ip, int port, void *out_addr, size_t addr_size)
{
    if (addr_size < sizeof(struct sockaddr_in) || !out_addr)
        return -1;

    struct sockaddr_in *addr = (struct sockaddr_in *)out_addr;
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &addr->sin_addr) != 1)
        return -1;

    return 0;
}
