// SPDX-License-Identifier: MIT

#include "keymap.h"
#include <stdio.h>
#include <string.h>

// xterm encodes modified special keys as CSI 1;<n> <final>, where n is a
// bitfield offset by one: shift 1, alt 2, ctrl 4.
static int modifier_code(uint32_t modifiers) {
    int code = 0;
    if (modifiers & BSP_INPUT_MODIFIER_SHIFT) {
        code |= 1;
    }
    if (modifiers & BSP_INPUT_MODIFIER_ALT) {
        code |= 2;
    }
    if (modifiers & BSP_INPUT_MODIFIER_CTRL) {
        code |= 4;
    }
    return code ? code + 1 : 0;
}

static void emit(keymap_result_t* out, char const* text) {
    size_t len = strlen(text);
    if (len > KEYMAP_MAX_BYTES) {
        len = KEYMAP_MAX_BYTES;
    }
    memcpy(out->bytes, text, len);
    out->len = len;
}

static void emit_csi(keymap_result_t* out, uint32_t modifiers, char final) {
    int mod = modifier_code(modifiers);
    if (mod) {
        char buffer[KEYMAP_MAX_BYTES];
        snprintf(buffer, sizeof(buffer), "\033[1;%d%c", mod, final);
        emit(out, buffer);
    } else {
        char buffer[4] = {'\033', '[', final, 0};
        emit(out, buffer);
    }
}

// Keys that report as "CSI <n> ~": Home, End, PgUp, PgDn, Delete, F5 and up.
static void emit_tilde(keymap_result_t* out, uint32_t modifiers, int number) {
    char buffer[KEYMAP_MAX_BYTES];
    int  mod = modifier_code(modifiers);
    if (mod) {
        snprintf(buffer, sizeof(buffer), "\033[%d;%d~", number, mod);
    } else {
        snprintf(buffer, sizeof(buffer), "\033[%d~", number);
    }
    emit(out, buffer);
}

static bool translate_navigation(bsp_input_event_args_navigation_t const* nav, bool app_cursor, keymap_result_t* out) {
    uint32_t mods      = nav->modifiers;
    // Tanmatsu has no dedicated Home/End/PgUp/PgDn/Delete keys, so Fn turns the
    // arrows and backspace into them. Fn is then not a modifier for the host.
    bool     fn        = (mods & BSP_INPUT_MODIFIER_FUNCTION) != 0;
    uint32_t host_mods = mods & ~BSP_INPUT_MODIFIER_FUNCTION;

    switch (nav->key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            if (fn) {
                emit_tilde(out, host_mods, 5);  // PgUp
            } else if (app_cursor && !modifier_code(host_mods)) {
                emit(out, "\033OA");
            } else {
                emit_csi(out, host_mods, 'A');
            }
            return true;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            if (fn) {
                emit_tilde(out, host_mods, 6);  // PgDn
            } else if (app_cursor && !modifier_code(host_mods)) {
                emit(out, "\033OB");
            } else {
                emit_csi(out, host_mods, 'B');
            }
            return true;
        case BSP_INPUT_NAVIGATION_KEY_RIGHT:
            if (fn) {
                emit_csi(out, host_mods, 'F');  // End
            } else if (app_cursor && !modifier_code(host_mods)) {
                emit(out, "\033OC");
            } else {
                emit_csi(out, host_mods, 'C');
            }
            return true;
        case BSP_INPUT_NAVIGATION_KEY_LEFT:
            if (fn) {
                emit_csi(out, host_mods, 'H');  // Home
            } else if (app_cursor && !modifier_code(host_mods)) {
                emit(out, "\033OD");
            } else {
                emit_csi(out, host_mods, 'D');
            }
            return true;
        case BSP_INPUT_NAVIGATION_KEY_HOME:
            emit_csi(out, host_mods, 'H');
            return true;
        case BSP_INPUT_NAVIGATION_KEY_END:
            emit_csi(out, host_mods, 'F');
            return true;
        case BSP_INPUT_NAVIGATION_KEY_PGUP:
            emit_tilde(out, host_mods, 5);
            return true;
        case BSP_INPUT_NAVIGATION_KEY_PGDN:
            emit_tilde(out, host_mods, 6);
            return true;
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            emit(out, "\r");
            return true;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
            emit(out, "\033");
            return true;
        case BSP_INPUT_NAVIGATION_KEY_TAB:
            if (host_mods & BSP_INPUT_MODIFIER_SHIFT) {
                emit(out, "\033[Z");
            } else {
                emit(out, "\t");
            }
            return true;
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE:
            if (fn) {
                emit_tilde(out, host_mods, 3);  // Delete
            } else {
                emit(out, "\177");
            }
            return true;
        case BSP_INPUT_NAVIGATION_KEY_SPACE_L:
        case BSP_INPUT_NAVIGATION_KEY_SPACE_M:
        case BSP_INPUT_NAVIGATION_KEY_SPACE_R:
            if (host_mods & BSP_INPUT_MODIFIER_CTRL) {
                out->bytes[0] = '\0';  // Ctrl+Space is NUL
                out->len      = 1;
            } else {
                emit(out, " ");
            }
            return true;

        // Fn shifts the six function keys up to F7..F12.
        case BSP_INPUT_NAVIGATION_KEY_F1:
            fn ? emit_tilde(out, host_mods, 18) : emit(out, "\033OP");
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F2:
            fn ? emit_tilde(out, host_mods, 19) : emit(out, "\033OQ");
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F3:
            fn ? emit_tilde(out, host_mods, 20) : emit(out, "\033OR");
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F4:
            fn ? emit_tilde(out, host_mods, 21) : emit(out, "\033OS");
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F5:
            emit_tilde(out, host_mods, fn ? 23 : 15);
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F6:
            emit_tilde(out, host_mods, fn ? 24 : 17);
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F7:
            emit_tilde(out, host_mods, 18);
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F8:
            emit_tilde(out, host_mods, 19);
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F9:
            emit_tilde(out, host_mods, 20);
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F10:
            emit_tilde(out, host_mods, 21);
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F11:
            emit_tilde(out, host_mods, 23);
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F12:
            emit_tilde(out, host_mods, 24);
            return true;
        default:
            return false;
    }
}

