// SPDX-License-Identifier: MIT

#include "ui.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "bsp/device.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hosts.h"
#include "keymap.h"
#include "keystore.h"
#include "mbedtls/platform_util.h"
#include "pax_fonts.h"
#include "pax_text.h"
#include "qrcode.h"
#include "term_render.h"
#include "terminal_font.h"

static char const TAG[] = "ui";

#define COL_BG     0xFF0E1116
#define COL_PANEL  0xFF19212B
#define COL_TEXT   0xFFD8E2EC
#define COL_DIM    0xFF7E8B99
#define COL_ACCENT 0xFF5CE07A
#define COL_WARN   0xFFE0B64A
#define COL_ERROR  0xFFE05C4A
#define COL_SELECT 0xFF2A3A4C

#define FONT_UI    terminal_font
#define SIZE_TITLE 16.0f
#define SIZE_BODY  16.0f
#define SIZE_SMALL 16.0f
#define LINE_BODY  20.0f

typedef enum {
    SCREEN_MENU = 0,
    SCREEN_EDIT,
    SCREEN_KEY,
    SCREEN_TERMINAL,
} screen_t;

// One editable line of text.
typedef struct {
    char*  buffer;
    size_t capacity;
    size_t cursor;
    bool   secret;
} field_t;

typedef enum {
    EDIT_HOST = 0,
    EDIT_PORT,
    EDIT_USER,
    EDIT_PASSWORD,
    EDIT_SAVE_PASSWORD,
    EDIT_USE_KEY,
    EDIT_FIELD_COUNT,
} edit_field_t;

static struct {
    pax_buf_t*        fb;
    term_t*           term;
    SemaphoreHandle_t term_lock;
    ssh_client_t*     ssh;

    screen_t screen;
    bool     needs_full_redraw;

    // Connection list
    int menu_index;
    // The entry F3 is armed to delete, or -1. Deleting cannot be undone, so it
    // takes two presses, the way the key page treats regenerating.
    int delete_index;

    // Editor
    host_profile_t draft;
    // Whether F3 in the editor is armed to delete. Two presses, like the menu.
    bool           edit_confirm_delete;
    char           port_text[8];
    int            edit_index;  // Index in the saved list, or -1 for a new one
    edit_field_t   edit_field;

    // Key page
    int  key_scroll;
    bool key_confirm_regenerate;

    // Terminal
    term_render_t render;
    int           font_scale;
    bool          cursor_on;
    int64_t       cursor_toggled_us;
    ssh_state_t   last_state;
    // A changed host key is accepted only on a second, deliberate press, so a
    // habitual Enter cannot re-pin a man-in-the-middle's key.
    bool          host_change_armed;

    // The saved connection a key install is running for, or -1
    int copy_id_index;

    // Modal password entry inside the terminal screen
    bool modal_password;
    char modal_buffer[HOST_PASSWORD_MAX];

    char    network[64];
    char    toast[96];
    int64_t toast_until_us;
} app;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void toast(char const* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(app.toast, sizeof(app.toast), format, args);
    va_end(args);
    app.toast_until_us = esp_timer_get_time() + 3000000;
    ESP_LOGI(TAG, "%s", app.toast);
}

static int menu_entry_count(void) {
    // Saved connections, then: new connection, SSH key, exit.
    return hosts_count() + 3;
}

static void field_insert(field_t* field, char const* text) {
    size_t length = strlen(field->buffer);
    size_t add    = strlen(text);
    if (length + add >= field->capacity) {
        return;
    }
    memmove(field->buffer + field->cursor + add, field->buffer + field->cursor, length - field->cursor + 1);
    memcpy(field->buffer + field->cursor, text, add);
    field->cursor += add;
}

static void field_backspace(field_t* field) {
    if (field->cursor == 0) {
        return;
    }
    // Step back over a whole UTF-8 character, not just one byte.
    size_t start = field->cursor - 1;
    while (start > 0 && (field->buffer[start] & 0xC0) == 0x80) {
        start--;
    }
    size_t length = strlen(field->buffer);
    memmove(field->buffer + start, field->buffer + field->cursor, length - field->cursor + 1);
    field->cursor = start;
}

