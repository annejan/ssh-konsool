// SPDX-License-Identifier: MIT

#include "ssh_client.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "esp_log.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "keystore.h"
#include "libssh2.h"
#include "mbedtls/base64.h"

static char const TAG[] = "ssh";

#define OUTGOING_BUFFER    4096
#define READ_CHUNK         2048
#define CONNECT_TIMEOUT_MS 15000
#define TASK_STACK         16384

struct ssh_client {
    term_t*           term;
    SemaphoreHandle_t term_lock;

    host_profile_t profile;

    volatile ssh_state_t state;
    char                 status[128];
    char                 fingerprint[80];
    bool                 host_changed;

    StreamBufferHandle_t outgoing;
    TaskHandle_t         task;

    volatile bool stop;
    volatile bool answer_ready;
    bool          host_accepted;
    bool          host_remember;
    char          password[HOST_PASSWORD_MAX];

    volatile bool resize_pending;
    volatile int  pending_cols;
    volatile int  pending_rows;

    int              socket;
    LIBSSH2_SESSION* session;
    LIBSSH2_CHANNEL* channel;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void set_status(ssh_client_t* client, ssh_state_t state, char const* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(client->status, sizeof(client->status), format, args);
    va_end(args);
    client->state = state;
    ESP_LOGI(TAG, "%s", client->status);
}

static void term_feed(ssh_client_t* client, void const* data, size_t len) {
    xSemaphoreTake(client->term_lock, portMAX_DELAY);
    term_write(client->term, data, len);
    xSemaphoreGive(client->term_lock);
}

// Write a line of our own into the terminal, so status and errors appear where
// the user is already looking.
static void term_message(ssh_client_t* client, char const* format, ...) {
    char    line[192];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(line, sizeof(line) - 3, format, args);
    va_end(args);
    if (len < 0) {
        return;
    }
    strcat(line, "\r\n");
    term_feed(client, line, strlen(line));
}

static char const* last_error(LIBSSH2_SESSION* session) {
    char* message = NULL;
    int   length  = 0;
    libssh2_session_last_error(session, &message, &length, 0);
    return message ? message : "unknown error";
}

// The crypto backend is built on PSA, which has no big integer arithmetic, so
// the finite field Diffie-Hellman exchanges cannot be offered. Naming the
// algorithms explicitly also keeps the deprecated ones (SHA-1 signatures, CBC
// ciphers, 3DES) off the wire.
static void apply_algorithm_preferences(ssh_client_t* client) {
    struct {
        int         method;
        char const* preference;
    } const preferences[] = {
        {LIBSSH2_METHOD_KEX,
         "curve25519-sha256,curve25519-sha256@libssh.org,"
         "ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521"},
        {LIBSSH2_METHOD_HOSTKEY,
         "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,"
         "ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256"},
        {LIBSSH2_METHOD_CRYPT_CS, "chacha20-poly1305@openssh.com,aes256-ctr,aes192-ctr,aes128-ctr"},
        {LIBSSH2_METHOD_CRYPT_SC, "chacha20-poly1305@openssh.com,aes256-ctr,aes192-ctr,aes128-ctr"},
        {LIBSSH2_METHOD_MAC_CS,
         "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,"
         "hmac-sha2-256,hmac-sha2-512"},
        {LIBSSH2_METHOD_MAC_SC,
         "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,"
         "hmac-sha2-256,hmac-sha2-512"},
    };

    for (size_t i = 0; i < sizeof(preferences) / sizeof(preferences[0]); i++) {
        int rc = libssh2_session_method_pref(client->session, preferences[i].method, preferences[i].preference);
        if (rc != 0) {
            ESP_LOGW(TAG, "Method %d not accepted: %s", preferences[i].method, last_error(client->session));
        }
    }
}

// ---------------------------------------------------------------------------
// Connecting
// ---------------------------------------------------------------------------

static int open_socket(ssh_client_t* client) {
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)client->profile.port);

    struct addrinfo hints = {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo* results = NULL;

    int err = getaddrinfo(client->profile.host, port_text, &hints, &results);
    if (err != 0 || !results) {
        set_status(client, SSH_STATE_ERROR, "Cannot resolve %s", client->profile.host);
        return -1;
    }

    int fd = -1;
    for (struct addrinfo* entry = results; entry; entry = entry->ai_next) {
        fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0) {
            continue;
        }
        struct timeval timeout = {.tv_sec = CONNECT_TIMEOUT_MS / 1000, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);

    if (fd < 0) {
        set_status(client, SSH_STATE_ERROR, "Cannot reach %s:%u", client->profile.host, (unsigned)client->profile.port);
        return -1;
    }

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    return fd;
}

