/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — tools/net.h: TCP transport for MSP/1 and the server/client entry
 * points. Frames are ms_link frames; the transport is a byte stream, so the
 * same code drives a socket, a pipe or a UART. */
#ifndef MOSAIC_TOOLS_NET_H
#define MOSAIC_TOOLS_NET_H
#include "mosaic/ms_link.h"
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int      fd;
    uint8_t  buf[MS_LINK_MTU + 16];
    size_t   have;
} ms_io_conn_t;

int  ms_io_init(void);
void ms_io_shutdown(void);
int  ms_io_listen(const char *bind_addr, int port, int *out_port);
int  ms_io_accept(int lfd);                 /* accepted fd, or -1 */
int  ms_io_connect(const char *host, int port);
void ms_io_close(int fd);
int  ms_io_nonblock(int fd, int on);
int  ms_io_fill(ms_io_conn_t *c);           /* >0 bytes, 0 nothing, -1 closed */
int  ms_io_shift(ms_io_conn_t *c, ms_link_frame_t *f);  /* 1 frame, 0 need more, -2 bad */
int  ms_io_send(int fd, const ms_link_frame_t *f);
uint64_t ms_io_now_us(void);
void ms_io_sleep_us(uint64_t us);

typedef struct {
    const char *cart;
    const char *saves;      /* console save image, optional */
    const char *bind_addr;  /* default 0.0.0.0 */
    int  port;              /* 0 = pick one */
    int  frames;            /* 0 = until stopped */
    int  fps;               /* default 60 */
    int  quiet, verbose, fast;
    int  wait_clients;      /* hold frame 0 until this many are live */
    int *port_out;          /* written once bound */
    volatile int *ready;    /* set to 1 once listening */
} ms_server_opts_t;

typedef struct {
    int      rc, port, clients;
    uint32_t frames, crc_errors, hash_mismatches, overflow, keys_sent;
    uint64_t final_hash;
    uint32_t client_frames[MS_LINK_MAX_NODES];
    uint64_t client_hash[MS_LINK_MAX_NODES];
} ms_server_result_t;

int ms_server_run(const ms_server_opts_t *o, ms_server_result_t *res);
int mosaic_server_main(int argc, char **argv);

typedef struct {
    const char *host, *cart;
    int port, frames, quiet;
    uint16_t hold;
} ms_client_opts_t;

typedef struct {
    int      rc, node;
    uint32_t frames, overflow, crc_errors;
    uint64_t final_hash;
} ms_client_result_t;

int ms_client_run(const ms_client_opts_t *o, ms_client_result_t *res);
int mosaic_client_main(int argc, char **argv);

#endif
