// SPDX-License-Identifier: MIT

#include "hosts.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static char const TAG[] = "hosts";

#define NVS_NAMESPACE "sshhosts"
#define NVS_KEY_LIST  "list"
#define NVS_KNOWN_NS  "sshknown"

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
        memset(profiles[index].password, 0, sizeof(profiles[index].password));
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

// NVS keys are limited to 15 characters, so the host:port pair is hashed rather
// than spelled out.
static void known_key(char const* host, uint16_t port, char* out, size_t len) {
    uint32_t hash = 2166136261u;
    for (char const* c = host; *c; c++) {
        hash = (hash ^ (uint8_t)*c) * 16777619u;
    }
    hash = (hash ^ port) * 16777619u;
    snprintf(out, len, "h%08" PRIx32, hash);
}

bool knownhost_get(char const* host, uint16_t port, char* out_fingerprint, size_t len) {
    char key[16];
    known_key(host, port, key, sizeof(key));

    nvs_handle_t handle;
    if (nvs_open(NVS_KNOWN_NS, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t    size = len;
    esp_err_t err  = nvs_get_str(handle, key, out_fingerprint, &size);
    nvs_close(handle);
    return err == ESP_OK;
}

esp_err_t knownhost_set(char const* host, uint16_t port, char const* fingerprint) {
    char key[16];
    known_key(host, port, key, sizeof(key));

    nvs_handle_t handle;
    esp_err_t    err = nvs_open(NVS_KNOWN_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, key, fingerprint);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