// Wait for the user to answer a prompt, or for the session to be cancelled.
static bool wait_for_answer(ssh_client_t* client) {
    client->answer_ready = false;
    while (!client->answer_ready) {
        if (client->stop) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

static bool verify_host_key(ssh_client_t* client) {
    char const* hash = libssh2_hostkey_hash(client->session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!hash) {
        set_status(client, SSH_STATE_ERROR, "Server did not present a host key");
        return false;
    }

    char   encoded[64];
    size_t encoded_len = 0;
    mbedtls_base64_encode((unsigned char*)encoded, sizeof(encoded), &encoded_len, (unsigned char const*)hash, 32);
    encoded[encoded_len] = '\0';
    while (encoded_len > 0 && encoded[encoded_len - 1] == '=') {
        encoded[--encoded_len] = '\0';
    }
    snprintf(client->fingerprint, sizeof(client->fingerprint), "SHA256:%s", encoded);

    char stored[sizeof(client->fingerprint)];
    if (knownhost_get(client->profile.host, client->profile.port, stored, sizeof(stored))) {
        if (strcmp(stored, client->fingerprint) == 0) {
            return true;
        }
        client->host_changed = true;
    } else {
        client->host_changed = false;
    }

    set_status(client, SSH_STATE_VERIFY_HOST, "Host key %s", client->fingerprint);
    if (!wait_for_answer(client)) {
        return false;
    }
    if (!client->host_accepted) {
        set_status(client, SSH_STATE_ERROR, "Host key rejected");
        return false;
    }
    if (client->host_remember) {
        knownhost_set(client->profile.host, client->profile.port, client->fingerprint);
    }
    return true;
}

// libssh2 hands over the blob to sign and wraps whatever comes back in the
// authentication packet, so the badge key never has to leave the keystore.
static LIBSSH2_USERAUTH_PUBLICKEY_SIGN_FUNC(badge_key_sign) {
    (void)session;
    (void)abstract;

    unsigned char* signature = malloc(KEYSTORE_SIGNATURE_LEN);
    if (!signature) {
        return -1;
    }
    if (keystore_sign(data, data_len, signature) != ESP_OK) {
        free(signature);
        return -1;
    }
    *sig     = signature;
    *sig_len = KEYSTORE_SIGNATURE_LEN;
    return 0;
}

static bool try_public_key(ssh_client_t* client) {
    size_t         blob_len = 0;
    uint8_t const* blob     = keystore_public_blob(&blob_len);
    if (!blob) {
        return false;
    }

    void* abstract = NULL;
    int   rc =
        libssh2_userauth_publickey(client->session, client->profile.user, blob, blob_len, badge_key_sign, &abstract);
    if (rc == 0) {
        ESP_LOGI(TAG, "Authenticated with the badge key");
        return true;
    }
    ESP_LOGW(TAG, "Public key auth refused: %s", last_error(client->session));
    return false;
}

static bool try_password(ssh_client_t* client) {
    if (client->password[0] == '\0') {
        set_status(client, SSH_STATE_NEED_PASSWORD, "Password for %s@%s", client->profile.user, client->profile.host);
        if (!wait_for_answer(client)) {
            return false;
        }
        if (client->password[0] == '\0') {
            set_status(client, SSH_STATE_ERROR, "No password given");
            return false;
        }
        client->state = SSH_STATE_AUTHENTICATING;
    }

    int rc = libssh2_userauth_password(client->session, client->profile.user, client->password);
    if (rc == 0) {
        return true;
    }
    ESP_LOGW(TAG, "Password auth failed: %s", last_error(client->session));
    // Wrong password: forget it so the next attempt asks again.
    memset(client->password, 0, sizeof(client->password));
    return false;
}

static bool authenticate(ssh_client_t* client) {
    char* methods = libssh2_userauth_list(client->session, client->profile.user, strlen(client->profile.user));
    if (!methods) {
        // A server with no authentication at all reports success immediately.
        if (libssh2_userauth_authenticated(client->session)) {
            return true;
        }
        set_status(client, SSH_STATE_ERROR, "Server offered no login methods");
        return false;
    }
    ESP_LOGI(TAG, "Server accepts: %s", methods);

    if (client->profile.use_key && strstr(methods, "publickey")) {
        term_message(client, "Trying the badge key...");
        if (try_public_key(client)) {
            return true;
        }
    }

    if (strstr(methods, "password")) {
        for (int attempt = 0; attempt < 3; attempt++) {
            if (client->stop) {
                return false;
            }
            if (try_password(client)) {
                return true;
            }
            term_message(client, "Login failed.");
        }
    }

    set_status(client, SSH_STATE_ERROR, "Authentication failed");
    return false;
}

static bool open_shell(ssh_client_t* client) {
    int cols = 80;
    int rows = 24;
    xSemaphoreTake(client->term_lock, portMAX_DELAY);
    cols = term_cols(client->term);
    rows = term_rows(client->term);
    xSemaphoreGive(client->term_lock);

    client->channel = libssh2_channel_open_session(client->session);
    if (!client->channel) {
        set_status(client, SSH_STATE_ERROR, "Cannot open a channel: %s", last_error(client->session));
        return false;
    }

    if (libssh2_channel_request_pty_ex(client->channel, "xterm-256color", 14, NULL, 0, cols, rows, 0, 0)) {
        set_status(client, SSH_STATE_ERROR, "Server refused a terminal");
        return false;
    }

    if (libssh2_channel_shell(client->channel)) {
        set_status(client, SSH_STATE_ERROR, "Server refused a shell: %s", last_error(client->session));
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Session task
// ---------------------------------------------------------------------------

static void pump(ssh_client_t* client) {
    uint8_t buffer[READ_CHUNK];

    while (!client->stop) {
        bool did_work = false;

        if (client->resize_pending) {
            client->resize_pending = false;
            libssh2_channel_request_pty_size(client->channel, client->pending_cols, client->pending_rows);
        }

        size_t taken = xStreamBufferReceive(client->outgoing, buffer, sizeof(buffer), 0);
        size_t sent  = 0;
        while (sent < taken) {
            ssize_t written = libssh2_channel_write(client->channel, (char const*)buffer + sent, taken - sent);
            if (written == LIBSSH2_ERROR_EAGAIN) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            if (written < 0) {
                set_status(client, SSH_STATE_ERROR, "Write failed: %s", last_error(client->session));
                return;
            }
            sent     += (size_t)written;
            did_work  = true;
        }

        ssize_t read = libssh2_channel_read(client->channel, (char*)buffer, sizeof(buffer));
        if (read > 0) {
            term_feed(client, buffer, (size_t)read);
            did_work = true;
        } else if (read < 0 && read != LIBSSH2_ERROR_EAGAIN) {
            set_status(client, SSH_STATE_ERROR, "Read failed: %s", last_error(client->session));
            return;
        }

        // Drain stderr into the same screen; a shell rarely uses it, but when it
        // does the message matters.
        read = libssh2_channel_read_stderr(client->channel, (char*)buffer, sizeof(buffer));
        if (read > 0) {
            term_feed(client, buffer, (size_t)read);
            did_work = true;
        }

        if (libssh2_channel_eof(client->channel)) {
            set_status(client, SSH_STATE_CLOSED, "Session closed by the remote host");
            return;
        }

        if (!did_work) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    set_status(client, SSH_STATE_CLOSED, "Disconnected");
}

static void session_task(void* argument) {
    ssh_client_t* client = argument;

    set_status(client, SSH_STATE_CONNECTING, "Connecting to %s:%u", client->profile.host,
               (unsigned)client->profile.port);
    term_message(client, "Connecting to %s:%u...", client->profile.host, (unsigned)client->profile.port);

    client->socket = open_socket(client);
    if (client->socket < 0) {
        goto done;
    }

    client->session = libssh2_session_init();
    if (!client->session) {
        set_status(client, SSH_STATE_ERROR, "Out of memory starting the session");
        goto done;
    }
    libssh2_session_set_timeout(client->session, CONNECT_TIMEOUT_MS);
    libssh2_session_set_blocking(client->session, 1);
    apply_algorithm_preferences(client);

    if (libssh2_session_handshake(client->session, client->socket)) {
        set_status(client, SSH_STATE_ERROR, "Handshake failed: %s", last_error(client->session));
        goto done;
    }

    if (!verify_host_key(client)) {
        goto done;
    }

    client->state = SSH_STATE_AUTHENTICATING;
    if (!authenticate(client)) {
        goto done;
    }

    if (!open_shell(client)) {
        goto done;
    }

    set_status(client, SSH_STATE_CONNECTED, "Connected to %s", client->profile.host);
    // The interactive phase must not block: input has to keep flowing while the
    // remote side is quiet.
    libssh2_session_set_blocking(client->session, 0);
    libssh2_session_set_timeout(client->session, 0);
    pump(client);

done:
    if (client->state == SSH_STATE_ERROR) {
        term_message(client, "%s", client->status);
    } else if (client->state != SSH_STATE_CLOSED) {
        client->state = SSH_STATE_CLOSED;
    }

    if (client->channel) {
        libssh2_channel_free(client->channel);
        client->channel = NULL;
    }
    if (client->session) {
        libssh2_session_disconnect(client->session, "Bye");
        libssh2_session_free(client->session);
        client->session = NULL;
    }
    if (client->socket >= 0) {
        close(client->socket);
        client->socket = -1;
    }

    // Never leave a password lying around after the session it belonged to.
    memset(client->password, 0, sizeof(client->password));

    client->task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

ssh_client_t* ssh_client_create(term_t* term, SemaphoreHandle_t term_lock) {
    static bool library_started = false;
    if (!library_started) {
        if (libssh2_init(0) != 0) {
            ESP_LOGE(TAG, "libssh2 failed to initialise");
            return NULL;
        }
        library_started = true;
    }

    ssh_client_t* client = calloc(1, sizeof(ssh_client_t));
    if (!client) {
        return NULL;
    }
    client->term      = term;
    client->term_lock = term_lock;
    client->socket    = -1;
    client->state     = SSH_STATE_IDLE;
    client->outgoing  = xStreamBufferCreate(OUTGOING_BUFFER, 1);
    if (!client->outgoing) {
        free(client);
        return NULL;
    }
    return client;
}

void ssh_client_destroy(ssh_client_t* client) {
    if (!client) {
        return;
    }
    ssh_client_disconnect(client);
    vStreamBufferDelete(client->outgoing);
    free(client);
}

esp_err_t ssh_client_connect(ssh_client_t* client, host_profile_t const* profile) {
    if (client->task) {
        return ESP_ERR_INVALID_STATE;
    }

    client->profile = *profile;
    if (client->profile.port == 0) {
        client->profile.port = 22;
    }
    client->stop         = false;
    client->answer_ready = false;
    client->host_changed = false;
    client->state        = SSH_STATE_CONNECTING;
    client->status[0]    = '\0';
    strlcpy(client->password, profile->password, sizeof(client->password));
    xStreamBufferReset(client->outgoing);

    if (xTaskCreate(session_task, "ssh", TASK_STACK, client, 5, &client->task) != pdPASS) {
        client->state = SSH_STATE_ERROR;
        snprintf(client->status, sizeof(client->status), "Cannot start the session task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ssh_client_disconnect(ssh_client_t* client) {
    if (!client->task) {
        return;
    }
    client->stop         = true;
    client->answer_ready = true;  // Release anything waiting on the user
    for (int waited = 0; client->task && waited < 100; waited++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

ssh_state_t ssh_client_state(ssh_client_t const* client) {
    return client->state;
}

char const* ssh_client_status(ssh_client_t const* client) {
    return client->status;
}

char const* ssh_client_host_fingerprint(ssh_client_t const* client) {
    return client->fingerprint;
}

bool ssh_client_host_changed(ssh_client_t const* client) {
    return client->host_changed;
}

void ssh_client_accept_host(ssh_client_t* client, bool accept, bool remember) {
    client->host_accepted = accept;
    client->host_remember = remember;
    client->answer_ready  = true;
}

void ssh_client_provide_password(ssh_client_t* client, char const* password) {
    strlcpy(client->password, password ? password : "", sizeof(client->password));
    client->answer_ready = true;
}

void ssh_client_send(ssh_client_t* client, void const* data, size_t len) {
    if (client->state != SSH_STATE_CONNECTED || len == 0) {
        return;
    }
    xStreamBufferSend(client->outgoing, data, len, 0);
}

void ssh_client_resize(ssh_client_t* client, int cols, int rows) {
    client->pending_cols   = cols;
    client->pending_rows   = rows;
    client->resize_pending = true;
}
