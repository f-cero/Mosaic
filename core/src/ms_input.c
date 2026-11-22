/* SPDX-License-Identifier: Apache-2.0 */
#include "mosaic/ms_input.h"
#include <string.h>
void ms_input_clear(ms_input_t *in) { if (in) memset(in, 0, sizeof(*in)); }
void ms_input_edge(ms_input_t *in, uint16_t now)
{
    if (!in) return;
    in->pressed  = (uint16_t)(now & ~in->held);
    in->released = (uint16_t)(~now & in->held);
    in->held = now;
}
