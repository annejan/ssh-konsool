// SPDX-License-Identifier: MIT

#include "keystore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "badge_ed25519.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

static char const TAG[] = "keystore";

#define NVS_NAMESPACE  "sshkey"
#define NVS_KEY_SECRET "ed25519"

#define KEY_TYPE    "ssh-ed25519"
#define KEY_COMMENT "ssh@tanmatsu"

#define SECRET_LEN 64  // TweetNaCl keeps the seed and the public half together
#define PUBLIC_LEN 32

static uint8_t secret_key[SECRET_LEN];
static uint8_t public_key[PUBLIC_LEN];
static bool    have_key = false;

// string("ssh-ed25519") || string(public key)
static uint8_t public_blob[4 + sizeof(KEY_TYPE) - 1 + 4 + PUBLIC_LEN];
static char*   public_line     = NULL;
static char    fingerprint[80] = {0};

static void store_u32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

// Base64 without the padding, the way OpenSSH prints fingerprints.
static void strip_padding(char* text) {
    size_t length = strlen(text);
    while (length > 0 && text[length - 1] == '=') {
        text[--length] = '\0';
    }
}

static esp_err_t derive_public_forms(void) {
    size_t offset = 0;
    store_u32(public_blob + offset, sizeof(KEY_TYPE) - 1);
    offset += 4;
    memcpy(public_blob + offset, KEY_TYPE, sizeof(KEY_TYPE) - 1);
    offset += sizeof(KEY_TYPE) - 1;
    store_u32(public_blob + offset, PUBLIC_LEN);
    offset += 4;
    memcpy(public_blob + offset, public_key, PUBLIC_LEN);

    char   encoded[128];
    size_t encoded_len = 0;
    if (mbedtls_base64_encode((unsigned char*)encoded, sizeof(encoded), &encoded_len, public_blob,
                              sizeof(public_blob)) != 0) {
        return ESP_FAIL;
    }
    encoded[encoded_len] = '\0';

    free(public_line);
    size_t line_len = sizeof(KEY_TYPE) + encoded_len + sizeof(KEY_COMMENT) + 2;
    public_line     = malloc(line_len);
    if (!public_line) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(public_line, line_len, "%s %s %s", KEY_TYPE, encoded, KEY_COMMENT);

    uint8_t digest[32];
    size_t  digest_len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, public_blob, sizeof(public_blob), digest, sizeof(digest), &digest_len) !=
        PSA_SUCCESS) {
        return ESP_FAIL;
    }

    char   digest_b64[64];
    size_t digest_b64_len = 0;
    mbedtls_base64_encode((unsigned char*)digest_b64, sizeof(digest_b64), &digest_b64_len, digest, digest_len);
    digest_b64[digest_b64_len] = '\0';
    strip_padding(digest_b64);
    snprintf(fingerprint, sizeof(fingerprint), "SHA256:%s", digest_b64);

    return ESP_OK;
}

static esp_err_t store_secret(void) {
    nvs_handle_t handle;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, NVS_KEY_SECRET, secret_key, sizeof(secret_key));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static bool load_secret(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t    length = sizeof(secret_key);
    esp_err_t err    = nvs_get_blob(handle, NVS_KEY_SECRET, secret_key, &length);
    nvs_close(handle);
    return err == ESP_OK && length == sizeof(secret_key);
}

esp_err_t keystore_init(void) {
    if (have_key) {
        return ESP_OK;
    }

    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PSA crypto failed to start");
        return ESP_FAIL;
    }

    if (load_secret()) {
        // The public half is the second block of the stored secret.
        memcpy(public_key, secret_key + 32, PUBLIC_LEN);
        if (derive_public_forms() == ESP_OK) {
            have_key = true;
            ESP_LOGI(TAG, "Loaded SSH key %s", fingerprint);
            return ESP_OK;
        }
        // A key is stored but could not be prepared this boot (a transient PSA
        // or base64 failure). Do NOT fall through to regenerate: that would
        // overwrite a good, in-use identity on a passing hiccup. Fail instead,
        // and the next boot loads the same stored key again.
        ESP_LOGE(TAG, "Stored SSH key could not be prepared; keeping it untouched");
        mbedtls_platform_zeroize(secret_key, sizeof(secret_key));
        have_key = false;
        return ESP_FAIL;
    }

    return keystore_regenerate();
}

esp_err_t keystore_regenerate(void) {
    if (badge_ed25519_keypair(public_key, secret_key) != 0) {
        ESP_LOGE(TAG, "Key generation failed");
        return ESP_FAIL;
    }
    // The RNG glue zero-fills on failure rather than handing back stale stack
    // bytes, so an all-zero seed means the randomness was not there. Refuse to
    // install a predictable identity, and leave any existing stored key intact.
    bool seed_zero = true;
    for (size_t i = 0; i < 32; i++) {
        if (secret_key[i] != 0) {
            seed_zero = false;
            break;
        }
    }
    if (seed_zero) {
        ESP_LOGE(TAG, "Refusing to install a key from a failed RNG");
        mbedtls_platform_zeroize(secret_key, sizeof(secret_key));
        have_key = false;
        return ESP_FAIL;
    }
    have_key = true;

    esp_err_t err = derive_public_forms();
    if (err != ESP_OK) {
        have_key = false;
        return err;
    }

    err = store_secret();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store the new key: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Generated SSH key %s", fingerprint);
    return ESP_OK;
}

char const* keystore_public_key(void) {
    return have_key ? public_line : NULL;
}

uint8_t const* keystore_public_blob(size_t* out_len) {
    if (!have_key) {
        return NULL;
    }
    if (out_len) {
        *out_len = sizeof(public_blob);
    }
    return public_blob;
}

char const* keystore_fingerprint(void) {
    return fingerprint[0] ? fingerprint : NULL;
}

esp_err_t keystore_sign(void const* data, size_t len, uint8_t* out_signature) {
    if (!have_key) {
        return ESP_ERR_INVALID_STATE;
    }

    return badge_ed25519_sign(secret_key, data, len, out_signature) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t keystore_write_public_key(char const* path) {
    if (!have_key) {
        return ESP_ERR_INVALID_STATE;
    }
    FILE* file = fopen(path, "w");
    if (!file) {
        ESP_LOGE(TAG, "Cannot open %s for writing", path);
        return ESP_FAIL;
    }
    int written = fprintf(file, "%s\n", public_line);
    fclose(file);
    return written > 0 ? ESP_OK : ESP_FAIL;
}
