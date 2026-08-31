// SPDX-License-Identifier: MIT
//
// The badge's six function keys carry no numbers: each keycap is a coloured
// shape. Text that names one embeds the matching marker below, and
// keycap_draw_text() paints that byte as the shape in the keycap's colour, so
// the screen names the key the same way the hardware does.

#pragma once

#include "pax_gfx.h"

#define KEYCAP_F1 "\x01"  // red cross
#define KEYCAP_F2 "\x02"  // orange triangle
#define KEYCAP_F3 "\x03"  // yellow square
#define KEYCAP_F4 "\x04"  // green circle
#define KEYCAP_F5 "\x05"  // blue cloud
#define KEYCAP_F6 "\x06"  // purple diamond

// Like pax_draw_text, except the markers above draw as their keycap. Returns
// the x coordinate just past the end of the text.
float keycap_draw_text(pax_buf_t* fb, pax_col_t color, pax_font_t const* font, float size, float x, float y,
                       char const* text);
