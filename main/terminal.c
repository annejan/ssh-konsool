// SPDX-License-Identifier: MIT

#include "terminal.h"
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static char const TAG[] = "term";

#define DEFAULT_FG 0xFFD0D0D0
#define DEFAULT_BG 0xFF000000

#define STRIDE     TERM_MAX_COLS
#define MAX_PARAMS 16
#define OSC_MAX    256

enum {
    ST_GROUND = 0,
    ST_ESC,
    ST_ESC_INT,  // ESC with an intermediate byte pending, e.g. "ESC ( B"
    ST_CSI,
    ST_OSC,
    ST_STRING,  // DCS/APC/PM/SOS: swallowed until ST
};

typedef struct {
    int      cx, cy;
    uint32_t fg, bg;
    uint8_t  attr;
    bool     origin;
} saved_cursor_t;

struct term {
    int cols, rows;

    term_cell_t* screen;
    term_cell_t* alt;
    term_cell_t* active;
    bool         alt_active;

    term_cell_t* sb;
    int          sb_count;
    int          sb_head;
    int          view_offset;

    int  cx, cy;
    bool wrap_pending;
    int  top, bot;

    uint32_t fg, bg;
    uint8_t  attr;

    bool cursor_visible;
    bool autowrap;
    bool origin_mode;
    bool app_cursor;
    bool app_keypad;
    bool bracketed_paste;
    bool insert_mode;

    saved_cursor_t saved_main, saved_alt;

    int  state;
    int  params[MAX_PARAMS];
    int  nparams;
    bool param_pending;
    char intermediate;
    char priv;
    char osc[OSC_MAX];
    int  osc_len;

    uint32_t utf_cp;
    int      utf_left;

    bool tabstop[TERM_MAX_COLS];

    uint64_t dirty[(TERM_MAX_ROWS + 63) / 64];

    term_response_fn resp;
    void*            resp_ctx;
    term_title_fn    title_cb;
    void*            title_ctx;
};

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------

// The 16 ANSI colours, in the shades xterm uses.
static uint32_t const ansi_colors[16] = {
    0xFF000000, 0xFFCD0000, 0xFF00CD00, 0xFFCDCD00, 0xFF0000EE, 0xFFCD00CD, 0xFF00CDCD, 0xFFE5E5E5,
    0xFF7F7F7F, 0xFFFF0000, 0xFF00FF00, 0xFFFFFF00, 0xFF5C5CFF, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF,
};

static uint32_t color_from_index(int index) {
    if (index < 0) {
        return DEFAULT_FG;
    }
    if (index < 16) {
        return ansi_colors[index];
    }
    if (index < 232) {
        // 6x6x6 colour cube
        int              n        = index - 16;
        static int const steps[6] = {0, 95, 135, 175, 215, 255};
        int              r        = steps[(n / 36) % 6];
        int              g        = steps[(n / 6) % 6];
        int              b        = steps[n % 6];
        return 0xFF000000 | (r << 16) | (g << 8) | b;
    }
    if (index < 256) {
        int v = 8 + (index - 232) * 10;
        return 0xFF000000 | (v << 16) | (v << 8) | v;
    }
    return DEFAULT_FG;
}

// ---------------------------------------------------------------------------
// Grid helpers
// ---------------------------------------------------------------------------

static inline term_cell_t* cell_at(term_t* t, int col, int row) {
    return &t->active[row * STRIDE + col];
}

static void mark_dirty(term_t* t, int row) {
    if (row >= 0 && row < TERM_MAX_ROWS) {
        t->dirty[row / 64] |= (uint64_t)1 << (row % 64);
    }
}

static void mark_all_rows(term_t* t) {
    memset(t->dirty, 0xFF, sizeof(t->dirty));
}

static void blank_cell(term_t* t, term_cell_t* c) {
    c->cp   = ' ';
    c->fg   = t->fg;
    c->bg   = t->bg;
    c->attr = 0;
}

static void clear_region(term_t* t, int row, int from_col, int to_col) {
    if (row < 0 || row >= t->rows) {
        return;
    }
    if (from_col < 0) {
        from_col = 0;
    }
    if (to_col >= t->cols) {
        to_col = t->cols - 1;
    }
    for (int x = from_col; x <= to_col; x++) {
        blank_cell(t, cell_at(t, x, row));
    }
    mark_dirty(t, row);
}

