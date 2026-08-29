// SPDX-License-Identifier: MIT
//
// An SSH client for Tanmatsu and Konsool.

#include <stdio.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hosts.h"
#include "keystore.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "ssh_client.h"
#include "terminal.h"
#include "terminal_font.h"
#include "ui.h"
#include "wifi_connection.h"
#include "wifi_remote.h"

static char const TAG[] = "main";

#define FRAME_INTERVAL_US 33000  // Roughly 30 frames per second

static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static pax_buf_t                  fb                   = {0};
static QueueHandle_t              input_event_queue    = NULL;

static term_t*           terminal   = NULL;
static SemaphoreHandle_t term_lock  = NULL;
static ssh_client_t*     ssh_client = NULL;

static void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to blit to display: %d", res);
    }
}

// A holding screen for the things that happen before the UI exists.
static void splash(char const* message) {
    if (pax_buf_get_width(&fb) == 0) {
        ESP_LOGI(TAG, "%s", message);
        return;
    }
    pax_background(&fb, 0xFF0E1116);
    pax_center_text(&fb, 0xFF5CE07A, terminal_font, 16, pax_buf_get_width(&fb) / 2, pax_buf_get_height(&fb) / 2 - 20,
                    "SSH");
    pax_center_text(&fb, 0xFFD8E2EC, terminal_font, 16, pax_buf_get_width(&fb) / 2, pax_buf_get_height(&fb) / 2 + 10,
                    message);
    blit();
}

static pax_buf_type_t pax_format_for(bsp_display_color_format_t format) {
    switch (format) {
        case BSP_DISPLAY_COLOR_FORMAT_8_332RGB:
            return PAX_BUF_8_332RGB;
        case BSP_DISPLAY_COLOR_FORMAT_16_565RGB:
            return PAX_BUF_16_565RGB;
        case BSP_DISPLAY_COLOR_FORMAT_24_888RGB:
            return PAX_BUF_24_888RGB;
        case BSP_DISPLAY_COLOR_FORMAT_32_8888ARGB:
            return PAX_BUF_32_8888ARGB;
        default:
            ESP_LOGW(TAG, "Unexpected display colour format %u, assuming 24 bit RGB", (unsigned)format);
            return PAX_BUF_24_888RGB;
    }
}

static pax_orientation_t pax_orientation_for(bsp_display_rotation_t rotation) {
    switch (rotation) {
        case BSP_DISPLAY_ROTATION_90:
            return PAX_O_ROT_CCW;
        case BSP_DISPLAY_ROTATION_180:
            return PAX_O_ROT_HALF;
        case BSP_DISPLAY_ROTATION_270:
            return PAX_O_ROT_CW;
        default:
            return PAX_O_UPRIGHT;
    }
}

static esp_err_t init_display(void) {
    esp_err_t res =
        bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "This board has no display: %d", res);
        return res;
    }

    pax_buf_init(&fb, NULL, display_h_res, display_v_res, pax_format_for(display_color_format));
    pax_buf_reversed(&fb, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);
    pax_buf_set_orientation(&fb, pax_orientation_for(bsp_display_get_default_rotation()));
    return ESP_OK;
}

// Bring up the radio and join whatever network the launcher has stored.
static void connect_network(void) {
    bsp_radio_state_t previous = BSP_POWER_RADIO_STATE_OFF;
    bsp_power_get_radio_state(&previous);
    if (previous != BSP_POWER_RADIO_STATE_OFF) {
        // ESP-HOSTED does not survive being re-opened on a live link.
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_APPLICATION);

    splash("Starting the radio...");
    if (wifi_remote_initialize() != ESP_OK) {
        ESP_LOGE(TAG, "The radio is not responding");
        ui_set_network("radio failed");
        return;
    }
    if (wifi_connection_init_stack() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start the network stack");
        ui_set_network("no network stack");
        return;
    }

    splash("Joining a network...");
    if (wifi_connect_try_all() != ESP_OK || !wifi_connection_await(30000)) {
        ESP_LOGW(TAG, "Could not join a network");
        ui_set_network("not connected");
        return;
    }

    esp_netif_ip_info_t* ip = wifi_get_ip_info();
    char                 description[64];
    if (ip) {
        snprintf(description, sizeof(description), IPSTR, IP2STR(&ip->ip));
    } else {
        strlcpy(description, "connected", sizeof(description));
    }
    ui_set_network(description);
    ESP_LOGI(TAG, "Network ready: %s", description);
}

// The terminal answers some escape sequences itself; those replies go back up
// the same channel as typed input.
static void terminal_response(void const* data, size_t len, void* ctx) {
    ssh_client_send((ssh_client_t*)ctx, data, len);
}

void app_main(void) {
    gpio_install_isr_service(0);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    bsp_configuration_t const configuration = {
        .display =
            {
                .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
                .num_fbs                = 1,
            },
    };
    ESP_ERROR_CHECK(bsp_device_initialize(&configuration));
    ESP_ERROR_CHECK(init_display());
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    bsp_led_set_mode(false);

    splash("Preparing the SSH key...");
    ESP_ERROR_CHECK(hosts_init());
    if (keystore_init() != ESP_OK) {
        ESP_LOGE(TAG, "No SSH key available; public key login will not work");
    }

    term_lock = xSemaphoreCreateMutex();
    terminal  = term_create(80, 24);
    if (!term_lock || !terminal) {
        ESP_LOGE(TAG, "Out of memory setting up the terminal");
        return;
    }

    ssh_client = ssh_client_create(terminal, term_lock);
    if (!ssh_client) {
        ESP_LOGE(TAG, "Failed to set up the SSH client");
        return;
    }
    term_set_response_cb(terminal, terminal_response, ssh_client);

    ESP_ERROR_CHECK(ui_init(&fb, terminal, term_lock, ssh_client));

    connect_network();

    bool    redraw     = true;
    int64_t last_frame = 0;

    while (1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (event.type == INPUT_EVENT_TYPE_ACTION && event.args_action.type == BSP_INPUT_ACTION_TYPE_POWER_BUTTON &&
                event.args_action.state) {
                bsp_device_restart_to_launcher();
            }
            redraw |= ui_handle_event(&event);
        }

        redraw |= ui_tick();

        int64_t now = esp_timer_get_time();
        if (redraw && now - last_frame >= FRAME_INTERVAL_US) {
            ui_draw();
            blit();
            last_frame = now;
            redraw     = false;
        }
    }
}
