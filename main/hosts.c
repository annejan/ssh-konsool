// SPDX-License-Identifier: MIT

#include "hosts.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

static char const TAG[] = "hosts";

#define NVS_NAMESPACE "sshhosts"
#define NVS_KEY_LIST  "list"
// Bumped from "sshknown" when the key derivation below changed: entries written
// under the old 32-bit scheme cannot be checked after the fact, so they are left
// behind rather than trusted.
#define NVS_KNOWN_NS  "sshknown2"

// A record is "host:port\nSHA256:...". The host travels with the fingerprint so
// that an entry can never be passed off as belonging to somewhere else.
#define KNOWN_RECORD_MAX (HOST_NAME_MAX + 8 + 80)

static host_profile_t profiles[HOSTS_MAX];
static int            profile_count = 0;

static esp_err_t save_all(void) {
    nvs_handle_t handle;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, NVS_KEY_LIST, profiles, sizeof(host_profile_t) * profile_count);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t hosts_init(void) {
    nvs_handle_t handle;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // Nothing saved yet; that is not an error worth reporting upwards.
        profile_count = 0;
        return ESP_OK;
    }
    size_t length = 0;
    err           = nvs_get_blob(handle, NVS_KEY_LIST, NULL, &length);
    if (err == ESP_OK && length > 0 && length % sizeof(host_profile_t) == 0 && length <= sizeof(profiles)) {
        err = nvs_get_blob(handle, NVS_KEY_LIST, profiles, &length);
        if (err == ESP_OK) {
            profile_count = (int)(length / sizeof(host_profile_t));
        }
    } else {
        profile_count = 0;
        err           = ESP_OK;
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded %d saved connections", profile_count);
    return err;
}

int hosts_count(void) {
    return profile_count;
}

bool hosts_get(int index, host_profile_t* out) {
    if (index < 0 || index >= profile_count || !out) {
        return false;
    }
    *out = profiles[index];
    return true;
}

esp_err_t hosts_set(int index, host_profile_t const* profile) {
    if (!profile || index < 0 || index > profile_count || index >= HOSTS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    profiles[index] = *profile;
    if (!profiles[index].save_password) {
        mbedtls_platform_zeroize(profiles[index].password, sizeof(profiles[index].password));
    }
    if (index == profile_count) {
        profile_count++;
    }
    return save_all();
}

esp_err_t hosts_remove(int index) {
    if (index < 0 || index >= profile_count) {
        return ESP_ERR_INVALID_ARG;
    }
    memmove(&profiles[index], &profiles[index + 1], sizeof(host_profile_t) * (profile_count - index - 1));
    profile_count--;
    memset(&profiles[profile_count], 0, sizeof(host_profile_t));
    return save_all();
}

// NVS key names are limited to 15 characters, so the host and port are hashed
// rather than spelled out. The digest has to be cryptographic: with a
// non-collision-resistant hash an attacker can pick a host name that lands on
// the same entry as somewhere you trust, and have you pin their key under it.
static void known_key(char const* host, uint16_t port, char* out, size_t len) {
    // Length delimited, so host "a" on port 2258 cannot collide with host
    // "a:22" on port 58.
    uint8_t input[HOST_NAME_MAX + 3];
    size_t  host_len = strnlen(host, HOST_NAME_MAX - 1);
    memcpy(input, host, host_len);
    input[host_len]     = '\0';
    input[host_len + 1] = (uint8_t)(port >> 8);
    input[host_len + 2] = (uint8_t)port;

    uint8_t digest[32];
    size_t  digest_len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, input, host_len + 3, digest, sizeof(digest), &digest_len) != PSA_SUCCESS) {
        // Without a digest there is no safe key to use, so use one that cannot
        // collide with a real entry and will simply never match.
        snprintf(out, len, "h!");
        return;
    }

    // Seven bytes of digest is fourteen hex characters, which with the prefix
    // is exactly the fifteen an NVS key name allows.
    snprintf(out, len, "h%02x%02x%02x%02x%02x%02x%02x", digest[0], digest[1], digest[2], digest[3], digest[4],
             digest[5], digest[6]);
}

// Records are "host:port\nfingerprint". Returns false unless the record really
// belongs to this host and port.
static bool parse_record(char const* record, char const* host, uint16_t port, char* out_fingerprint, size_t len) {
    char const* newline = strchr(record, '\n');
    if (!newline) {
        return false;
    }

    // The last colon on the first line: the host may be a bare IPv6 address, and
    // the fingerprint on the second line contains a colon of its own.
    char const* colon = NULL;
    for (char const* p = record; p < newline; p++) {
        if (*p == ':') {
            colon = p;
        }
    }
    if (!colon) {
        return false;
    }

    size_t stored_host_len = (size_t)(colon - record);
    if (stored_host_len != strlen(host) || strncasecmp(record, host, stored_host_len) != 0) {
        return false;
    }
    if ((uint16_t)atoi(colon + 1) != port) {
        return false;
    }

    strlcpy(out_fingerprint, newline + 1, len);
    return true;
}

bool knownhost_get(char const* host, uint16_t port, char* out_fingerprint, size_t len) {
    char key[NVS_KEY_NAME_MAX_SIZE];
    known_key(host, port, key, sizeof(key));

    nvs_handle_t handle;
    if (nvs_open(NVS_KNOWN_NS, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    // The record holds more than the fingerprint, so it cannot be read straight
    // into the caller's buffer.
    char      record[KNOWN_RECORD_MAX];
    size_t    size = sizeof(record);
    esp_err_t err  = nvs_get_str(handle, key, record, &size);
    nvs_close(handle);

    if (err != ESP_OK) {
        return false;
    }
    return parse_record(record, host, port, out_fingerprint, len);
}

esp_err_t knownhost_set(char const* host, uint16_t port, char const* fingerprint) {
    char key[NVS_KEY_NAME_MAX_SIZE];
    known_key(host, port, key, sizeof(key));

    char record[KNOWN_RECORD_MAX];
    if (snprintf(record, sizeof(record), "%s:%u\n%s", host, (unsigned)port, fingerprint) >= (int)sizeof(record)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t    err = nvs_open(NVS_KNOWN_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    // Refuse to overwrite an entry belonging to a different host. At this key
    // width that should be unreachable, but silently unpinning somewhere else
    // is not a failure mode worth leaving open.
    char   existing[KNOWN_RECORD_MAX];
    size_t size = sizeof(existing);
    if (nvs_get_str(handle, key, existing, &size) == ESP_OK) {
        char scratch[80];
        if (!parse_record(existing, host, port, scratch, sizeof(scratch))) {
            nvs_close(handle);
            ESP_LOGE(TAG, "Refusing to replace the remembered key of another host");
            return ESP_ERR_INVALID_STATE;
        }
        mbedtls_platform_zeroize(scratch, sizeof(scratch));
    }

    err = nvs_set_str(handle, key, record);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
