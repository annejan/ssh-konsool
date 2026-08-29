// SPDX-License-Identifier: MIT
//
// One SSH session, driven by a task of its own. Everything libssh2 touches
// stays inside that task; the rest of the app talks to it through this header.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hosts.h"
#include "terminal.h"

typedef enum {
    SSH_STATE_IDLE = 0,
    SSH_STATE_CONNECTING,
    SSH_STATE_VERIFY_HOST,    // Waiting for the user to accept an unknown host key
    SSH_STATE_NEED_PASSWORD,  // Waiting for the user to type a password
    SSH_STATE_AUTHENTICATING,
    SSH_STATE_CONNECTED,
    SSH_STATE_CLOSED,
    SSH_STATE_ERROR,
} ssh_state_t;

typedef struct ssh_client ssh_client_t;

// The terminal is written from the session task, so every read of it from
// elsewhere has to hold `term_lock`.
ssh_client_t* ssh_client_create(term_t* term, SemaphoreHandle_t term_lock);
void          ssh_client_destroy(ssh_client_t* client);

esp_err_t ssh_client_connect(ssh_client_t* client, host_profile_t const* profile);
void      ssh_client_disconnect(ssh_client_t* client);

ssh_state_t ssh_client_state(ssh_client_t const* client);

// A line describing what the session is doing, or why it stopped.
char const* ssh_client_status(ssh_client_t const* client);

// Valid while the state is SSH_STATE_VERIFY_HOST.
char const* ssh_client_host_fingerprint(ssh_client_t const* client);
bool        ssh_client_host_changed(ssh_client_t const* client);

// Answers to the two states that wait for the user.
void ssh_client_accept_host(ssh_client_t* client, bool accept, bool remember);
void ssh_client_provide_password(ssh_client_t* client, char const* password);

// Bytes typed by the user, on their way to the remote shell.
void ssh_client_send(ssh_client_t* client, void const* data, size_t len);

// Tell the remote side the window changed size.
void ssh_client_resize(ssh_client_t* client, int cols, int rows);
