#!/usr/bin/env python3
"""Turn GNU Unifont's .hex into a PAX bitmap font for the terminal.

Unifont is dual licensed GPLv2+ (with the font embedding exception) and
SIL OFL-1.1; this project uses it under the OFL.

Two things come out of here, and they are deliberately separate:

  Glyphs.  Unifont draws most characters 8x16 and East Asian ones 16x16, so the
  generated font has ranges of both widths. PAX takes one width per range, so a
  block containing both is split into runs.

  Cell widths.  How many columns a character occupies is a property of Unicode,
  not of the font: the remote host advances its own cursor using its own
  wcwidth, and if we disagree the whole line slides. So the width table is
  generated from EastAsianWidth.txt and covers all of Unicode, including
  characters this font has no glyph for — those still take two columns, they
  just draw as U+FFFD.

Usage: make_font.py <unifont.hex> <EastAsianWidth.txt> <out.c> <out.h> [options]
Options:
  --ideographs   include CJK Unified Ideographs U+4E00..U+9FFF (about 660 KB)
  --hangul       include Hangul Syllables U+AC00..U+D7A3 (about 350 KB)
"""

import sys

WIDTH_NARROW = 8
WIDTH_WIDE = 16
HEIGHT = 16

# Blocks a terminal needs. Control ranges are deliberately absent: Unifont draws
# a hex box for them, and showing that instead of nothing would only disguise
# the fact that something unprintable arrived.
NARROW_BLOCKS = [
    (0x0020, 0x007E, "Basic Latin"),
    (0x00A0, 0x00FF, "Latin-1 supplement"),
    (0x0100, 0x017F, "Latin Extended-A"),
    (0x0180, 0x024F, "Latin Extended-B"),
    (0x0250, 0x02AF, "IPA extensions"),
    (0x02B0, 0x02FF, "Spacing modifiers"),
    (0x0300, 0x036F, "Combining diacriticals"),
    (0x0370, 0x03FF, "Greek and Coptic"),
    (0x0400, 0x04FF, "Cyrillic"),
    (0x0500, 0x052F, "Cyrillic supplement"),
    (0x2000, 0x206F, "General punctuation"),
    (0x2070, 0x209F, "Super- and subscripts"),
    (0x20A0, 0x20BF, "Currency symbols"),
    (0x2100, 0x214F, "Letterlike symbols"),
    (0x2150, 0x218F, "Number forms"),
    (0x2190, 0x21FF, "Arrows"),
    (0x2200, 0x22FF, "Mathematical operators"),
    (0x2300, 0x23FF, "Miscellaneous technical"),
    (0x2400, 0x243F, "Control pictures"),
    (0x2500, 0x257F, "Box drawing"),
    (0x2580, 0x259F, "Block elements"),
    (0x25A0, 0x25FF, "Geometric shapes"),
    (0x2600, 0x26FF, "Miscellaneous symbols"),
    (0x2700, 0x27BF, "Dingbats"),
    (0x2800, 0x28FF, "Braille patterns"),
    (0xFFFD, 0xFFFD, "Replacement character"),
]

# East Asian blocks, all of them double width.
WIDE_BLOCKS = [
    (0x1100, 0x115F, "Hangul Jamo"),
    (0x2E80, 0x2EFF, "CJK radicals supplement"),
    (0x3000, 0x303F, "CJK symbols and punctuation"),
    (0x3040, 0x309F, "Hiragana"),
    (0x30A0, 0x30FF, "Katakana"),
    (0x3100, 0x312F, "Bopomofo"),
    (0x3130, 0x318F, "Hangul compatibility jamo"),
    (0x31F0, 0x31FF, "Katakana phonetic extensions"),
    (0x3200, 0x32FF, "Enclosed CJK letters and months"),
    (0x3300, 0x33FF, "CJK compatibility"),
    (0xFF00, 0xFF60, "Fullwidth forms"),
    (0xFFE0, 0xFFE6, "Fullwidth signs"),
]

OPTIONAL_BLOCKS = {
    "--ideographs": (0x4E00, 0x9FFF, "CJK unified ideographs"),
    "--hangul": (0xAC00, 0xD7A3, "Hangul syllables"),
}

# PAX reads the leftmost pixel of a row from the least significant bit; Unifont
# writes it in the most significant one.
REVERSE = [int(f"{n:08b}"[::-1], 2) for n in range(256)]


