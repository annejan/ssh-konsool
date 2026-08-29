// SPDX-License-Identifier: MIT
//
// Screens: the connection list, the connection editor, the key page and the
// terminal itself.

#pragma once

#include <stdbool.h>
#include "bsp/input.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pax_gfx.h"
#include "ssh_client.h"
#include "terminal.h"

esp_err_t ui_init(pax_buf_t* fb, term_t* term, SemaphoreHandle_t term_lock, ssh_client_t* ssh);

// Feed one input event. Returns true when the screen has to be redrawn.
bool ui_handle_event(bsp_input_event_t const* event);

// Called regularly; picks up session state changes and blinks the cursor.
// Returns true when the screen has to be redrawn.
bool ui_tick(void);

// Paint the current screen into the framebuffer.
void ui_draw(void);

// Set once the network is up, for the status bar.
void ui_set_network(char const* description);