static void clear_rows(term_t* t, int from_row, int to_row) {
    for (int y = from_row; y <= to_row; y++) {
        clear_region(t, y, 0, t->cols - 1);
    }
}

// Copy the top line of the main screen into the scrollback ring before it is
// lost to a scroll. Only the main screen scrolls back; full screen programs on
// the alternate screen would otherwise flood it with redraw noise.
static void push_scrollback(term_t* t) {
    if (t->alt_active || !t->sb) {
        return;
    }
    memcpy(&t->sb[t->sb_head * STRIDE], &t->screen[0], sizeof(term_cell_t) * STRIDE);
    t->sb_head = (t->sb_head + 1) % TERM_SCROLLBACK;
    if (t->sb_count < TERM_SCROLLBACK) {
        t->sb_count++;
    } else if (t->view_offset > 0) {
        // The line the view pointed at just fell off the end; stay put rather
        // than silently shifting the user's reading position by one.
        t->view_offset--;
    }
}

static void scroll_up(term_t* t, int lines) {
    if (lines <= 0) {
        return;
    }
    int region = t->bot - t->top + 1;
    if (lines > region) {
        lines = region;
    }
    for (int i = 0; i < lines; i++) {
        if (t->top == 0) {
            push_scrollback(t);
        }
        memmove(&t->active[t->top * STRIDE], &t->active[(t->top + 1) * STRIDE],
                sizeof(term_cell_t) * STRIDE * (t->bot - t->top));
        clear_region(t, t->bot, 0, t->cols - 1);
    }
    for (int y = t->top; y <= t->bot; y++) {
        mark_dirty(t, y);
    }
}

static void scroll_down(term_t* t, int lines) {
    if (lines <= 0) {
        return;
    }
    int region = t->bot - t->top + 1;
    if (lines > region) {
        lines = region;
    }
    for (int i = 0; i < lines; i++) {
        memmove(&t->active[(t->top + 1) * STRIDE], &t->active[t->top * STRIDE],
                sizeof(term_cell_t) * STRIDE * (t->bot - t->top));
        clear_region(t, t->top, 0, t->cols - 1);
    }
    for (int y = t->top; y <= t->bot; y++) {
        mark_dirty(t, y);
    }
}

static void reset_tabstops(term_t* t) {
    memset(t->tabstop, 0, sizeof(t->tabstop));
    for (int x = 8; x < TERM_MAX_COLS; x += 8) {
        t->tabstop[x] = true;
    }
}

static void move_to(term_t* t, int col, int row) {
    int min_row = t->origin_mode ? t->top : 0;
    int max_row = t->origin_mode ? t->bot : t->rows - 1;
    if (row < min_row) {
        row = min_row;
    }
    if (row > max_row) {
        row = max_row;
    }
    if (col < 0) {
        col = 0;
    }
    if (col > t->cols - 1) {
        col = t->cols - 1;
    }
    mark_dirty(t, t->cy);
    t->cx           = col;
    t->cy           = row;
    t->wrap_pending = false;
    mark_dirty(t, t->cy);
}

static void line_feed(term_t* t) {
    if (t->cy == t->bot) {
        scroll_up(t, 1);
    } else if (t->cy < t->rows - 1) {
        mark_dirty(t, t->cy);
        t->cy++;
        mark_dirty(t, t->cy);
    }
}

static void put_codepoint(term_t* t, uint32_t cp) {
    if (t->wrap_pending && t->autowrap) {
        t->cx           = 0;
        t->wrap_pending = false;
        line_feed(t);
    }

    if (t->insert_mode && t->cx < t->cols - 1) {
        memmove(cell_at(t, t->cx + 1, t->cy), cell_at(t, t->cx, t->cy), sizeof(term_cell_t) * (t->cols - t->cx - 1));
    }

    term_cell_t* c = cell_at(t, t->cx, t->cy);
    c->cp          = cp;
    c->fg          = t->fg;
    c->bg          = t->bg;
    c->attr        = t->attr;
    mark_dirty(t, t->cy);

    if (t->cx == t->cols - 1) {
        t->wrap_pending = true;
    } else {
        t->cx++;
    }
}