// Returns true when the event was text editing that changed the field.
static bool field_handle(field_t* field, bsp_input_event_t const* event) {
    char const* text = keymap_text(event);
    if (text) {
        field_insert(field, text);
        return true;
    }
    if (event->type != INPUT_EVENT_TYPE_NAVIGATION || !event->args_navigation.state) {
        return false;
    }
    switch (event->args_navigation.key) {
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE:
            field_backspace(field);
            return true;
        case BSP_INPUT_NAVIGATION_KEY_LEFT:
            if (field->cursor > 0) {
                do {
                    field->cursor--;
                } while (field->cursor > 0 && (field->buffer[field->cursor] & 0xC0) == 0x80);
            }
            return true;
        case BSP_INPUT_NAVIGATION_KEY_RIGHT:
            if (field->buffer[field->cursor]) {
                do {
                    field->cursor++;
                } while (field->buffer[field->cursor] && (field->buffer[field->cursor] & 0xC0) == 0x80);
            }
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

static float draw_title(char const* text) {
    float width = pax_buf_get_width(app.fb);
    pax_simple_rect(app.fb, COL_PANEL, 0, 0, width, 24);
    pax_draw_text(app.fb, COL_ACCENT, FONT_UI, SIZE_TITLE, 8, 4, text);
    return 32;
}

static void draw_footer(char const* text) {
    float width  = pax_buf_get_width(app.fb);
    float height = pax_buf_get_height(app.fb);
    pax_simple_rect(app.fb, COL_PANEL, 0, height - 22, width, 22);
    pax_draw_text(app.fb, COL_DIM, FONT_UI, SIZE_SMALL, 8, height - 19, text);
}

static void draw_toast(void) {
    if (esp_timer_get_time() > app.toast_until_us || !app.toast[0]) {
        return;
    }
    float width  = pax_buf_get_width(app.fb);
    float height = pax_buf_get_height(app.fb);
    float box    = pax_text_size(FONT_UI, SIZE_BODY, app.toast).x + 16;
    pax_simple_rect(app.fb, COL_SELECT, (width - box) / 2, height - 56, box, 26);
    pax_center_text(app.fb, COL_TEXT, FONT_UI, SIZE_BODY, width / 2, height - 52, app.toast);
}

// ---------------------------------------------------------------------------
// Connection list
// ---------------------------------------------------------------------------

static void draw_menu(void) {
    pax_background(app.fb, COL_BG);
    float y     = draw_title("SSH");
    float width = pax_buf_get_width(app.fb);

    int count = menu_entry_count();
    for (int index = 0; index < count; index++) {
        bool  selected = index == app.menu_index;
        bool  doomed   = index == app.delete_index;
        float row_y    = y + index * LINE_BODY;
        if (selected || doomed) {
            pax_simple_rect(app.fb, doomed ? COL_ERROR : COL_SELECT, 4, row_y - 2, width - 8, LINE_BODY);
        }

        char line[128];
        if (index < hosts_count()) {
            host_profile_t profile;
            hosts_get(index, &profile);
            snprintf(line, sizeof(line), "%s@%s:%u", profile.user, profile.host, (unsigned)profile.port);
        } else if (index == hosts_count()) {
            snprintf(line, sizeof(line), "+ New connection");
        } else if (index == hosts_count() + 1) {
            snprintf(line, sizeof(line), "* SSH key: %s", keystore_fingerprint() ? keystore_fingerprint() : "-");
        } else {
            snprintf(line, sizeof(line), "< Back to the launcher");
        }
        pax_draw_text(app.fb, selected ? COL_TEXT : COL_DIM, FONT_UI, SIZE_BODY, 12, row_y, line);
    }

    char status[160];
    if (app.delete_index >= 0 && app.delete_index < hosts_count()) {
        host_profile_t pending;
        if (hosts_get(app.delete_index, &pending)) {
            snprintf(status, sizeof(status), "F3 again to delete %s@%s   any other key cancels", pending.user,
                     pending.host);
            mbedtls_platform_zeroize(&pending, sizeof(pending));
        }
    } else {
        snprintf(status, sizeof(status), "%s   enter: open   F2: edit   F3: delete   F4: install badge key",
                 app.network);
    }
    draw_footer(status);
    draw_toast();
}

// ---------------------------------------------------------------------------
// Connection editor
// ---------------------------------------------------------------------------

static field_t editor_field(void) {
    switch (app.edit_field) {
        case EDIT_HOST:
            return (field_t){app.draft.host, sizeof(app.draft.host), strlen(app.draft.host), false};
        case EDIT_PORT:
            return (field_t){app.port_text, sizeof(app.port_text), strlen(app.port_text), false};
        case EDIT_USER:
            return (field_t){app.draft.user, sizeof(app.draft.user), strlen(app.draft.user), false};
        case EDIT_PASSWORD:
            return (field_t){app.draft.password, sizeof(app.draft.password), strlen(app.draft.password), true};
        default:
            return (field_t){NULL, 0, 0, false};
    }
}

static void draw_edit(void) {
    pax_background(app.fb, COL_BG);
    float y = draw_title(app.edit_index < 0 ? "New connection" : "Edit connection");

    char const* labels[EDIT_FIELD_COUNT] = {"Host",         "Port", "User", "Password", "Keep password on badge",
                                            "Use badge key"};
    char        values[EDIT_FIELD_COUNT][96];

    snprintf(values[EDIT_HOST], sizeof(values[0]), "%s", app.draft.host);
    snprintf(values[EDIT_PORT], sizeof(values[0]), "%s", app.port_text);
    snprintf(values[EDIT_USER], sizeof(values[0]), "%s", app.draft.user);

    size_t hidden = strlen(app.draft.password);
    if (hidden > sizeof(values[0]) - 1) {
        hidden = sizeof(values[0]) - 1;
    }
    memset(values[EDIT_PASSWORD], '*', hidden);
    values[EDIT_PASSWORD][hidden] = '\0';

    snprintf(values[EDIT_SAVE_PASSWORD], sizeof(values[0]), "%s", app.draft.save_password ? "[x]" : "[ ]");
    snprintf(values[EDIT_USE_KEY], sizeof(values[0]), "%s", app.draft.use_key ? "[x]" : "[ ]");

    float width = pax_buf_get_width(app.fb);
    for (int index = 0; index < EDIT_FIELD_COUNT; index++) {
        float row_y    = y + index * (LINE_BODY + 4);
        bool  selected = index == app.edit_field;
        if (selected) {
            pax_simple_rect(app.fb, COL_SELECT, 4, row_y - 2, width - 8, LINE_BODY);
        }
        pax_draw_text(app.fb, COL_DIM, FONT_UI, SIZE_BODY, 12, row_y, labels[index]);
        pax_draw_text(app.fb, selected ? COL_ACCENT : COL_TEXT, FONT_UI, SIZE_BODY, 180, row_y, values[index]);
    }

    float note_y = y + EDIT_FIELD_COUNT * (LINE_BODY + 4) + 10;
    if (app.draft.save_password) {
        // Say what keeping it actually means, rather than leaving "save" to
        // sound harmless.
        pax_draw_text(app.fb, COL_WARN, FONT_UI, SIZE_SMALL, 12, note_y,
                      "Stored unencrypted; anyone with the badge can read it. F4 installs the key instead.");
        note_y += LINE_BODY;
    }
    if (app.draft.use_key) {
        pax_draw_text(app.fb, COL_DIM, FONT_UI, SIZE_SMALL, 12, note_y,
                      "The badge key has to be in the server's authorized_keys; see the SSH key page.");
    }

    if (app.edit_confirm_delete) {
        draw_footer("F3 again to delete this connection   any other key cancels");
    } else {
        draw_footer("enter: connect   F2: save   F3: delete   F4: install badge key   esc: back   space: toggle");
    }
    draw_toast();
}

// ---------------------------------------------------------------------------
// Key page
// ---------------------------------------------------------------------------

static float qr_origin_x;
static float qr_origin_y;
static float qr_module;

static void draw_qr_module(esp_qrcode_handle_t qrcode) {
    int size = esp_qrcode_get_size(qrcode);
    if (size <= 0) {
        return;
    }
    float available = 200;
    qr_module       = (float)((int)(available / (size + 8)));
    if (qr_module < 1) {
        qr_module = 1;
    }
    float extent = (size + 8) * qr_module;

    pax_simple_rect(app.fb, 0xFFFFFFFF, qr_origin_x, qr_origin_y, extent, extent);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (esp_qrcode_get_module(qrcode, x, y)) {
                pax_simple_rect(app.fb, 0xFF000000, qr_origin_x + (x + 4) * qr_module,
                                qr_origin_y + (y + 4) * qr_module, qr_module, qr_module);
            }
        }
    }
}

