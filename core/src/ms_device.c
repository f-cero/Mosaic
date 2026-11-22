/* SPDX-License-Identifier: Apache-2.0 */
#include "mosaic/ms_device.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int ms_device_add(ms_device_t *d, const char *name, int role, int cs)
{
    if (!d || d->node_count >= (uint8_t)MS_LINK_MAX_NODES) return -1;
    ms_node_t *n = &d->nodes[d->node_count++];
    memset(n, 0, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->role = (uint8_t)role; n->cs_pin = (uint8_t)cs;
    if (role == 2) { n->width = MS_SCREEN_W; n->height = MS_SCREEN_H; }
    return 0;
}

int ms_device_parse(ms_device_t *d, const char *text, size_t bytes)
{
    (void)bytes;
    if (!d || !text) return -1;
    memset(d, 0, sizeof(*d));
    snprintf(d->name, sizeof(d->name), "mosaic");
    snprintf(d->chip, sizeof(d->chip), "esp32s3");
    d->spi_hz = MS_LINK_SPI_HZ;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[160];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len); line[len] = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!strncmp(s, "name=", 5)) snprintf(d->name, sizeof(d->name), "%s", s + 5);
        else if (!strncmp(s, "chip=", 5)) snprintf(d->chip, sizeof(d->chip), "%s", s + 5);
        else if (!strncmp(s, "spi_hz=", 7)) d->spi_hz = (uint32_t)strtoul(s + 7, NULL, 0);
        else if (!strncmp(s, "wifi=", 5)) d->wifi = (uint8_t)atoi(s + 5);
        else if (!strncmp(s, "bt=", 3)) d->bt = (uint8_t)atoi(s + 3);
        else if (!strncmp(s, "sd=", 3)) d->storage_sd = (uint8_t)atoi(s + 3);
        else if (!strncmp(s, "cart=", 5)) d->storage_cart = (uint8_t)atoi(s + 5);
        else if (!strncmp(s, "node ", 5)) {
            char name[24], role[16]; int cs = 0;
            if (sscanf(s + 5, "%23s role=%15s cs=%d", name, role, &cs) >= 2) {
                int r = !strcmp(role, "core") ? MS_ROLE_CORE : !strcmp(role, "ppu") ? MS_ROLE_PPU
                      : !strcmp(role, "audio") ? MS_ROLE_AUDIO : !strcmp(role, "io") ? MS_ROLE_IO
                      : !strcmp(role, "storage") ? MS_ROLE_STORAGE : 0;
                ms_device_add(d, name, r, cs);
                if (r == MS_ROLE_CORE) d->core_node = (uint8_t)(d->node_count - 1);
            }
        }
        p = e ? e + 1 : p + len;
    }
    if (!d->node_count) { ms_device_add(d, "main", MS_ROLE_CORE, 0); d->core_node = 0; }
    return 0;
}

int ms_device_role_count(const ms_device_t *d, int role)
{
    int n = 0;
    for (int i = 0; d && i < d->node_count; i++) if (d->nodes[i].role == role) n++;
    return n;
}

const char *ms_device_summary(const ms_device_t *d, char *buf, size_t cap)
{
    if (!buf || !cap) return "";
    size_t o = 0;
    o += (size_t)snprintf(buf + o, cap - o, "%s (%s): ", d->name, d->chip);
    for (int i = 0; i < d->node_count && o < cap; i++)
        o += (size_t)snprintf(buf + o, cap - o, "%s/%s%s", d->nodes[i].name,
                              ms_link_role_name(d->nodes[i].role),
                              i + 1 < d->node_count ? ", " : "");
    return buf;
}