static void respond(term_t* t, char const* text) {
    if (t->resp) {
        t->resp(text, strlen(text), t->resp_ctx);
    }
}

// ---------------------------------------------------------------------------
// Escape sequence handling
// ---------------------------------------------------------------------------

static int param(term_t* t, int index, int fallback) {
    if (index >= t->nparams || t->params[index] < 0) {
        return fallback;
    }
    return t->params[index];
}

static void save_cursor(term_t* t) {
    saved_cursor_t* s = t->alt_active ? &t->saved_alt : &t->saved_main;
    s->cx             = t->cx;
    s->cy             = t->cy;
    s->fg             = t->fg;
    s->bg             = t->bg;
    s->attr           = t->attr;
    s->origin         = t->origin_mode;
}

static void restore_cursor(term_t* t) {
    saved_cursor_t* s = t->alt_active ? &t->saved_alt : &t->saved_main;
    t->fg             = s->fg;
    t->bg             = s->bg;
    t->attr           = s->attr;
    t->origin_mode    = s->origin;
    move_to(t, s->cx, s->cy);
}

static void switch_screen(term_t* t, bool to_alt) {
    if (t->alt_active == to_alt) {
        return;
    }
    t->alt_active = to_alt;
    t->active     = to_alt ? t->alt : t->screen;
    if (to_alt) {
        // Entering the alternate screen always starts from a blank page.
        clear_rows(t, 0, t->rows - 1);
    }
    t->view_offset = 0;
    mark_all_rows(t);
}

static void handle_sgr(term_t* t) {
    if (t->nparams == 0) {
        t->fg   = DEFAULT_FG;
        t->bg   = DEFAULT_BG;
        t->attr = 0;
        return;
    }
    for (int i = 0; i < t->nparams; i++) {
        int p = param(t, i, 0);
        switch (p) {
            case 0:
                t->fg   = DEFAULT_FG;
                t->bg   = DEFAULT_BG;
                t->attr = 0;
                break;
            case 1:
                t->attr |= TERM_ATTR_BOLD;
                break;
            case 2:
                t->attr |= TERM_ATTR_DIM;
                break;
            case 4:
                t->attr |= TERM_ATTR_UNDERLINE;
                break;
            case 7:
                t->attr |= TERM_ATTR_REVERSE;
                break;
            case 8:
                t->attr |= TERM_ATTR_INVISIBLE;
                break;
            case 9:
                t->attr |= TERM_ATTR_STRIKE;
                break;
            case 21:
            case 22:
                t->attr &= ~(TERM_ATTR_BOLD | TERM_ATTR_DIM);
                break;
            case 24:
                t->attr &= ~TERM_ATTR_UNDERLINE;
                break;
            case 27:
                t->attr &= ~TERM_ATTR_REVERSE;
                break;
            case 28:
                t->attr &= ~TERM_ATTR_INVISIBLE;
                break;
            case 29:
                t->attr &= ~TERM_ATTR_STRIKE;
                break;
            case 39:
                t->fg = DEFAULT_FG;
                break;
            case 49:
                t->bg = DEFAULT_BG;
                break;
            case 38:
            case 48: {
                // Extended colour: "38;5;N" (palette) or "38;2;R;G;B" (direct)
                uint32_t color = (p == 38) ? DEFAULT_FG : DEFAULT_BG;
                int      kind  = param(t, i + 1, 0);
                if (kind == 5) {
                    color  = color_from_index(param(t, i + 2, 0));
                    i     += 2;
                } else if (kind == 2) {
                    int r  = param(t, i + 2, 0) & 0xFF;
                    int g  = param(t, i + 3, 0) & 0xFF;
                    int b  = param(t, i + 4, 0) & 0xFF;
                    color  = 0xFF000000 | (r << 16) | (g << 8) | b;
                    i     += 4;
                } else {
                    i += 1;
                }
                if (p == 38) {
                    t->fg = color;
                } else {
                    t->bg = color;
                }
                break;
            }
            default:
                if (p >= 30 && p <= 37) {
                    t->fg = ansi_colors[p - 30];
                } else if (p >= 40 && p <= 47) {
                    t->bg = ansi_colors[p - 40];
                } else if (p >= 90 && p <= 97) {
                    t->fg = ansi_colors[p - 90 + 8];
                } else if (p >= 100 && p <= 107) {
                    t->bg = ansi_colors[p - 100 + 8];
                }
                break;
        }
    }
}

