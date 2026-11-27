/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — tools/net_io.c: blocking/non-blocking TCP around ms_link frames. */
#include "net.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define MS_CLOSE(fd) closesocket(fd)
#  define MS_EWOULDBLOCK(e) ((e) == WSAEWOULDBLOCK)
typedef int socklen_t;
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <signal.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <time.h>
#  include <unistd.h>
#  define MS_CLOSE(fd) close(fd)
#  define MS_EWOULDBLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#endif

int ms_io_init(void)
{
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1;
#else
    signal(SIGPIPE, SIG_IGN);   /* a dropped peer is an error, not a crash */
    return 0;
#endif
}

void ms_io_shutdown(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

int ms_io_listen(const char *bind_addr, int port, int *out_port)
{
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (!bind_addr || !*bind_addr) bind_addr = "0.0.0.0";
    if (inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1) { MS_CLOSE(fd); return -1; }
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { MS_CLOSE(fd); return -1; }
    if (listen(fd, 8) != 0) { MS_CLOSE(fd); return -1; }
    socklen_t sl = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &sl) == 0 && out_port)
        *out_port = ntohs(a.sin_port);
    ms_io_nonblock(fd, 1);
    return fd;
}

int ms_io_accept(int lfd)
{
    int fd = (int)accept(lfd, NULL, NULL);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    ms_io_nonblock(fd, 1);
    return fd;
}

int ms_io_connect(const char *host, int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        struct hostent *he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) return -1;
        memcpy(&a.sin_addr, he->h_addr_list[0], sizeof a.sin_addr);
    }
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { MS_CLOSE(fd); return -1; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return fd;
}

void ms_io_close(int fd)
{
    if (fd >= 0) MS_CLOSE(fd);
}

int ms_io_nonblock(int fd, int on)
{
#ifdef _WIN32
    u_long v = on ? 1u : 0u;
    return ioctlsocket(fd, FIONBIO, &v) == 0 ? 0 : -1;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, fl);
#endif
}

int ms_io_fill(ms_io_conn_t *c)
{
    if (!c || c->fd < 0) return -1;
    if (c->have >= sizeof c->buf) return 0;
    int n = (int)recv(c->fd, (char *)c->buf + c->have, (int)(sizeof c->buf - c->have), 0);
    if (n > 0) { c->have += (size_t)n; return n; }
    if (n == 0) return -1;
#ifdef _WIN32
    return MS_EWOULDBLOCK(WSAGetLastError()) ? 0 : -1;
#else
    return MS_EWOULDBLOCK(errno) ? 0 : -1;
#endif
}

int ms_io_shift(ms_io_conn_t *c, ms_link_frame_t *f)
{
    if (!c || c->have < 2) return 0;
    size_t off = 0;
    while (off + 1 < c->have && !(c->buf[off] == 0xA5 && c->buf[off + 1] == 0x5A)) off++;
    if (off) { memmove(c->buf, c->buf + off, c->have - off); c->have -= off; }
    if (c->have < 10) return 0;
    const uint16_t len = (uint16_t)(c->buf[6] | ((uint16_t)c->buf[7] << 8));
    if (len > MS_LINK_MTU) {
        memmove(c->buf, c->buf + 1, c->have - 1);
        c->have--;
        return -2;
    }
    const size_t total = 8u + len + 2u;
    if (c->have < total) return 0;
    if (ms_link_decode(f, c->buf, total) != 0) {
        memmove(c->buf, c->buf + 1, c->have - 1);
        c->have--;
        return -2;
    }
    memmove(c->buf, c->buf + total, c->have - total);
    c->have -= total;
    return 1;
}

int ms_io_send(int fd, const ms_link_frame_t *f)
{
    uint8_t out[MS_LINK_MTU + 10];
    if (fd < 0 || !f) return -1;
    const size_t n = ms_link_encode(f, out, sizeof out);
    if (!n) return -1;
    size_t off = 0;
    int spins = 0;
    while (off < n) {
        int k = (int)send(fd, (const char *)out + off, (int)(n - off), 0);
        if (k > 0) { off += (size_t)k; continue; }
#ifdef _WIN32
        if (k < 0 && MS_EWOULDBLOCK(WSAGetLastError()) && ++spins < 100) {
#else
        if (k < 0 && MS_EWOULDBLOCK(errno) && ++spins < 100) {
#endif
            ms_io_sleep_us(1000);
            continue;
        }
        return -1;
    }
    return 0;
}

uint64_t ms_io_now_us(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64() * 1000ull;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
#endif
}

void ms_io_sleep_us(uint64_t us)
{
#ifdef _WIN32
    Sleep((DWORD)(us / 1000u));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000ull);
    ts.tv_nsec = (long)((us % 1000000ull) * 1000ull);
    nanosleep(&ts, NULL);
#endif
}
