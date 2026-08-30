// SPDX-License-Identifier: MIT
// Stand-in for the PAX header of the same name, with only what the generated
// font data needs. See shims.h.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { PAX_FONT_TYPE_BITMAP_MONO = 0, PAX_FONT_TYPE_BITMAP_VAR = 1 } pax_font_type_t;

typedef struct {
    pax_font_type_t type;
    uint32_t        start;
    uint32_t        end;
    union {
        struct {
            uint8_t const* glyphs;
            uint8_t        width;
            uint8_t        height;
            uint8_t        bpp;
        } bitmap_mono;
    };
} pax_font_range_t;

typedef struct {
    char const*             name;
    size_t                  n_ranges;
    pax_font_range_t const* ranges;
    uint16_t                default_size;
    bool                    recommend_aa;
} pax_font_t;