static void set_mode(term_t* t, bool enable) {
    for (int i = 0; i < t->nparams; i++) {
        int p = param(t, i, 0);
        if (t->priv == '?') {
            switch (p) {
                case 1:
                    t->app_cursor = enable;
                    break;
                case 6:
                    t->origin_mode = enable;
                    move_to(t, 0, enable ? t->top : 0);
                    break;
                case 7:
                    t->autowrap = enable;
                    break;
                case 25:
                    t->cursor_visible = enable;
                    break;
                case 47:
                case 1047:
                    switch_screen(t, enable);
                    break;
                case 1048:
                    enable ? save_cursor(t) : restore_cursor(t);
                    break;
                case 1049:
                    if (enable) {
                        save_cursor(t);
                        switch_screen(t, true);
                    } else {
                        switch_screen(t, false);
                        restore_cursor(t);
                    }
                    break;
                case 2004:
                    t->bracketed_paste = enable;
                    break;
                default:
                    break;  // Mouse reporting and friends: nothing to do
            }
        } else {
            switch (p) {
                case 4:
                    t->insert_mode = enable;
                    break;
                default:
                    break;
            }
        }
    }
}

static void handle_csi(term_t* t, char final) {
    switch (final) {
        case '@': {  // ICH: insert blanks
            int n = param(t, 0, 1);
            if (n < 1) n = 1;
            if (n > t->cols - t->cx) n = t->cols - t->cx;
            memmove(cell_at(t, t->cx + n, t->cy), cell_at(t, t->cx, t->cy),
                    sizeof(term_cell_t) * (t->cols - t->cx - n));
            clear_region(t, t->cy, t->cx, t->cx + n - 1);
            break;
        }
        case 'A':
            move_to(t, t->cx, t->cy - (param(t, 0, 1) ? param(t, 0, 1) : 1));
            break;
        case 'B':
            move_to(t, t->cx, t->cy + (param(t, 0, 1) ? param(t, 0, 1) : 1));
            break;
        case 'C':
            move_to(t, t->cx + (param(t, 0, 1) ? param(t, 0, 1) : 1), t->cy);
            break;
        case 'D':
            move_to(t, t->cx - (param(t, 0, 1) ? param(t, 0, 1) : 1), t->cy);
            break;
        case 'E':
            move_to(t, 0, t->cy + (param(t, 0, 1) ? param(t, 0, 1) : 1));
            break;
        case 'F':
            move_to(t, 0, t->cy - (param(t, 0, 1) ? param(t, 0, 1) : 1));
            break;
        case 'G':
        case '`':
            move_to(t, param(t, 0, 1) - 1, t->cy);
            break;
        case 'H':
        case 'f': {
            int row = param(t, 0, 1) - 1;
            int col = param(t, 1, 1) - 1;
            if (t->origin_mode) {
                row += t->top;
            }
            move_to(t, col, row);
            break;
        }
        case 'I': {  // CHT: forward tab stops
            // Each useful step moves the cursor at least one column, so more
            // repeats than there are columns cannot do anything except burn
            // time on a hostile sequence.
            int n = param(t, 0, 1);
            if (n < 1) n = 1;
            if (n > t->cols) n = t->cols;
            for (; n > 0; n--) {
                int x = t->cx + 1;
                while (x < t->cols - 1 && !t->tabstop[x]) x++;
                move_to(t, x, t->cy);
            }
            break;
        }
        case 'J': {  // ED
            int mode = param(t, 0, 0);
            if (mode == 0) {
                clear_region(t, t->cy, t->cx, t->cols - 1);
                clear_rows(t, t->cy + 1, t->rows - 1);
            } else if (mode == 1) {
                clear_rows(t, 0, t->cy - 1);
                clear_region(t, t->cy, 0, t->cx);
            } else {
                clear_rows(t, 0, t->rows - 1);
            }
            break;
        }
        case 'K': {  // EL
            int mode = param(t, 0, 0);
            if (mode == 0) {
                clear_region(t, t->cy, t->cx, t->cols - 1);
            } else if (mode == 1) {
                clear_region(t, t->cy, 0, t->cx);
            } else {
                clear_region(t, t->cy, 0, t->cols - 1);
            }
            break;
        }
        case 'L': {  // IL: insert lines at the cursor, within the scroll region
            if (t->cy < t->top || t->cy > t->bot) break;
            int n     = param(t, 0, 1);
            int saved = t->top;
            t->top    = t->cy;
            scroll_down(t, n);
            t->top = saved;
            break;
        }
        case 'M': {  // DL
            if (t->cy < t->top || t->cy > t->bot) break;
            int n     = param(t, 0, 1);
            int saved = t->top;
            t->top    = t->cy;
            scroll_up(t, n);
            t->top = saved;
            break;
        }
        case 'P': {  // DCH: delete characters
            int n = param(t, 0, 1);
            if (n < 1) n = 1;
            if (n > t->cols - t->cx) n = t->cols - t->cx;
            memmove(cell_at(t, t->cx, t->cy), cell_at(t, t->cx + n, t->cy),
                    sizeof(term_cell_t) * (t->cols - t->cx - n));
            clear_region(t, t->cy, t->cols - n, t->cols - 1);
            break;
        }
        case 'S':
            scroll_up(t, param(t, 0, 1));
            break;
        case 'T':
            scroll_down(t, param(t, 0, 1));
            break;
        case 'X': {  // ECH: erase characters in place
            int n = param(t, 0, 1);
            if (n < 1) n = 1;
            clear_region(t, t->cy, t->cx, t->cx + n - 1);
            break;
        }
        case 'Z': {  // CBT: backward tab stops
            int n = param(t, 0, 1);
            if (n < 1) n = 1;
            if (n > t->cols) n = t->cols;
            for (; n > 0; n--) {
                int x = t->cx - 1;
                while (x > 0 && !t->tabstop[x]) x--;
                move_to(t, x, t->cy);
            }
            break;
        }
        case 'd':
            move_to(t, t->cx, param(t, 0, 1) - 1);
            break;
        case 'g':
            if (param(t, 0, 0) == 3) {
                memset(t->tabstop, 0, sizeof(t->tabstop));
            } else {
                t->tabstop[t->cx] = false;
            }
            break;
        case 'h':
            set_mode(t, true);
            break;
        case 'l':
            set_mode(t, false);
            break;
        case 'm':
            handle_sgr(t);
            break;
        case 'n':
            if (param(t, 0, 0) == 6) {
                char reply[32];
                snprintf(reply, sizeof(reply), "\033[%d;%dR", t->cy + 1, t->cx + 1);
                respond(t, reply);
            } else if (param(t, 0, 0) == 5) {
                respond(t, "\033[0n");
            }
            break;
        case 'c':  // Device attributes: claim to be a VT100 with AVO
            respond(t, "\033[?1;2c");
            break;
        case 'r': {  // DECSTBM
            int top = param(t, 0, 1) - 1;
            int bot = param(t, 1, t->rows) - 1;
            if (top < 0) top = 0;
            if (bot > t->rows - 1) bot = t->rows - 1;
            if (top < bot) {
                t->top = top;
                t->bot = bot;
                move_to(t, 0, t->origin_mode ? t->top : 0);
            }
            break;
        }
        case 's':
            save_cursor(t);
            break;
        case 'u':
            restore_cursor(t);
            break;
        default:
            ESP_LOGD(TAG, "unhandled CSI %c%c", t->priv ? t->priv : ' ', final);
            break;
    }
}

