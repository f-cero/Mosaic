/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MS_PORT_ESP32_H
#define MS_PORT_ESP32_H
#include "mosaic/ms_core.h"
#include "mosaic/ms_device.h"
#ifdef __cplusplus
extern "C" {
#endif
int  ms_port_init(ms_core_t *core, const ms_device_t *dev);
void ms_port_frame(ms_core_t *core, ms_game_t *game);
void ms_port_audio_sink(void *user, const int16_t *stereo, int frames);
int  ms_port_link_start(const ms_device_t *dev);
int  ms_port_net_poll(void);
#ifdef __cplusplus
}
#endif
#endif
