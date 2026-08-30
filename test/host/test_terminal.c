// SPDX-License-Identifier: MIT
//
// Tests for the screen model and escape sequence parser in main/terminal.c.
// This is the code that eats bytes straight off the network, so a mistake here
// is reachable by any server the badge connects to. The cases below are the ones
// where being wrong is silent: a codepoint that should never have reached a cell,
// a cell that outlives the session that wrote it, a wide glyph without its other
// half.

#include "shims.h"

// The code under test, compiled straight into this program so the tests can
// reach its statics and cannot drift away from it.
#include "../../main/terminal.c"

static int failures = 0;

#define CHECK(condition, ...)                           \
    do {                                                \
        if (!(condition)) {                             \
            printf("FAIL %s:%d: ", __func__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            failures++;                                 \
        }                                               \
    } while (0)

#define REPLACEMENT 0xFFFD

static void feed(term_t* t, char const* bytes, size_t len) {
    term_write(t, bytes, len);
}

static void feed_str(term_t* t, char const* s) {
    term_write(t, s, strlen(s));
}

static uint32_t cp_at(term_t* t, int col, int row) {
    return term_cell(t, col, row)->cp;
}

// Invalid sequences that still decode to a number. Every one of these has to
// land as U+FFFD rather than as the value it encodes: an overlong form can spell
// ESC or '[' in a way a naive filter would miss, a surrogate is not a scalar
// value, and anything past U+10FFFF is not a codepoint at all.
static void test_invalid_scalars_become_replacement(void) {
    struct {
        char const* name;
        char const* bytes;
        size_t      len;
    } const cases[] = {
        {"overlong 2 byte '/'",      "\xC0\xAF",             2},
        {"overlong 2 byte NUL",      "\xC0\x80",             2},
        {"overlong 3 byte",          "\xE0\x80\xAF",         3},
        {"overlong 3 byte ESC",      "\xE0\x81\x9B",         3},
        {"overlong 4 byte",          "\xF0\x80\x80\xAF",     4},
        {"surrogate D800",           "\xED\xA0\x80",         3},
        {"surrogate DFFF",           "\xED\xBF\xBF",         3},
        {"above U+10FFFF",           "\xF4\x90\x80\x80",     4},
        {"far above U+10FFFF",       "\xF7\xBF\xBF\xBF",     4},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        term_t* t = term_create(20, 4);
        CHECK(t != NULL, "%s: term_create failed", cases[i].name);
        if (!t) {
            continue;
        }
        feed(t, cases[i].bytes, cases[i].len);
        // Exactly one cell consumed, and the next character is not swallowed:
        // that is what proves the parser resynchronised.
        feed_str(t, "A");
        CHECK(cp_at(t, 0, 0) == REPLACEMENT, "%s: cell 0 is U+%04X", cases[i].name, cp_at(t, 0, 0));
        CHECK(cp_at(t, 1, 0) == 'A', "%s: cell 1 is U+%04X, not 'A'", cases[i].name, cp_at(t, 1, 0));
        term_destroy(t);
    }
}

// Bytes that never complete a sequence are dropped, and the byte that broke the
// sequence is reconsidered rather than eaten. A server that truncates on purpose
// must not be able to make the next character disappear.
static void test_truncated_and_stray_bytes_are_dropped(void) {
    struct {
        char const* name;
        char const* bytes;
        size_t      len;
    } const cases[] = {
        {"truncated 3 byte",     "\xE2\x82",     2},
        {"truncated 4 byte",     "\xF0\x9F\x98", 3},
        {"lone continuation",    "\x80",         1},
        {"two continuations",    "\x80\xBF",     2},
        {"invalid lead F8",      "\xF8",         1},
        {"invalid lead FE",      "\xFE",         1},
        {"invalid lead FF",      "\xFF",         1},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        term_t* t = term_create(20, 4);
        CHECK(t != NULL, "%s: term_create failed", cases[i].name);
        if (!t) {
            continue;
        }
        feed(t, cases[i].bytes, cases[i].len);
        feed_str(t, "A");
        // Nothing was drawn for the broken sequence, so 'A' is the first cell.
        CHECK(cp_at(t, 0, 0) == 'A', "%s: cell 0 is U+%04X, not 'A'", cases[i].name, cp_at(t, 0, 0));
        term_destroy(t);
    }
}

// The valid cases still have to work, including a sequence split across two
// writes, which is what a real socket does.
static void test_valid_sequences_decode(void) {
    term_t* t = term_create(20, 4);
    CHECK(t != NULL, "term_create failed");
    if (!t) {
        return;
    }

    feed_str(t, "\xE2\x82\xAC");  // U+20AC EURO SIGN
    CHECK(cp_at(t, 0, 0) == 0x20AC, "euro decoded as U+%04X", cp_at(t, 0, 0));

    // Split across writes: the decoder keeps its state between calls.
    feed(t, "\xF0\x9F", 2);
    feed(t, "\x98\x80", 2);  // U+1F600 GRINNING FACE
    CHECK(cp_at(t, 1, 0) == 0x1F600, "split emoji decoded as U+%04X", cp_at(t, 1, 0));

    term_destroy(t);
}

// A double width character owns two cells: the left half carries the codepoint
// and TERM_ATTR_WIDE, the right half is a TERM_ATTR_CONT placeholder. Nothing a
// server sends may leave a CONT cell without its WIDE partner, because the
// renderer trusts that pairing.
static bool wide_pairs_are_consistent(term_t* t) {
    for (int row = 0; row < term_rows(t); row++) {
        for (int col = 0; col < term_cols(t); col++) {
            term_cell_t const* cell = term_cell(t, col, row);
            if (!(cell->attr & TERM_ATTR_CONT)) {
                continue;
            }
            if (col == 0) {
                return false;
            }
            if (!(term_cell(t, col - 1, row)->attr & TERM_ATTR_WIDE)) {
                return false;
            }
        }
    }
    return true;
}

static void test_wide_characters_keep_their_pairing(void) {
    term_t* t = term_create(10, 4);
    CHECK(t != NULL, "term_create failed");
    if (!t) {
        return;
    }

    feed_str(t, "\xE6\xBC\xA2");  // U+6F22, double width
    CHECK(cp_at(t, 0, 0) == 0x6F22, "wide char decoded as U+%04X", cp_at(t, 0, 0));
    CHECK((term_cell(t, 0, 0)->attr & TERM_ATTR_WIDE) != 0, "left half is not marked WIDE");
    CHECK((term_cell(t, 1, 0)->attr & TERM_ATTR_CONT) != 0, "right half is not marked CONT");
    int col = 0, row = 0;
    term_cursor(t, &col, &row);
    CHECK(col == 2, "cursor advanced to %d, not 2", col);

    // A wide character in the last column cannot be split, and whatever the
    // parser does about it must not leave a stranded half.
    term_reset(t);
    feed_str(t, "\x1B[1;10H");  // last column of a 10 column screen
    feed_str(t, "\xE6\xBC\xA2");
    CHECK(wide_pairs_are_consistent(t), "a wide pair was stranded at the right margin");

    // Insert mode pushing a wide pair against the margin, then erasing into it.
    term_reset(t);
    feed_str(t, "\xE6\xBC\xA2\xE6\xBC\xA2\xE6\xBC\xA2");
    feed_str(t, "\x1B[H\x1B[4h");  // home, insert mode on
    feed_str(t, "x");
    CHECK(wide_pairs_are_consistent(t), "insert mode stranded a wide half");
    feed_str(t, "\x1B[2P");  // delete two characters
    CHECK(wide_pairs_are_consistent(t), "DCH stranded a wide half");

    term_destroy(t);
}

// Regression: the scrollback is a fixed stride array, and a line entering it was
// copied at full stride. Columns past the current width are not part of this
// session's screen — clear_region clamps to cols - 1, so term_reset never wipes
// them — so the copy could carry text from an earlier, wider session into this
// session's history, where widening the terminal again would render it.
static void test_scrollback_does_not_carry_a_previous_session(void) {
    term_t* t = term_create(80, 4);
    CHECK(t != NULL, "term_create failed");
    if (!t) {
        return;
    }

    // Session one, wide: fill the full width of every row with a mark.
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 80; col++) {
            feed_str(t, "S");
        }
    }
    CHECK(cp_at(t, 60, 0) == 'S', "the wide session did not write column 60");

    // The user shrinks the font, so the terminal narrows. Columns 40..79 keep
    // their glyphs; nothing clears past the new width.
    term_resize(t, 40, 4);

    // Session two starts. A reset is the session boundary.
    term_reset(t);

    // Enough output to push lines through the screen and into the scrollback.
    for (int row = 0; row < 8; row++) {
        feed_str(t, "second\r\n");
    }

    // The user widens the font again and scrolls back.
    term_resize(t, 80, 4);
    term_scroll_view(t, 3);
    CHECK(term_scroll_offset(t) > 0, "the view did not scroll back");

    for (int row = 0; row < term_scroll_offset(t); row++) {
        for (int col = 40; col < 80; col++) {
            uint32_t cp = cp_at(t, col, row);
            CHECK(cp == ' ', "scrollback row %d column %d holds U+%04X from the previous session", row, col, cp);
        }
    }

    term_destroy(t);
}