static void handle_esc(term_t* t, char final) {
    switch (final) {
        case 'D':
            line_feed(t);
            break;
        case 'E':
            t->cx = 0;
            line_feed(t);
            break;
        case 'M':  // RI: reverse index
            if (t->cy == t->top) {
                scroll_down(t, 1);
            } else if (t->cy > 0) {
                move_to(t, t->cx, t->cy - 1);
            }
            break;
        case 'H':
            t->tabstop[t->cx] = true;
            break;
        case '7':
            save_cursor(t);
            break;
        case '8':
            restore_cursor(t);
            break;
        case '=':
            t->app_keypad = true;
            break;
        case '>':
            t->app_keypad = false;
            break;
        case 'c':
            term_reset(t);
            break;  // RIS
        default:
            break;
    }
}

void term_reset(term_t* t) {
    t->fg              = DEFAULT_FG;
    t->bg              = DEFAULT_BG;
    t->attr            = 0;
    t->top             = 0;
    t->bot             = t->rows - 1;
    t->autowrap        = true;
    t->origin_mode     = false;
    t->app_cursor      = false;
    t->insert_mode     = false;
    t->cursor_visible  = true;
    t->bracketed_paste = false;
    switch_screen(t, false);
    clear_rows(t, 0, t->rows - 1);
    reset_tabstops(t);
    move_to(t, 0, 0);

    // A session can die part way through an escape sequence, so the parser has
    // to be put back on the ground as well as the screen. Otherwise the next
    // session's opening bytes are read as parameters of a dead sequence, or
    // swallowed into an unterminated string.
    t->state         = ST_GROUND;
    t->utf_left      = 0;
    t->nparams       = 0;
    t->param_pending = false;
    t->priv          = 0;
    t->intermediate  = 0;
    t->osc_len       = 0;
    t->view_offset   = 0;
    mark_all_rows(t);
}

