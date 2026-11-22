/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_device.h: modular console descriptors (device.conf).
 * A console is a core plus any mix of CYD displays, audio nodes, I/O and
 * storage, each on its own SPI chip-select. */
#ifndef MOSAIC_MS_DEVICE_H
#define MOSAIC_MS_DEVICE_H
#include "mosaic/ms_limits.h"
#include "mosaic/ms_link.h"
#include <stdint.h>
#include <stddef.h>

typedef struct {
    char name[24];
    uint8_t role, cs_pin, irq_pin, caps;
    uint16_t width, height;
} ms_node_t;

typedef struct {
    char name[32], chip[24];
    uint8_t node_count, core_node;
    ms_node_t nodes[MS_LINK_MAX_NODES];
    uint32_t spi_hz;
    uint8_t storage_sd, storage_cart, wifi, bt;
} ms_device_t;

int  ms_device_parse(ms_device_t *d, const char *text, size_t bytes);
int  ms_device_add(ms_device_t *d, const char *name, int role, int cs);
int  ms_device_role_count(const ms_device_t *d, int role);
const char *ms_device_summary(const ms_device_t *d, char *buf, size_t cap);

#endif
