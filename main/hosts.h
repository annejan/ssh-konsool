// SPDX-License-Identifier: MIT
//
// Saved connections and remembered host keys, kept in NVS.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define HOSTS_MAX         16
#define HOST_NAME_MAX     64
#define HOST_USER_MAX     32
#define HOST_PASSWORD_MAX 64

typedef struct {
    char     host[HOST_NAME_MAX];
    uint16_t port;
    char     user[HOST_USER_MAX];
    // Only filled in when the user asked for the password to be remembered.
    // NVS is not encrypted by default, so this is plain text on the flash.
    char     password[HOST_PASSWORD_MAX];
    bool     save_password;
    bool     use_key;  // Offer the badge's own key before asking for a password
} host_profile_t;

esp_err_t hosts_init(void);

int  hosts_count(void);
bool hosts_get(int index, host_profile_t* out);

// Writing at index == hosts_count() appends, up to HOSTS_MAX entries.
esp_err_t hosts_set(int index, host_profile_t const* profile);
esp_err_t hosts_remove(int index);

// Remembered host keys, keyed on "host:port". The fingerprint is the same
// "SHA256:..." string OpenSSH shows.
bool      knownhost_get(char const* host, uint16_t port, char* out_fingerprint, size_t len);
esp_err_t knownhost_set(char const* host, uint16_t port, char const* fingerprint);
