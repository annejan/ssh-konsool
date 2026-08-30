// SPDX-License-Identifier: MIT
// Stand-in for the ESP-IDF header of the same name; see shims.h. The badge asks
// for the cell grids in SPIRAM and falls back to internal RAM; a development
// machine has one heap, so the capabilities are ignored.
#pragma once
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_8BIT   (1 << 2)
#define MALLOC_CAP_SPIRAM (1 << 10)

static inline void* heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}
