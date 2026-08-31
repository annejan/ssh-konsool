// SPDX-License-Identifier: MIT
//
// Tests for the generated terminal font. Two properties matter and neither is
// obvious from reading 600 KB of generated tables:
//
//   Every codepoint the font claims a glyph for must actually have one. A range
//   that says it covers a character it draws as sixteen zero bytes renders an
//   invisible blank, which is worse than a visible replacement mark because
//   nothing on screen says anything went wrong.
//
//   Column widths have to match what a remote host's wcwidth would say, or the
//   cursor drifts apart from the host's idea of it and the line slides.

#include <stdio.h>
#include <string.h>
#include "../../main/terminal_font.h"

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

// Reach into the font the way PAX does, so the test sees exactly what gets drawn.
static uint8_t const* glyph_of(uint32_t codepoint, int* out_width) {
    for (size_t i = 0; i < terminal_font_raw.n_ranges; i++) {
        pax_font_range_t const* range = &terminal_font_raw.ranges[i];
        if (codepoint < range->start || codepoint > range->end) {
            continue;
        }
        int width      = range->bitmap_mono.width;
        int row_stride = (width * range->bitmap_mono.bpp + 7) / 8;
        *out_width     = width;
        return range->bitmap_mono.glyphs + (size_t)row_stride * range->bitmap_mono.height * (codepoint - range->start);
    }
    return NULL;
}

static bool is_blank(uint8_t const* glyph, int width) {
    int row_stride = (width + 7) / 8;
    for (int i = 0; i < row_stride * TERMINAL_FONT_HEIGHT; i++) {
        if (glyph[i]) {
            return false;
        }
    }
    return true;
}

// Characters that are supposed to draw nothing: the spaces, the separators and
// the zero width marks. Anything else drawing nothing is the bug this file
// exists for.
static bool is_space_like(uint32_t cp) {
    if (cp == 0x20 || cp == 0xA0 || cp == 0x1680 || cp == 0x3000) {
        return true;
    }
    if (cp >= 0x2000 && cp <= 0x200F) {  // Spaces of various widths, then the
        return true;                     // zero width marks
    }
    if (cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x2060) {
        return true;
    }
    return cp == 0xFEFF;
}

static void test_no_invisible_glyphs(void) {
    int blanks = 0;
    for (uint32_t cp = 0x21; cp < 0x110000; cp++) {
        if (is_space_like(cp)) {
            continue;
        }
        if (!terminal_font_has_glyph(cp)) {
            continue;
        }
        int            width = 0;
        uint8_t const* glyph = glyph_of(cp, &width);
        CHECK(glyph != NULL, "U+%04X claimed but not found", cp);
        if (glyph && is_blank(glyph, width)) {
            if (blanks < 8) {
                printf("      U+%04X is covered but blank\n", cp);
            }
            blanks++;
        }
    }
    CHECK(blanks == 0, "%d covered codepoints draw nothing", blanks);
}

// A glyph must never be wider than the columns the character occupies, or it
// paints over its neighbour while the terminal advances past only its own
// cells. Narrower is allowed: Unifont draws a handful of East Asian Wide
// characters (U+231A WATCH and friends) at eight pixels, which leaves the right
// half of the pair empty but keeps our column count matching the host's.
static void test_glyph_never_wider_than_its_cells(void) {
    int mismatches = 0;
    int narrower   = 0;
    for (uint32_t cp = 0x20; cp < 0x110000; cp++) {
        if (!terminal_font_has_glyph(cp)) {
            continue;
        }
        int            width = 0;
        uint8_t const* glyph = glyph_of(cp, &width);
        if (!glyph) {
            continue;
        }
        int allowed = terminal_char_width(cp) * TERMINAL_FONT_WIDTH;
        if (width > allowed) {
            if (mismatches < 8) {
                printf("      U+%04X glyph is %d px but only %d column(s)\n", cp, width, terminal_char_width(cp));
            }
            mismatches++;
        } else if (width < allowed) {
            narrower++;
        }
    }
    CHECK(mismatches == 0, "%d glyphs are wider than their cells", mismatches);
    printf("      (%d wide characters have a narrow glyph, cosmetic only)\n", narrower);
}

static void test_known_widths(void) {
    // Latin, Greek, Cyrillic and box drawing are all one column.
    CHECK(terminal_char_width('A') == 1, "'A' is not one column");
    CHECK(terminal_char_width(0x03B1) == 1, "Greek alpha is not one column");
    CHECK(terminal_char_width(0x0416) == 1, "Cyrillic Zhe is not one column");
    CHECK(terminal_char_width(0x2500) == 1, "box drawing is not one column");
    CHECK(terminal_char_width(0xFFFD) == 1, "the replacement mark is not one column");

    // East Asian Wide and Fullwidth are two.
    CHECK(terminal_char_width(0x3042) == 2, "Hiragana A is not two columns");
    CHECK(terminal_char_width(0x30AB) == 2, "Katakana KA is not two columns");
    CHECK(terminal_char_width(0x4E00) == 2, "CJK one is not two columns");
    CHECK(terminal_char_width(0xFF21) == 2, "fullwidth A is not two columns");
    CHECK(terminal_char_width(0x3000) == 2, "ideographic space is not two columns");
    CHECK(terminal_char_width(0xAC00) == 2, "Hangul GA is not two columns");
}

// The characters a terminal cannot do without.
static void test_essential_coverage(void) {
    struct {
        uint32_t    cp;
        char const* what;
    } const needed[] = {
        {'A', "Latin A"},
        {0x00E9, "e acute"},
        {0x03B1, "Greek alpha"},
        {0x0416, "Cyrillic Zhe"},
        {0x2500, "box light horizontal"},
        {0x2502, "box light vertical"},
        {0x250C, "box down and right"},
        {0x2588, "full block"},
        {0x2591, "light shade"},
        {0x2192, "rightwards arrow"},
        {0x2026, "ellipsis"},
        {0x2018, "left quote"},
        {0x20AC, "euro"},
        {0xFFFD, "replacement"},
        {0x3042, "Hiragana A"},
        {0x30AB, "Katakana KA"},
        {0xFF21, "fullwidth A"},
        // The six function keycaps are drawn from the font, so the UI loses its
        // key labels if these ever fall out of the covered blocks.
        {0x2717, "keycap cross"},
        {0x25B2, "keycap triangle"},
        {0x25A0, "keycap square"},
        {0x25CF, "keycap circle"},
        {0x2601, "keycap cloud"},
        {0x25C6, "keycap diamond"},
    };
    for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); i++) {
        CHECK(terminal_font_has_glyph(needed[i].cp), "missing %s (U+%04X)", needed[i].what, needed[i].cp);
    }
}

// Control codes must not be covered: Unifont draws a hex box for them, which
// would disguise the fact that something unprintable arrived.
static void test_controls_are_not_covered(void) {
    for (uint32_t cp = 0; cp < 0x20; cp++) {
        CHECK(!terminal_font_has_glyph(cp), "control U+%04X is covered", cp);
    }
    for (uint32_t cp = 0x7F; cp < 0xA0; cp++) {
        CHECK(!terminal_font_has_glyph(cp), "control U+%04X is covered", cp);
    }
}

int main(void) {
    test_no_invisible_glyphs();
    test_glyph_never_wider_than_its_cells();
    test_known_widths();
    test_essential_coverage();
    test_controls_are_not_covered();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