static void handle_osc(term_t* t) {
    // "0;title" and "2;title" set the window title; the rest we do not use.
    char* sep = strchr(t->osc, ';');
    if (!sep) {
        return;
    }
    *sep    = '\0';
    int cmd = atoi(t->osc);
    if ((cmd == 0 || cmd == 2) && t->title_cb) {
        t->title_cb(sep + 1, t->title_ctx);
    }
}

// ---------------------------------------------------------------------------
// Byte stream
// ---------------------------------------------------------------------------

static void feed_byte(term_t* t, uint8_t b);

static void ground_control(term_t* t, uint8_t b) {
    switch (b) {
        case 0x07:
            break;  // BEL: no speaker use for it here
        case 0x08:  // BS
            if (t->wrap_pending) {
                t->wrap_pending = false;
            } else if (t->cx > 0) {
                move_to(t, t->cx - 1, t->cy);
            }
            break;
        case 0x09: {  // HT
            int x = t->cx + 1;
            while (x < t->cols - 1 && !t->tabstop[x]) {
                x++;
            }
            move_to(t, x, t->cy);
            break;
        }
        case 0x0A:
        case 0x0B:
        case 0x0C:
            t->wrap_pending = false;
            line_feed(t);
            break;
        case 0x0D:
            move_to(t, 0, t->cy);
            break;
        case 0x1B:
            t->state = ST_ESC;
            break;
        default:
            break;
    }
}

