// SPDX-License-Identifier: MIT
//
// A VT100/xterm compatible screen model: a grid of cells, a cursor, and a
// parser that turns a byte stream from the remote host into edits on that grid.
// Nothing in here draws anything; see term_render.c for that.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TERM_MAX_COLS   160
#define TERM_MAX_ROWS   64
#define TERM_SCROLLBACK 512

// Cell attributes
#define TERM_ATTR_BOLD      (1 << 0)
#define TERM_ATTR_DIM       (1 << 1)
#define TERM_ATTR_UNDERLINE (1 << 2)
#define TERM_ATTR_REVERSE   (1 << 3)
#define TERM_ATTR_INVISIBLE (1 << 4)
#define TERM_ATTR_STRIKE    (1 << 5)

typedef struct {
    uint32_t cp;  // Unicode codepoint, ' ' for blank
    uint32_t fg;  // Resolved 0xAARRGGBB colour
    uint32_t bg;
    uint8_t  attr;
} term_cell_t;

// Called when the terminal has to answer the host (cursor reports, device
// attributes). The reply must go back over the same channel the input came from.
typedef void (*term_response_fn)(void const* data, size_t len, void* ctx);

// Called when the host sets the window title through OSC 0/2.
typedef void (*term_title_fn)(char const* title, void* ctx);

typedef struct term term_t;

// Allocate a terminal of the given size. Rows and columns are clamped to
// TERM_MAX_ROWS/TERM_MAX_COLS. Returns NULL when out of memory.
term_t* term_create(int cols, int rows);
void    term_destroy(term_t* term);

void term_set_response_cb(term_t* term, term_response_fn fn, void* ctx);
void term_set_title_cb(term_t* term, term_title_fn fn, void* ctx);

// Feed bytes received from the host.
void term_write(term_t* term, void const* data, size_t len);

// Resize the grid. Content is kept top-left anchored; the cursor is clamped.
void term_resize(term_t* term, int cols, int rows);

// Clear the screen and put every mode back to its default, as the RIS escape
// sequence does. Use this rather than feeding RIS in, so nothing outside the
// session task can end up on the terminal's reply path.
void term_reset(term_t* term);

int  term_cols(term_t const* term);
int  term_rows(term_t const* term);
void term_cursor(term_t const* term, int* out_col, int* out_row);
bool term_cursor_visible(term_t const* term);

// True while the host has switched to the alternate screen (vim, htop, ...).
bool term_in_alt_screen(term_t const* term);

// True while the host asked for application cursor keys (DECCKM), which changes
// what the arrow keys send.
bool term_app_cursor_keys(term_t const* term);

// True while bracketed paste is enabled.
bool term_bracketed_paste(term_t const* term);

// Read a cell of the visible screen, taking the scrollback view offset into
// account. Never returns NULL for coordinates inside the grid.
term_cell_t const* term_cell(term_t const* term, int col, int row);

// Scrollback. The offset counts lines scrolled back from the live screen.
void term_scroll_view(term_t* term, int delta_lines);
void term_scroll_reset(term_t* term);
int  term_scroll_offset(term_t const* term);

// Row-level damage tracking, so the renderer can redraw only what changed.
bool term_row_dirty(term_t const* term, int row);
void term_clear_dirty(term_t* term);
void term_mark_all_dirty(term_t* term);
