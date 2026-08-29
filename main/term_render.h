// SPDX-License-Identifier: MIT
//
// Drawing the terminal grid with PAX.

#pragma once

#include <stdbool.h>
#include "pax_gfx.h"
#include "terminal.h"

typedef struct {
    pax_buf_t*        fb;
    pax_font_t const* font;
    float             font_size;
    float             cell_w;
    float             cell_h;
    float             origin_x;
    float             origin_y;
    int               cols;
    int               rows;
    bool              cursor_on;
} term_render_t;

// Work out the cell size and how many of them fit. `scale` is 1 for the native
// 7x9 font and 2 for double size. `reserve_bottom` keeps that many pixels free
// along the bottom edge for the status bar.
void term_render_configure(term_render_t* render, pax_buf_t* fb, int scale, float reserve_bottom);

#define TERM_STATUS_BAR_HEIGHT 18.0f

// Draw the terminal. Only rows the terminal marked dirty are repainted unless
// `force` is set.
void term_render_draw(term_render_t* render, term_t* term, bool force);

// A one line bar along the bottom, drawn over the terminal.
void term_render_status(term_render_t* render, char const* text);