static void feed_byte(term_t* t, uint8_t b) {
    switch (t->state) {
        case ST_GROUND:
            if (t->utf_left > 0) {
                if ((b & 0xC0) == 0x80) {
                    t->utf_cp = (t->utf_cp << 6) | (b & 0x3F);
                    if (--t->utf_left == 0) {
                        put_codepoint(t, t->utf_cp);
                    }
                    return;
                }
                // Malformed sequence: drop it and reconsider this byte.
                t->utf_left = 0;
            }
            if (b < 0x20) {
                ground_control(t, b);
            } else if (b < 0x7F) {
                put_codepoint(t, b);
            } else if (b == 0x7F) {
                // DEL: ignored
            } else if ((b & 0xE0) == 0xC0) {
                t->utf_cp   = b & 0x1F;
                t->utf_left = 1;
            } else if ((b & 0xF0) == 0xE0) {
                t->utf_cp   = b & 0x0F;
                t->utf_left = 2;
            } else if ((b & 0xF8) == 0xF0) {
                t->utf_cp   = b & 0x07;
                t->utf_left = 3;
            }
            break;

        case ST_ESC:
            t->nparams       = 0;
            t->param_pending = false;
            t->priv          = 0;
            t->intermediate  = 0;
            if (b == '[') {
                t->state = ST_CSI;
            } else if (b == ']') {
                t->state   = ST_OSC;
                t->osc_len = 0;
            } else if (b == 'P' || b == '_' || b == '^' || b == 'X') {
                t->state = ST_STRING;
            } else if (b == '(' || b == ')' || b == '*' || b == '+' || b == '#' || b == '%') {
                t->intermediate = (char)b;
                t->state        = ST_ESC_INT;
            } else {
                handle_esc(t, (char)b);
                t->state = ST_GROUND;
            }
            break;

        case ST_ESC_INT:
            // Character set and DEC private selections; none of them change how
            // we render, so the final byte is simply consumed.
            t->state = ST_GROUND;
            break;

        case ST_CSI:
            if (b >= '0' && b <= '9') {
                if (t->nparams == 0) {
                    t->nparams   = 1;
                    t->params[0] = 0;
                }
                if (!t->param_pending) {
                    t->params[t->nparams - 1] = 0;
                    t->param_pending          = true;
                }
                int* p = &t->params[t->nparams - 1];
                if (*p < 10000) {
                    *p = *p * 10 + (b - '0');
                }
            } else if (b == ';' || b == ':') {
                if (t->nparams == 0) {
                    t->nparams   = 1;
                    t->params[0] = -1;
                }
                if (t->nparams < MAX_PARAMS) {
                    t->nparams++;
                    t->params[t->nparams - 1] = -1;
                }
                t->param_pending = false;
            } else if (b == '?' || b == '>' || b == '<' || b == '!') {
                t->priv = (char)b;
            } else if (b >= 0x20 && b <= 0x2F) {
                t->intermediate = (char)b;
            } else if (b >= 0x40 && b <= 0x7E) {
                handle_csi(t, (char)b);
                t->state = ST_GROUND;
            } else if (b < 0x20) {
                ground_control(t, b);
            }
            break;

        case ST_OSC:
            if (b == 0x07) {
                t->osc[t->osc_len] = '\0';
                handle_osc(t);
                t->state = ST_GROUND;
            } else if (b == 0x1B) {
                // Expect the '\' of the string terminator next
                t->osc[t->osc_len] = '\0';
                handle_osc(t);
                t->state = ST_ESC_INT;
            } else if (t->osc_len < OSC_MAX - 1) {
                t->osc[t->osc_len++] = (char)b;
            }
            break;

        case ST_STRING:
            if (b == 0x1B) {
                t->state = ST_ESC_INT;
            } else if (b == 0x07) {
                t->state = ST_GROUND;
            }
            break;

        default:
            t->state = ST_GROUND;
            break;
    }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

static void* term_alloc(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

term_t* term_create(int cols, int rows) {
    term_t* t = calloc(1, sizeof(term_t));
    if (!t) {
        return NULL;
    }

    size_t screen_bytes = sizeof(term_cell_t) * STRIDE * TERM_MAX_ROWS;
    t->screen           = term_alloc(screen_bytes);
    t->alt              = term_alloc(screen_bytes);
    t->sb               = term_alloc(sizeof(term_cell_t) * STRIDE * TERM_SCROLLBACK);
    if (!t->screen || !t->alt) {
        term_destroy(t);
        return NULL;
    }
    // Scrollback is a nicety; carry on without it rather than refusing to run.
    if (!t->sb) {
        ESP_LOGW(TAG, "No memory for scrollback, continuing without it");
    }

    t->active         = t->screen;
    t->fg             = DEFAULT_FG;
    t->bg             = DEFAULT_BG;
    t->cursor_visible = true;
    t->autowrap       = true;

    t->cols = 80;
    t->rows = 24;
    memset(t->screen, 0, screen_bytes);
    memset(t->alt, 0, screen_bytes);
    reset_tabstops(t);

    term_resize(t, cols, rows);
    clear_rows(t, 0, t->rows - 1);
    // The alternate screen starts blank too, otherwise the first switch shows
    // whatever the allocator left behind.
    term_cell_t* keep = t->active;
    t->active         = t->alt;
    clear_rows(t, 0, t->rows - 1);
    t->active = keep;

    mark_all_rows(t);
    return t;
}

void term_destroy(term_t* t) {
    if (!t) {
        return;
    }
    free(t->screen);
    free(t->alt);
    free(t->sb);
    free(t);
}

void term_set_response_cb(term_t* t, term_response_fn fn, void* ctx) {
    t->resp     = fn;
    t->resp_ctx = ctx;
}

void term_set_title_cb(term_t* t, term_title_fn fn, void* ctx) {
    t->title_cb  = fn;
    t->title_ctx = ctx;
}

void term_write(term_t* t, void const* data, size_t len) {
    uint8_t const* bytes = data;
    // Output from the host means the user is looking at live content again.
    if (len && t->view_offset) {
        t->view_offset = 0;
        mark_all_rows(t);
    }
    for (size_t i = 0; i < len; i++) {
        feed_byte(t, bytes[i]);
    }
}

void term_resize(term_t* t, int cols, int rows) {
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    if (cols == t->cols && rows == t->rows) {
        return;
    }

    int old_rows = t->rows;
    int old_cols = t->cols;
    t->cols      = cols;
    t->rows      = rows;

    // Blank whatever the new size exposes; the stride is fixed so existing
    // content keeps its place.
    term_cell_t* keep = t->active;
    for (int pass = 0; pass < 2; pass++) {
        t->active = pass ? t->alt : t->screen;
        for (int y = 0; y < rows; y++) {
            int from = (y < old_rows) ? old_cols : 0;
            if (from < cols) {
                clear_region(t, y, from, cols - 1);
            }
        }
    }
    t->active = keep;

    t->top = 0;
    t->bot = rows - 1;
    if (t->cx >= cols) t->cx = cols - 1;
    if (t->cy >= rows) t->cy = rows - 1;
    t->wrap_pending = false;
    mark_all_rows(t);
}

int term_cols(term_t const* t) {
    return t->cols;
}

int term_rows(term_t const* t) {
    return t->rows;
}

void term_cursor(term_t const* t, int* out_col, int* out_row) {
    if (out_col) *out_col = t->cx;
    if (out_row) *out_row = t->cy;
}

bool term_cursor_visible(term_t const* t) {
    return t->cursor_visible && t->view_offset == 0;
}

bool term_in_alt_screen(term_t const* t) {
    return t->alt_active;
}

bool term_app_cursor_keys(term_t const* t) {
    return t->app_cursor;
}

bool term_bracketed_paste(term_t const* t) {
    return t->bracketed_paste;
}

term_cell_t const* term_cell(term_t const* t, int col, int row) {
    static term_cell_t const blank = {' ', DEFAULT_FG, DEFAULT_BG, 0};
    if (col < 0 || col >= t->cols || row < 0 || row >= t->rows) {
        return &blank;
    }
    if (t->view_offset > 0 && row < t->view_offset) {
        if (!t->sb) {
            return &blank;
        }
        int index = t->sb_head - t->view_offset + row;
        while (index < 0) {
            index += TERM_SCROLLBACK;
        }
        index %= TERM_SCROLLBACK;
        return &t->sb[index * STRIDE + col];
    }
    return &t->active[(row - t->view_offset) * STRIDE + col];
}

void term_scroll_view(term_t* t, int delta_lines) {
    if (!t->sb || t->alt_active) {
        return;
    }
    int offset = t->view_offset + delta_lines;
    if (offset < 0) {
        offset = 0;
    }
    if (offset > t->sb_count) {
        offset = t->sb_count;
    }
    if (offset != t->view_offset) {
        t->view_offset = offset;
        mark_all_rows(t);
    }
}

void term_scroll_reset(term_t* t) {
    if (t->view_offset) {
        t->view_offset = 0;
        mark_all_rows(t);
    }
}

int term_scroll_offset(term_t const* t) {
    return t->view_offset;
}

bool term_row_dirty(term_t const* t, int row) {
    if (row < 0 || row >= TERM_MAX_ROWS) {
        return false;
    }
    return (t->dirty[row / 64] & ((uint64_t)1 << (row % 64))) != 0;
}

void term_clear_dirty(term_t* t) {
    memset(t->dirty, 0, sizeof(t->dirty));
}

void term_mark_all_dirty(term_t* t) {
    mark_all_rows(t);
}