static void draw_key(void) {
    pax_background(app.fb, COL_BG);
    float y     = draw_title("SSH key");
    float width = pax_buf_get_width(app.fb);

    char const* public_key = keystore_public_key();
    if (!public_key) {
        pax_draw_text(app.fb, COL_ERROR, FONT_UI, SIZE_BODY, 12, y, "No key available.");
        draw_footer("esc: back");
        return;
    }

    pax_draw_text(app.fb, COL_DIM, FONT_UI, SIZE_BODY, 12, y, "Fingerprint");
    pax_draw_text(app.fb, COL_ACCENT, FONT_UI, SIZE_BODY, 12, y + LINE_BODY, keystore_fingerprint());

    // The key itself, wrapped to the width left of the QR code.
    float text_top   = y + LINE_BODY * 2 + 8;
    float text_width = width - 240;
    int   per_line   = (int)(text_width / TERMINAL_FONT_WIDTH);
    if (per_line < 8) {
        per_line = 8;
    }

    pax_draw_text(app.fb, COL_DIM, FONT_UI, SIZE_SMALL, 12, text_top, "Add this line to ~/.ssh/authorized_keys:");
    float line_y = text_top + 12;
    for (size_t offset = 0; public_key[offset]; offset += per_line) {
        char   chunk[160];
        size_t length = strlen(public_key + offset);
        if (length > (size_t)per_line) {
            length = per_line;
        }
        if (length >= sizeof(chunk)) {
            length = sizeof(chunk) - 1;
        }
        memcpy(chunk, public_key + offset, length);
        chunk[length] = '\0';
        pax_draw_text(app.fb, COL_TEXT, FONT_UI, SIZE_SMALL, 12, line_y, chunk);
        line_y += TERMINAL_FONT_HEIGHT + 1;
        if (line_y > pax_buf_get_height(app.fb) - 60) {
            break;
        }
    }

    qr_origin_x                = width - 220;
    qr_origin_y                = y + 10;
    esp_qrcode_config_t config = ESP_QRCODE_CONFIG_DEFAULT();
    config.display_func        = draw_qr_module;
    config.max_qrcode_version  = 20;
    if (esp_qrcode_generate(&config, public_key) != ESP_OK) {
        pax_draw_text(app.fb, COL_WARN, FONT_UI, SIZE_SMALL, qr_origin_x, qr_origin_y,
                      "The key does not fit in a QR code.");
    }

    if (app.key_confirm_regenerate) {
        draw_footer("F4 again to replace the key - every server trusting the old one stops working");
    } else {
        draw_footer("F1: save to SD   F2: save to /int/ssh   F3: print on serial   F4: new key   esc: back");
    }
    draw_toast();
}

