// SPDX-License-Identifier: MIT

#include "keycaps.h"
#include "pax_text.h"

// What is printed on the six keys, F1 first. The shapes come out of the
// terminal font, which carries the Geometric Shapes and Miscellaneous Symbols
// blocks, so no separate artwork is needed.
static struct {
    char const* glyph;
    pax_col_t   color;
} const keycaps[6] = {
    {"✗", 0xFFE05C4A},  // F1: red cross
    {"▲", 0xFFE08A2A},  // F2: orange triangle
    {"■", 0xFFE0C83C},  // F3: yellow square
    {"●", 0xFF5CE07A},  // F4: green circle
    {"☁", 0xFF5C9CE0},  // F5: blue cloud
    {"◆", 0xFFB06CE0},  // F6: purple diamond
};

float keycap_draw_text(pax_buf_t* fb, pax_col_t color, pax_font_t const* font, float size, float x, float y,
                       char const* text) {
    char const* run = text;
    for (char const* cursor = text;; cursor++) {
        unsigned char byte   = (unsigned char)*cursor;
        bool          marker = byte >= 1 && byte <= 6;
        if (byte && !marker) {
            continue;
        }
        if (cursor > run) {
            // Drawn by length, so the run needs no copy to be terminated.
            x += pax_draw_text_adv(fb, color, font, size, x, y, run, (size_t)(cursor - run), PAX_ALIGN_BEGIN,
                                   PAX_ALIGN_BEGIN, -1)
                     .x0;
        }
        if (!byte) {
            return x;
        }
        x   += pax_draw_text(fb, keycaps[byte - 1].color, font, size, x, y, keycaps[byte - 1].glyph).x;
        run  = cursor + 1;
    }
}