static bool translate_keyboard(bsp_input_event_args_keyboard_t const* key, keymap_result_t* out) {
    char const* text  = key->utf8[0] ? key->utf8 : NULL;
    char        ascii = key->ascii;

    if (key->modifiers & BSP_INPUT_MODIFIER_CTRL) {
        // Ctrl clears the top three bits: Ctrl+A is 0x01, Ctrl+[ is escape.
        char c = ascii;
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 1;
        } else if (c >= 'A' && c <= 'Z') {
            c -= 'A' - 1;
        } else if (c == '@' || c == ' ') {
            c = 0;
        } else if (c >= '[' && c <= '_') {
            c -= '@';
        } else if (c == '?') {
            c = 0x7F;
        } else if (c == '2') {
            c = 0;
        } else if (c >= '3' && c <= '7') {
            c = c - '3' + 0x1B;
        } else if (c == '8') {
            c = 0x7F;
        } else {
            return false;
        }
        out->bytes[0] = c;
        out->len      = 1;
        return true;
    }

    if (!text && !ascii) {
        return false;
    }
    if (!text) {
        out->bytes[0] = ascii;
        out->len      = 1;
    } else {
        emit(out, text);
    }

    // Left Alt is the meta prefix; right Alt is AltGr and already folded into
    // the UTF-8 the board reports.
    if (key->modifiers & BSP_INPUT_MODIFIER_ALT_L) {
        if (out->len + 1 <= KEYMAP_MAX_BYTES) {
            memmove(out->bytes + 1, out->bytes, out->len);
            out->bytes[0] = '\033';
            out->len++;
        }
    }
    return true;
}

bool keymap_translate(bsp_input_event_t const* event, bool app_cursor, keymap_result_t* out) {
    out->len = 0;
    switch (event->type) {
        case INPUT_EVENT_TYPE_NAVIGATION:
            if (!event->args_navigation.state) {
                return false;  // Key releases send nothing
            }
            return translate_navigation(&event->args_navigation, app_cursor, out);
        case INPUT_EVENT_TYPE_KEYBOARD:
            return translate_keyboard(&event->args_keyboard, out);
        default:
            return false;
    }
}

char const* keymap_text(bsp_input_event_t const* event) {
    if (event->type == INPUT_EVENT_TYPE_KEYBOARD) {
        if (event->args_keyboard.modifiers & (BSP_INPUT_MODIFIER_CTRL | BSP_INPUT_MODIFIER_ALT_L)) {
            return NULL;
        }
        if (event->args_keyboard.utf8[0] >= ' ') {
            return event->args_keyboard.utf8;
        }
        return NULL;
    }
    if (event->type == INPUT_EVENT_TYPE_NAVIGATION && event->args_navigation.state) {
        switch (event->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_SPACE_L:
            case BSP_INPUT_NAVIGATION_KEY_SPACE_M:
            case BSP_INPUT_NAVIGATION_KEY_SPACE_R:
                return " ";
            default:
                return NULL;
        }
    }
    return NULL;
}