// ---------------------------------------------------------------------------
// Terminal screen
// ---------------------------------------------------------------------------

static void draw_modal_ex(char const* title, char const* body, char const* hint, uint32_t accent) {
    float width  = pax_buf_get_width(app.fb);
    float height = pax_buf_get_height(app.fb);
    float box_w  = width - 80;
    float box_h  = 150;
    float box_x  = 40;
    float box_y  = (height - box_h) / 2;

    pax_simple_rect(app.fb, COL_PANEL, box_x, box_y, box_w, box_h);
    pax_outline_rect(app.fb, accent, box_x, box_y, box_w, box_h);
    pax_draw_text(app.fb, accent, FONT_UI, SIZE_TITLE, box_x + 12, box_y + 10, title);

    float line_y = box_y + 42;
    // The body may be several lines, separated by newlines.
    char  copy[256];
    strlcpy(copy, body, sizeof(copy));
    char* cursor = copy;
    while (cursor && *cursor) {
        char* newline = strchr(cursor, '\n');
        if (newline) {
            *newline = '\0';
        }
        pax_draw_text(app.fb, COL_TEXT, FONT_UI, SIZE_BODY, box_x + 12, line_y, cursor);
        line_y += LINE_BODY;
        cursor  = newline ? newline + 1 : NULL;
    }

    pax_draw_text(app.fb, COL_DIM, FONT_UI, SIZE_SMALL, box_x + 12, box_y + box_h - 16, hint);
}

static void draw_modal(char const* title, char const* body, char const* hint) {
    draw_modal_ex(title, body, hint, COL_ACCENT);
}

