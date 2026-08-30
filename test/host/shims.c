// SPDX-License-Identifier: MIT

#include "shims.h"
#include <stdlib.h>

// A tiny in-memory stand-in for NVS: one namespace, a handful of entries.
#define MAX_ENTRIES 16
#define MAX_VALUE   512

static struct {
    char   key[NVS_KEY_NAME_MAX_SIZE];
    char   value[MAX_VALUE];
    size_t len;
    bool   used;
} entries[MAX_ENTRIES];

void nvs_test_reset(void) {
    memset(entries, 0, sizeof(entries));
}

esp_err_t nvs_open(char const* name, nvs_open_mode_t mode, nvs_handle_t* out) {
    (void)name;
    (void)mode;
    *out = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
    (void)handle;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    (void)handle;
    return ESP_OK;
}

static int find(char const* key) {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (entries[i].used && strcmp(entries[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static esp_err_t store(char const* key, void const* value, size_t len) {
    if (len > MAX_VALUE) {
        return ESP_ERR_INVALID_ARG;
    }
    int slot = find(key);
    if (slot < 0) {
        for (int i = 0; i < MAX_ENTRIES && slot < 0; i++) {
            if (!entries[i].used) {
                slot = i;
            }
        }
    }
    if (slot < 0) {
        return ESP_FAIL;
    }
    entries[slot].used = true;
    snprintf(entries[slot].key, sizeof(entries[slot].key), "%s", key);
    memcpy(entries[slot].value, value, len);
    entries[slot].len = len;
    return ESP_OK;
}

static esp_err_t load(char const* key, void* out, size_t* len) {
    int slot = find(key);
    if (slot < 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out) {
        if (*len < entries[slot].len) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(out, entries[slot].value, entries[slot].len);
    }
    *len = entries[slot].len;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, char const* key, char const* value) {
    (void)handle;
    return store(key, value, strlen(value) + 1);
}

esp_err_t nvs_get_str(nvs_handle_t handle, char const* key, char* out, size_t* len) {
    (void)handle;
    return load(key, out, len);
}

esp_err_t nvs_erase_key(nvs_handle_t handle, char const* key) {
    (void)handle;
    int slot = find(key);
    if (slot < 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memset(&entries[slot], 0, sizeof(entries[slot]));
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, char const* key, void const* value, size_t len) {
    (void)handle;
    return store(key, value, len);
}

esp_err_t nvs_get_blob(nvs_handle_t handle, char const* key, void* out, size_t* len) {
    (void)handle;
    return load(key, out, len);
}

psa_status_t psa_hash_compute(psa_algorithm_t alg, uint8_t const* input, size_t input_len, uint8_t* out,
                              size_t out_size, size_t* out_len) {
    (void)alg;
    if (out_size < 32) {
        return -1;
    }
    // FNV-1a over the input, spread across the digest. Not a real hash; enough
    // for the tests, which care about the plumbing and the bounds.
    uint64_t acc = 1469598103934665603ull;
    for (size_t i = 0; i < input_len; i++) {
        acc = (acc ^ input[i]) * 1099511628211ull;
    }
    for (size_t i = 0; i < 32; i++) {
        acc  = (acc ^ (uint8_t)i) * 1099511628211ull;
        out[i] = (uint8_t)(acc >> 32);
    }
    *out_len = 32;
    return PSA_SUCCESS;
}

void mbedtls_platform_zeroize(void* buf, size_t len) {
    memset(buf, 0, len);
}

#ifndef HAVE_STRLCPY
size_t strlcpy(char* dst, char const* src, size_t size) {
    size_t len = strlen(src);
    if (size) {
        size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}
#endif