// A reset is a session boundary, so the history goes with it: one server's
// output must not be readable as scrollback during a session to another host.
static void test_reset_drops_the_scrollback(void) {
    term_t* t = term_create(20, 4);
    CHECK(t != NULL, "term_create failed");
    if (!t) {
        return;
    }

    for (int row = 0; row < 8; row++) {
        feed_str(t, "secret\r\n");
    }
    term_scroll_view(t, 2);
    CHECK(term_scroll_offset(t) > 0, "nothing reached the scrollback to begin with");
    term_scroll_reset(t);

    // ESC c, the two bytes a server can send to do this itself. Split so that
    // the 'c' is not swallowed into the hex escape.
    feed_str(t, "\x1B" "c");
    term_scroll_view(t, 2);
    CHECK(term_scroll_offset(t) == 0, "the scrollback survived a reset");
    CHECK(cp_at(t, 0, 0) == ' ', "the screen survived a reset");

    term_destroy(t);
}

// Parameters arrive as attacker-controlled decimal digits. Whatever they say,
// the cursor stays on the grid; under the sanitisers an out of range write here
// would be a fault rather than a wrong character.
static void test_hostile_parameters_stay_in_bounds(void) {
    term_t* t = term_create(20, 4);
    CHECK(t != NULL, "term_create failed");
    if (!t) {
        return;
    }

    char const* const hostile[] = {
        "\x1B[999999999;999999999H",
        "\x1B[4294967296;4294967296H",
        "\x1B[999999999A\x1B[999999999B\x1B[999999999C\x1B[999999999D",
        "\x1B[999999999L\x1B[999999999M\x1B[999999999P\x1B[999999999@",
        "\x1B[999999999;1r",
        "\x1B[-1;-1H",
        "\x1B[;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;H",
        "\x1B[1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1H",
    };

    for (size_t i = 0; i < sizeof(hostile) / sizeof(hostile[0]); i++) {
        feed_str(t, hostile[i]);
        feed_str(t, "A");
        int col = 0, row = 0;
        term_cursor(t, &col, &row);
        CHECK(col >= 0 && col <= term_cols(t), "case %zu left the cursor at column %d", i, col);
        CHECK(row >= 0 && row < term_rows(t), "case %zu left the cursor at row %d", i, row);
        CHECK(wide_pairs_are_consistent(t), "case %zu stranded a wide half", i);
    }

    term_destroy(t);
}