static void draw_terminal(void) {
    ssh_state_t state = ssh_client_state(app.ssh);

    xSemaphoreTake(app.term_lock, portMAX_DELAY);
    app.render.cursor_on = app.cursor_on;
    term_render_draw(&app.render, app.term, app.needs_full_redraw);
    xSemaphoreGive(app.term_lock);
    app.needs_full_redraw = false;

    char        status[160];
    char const* scroll_note = "";
    xSemaphoreTake(app.term_lock, portMAX_DELAY);
    int offset = term_scroll_offset(app.term);
    xSemaphoreGive(app.term_lock);
    if (offset > 0) {
        scroll_note = "  [scrollback]";
    }
    snprintf(status, sizeof(status), "%s  %dx%d%s   fn+esc: menu   fn+F1: zoom", ssh_client_status(app.ssh),
             app.render.cols, app.render.rows, scroll_note);
    term_render_status(&app.render, status);

    if (state == SSH_STATE_VERIFY_HOST) {
        if (ssh_client_host_changed(app.ssh)) {
            char body[256];
            snprintf(body, sizeof(body), "WARNING: this host key has CHANGED since last time.\n%s",
                     ssh_client_host_fingerprint(app.ssh));
            // A changed key is the man-in-the-middle case, so it is painted red
            // and takes two presses, unlike the benign first-contact prompt.
            draw_modal_ex("Host key CHANGED", body,
                          app.host_change_armed ? "enter AGAIN to accept the new key   y: accept once   esc: cancel"
                                                : "possible interception. enter: review   y: accept once   esc: cancel",
                          COL_ERROR);
        } else {
            char body[256];
            snprintf(body, sizeof(body), "This host has not been seen before.\n%s",
                     ssh_client_host_fingerprint(app.ssh));
            draw_modal("Host key", body, "enter: accept and remember   y: accept once   esc: cancel");
        }
    } else if (state == SSH_STATE_NEED_PASSWORD || app.modal_password) {
        char   stars[HOST_PASSWORD_MAX + 1];
        size_t length = strlen(app.modal_buffer);
        if (length > HOST_PASSWORD_MAX) {
            length = HOST_PASSWORD_MAX;
        }
        memset(stars, '*', length);
        stars[length] = '\0';
        draw_modal("Password", stars, "enter: send   esc: cancel");
    }

    draw_toast();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

// Both the shell and the key install run on the terminal screen, so the host
// key and password prompts work the same way for either.
static void open_terminal_screen(void) {
    xSemaphoreTake(app.term_lock, portMAX_DELAY);
    term_resize(app.term, app.render.cols, app.render.rows);
    // A full reset, so the previous session's output is not mistaken for this
    // one's.
    term_reset(app.term);
    xSemaphoreGive(app.term_lock);

    app.screen            = SCREEN_TERMINAL;
    app.needs_full_redraw = true;
    app.modal_password    = false;
    app.host_change_armed = false;
    // Wipe the whole buffer, not just the first byte: a previous prompt's
    // password would otherwise linger in the tail.
    mbedtls_platform_zeroize(app.modal_buffer, sizeof(app.modal_buffer));
    app.last_state = SSH_STATE_IDLE;
}

static void start_connection(host_profile_t const* profile) {
    app.copy_id_index = -1;
    open_terminal_screen();
    if (ssh_client_connect(app.ssh, profile) != ESP_OK) {
        toast("Cannot start the session");
    }
}

// Once the key is on the server the password has done its job. Hand the
// connection over to the key and forget the password rather than leaving both
// on the badge, which is the whole point of installing the key.
static void finish_copy_id(void) {
    int index         = app.copy_id_index;
    app.copy_id_index = -1;
    if (index < 0) {
        return;
    }

    if (!ssh_client_copy_id_succeeded(app.ssh)) {
        return;
    }

    host_profile_t profile;
    if (!hosts_get(index, &profile)) {
        return;
    }

    bool had_password     = profile.save_password && profile.password[0];
    profile.use_key       = true;
    profile.save_password = false;
    mbedtls_platform_zeroize(profile.password, sizeof(profile.password));

    // hosts_set drops the stored password whenever save_password is clear.
    if (hosts_set(index, &profile) == ESP_OK) {
        toast(had_password ? "Key installed, password forgotten" : "Key installed, this connection now uses it");
    }
    mbedtls_platform_zeroize(&profile, sizeof(profile));
}

// Log in with the password once and leave the badge key behind, so every login
// after this one can use the key.
static void start_copy_id(int index) {
    host_profile_t profile;
    if (!hosts_get(index, &profile)) {
        return;
    }
    if (!keystore_public_key()) {
        toast("The badge has no key to install");
        return;
    }
    app.copy_id_index = index;
    open_terminal_screen();
    if (ssh_client_copy_id(app.ssh, &profile) != ESP_OK) {
        app.copy_id_index = -1;
        toast("Cannot start the session");
    }
    mbedtls_platform_zeroize(&profile, sizeof(profile));
}

static void open_editor(int index) {
    memset(&app.draft, 0, sizeof(app.draft));
    if (index >= 0 && hosts_get(index, &app.draft)) {
        app.edit_index = index;
    } else {
        app.edit_index    = -1;
        app.draft.port    = 22;
        app.draft.use_key = true;
        strlcpy(app.draft.user, "root", sizeof(app.draft.user));
    }
    snprintf(app.port_text, sizeof(app.port_text), "%u", (unsigned)(app.draft.port ? app.draft.port : 22));
    app.edit_field          = EDIT_HOST;
    app.edit_confirm_delete = false;
    app.screen              = SCREEN_EDIT;
}

static bool save_draft(void) {
    app.draft.port = (uint16_t)atoi(app.port_text);
    if (app.draft.port == 0) {
        app.draft.port = 22;
    }
    if (!app.draft.host[0] || !app.draft.user[0]) {
        toast("Host and user are required");
        return false;
    }
    int index = app.edit_index < 0 ? hosts_count() : app.edit_index;
    if (hosts_set(index, &app.draft) != ESP_OK) {
        toast("Could not save");
        return false;
    }
    app.edit_index = index;
    toast("Saved");
    return true;
}

static void export_public_key(char const* path, char const* label) {
    if (keystore_write_public_key(path) == ESP_OK) {
        toast("Written to %s", path);
    } else {
        toast("Could not write to %s", label);
    }
}

// ---------------------------------------------------------------------------
// Event handling per screen
// ---------------------------------------------------------------------------

static bool handle_menu(bsp_input_event_t const* event) {
    if (event->type != INPUT_EVENT_TYPE_NAVIGATION || !event->args_navigation.state) {
        return false;
    }
    int count = menu_entry_count();

    // Arming survives only the very next press of F3, so it cannot linger and
    // catch a press meant for something else.
    if (event->args_navigation.key != BSP_INPUT_NAVIGATION_KEY_F3) {
        app.delete_index = -1;
    }

    switch (event->args_navigation.key) {
        case BSP_INPUT_NAVIGATION_KEY_UP:
            app.menu_index = (app.menu_index + count - 1) % count;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            app.menu_index = (app.menu_index + 1) % count;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            if (app.menu_index < hosts_count()) {
                host_profile_t profile;
                if (hosts_get(app.menu_index, &profile)) {
                    start_connection(&profile);
                }
                mbedtls_platform_zeroize(&profile, sizeof(profile));
            } else if (app.menu_index == hosts_count()) {
                open_editor(-1);
            } else if (app.menu_index == hosts_count() + 1) {
                app.screen                 = SCREEN_KEY;
                app.key_confirm_regenerate = false;
            } else {
                bsp_device_restart_to_launcher();
            }
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F2:
            if (app.menu_index < hosts_count()) {
                open_editor(app.menu_index);
            } else {
                toast("Pick a connection first");
            }
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F4:
            if (app.menu_index < hosts_count()) {
                start_copy_id(app.menu_index);
            } else {
                toast("Pick a connection first");
            }
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F3:
            if (app.menu_index >= hosts_count()) {
                toast("Pick a connection first");
                return true;
            }
            if (app.delete_index != app.menu_index) {
                app.delete_index = app.menu_index;  // Ask first
                return true;
            }
            app.delete_index = -1;
            hosts_remove(app.menu_index);
            if (app.menu_index >= menu_entry_count()) {
                app.menu_index = menu_entry_count() - 1;
            }
            toast("Deleted");
            return true;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
            bsp_device_restart_to_launcher();
            return true;
        default:
            // A key the menu has no use for. Logged because a key that never
            // arrives and a key that arrives and is ignored look identical from
            // the outside.
            ESP_LOGI(TAG, "Menu ignored navigation key %d", (int)event->args_navigation.key);
            return false;
    }
}

static bool handle_edit(bsp_input_event_t const* event) {
    if (event->type == INPUT_EVENT_TYPE_NAVIGATION && event->args_navigation.state) {
        // Arming survives only the very next press of F3.
        if (event->args_navigation.key != BSP_INPUT_NAVIGATION_KEY_F3) {
            app.edit_confirm_delete = false;
        }
        switch (event->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_UP:
                app.edit_field = (app.edit_field + EDIT_FIELD_COUNT - 1) % EDIT_FIELD_COUNT;
                return true;
            case BSP_INPUT_NAVIGATION_KEY_DOWN:
            case BSP_INPUT_NAVIGATION_KEY_TAB:
                app.edit_field = (app.edit_field + 1) % EDIT_FIELD_COUNT;
                return true;
            case BSP_INPUT_NAVIGATION_KEY_ESC:
                // The draft outlives the screen, so the typed password would
                // otherwise sit in memory until the editor is opened again.
                mbedtls_platform_zeroize(app.draft.password, sizeof(app.draft.password));
                app.screen = SCREEN_MENU;
                return true;
            case BSP_INPUT_NAVIGATION_KEY_F2:
                save_draft();
                return true;
            case BSP_INPUT_NAVIGATION_KEY_F3:
                if (app.edit_index < 0) {
                    toast("Nothing saved to delete yet");
                    return true;
                }
                if (!app.edit_confirm_delete) {
                    app.edit_confirm_delete = true;  // Ask first
                    return true;
                }
                app.edit_confirm_delete = false;
                hosts_remove(app.edit_index);
                app.screen     = SCREEN_MENU;
                app.menu_index = 0;
                toast("Deleted");
                return true;
            case BSP_INPUT_NAVIGATION_KEY_F4:
                // Save first, so the connection has an index to turn the key on
                // for once the install succeeds.
                if (save_draft()) {
                    start_copy_id(app.edit_index);
                }
                return true;
            case BSP_INPUT_NAVIGATION_KEY_RETURN: {
                if (!save_draft()) {
                    return true;
                }
                host_profile_t profile = app.draft;
                profile.port           = (uint16_t)atoi(app.port_text);
                start_connection(&profile);
                mbedtls_platform_zeroize(&profile, sizeof(profile));
                return true;
            }
            case BSP_INPUT_NAVIGATION_KEY_SPACE_L:
            case BSP_INPUT_NAVIGATION_KEY_SPACE_M:
            case BSP_INPUT_NAVIGATION_KEY_SPACE_R:
                if (app.edit_field == EDIT_SAVE_PASSWORD) {
                    app.draft.save_password = !app.draft.save_password;
                    return true;
                }
                if (app.edit_field == EDIT_USE_KEY) {
                    app.draft.use_key = !app.draft.use_key;
                    return true;
                }
                break;
            default:
                ESP_LOGI(TAG, "Editor ignored navigation key %d", (int)event->args_navigation.key);
                break;
        }
    }

    field_t field = editor_field();
    if (field.buffer) {
        // The cursor always sits at the end; these fields are short enough that
        // retyping is faster than moving through them.
        field.cursor = strlen(field.buffer);
        return field_handle(&field, event);
    }
    return false;
}

static bool handle_key_screen(bsp_input_event_t const* event) {
    if (event->type != INPUT_EVENT_TYPE_NAVIGATION || !event->args_navigation.state) {
        return false;
    }
    // Arming survives only the very next press of F4, so an intervening key
    // (e.g. an export) cannot leave it primed to regenerate on the following F4.
    if (event->args_navigation.key != BSP_INPUT_NAVIGATION_KEY_F4) {
        app.key_confirm_regenerate = false;
    }
    switch (event->args_navigation.key) {
        case BSP_INPUT_NAVIGATION_KEY_ESC:
            app.screen                 = SCREEN_MENU;
            app.key_confirm_regenerate = false;
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F1:
            export_public_key("/sd/ssh_konsool.pub", "the SD card");
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F2:
            mkdir("/int/ssh", 0777);
            export_public_key("/int/ssh/id_ed25519.pub", "the internal filesystem");
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F3:
            // The serial console is the easiest place to copy from on a laptop.
            printf("\n%s\n\n", keystore_public_key());
            toast("Printed on the serial console");
            return true;
        case BSP_INPUT_NAVIGATION_KEY_F4:
            if (!app.key_confirm_regenerate) {
                app.key_confirm_regenerate = true;
            } else {
                app.key_confirm_regenerate = false;
                if (keystore_regenerate() == ESP_OK) {
                    toast("New key generated");
                } else {
                    toast("Key generation failed");
                }
            }
            return true;
        default:
            return false;
    }
}

static bool handle_terminal(bsp_input_event_t const* event) {
    ssh_state_t state = ssh_client_state(app.ssh);

    // Host key prompt takes every key.
    if (state == SSH_STATE_VERIFY_HOST) {
        bool changed = ssh_client_host_changed(app.ssh);
        if (event->type == INPUT_EVENT_TYPE_NAVIGATION && event->args_navigation.state) {
            if (event->args_navigation.key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
                // For an unchanged, first-contact key one Enter accepts and
                // remembers. For a CHANGED key the first Enter only arms the
                // choice, so a reflexive keypress cannot re-pin a swapped key.
                if (changed && !app.host_change_armed) {
                    app.host_change_armed = true;
                    return true;
                }
                app.host_change_armed = false;
                ssh_client_accept_host(app.ssh, true, true);
                return true;
            }
            if (event->args_navigation.key == BSP_INPUT_NAVIGATION_KEY_ESC) {
                app.host_change_armed = false;
                ssh_client_accept_host(app.ssh, false, false);
                return true;
            }
            // Any other key disarms, matching the delete and regenerate prompts.
            app.host_change_armed = false;
        }
        if (event->type == INPUT_EVENT_TYPE_KEYBOARD && event->args_keyboard.ascii == 'y') {
            app.host_change_armed = false;
            ssh_client_accept_host(app.ssh, true, false);
            return true;
        }
        return false;
    }

    if (state == SSH_STATE_NEED_PASSWORD) {
        if (event->type == INPUT_EVENT_TYPE_NAVIGATION && event->args_navigation.state) {
            if (event->args_navigation.key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
                ssh_client_provide_password(app.ssh, app.modal_buffer);
                mbedtls_platform_zeroize(app.modal_buffer, sizeof(app.modal_buffer));
                return true;
            }
            if (event->args_navigation.key == BSP_INPUT_NAVIGATION_KEY_ESC) {
                mbedtls_platform_zeroize(app.modal_buffer, sizeof(app.modal_buffer));
                ssh_client_disconnect(app.ssh);
                return true;
            }
        }
        field_t field = {app.modal_buffer, sizeof(app.modal_buffer), strlen(app.modal_buffer), true};
        return field_handle(&field, event);
    }

    // Fn combinations are the app's own controls; everything else goes to the host.
    if (event->type == INPUT_EVENT_TYPE_NAVIGATION && event->args_navigation.state &&
        (event->args_navigation.modifiers & BSP_INPUT_MODIFIER_FUNCTION)) {
        switch (event->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_ESC:
                ssh_client_disconnect(app.ssh);
                // Leaving the screen means nobody is left to notice the session
                // ending, so settle the key install here instead.
                finish_copy_id();
                app.screen = SCREEN_MENU;
                return true;
            case BSP_INPUT_NAVIGATION_KEY_F1:
                app.font_scale = app.font_scale == 1 ? 2 : 1;
                term_render_configure(&app.render, app.fb, app.font_scale, TERM_STATUS_BAR_HEIGHT);
                xSemaphoreTake(app.term_lock, portMAX_DELAY);
                term_resize(app.term, app.render.cols, app.render.rows);
                xSemaphoreGive(app.term_lock);
                ssh_client_resize(app.ssh, app.render.cols, app.render.rows);
                app.needs_full_redraw = true;
                return true;
            default:
                break;
        }
    }

    // Shift plus the arrows walks the scrollback.
    if (event->type == INPUT_EVENT_TYPE_NAVIGATION && event->args_navigation.state &&
        (event->args_navigation.modifiers & BSP_INPUT_MODIFIER_SHIFT)) {
        int delta = 0;
        if (event->args_navigation.key == BSP_INPUT_NAVIGATION_KEY_UP) {
            delta = 1;
        } else if (event->args_navigation.key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
            delta = -1;
        }
        if (delta) {
            xSemaphoreTake(app.term_lock, portMAX_DELAY);
            term_scroll_view(app.term, delta * (app.render.rows / 2));
            xSemaphoreGive(app.term_lock);
            app.needs_full_redraw = true;
            return true;
        }
    }

    if (state == SSH_STATE_CLOSED || state == SSH_STATE_ERROR) {
        if (event->type == INPUT_EVENT_TYPE_NAVIGATION && event->args_navigation.state &&
            (event->args_navigation.key == BSP_INPUT_NAVIGATION_KEY_ESC ||
             event->args_navigation.key == BSP_INPUT_NAVIGATION_KEY_RETURN)) {
            app.screen = SCREEN_MENU;
            return true;
        }
        return false;
    }

    keymap_result_t result;
    bool            app_cursor;
    xSemaphoreTake(app.term_lock, portMAX_DELAY);
    app_cursor = term_app_cursor_keys(app.term);
    xSemaphoreGive(app.term_lock);

    if (keymap_translate(event, app_cursor, &result) && result.len) {
        xSemaphoreTake(app.term_lock, portMAX_DELAY);
        term_scroll_reset(app.term);
        xSemaphoreGive(app.term_lock);
        ssh_client_send(app.ssh, result.bytes, result.len);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

esp_err_t ui_init(pax_buf_t* fb, term_t* term, SemaphoreHandle_t term_lock, ssh_client_t* ssh) {
    memset(&app, 0, sizeof(app));
    app.fb            = fb;
    app.term          = term;
    app.term_lock     = term_lock;
    app.ssh           = ssh;
    app.screen        = SCREEN_MENU;
    app.copy_id_index = -1;
    app.delete_index  = -1;
    app.font_scale    = 1;
    app.cursor_on     = true;
    strlcpy(app.network, "no network", sizeof(app.network));

    term_render_configure(&app.render, fb, app.font_scale, TERM_STATUS_BAR_HEIGHT);
    xSemaphoreTake(term_lock, portMAX_DELAY);
    term_resize(term, app.render.cols, app.render.rows);
    xSemaphoreGive(term_lock);

    ESP_LOGI(TAG, "Terminal is %dx%d cells", app.render.cols, app.render.rows);
    return ESP_OK;
}

bool ui_handle_event(bsp_input_event_t const* event) {
    switch (app.screen) {
        case SCREEN_MENU:
            return handle_menu(event);
        case SCREEN_EDIT:
            return handle_edit(event);
        case SCREEN_KEY:
            return handle_key_screen(event);
        case SCREEN_TERMINAL:
            return handle_terminal(event);
        default:
            return false;
    }
}

bool ui_tick(void) {
    bool redraw = false;

    if (app.screen == SCREEN_TERMINAL) {
        int64_t now = esp_timer_get_time();
        if (now - app.cursor_toggled_us > 500000) {
            app.cursor_toggled_us = now;
            app.cursor_on         = !app.cursor_on;
            redraw                = true;
        }

        ssh_state_t state = ssh_client_state(app.ssh);
        if (state != app.last_state) {
            app.last_state        = state;
            // A prompt appearing or going away covers part of the grid, so the
            // whole screen has to be painted again.
            app.needs_full_redraw = true;
            redraw                = true;

            if (state == SSH_STATE_CLOSED || state == SSH_STATE_ERROR) {
                finish_copy_id();
            }
        }

        xSemaphoreTake(app.term_lock, portMAX_DELAY);
        for (int row = 0; row < app.render.rows; row++) {
            if (term_row_dirty(app.term, row)) {
                redraw = true;
                break;
            }
        }
        xSemaphoreGive(app.term_lock);
    }

    if (app.toast[0] && esp_timer_get_time() > app.toast_until_us) {
        app.toast[0] = '\0';
        redraw       = true;
    }
    return redraw;
}

void ui_draw(void) {
    switch (app.screen) {
        case SCREEN_MENU:
            draw_menu();
            break;
        case SCREEN_EDIT:
            draw_edit();
            break;
        case SCREEN_KEY:
            draw_key();
            break;
        case SCREEN_TERMINAL:
            draw_terminal();
            break;
    }
}

void ui_set_network(char const* description) {
    strlcpy(app.network, description, sizeof(app.network));
}
