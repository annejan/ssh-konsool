// SPDX-License-Identifier: MIT
//
// Just enough of ESP-IDF to compile main/hosts.c on a development machine, so
// the known-host record handling can be tested without a badge. Everything here
// is a stand-in; nothing in this directory is built into the firmware.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK                  0
#define ESP_FAIL                -1
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_NVS_NOT_FOUND   0x1102

#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGE(tag, ...) ((void)0)

#define NVS_KEY_NAME_MAX_SIZE 16

typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
typedef int nvs_handle_t;

esp_err_t nvs_open(char const* name, nvs_open_mode_t mode, nvs_handle_t* out);
void      nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_set_str(nvs_handle_t handle, char const* key, char const* value);
esp_err_t nvs_get_str(nvs_handle_t handle, char const* key, char* out, size_t* len);
esp_err_t nvs_set_blob(nvs_handle_t handle, char const* key, void const* value, size_t len);
esp_err_t nvs_get_blob(nvs_handle_t handle, char const* key, void* out, size_t* len);

// The real thing is a cryptographic hash. The test only needs it to be a pure
// function of every input byte, so that a short one cannot be confused with a
// long one and the buffer arithmetic is exercised under a sanitiser.
#define PSA_ALG_SHA_256 1
#define PSA_SUCCESS     0
typedef int psa_status_t;
typedef int psa_algorithm_t;
psa_status_t psa_hash_compute(psa_algorithm_t alg, uint8_t const* input, size_t input_len, uint8_t* out,
                              size_t out_size, size_t* out_len);

void mbedtls_platform_zeroize(void* buf, size_t len);

#ifndef HAVE_STRLCPY
size_t strlcpy(char* dst, char const* src, size_t size);
#endif
