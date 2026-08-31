// SPDX-License-Identifier: MIT

#include "term_render.h"
#include <string.h>
#include "keycaps.h"
#include "pax_fonts.h"
#include "pax_text.h"
#include "terminal_font.h"

#define RUN_MAX (TERM_MAX_COLS + 1)

// The terminal stores codepoints; PAX draws UTF-8. Anything the font cannot
// draw becomes U+FFFD, because PAX advances by nothing for a missing glyph and
// the rest of the line would slide out of its cells.
static int encode_utf8(uint32_t cp, char* out) {
    if (cp < 0x20 || !terminal_font_has_glyph(cp)) {
        cp = 0xFFFD;
    }
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

void term_render_configure(term_render_t* render, pax_buf_t* fb, int scale, float reserve_bottom) {
    if (scale < 1) {
        scale = 1;
    }
    render->fb        = fb;
    render->font      = terminal_font;
    // The glyphs are 8x16; whole multiples keep them crisp.
    render->font_size = (float)TERMINAL_FONT_HEIGHT * (float)scale;
    render->cell_w    = (float)TERMINAL_FONT_WIDTH * (float)scale;
    render->cell_h    = render->font_size;

    float width  = pax_buf_get_width(fb);
    float height = pax_buf_get_height(fb) - reserve_bottom;
    render->cols = (int)(width / render->cell_w);
    render->rows = (int)(height / render->cell_h);
    if (render->cols > TERM_MAX_COLS) {
        render->cols = TERM_MAX_COLS;
    }
    if (render->rows > TERM_MAX_ROWS) {
        render->rows = TERM_MAX_ROWS;
    }
    // Centre whatever is left over, so the text does not hug one edge.
    render->origin_x = (width - render->cols * render->cell_w) / 2;
    render->origin_y = (height - render->rows * render->cell_h) / 2;
}

static void resolve_colors(term_cell_t const* cell, uint32_t* fg, uint32_t* bg) {
    *fg = cell->fg;
    *bg = cell->bg;
    if (cell->attr & TERM_ATTR_REVERSE) {
        uint32_t swap = *fg;
        *fg           = *bg;
        *bg           = swap;
    }
    if (cell->attr & TERM_ATTR_INVISIBLE) {
        *fg = *bg;
    }
    if (cell->attr & TERM_ATTR_DIM) {
        // Halve each channel, keeping the alpha.
        *fg = (*fg & 0xFF000000) | ((*fg >> 1) & 0x7F7F7F);
    }
}

static void draw_row(term_render_t* render, term_t* term, int row) {
    float y = render->origin_y + row * render->cell_h;

    int col = 0;
    while (col < render->cols) {
        term_cell_t const* first = term_cell(term, col, row);

        // The right half of a double width character carries no glyph of its
        // own; the left half already painted across both cells.
        if (first->attr & TERM_ATTR_CONT) {
            col++;
            continue;
        }

        // A double width character is drawn on its own, because its glyph is 16
        // pixels wide, which is exactly two cells at any whole scale.
        if (first->attr & TERM_ATTR_WIDE) {
            uint32_t wide_fg, wide_bg;
            resolve_colors(first, &wide_fg, &wide_bg);
            float x     = render->origin_x + col * render->cell_w;
            float width = render->cell_w * 2;
            char  text[5];
            int   length = encode_utf8(first->cp ? first->cp : ' ', text);
            text[length] = '\0';
            pax_simple_rect(render->fb, wide_bg, x, y, width, render->cell_h);
            pax_draw_text(render->fb, wide_fg, render->font, render->font_size, x, y, text);
            if (first->attr & TERM_ATTR_UNDERLINE) {
                pax_simple_rect(render->fb, wide_fg, x, y + render->cell_h - 1, width, 1);
            }
            if (first->attr & TERM_ATTR_STRIKE) {
                pax_simple_rect(render->fb, wide_fg, x, y + render->cell_h / 2, width, 1);
            }
            col += 2;
            continue;
        }

        uint32_t fg, bg;
        resolve_colors(first, &fg, &bg);
        uint8_t attr = first->attr;

        // Collect the longest run that shares colours and attributes.
        char text[RUN_MAX * 4 + 1];
        int  length = 0;
        int  begin  = col;
        while (col < render->cols) {
            term_cell_t const* cell = term_cell(term, col, row);
            uint32_t           cell_fg, cell_bg;
            if (cell->attr & (TERM_ATTR_WIDE | TERM_ATTR_CONT)) {
                break;
            }
            resolve_colors(cell, &cell_fg, &cell_bg);
            if (cell_fg != fg || cell_bg != bg || cell->attr != attr || length + 4 >= (int)sizeof(text)) {
                break;
            }
            length += encode_utf8(cell->cp ? cell->cp : ' ', text + length);
            col++;
        }
        text[length] = '\0';

        float x     = render->origin_x + begin * render->cell_w;
        float width = (col - begin) * render->cell_w;
        pax_simple_rect(render->fb, bg, x, y, width, render->cell_h);
        pax_draw_text(render->fb, fg, render->font, render->font_size, x, y, text);
        if (attr & TERM_ATTR_UNDERLINE) {
            pax_simple_rect(render->fb, fg, x, y + render->cell_h - 1, width, 1);
        }
        if (attr & TERM_ATTR_STRIKE) {
            pax_simple_rect(render->fb, fg, x, y + render->cell_h / 2, width, 1);
        }
    }
}

void term_render_draw(term_render_t* render, term_t* term, bool force) {
    static int last_cursor_row = -1;

    int cursor_col = 0;
    int cursor_row = 0;
    term_cursor(term, &cursor_col, &cursor_row);

    for (int row = 0; row < render->rows; row++) {
        bool dirty = force || term_row_dirty(term, row);
        // The cell the cursor left has to be repainted even when the terminal
        // itself did not change it.
        if (!dirty && (row == cursor_row || row == last_cursor_row)) {
            dirty = true;
        }
        if (dirty) {
            draw_row(render, term, row);
        }
    }

    if (term_cursor_visible(term) && render->cursor_on && cursor_row < render->rows && cursor_col < render->cols) {
        term_cell_t const* cell = term_cell(term, cursor_col, cursor_row);
        // Show it on the character itself, not on its trailing half.
        if ((cell->attr & TERM_ATTR_CONT) && cursor_col > 0) {
            cursor_col--;
            cell = term_cell(term, cursor_col, cursor_row);
        }
        uint32_t fg, bg;
        resolve_colors(cell, &fg, &bg);
        float x     = render->origin_x + cursor_col * render->cell_w;
        float y     = render->origin_y + cursor_row * render->cell_h;
        float width = (cell->attr & TERM_ATTR_WIDE) ? render->cell_w * 2 : render->cell_w;
        pax_simple_rect(render->fb, fg, x, y, width, render->cell_h);
        char text[5];
        int  length  = encode_utf8(cell->cp ? cell->cp : ' ', text);
        text[length] = '\0';
        pax_draw_text(render->fb, bg, render->font, render->font_size, x, y, text);
    }

    last_cursor_row = cursor_row;
    term_clear_dirty(term);
}

void term_render_status(term_render_t* render, char const* text) {
    float width  = pax_buf_get_width(render->fb);
    float height = pax_buf_get_height(render->fb);
    float bar    = TERM_STATUS_BAR_HEIGHT;
    pax_simple_rect(render->fb, 0xFF1E2A38, 0, height - bar, width, bar);
    keycap_draw_text(render->fb, 0xFFB8CCE0, terminal_font, TERMINAL_FONT_HEIGHT, 4, height - bar - 1, text);
}
