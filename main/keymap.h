// SPDX-License-Identifier: MIT
//
// Translation from board input events to the bytes a remote shell expects.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "bsp/input.h"

#define KEYMAP_MAX_BYTES 16

typedef struct {
    char   bytes[KEYMAP_MAX_BYTES];
    size_t len;
} keymap_result_t;

// Turn one input event into a byte sequence for the remote side.
// `app_cursor` selects the SS3 form of the arrow keys (DECCKM).
// Returns false when the event produces nothing to send.
bool keymap_translate(bsp_input_event_t const* event, bool app_cursor, keymap_result_t* out);

// Printable text for on-screen fields: returns the UTF-8 the key stands for, or
// NULL when the key is not text. Control and modifier combinations are excluded.
char const* keymap_text(bsp_input_event_t const* event);