// Autowrap is where off by one errors hide, and the last column is the place a
// server can aim at cheaply.
static void test_autowrap_at_the_last_column(void) {
    term_t* t = term_create(10, 4);
    CHECK(t != NULL, "term_create failed");
    if (!t) {
        return;
    }

    feed_str(t, "0123456789");  // exactly the width
    int col = 0, row = 0;
    term_cursor(t, &col, &row);
    CHECK(row == 0, "the cursor left row 0 before the extra character");
    feed_str(t, "X");
    term_cursor(t, &col, &row);
    CHECK(row == 1 && col == 1, "wrapped to row %d column %d, not row 1 column 1", row, col);
    CHECK(cp_at(t, 9, 0) == '9', "the last column of row 0 is U+%04X", cp_at(t, 9, 0));
    CHECK(cp_at(t, 0, 1) == 'X', "the wrapped character is U+%04X", cp_at(t, 0, 1));

    term_destroy(t);
}

int main(void) {
    test_invalid_scalars_become_replacement();
    test_truncated_and_stray_bytes_are_dropped();
    test_valid_sequences_decode();
    test_wide_characters_keep_their_pairing();
    test_scrollback_does_not_carry_a_previous_session();
    test_reset_drops_the_scrollback();
    test_hostile_parameters_stay_in_bounds();
    test_autowrap_at_the_last_column();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
