/* SPDX-License-Identifier: Apache-2.0 */
/* Mosaic — ms_input.h: the console pad and touch port. */
#ifndef MOSAIC_MS_INPUT_H
#define MOSAIC_MS_INPUT_H
#include <stdint.h>

typedef enum {
    MS_BTN_UP = 1u<<0, MS_BTN_DOWN = 1u<<1, MS_BTN_LEFT = 1u<<2,
    MS_BTN_RIGHT = 1u<<3, MS_BTN_A = 1u<<4, MS_BTN_B = 1u<<5,
    MS_BTN_START = 1u<<6, MS_BTN_SELECT = 1u<<7,
    MS_BTN_L = 1u<<8, MS_BTN_R = 1u<<9, MS_BTN_MENU = 1u<<10
} ms_btn_t;

typedef struct {
    uint16_t held, pressed, released;
    int16_t  touch_x, touch_y;   /* -1 when untouched */
    uint8_t  touch_active;
    uint8_t  pad_count;          /* connected pads (0 = built-in) */
} ms_input_t;

void ms_input_clear(ms_input_t *in);
void ms_input_edge(ms_input_t *in, uint16_t now_held); /* derive edges */

#endif