def load_glyphs(path):
    """Return (narrow, wide) dicts of codepoint -> bytes, already bit reversed."""
    narrow, wide = {}, {}
    with open(path, "r", encoding="ascii") as handle:
        for line in handle:
            line = line.strip()
            if not line or ":" not in line:
                continue
            code, bits = line.split(":", 1)
            data = bytes(REVERSE[int(bits[i:i + 2], 16)] for i in range(0, len(bits), 2))
            if len(bits) == HEIGHT * 2:
                narrow[int(code, 16)] = data
            elif len(bits) == HEIGHT * 4:
                wide[int(code, 16)] = data
    return narrow, wide


def load_wide_codepoints(path):
    """Codepoints that occupy two terminal columns: East Asian Wide and Fullwidth."""
    wide = set()
    # The file carries a copyright line in UTF-8.
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.split("#", 1)[0].strip()
            if not line or ";" not in line:
                continue
            codes, width = (part.strip() for part in line.split(";", 1))
            if width not in ("W", "F"):
                continue
            if ".." in codes:
                first, last = (int(part, 16) for part in codes.split(".."))
            else:
                first = last = int(codes, 16)
            wide.update(range(first, last + 1))
    return wide


def runs(pairs):
    """Collapse a sorted list of (codepoint, data) into contiguous runs."""
    out = []
    for code, data in pairs:
        if out and code == out[-1][-1][0] + 1:
            out[-1].append((code, data))
        else:
            out.append([(code, data)])
    return out


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    if len(argv) != 4:
        print(__doc__, file=sys.stderr)
        return 1

    hex_path, eaw_path, c_path, h_path = argv
    narrow_glyphs, wide_glyphs = load_glyphs(hex_path)
    wide_cells = load_wide_codepoints(eaw_path)

    blocks = [(a, b, name, False) for a, b, name in NARROW_BLOCKS]
    blocks += [(a, b, name, True) for a, b, name in WIDE_BLOCKS]
    for flag, (a, b, name) in OPTIONAL_BLOCKS.items():
        if flag in flags:
            blocks.append((a, b, name, True))

    narrow_pairs, wide_pairs, skipped = [], [], []
    for first, last, _name, _is_wide_block in blocks:
        for code in range(first, last + 1):
            if code in narrow_glyphs:
                narrow_pairs.append((code, narrow_glyphs[code]))
            elif code in wide_glyphs:
                # A 16 pixel glyph only earns a place if Unicode agrees the
                # character is two columns wide. Otherwise drawing it would
                # either spill into the next cell or have to be squeezed, and
                # either way our idea of the cursor would drift from the host's.
                if code in wide_cells:
                    wide_pairs.append((code, wide_glyphs[code]))
                else:
                    skipped.append(code)

    narrow_pairs.sort()
    wide_pairs.sort()

    out = [
        "// SPDX-License-Identifier: MIT",
        "//",
        "// Generated by tools/make_font.py; do not edit.",
        "// Glyph data is GNU Unifont, used under SIL OFL-1.1.",
        "// Cell widths are derived from the Unicode East Asian Width data file.",
        "",
        '#include "terminal_font.h"',
        "",
    ]

    tables = []  # (identifier, width, first, last)

    def emit(pairs, width, label):
        for run in runs(pairs):
            first, last = run[0][0], run[-1][0]
            name = f"glyphs_{width}_{first:04x}"
            out.append(f"// {label} U+{first:04X}..U+{last:04X}")
            out.append(f"static uint8_t const {name}[] = {{")
            for code, data in run:
                body = ", ".join(f"0x{b:02X}" for b in data)
                out.append(f"    {body},  // U+{code:04X}")
            out.append("};")
            out.append("")
            tables.append((name, width, first, last))

    # Narrow first: almost all text is Latin, and PAX scans the range list in
    # order for every character that leaves the previous range.
    emit(narrow_pairs, WIDTH_NARROW, "narrow")
    emit(wide_pairs, WIDTH_WIDE, "wide")

    out.append("static pax_font_range_t const terminal_ranges[] = {")
    for name, width, first, last in tables:
        out.append("    {")
        out.append("        .type  = PAX_FONT_TYPE_BITMAP_MONO,")
        out.append(f"        .start = 0x{first:05X},")
        out.append(f"        .end   = 0x{last:05X},")
        out.append("        .bitmap_mono =")
        out.append("            {")
        out.append(f"                .glyphs = {name},")
        out.append(f"                .width  = {width},")
        out.append(f"                .height = {HEIGHT},")
        out.append("                .bpp    = 1,")
        out.append("            },")
        out.append("    },")
    out.append("};")
    out.append("")
    out.append("#define RANGE_COUNT (sizeof(terminal_ranges) / sizeof(terminal_ranges[0]))")
    out.append("")
    out.append("pax_font_t const terminal_font_raw = {")
    out.append('    .name         = "Unifont",')
    out.append("    .n_ranges     = RANGE_COUNT,")
    out.append("    .ranges       = terminal_ranges,")
    out.append(f"    .default_size = {HEIGHT},")
    out.append("    .recommend_aa = false,")
    out.append("};")
    out.append("")

    # Cell widths: every Wide and Fullwidth range in Unicode, glyph or no glyph.
    wide_ranges = []
    for code in sorted(wide_cells):
        if wide_ranges and code == wide_ranges[-1][1] + 1:
            wide_ranges[-1][1] = code
        else:
            wide_ranges.append([code, code])

    out.append("// Every East Asian Wide or Fullwidth range in Unicode. Kept separate from")
    out.append("// the glyph ranges on purpose: a character we cannot draw still occupies two")
    out.append("// columns on the host, so the terminal has to agree about that either way.")
    out.append("static struct {")
    out.append("    uint32_t start;")
    out.append("    uint32_t end;")
    out.append("} const wide_cells[] = {")
    for first, last in wide_ranges:
        out.append(f"    {{0x{first:05X}, 0x{last:05X}}},")
    out.append("};")
    out.append("")
    out.append("bool terminal_font_has_glyph(uint32_t codepoint) {")
    out.append("    for (size_t i = 0; i < RANGE_COUNT; i++) {")
    out.append("        if (codepoint >= terminal_ranges[i].start && codepoint <= terminal_ranges[i].end) {")
    out.append("            return true;")
    out.append("        }")
    out.append("    }")
    out.append("    return false;")
    out.append("}")
    out.append("")
    out.append("int terminal_char_width(uint32_t codepoint) {")
    out.append("    size_t low  = 0;")
    out.append("    size_t high = sizeof(wide_cells) / sizeof(wide_cells[0]);")
    out.append("    while (low < high) {")
    out.append("        size_t middle = low + (high - low) / 2;")
    out.append("        if (codepoint < wide_cells[middle].start) {")
    out.append("            high = middle;")
    out.append("        } else if (codepoint > wide_cells[middle].end) {")
    out.append("            low = middle + 1;")
    out.append("        } else {")
    out.append("            return 2;")
    out.append("        }")
    out.append("    }")
    out.append("    return 1;")
    out.append("}")
    out.append("")

    with open(c_path, "w", encoding="ascii") as handle:
        handle.write("\n".join(out))

    header = f"""// SPDX-License-Identifier: MIT
//
// An 8x16 bitmap font for the terminal, with 16x16 glyphs for the East Asian
// characters that occupy two columns. Generated by tools/make_font.py; the
// glyphs are GNU Unifont (SIL OFL-1.1) and the cell widths come from the
// Unicode East Asian Width data file.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pax_types.h"

#define TERMINAL_FONT_WIDTH  {WIDTH_NARROW}
#define TERMINAL_FONT_HEIGHT {HEIGHT}

extern pax_font_t const terminal_font_raw;
#define terminal_font (&terminal_font_raw)

// True when the font can draw this codepoint. Anything else has to be replaced
// before drawing, because PAX advances by nothing for a glyph it does not have.
bool terminal_font_has_glyph(uint32_t codepoint);

// How many columns the character occupies: 2 for East Asian Wide and Fullwidth,
// 1 for everything else. This is a Unicode property and does not depend on
// whether the font has a glyph, because the host counts columns the same way.
int terminal_char_width(uint32_t codepoint);
"""
    with open(h_path, "w", encoding="ascii") as handle:
        handle.write(header)

    narrow_bytes = len(narrow_pairs) * HEIGHT
    wide_bytes = len(wide_pairs) * HEIGHT * 2
    print(
        f"{len(narrow_pairs)} narrow ({narrow_bytes // 1024} KB), "
        f"{len(wide_pairs)} wide ({wide_bytes // 1024} KB), "
        f"{len(tables)} ranges, {len(wide_ranges)} width ranges, "
        f"{len(skipped)} skipped as 16px but single width",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
