// SPDX-License-Identifier: MIT
//
// The badge's own SSH identity: an Ed25519 key kept in NVS, plus the OpenSSH
// formatted public key to hand to a server's authorized_keys.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define KEYSTORE_SIGNATURE_LEN 64

// Load the stored key, generating one on first run. Safe to call more than once.
esp_err_t keystore_init(void);

// "ssh-ed25519 AAAA... comment", NUL terminated. NULL before init.
char const* keystore_public_key(void);

// The SSH wire format public key: string("ssh-ed25519") || string(key).
// This is what libssh2 wants for public key authentication.
uint8_t const* keystore_public_blob(size_t* out_len);

// "SHA256:..." over the public key blob, the same string OpenSSH prints.
char const* keystore_fingerprint(void);

// Sign with the badge key. `out_signature` takes KEYSTORE_SIGNATURE_LEN bytes.
esp_err_t keystore_sign(void const* data, size_t len, uint8_t* out_signature);

// Throw the key away and make a new one. Every server that trusted the old
// public key stops accepting the badge until the new one is installed.
esp_err_t keystore_regenerate(void);

// Write the public key (with a trailing newline) to a file, for copying off the
// badge over badgelink or an SD card.
esp_err_t keystore_write_public_key(char const* path);
