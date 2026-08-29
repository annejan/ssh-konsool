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
#include "mbedtls/platform_util.h"

static char const TAG[] = "ssh";

#define OUTGOING_BUFFER    4096
#define READ_CHUNK         2048
#define CONNECT_TIMEOUT_MS 15000
// How long the remote side may refuse to accept a byte before we give up on it.
#define WRITE_STALL_MS     10000
// A key install prints a line or two; anything beyond this is the server being
// hostile rather than chatty.
#define COPY_ID_OUTPUT_MAX 8192
#define TASK_STACK         16384

// What the session is for. Both kinds share the connect, host key and
// authentication path; only what happens afterwards differs.
typedef enum {
    SSH_JOB_SHELL = 0,
    SSH_JOB_COPY_ID,
} ssh_job_t;

struct ssh_client {
    term_t*           term;
    SemaphoreHandle_t term_lock;

    host_profile_t profile;
    ssh_job_t      job;
    volatile bool  copy_id_ok;

    volatile ssh_state_t state;
    char                 status[128];
    char                 fingerprint[80];
    bool                 host_changed;

    StreamBufferHandle_t outgoing;
    TaskHandle_t         task;

    volatile bool stop;
    volatile bool answer_ready;
    // Written by the UI task, read by the session task.
    volatile bool host_accepted;
    volatile bool host_remember;
    char          password[HOST_PASSWORD_MAX];

    // Replies the terminal owes the host (cursor reports and the like). They
    // are produced on the session task inside term_feed, so they get their own
    // buffer instead of sharing the UI task's stream buffer, which allows only
    // one writer.
    char   reply[128];
    size_t reply_len;

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

// Output from a command run without a terminal arrives with bare line feeds,
// where a shell on a pty would have sent carriage returns too. Without this the
// remote's messages walk down the screen in steps.
static void term_feed_lines(ssh_client_t* client, char const* data, size_t len) {
    char   buffer[128];
    size_t used = 0;
    for (size_t i = 0; i < len; i++) {
        if (used + 2 > sizeof(buffer)) {
            term_feed(client, buffer, used);
            used = 0;
        }
        if (data[i] == '\n') {
            buffer[used++] = '\r';
        }
        buffer[used++] = data[i];
    }
    if (used) {
        term_feed(client, buffer, used);
    }
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
    while (!client->answer_ready && !client->stop) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    // Disconnecting releases this wait too, and that is a cancellation, not an
    // answer. Only a real reply from the user counts.
    return client->answer_ready && !client->stop;
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
            if (client->stop) {
                return false;  // Cancelled at the prompt, not a rejected password
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
// Installing the badge key
// ---------------------------------------------------------------------------

// What ssh-copy-id does, with the key arriving on standard input. Wrapped in
// "sh -c" because the account's login shell may not be Bourne compatible, and
// written without a single quote anywhere so the wrapping needs no escaping.
static char const COPY_ID_COMMAND[] =
    "exec sh -c '"
    "cd; umask 077; mkdir -p .ssh || exit 1; "
    "key=$(cat); "
    "touch .ssh/authorized_keys || exit 1; "
    "chmod u+rw,go-rwx .ssh/authorized_keys; "
    "if grep -qxF \"$key\" .ssh/authorized_keys 2>/dev/null; then "
    "echo \"Key was already installed.\"; "
    "else "
    "[ -z \"$(tail -c 1 .ssh/authorized_keys 2>/dev/null)\" ] || echo >> .ssh/authorized_keys; "
    "printf \"%s\\n\" \"$key\" >> .ssh/authorized_keys || exit 1; "
    "echo \"Key installed.\"; "
    "fi; "
    "command -v restorecon >/dev/null 2>&1 && restorecon -F .ssh .ssh/authorized_keys 2>/dev/null; "
    "exit 0'";

// Write the lot, giving up if the session is cancelled or the remote side stops
// accepting bytes altogether. A slow but live peer resets the deadline on every
// byte it takes, so only a genuinely stalled one is dropped.
static bool write_all(ssh_client_t* client, char const* data, size_t len) {
    size_t     sent     = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WRITE_STALL_MS);

    while (sent < len) {
        if (client->stop) {
            return false;
        }
        ssize_t written = libssh2_channel_write(client->channel, data + sent, len - sent);
        if (written == LIBSSH2_ERROR_EAGAIN || written == 0) {
            if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
                set_status(client, SSH_STATE_ERROR, "The remote host stopped accepting input");
                return false;
            }
            // At 100 Hz pdMS_TO_TICKS(5) rounds to nothing, which would turn
            // this into a busy wait, so never yield for less than a tick.
            vTaskDelay(1);
            continue;
        }
        if (written < 0) {
            set_status(client, SSH_STATE_ERROR, "Write failed: %s", last_error(client->session));
            return false;
        }
        sent     += (size_t)written;
        deadline  = xTaskGetTickCount() + pdMS_TO_TICKS(WRITE_STALL_MS);
    }
    return true;
}

static bool run_copy_id(ssh_client_t* client) {
    char const* public_key = keystore_public_key();
    if (!public_key) {
        set_status(client, SSH_STATE_ERROR, "The badge has no key to install");
        return false;
    }

    client->channel = libssh2_channel_open_session(client->session);
    if (!client->channel) {
        set_status(client, SSH_STATE_ERROR, "Cannot open a channel: %s", last_error(client->session));
        return false;
    }

    if (libssh2_channel_exec(client->channel, COPY_ID_COMMAND)) {
        set_status(client, SSH_STATE_ERROR, "Server refused the command: %s", last_error(client->session));
        return false;
    }

    if (!write_all(client, public_key, strlen(public_key)) || !write_all(client, "\n", 1)) {
        if (client->state != SSH_STATE_ERROR) {
            set_status(client, SSH_STATE_ERROR, "Could not send the key: %s", last_error(client->session));
        }
        return false;
    }
    // The remote side reads until end of file, so our half has to close.
    libssh2_channel_send_eof(client->channel);

    // The session is still in blocking mode here, so a server that keeps
    // talking would otherwise hold this task for as long as it likes.
    char    buffer[256];
    ssize_t read;
    size_t  total = 0;
    while (!client->stop && total < COPY_ID_OUTPUT_MAX &&
           (read = libssh2_channel_read(client->channel, buffer, sizeof(buffer))) > 0) {
        term_feed_lines(client, buffer, (size_t)read);
        total += (size_t)read;
    }
    while (!client->stop && total < COPY_ID_OUTPUT_MAX &&
           (read = libssh2_channel_read_stderr(client->channel, buffer, sizeof(buffer))) > 0) {
        term_feed_lines(client, buffer, (size_t)read);
        total += (size_t)read;
    }

    if (client->stop) {
        set_status(client, SSH_STATE_CLOSED, "Cancelled");
        return false;
    }
    if (total >= COPY_ID_OUTPUT_MAX) {
        set_status(client, SSH_STATE_ERROR, "The server would not stop talking");
        return false;
    }

    // Waiting for a polite close would resume consuming the stream, so it only
    // happens once the peer has already gone quiet.
    libssh2_channel_wait_eof(client->channel);
    libssh2_channel_close(client->channel);
    libssh2_channel_wait_closed(client->channel);

    int exit_status = libssh2_channel_get_exit_status(client->channel);
    if (exit_status != 0) {
        set_status(client, SSH_STATE_ERROR, "The server rejected the key (exit %d)", exit_status);
        return false;
    }

    client->copy_id_ok = true;
    set_status(client, SSH_STATE_CLOSED, "Key installed on %s", client->profile.host);
    term_message(client, "Done. This connection will use the badge key from now on.");
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

        // Anything the terminal owed the host from the last read goes first, so
        // a cursor report cannot be overtaken by later keystrokes.
        if (client->reply_len) {
            size_t owed       = client->reply_len;
            client->reply_len = 0;
            if (!write_all(client, client->reply, owed)) {
                return;
            }
            did_work = true;
        }

        size_t taken = xStreamBufferReceive(client->outgoing, buffer, sizeof(buffer), 0);
        if (taken) {
            if (!write_all(client, (char const*)buffer, taken)) {
                return;
            }
            did_work = true;
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

    if (client->job == SSH_JOB_COPY_ID) {
        run_copy_id(client);
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
        // Whatever the session was doing when it stopped, the status line has
        // to stop describing it. Leaving "Password for ..." up after a cancel
        // tells the user the session is still waiting on them.
        set_status(client, SSH_STATE_CLOSED, client->stop ? "Cancelled" : "Disconnected");
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
    // start_session copied the whole profile, so that copy holds one too.
    mbedtls_platform_zeroize(client->password, sizeof(client->password));
    mbedtls_platform_zeroize(client->profile.password, sizeof(client->profile.password));

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
    mbedtls_platform_zeroize(client, sizeof(*client));
    free(client);
}

static esp_err_t start_session(ssh_client_t* client, host_profile_t const* profile, ssh_job_t job) {
    if (client->task) {
        return ESP_ERR_INVALID_STATE;
    }

    client->job        = job;
    client->copy_id_ok = false;
    client->profile    = *profile;
    if (client->profile.port == 0) {
        client->profile.port = 22;
    }
    client->stop          = false;
    client->answer_ready  = false;
    client->host_changed  = false;
    // Without this the acceptance latched by an earlier session would carry
    // over and the next unknown host would be trusted silently.
    client->host_accepted = false;
    client->host_remember = false;
    client->reply_len     = 0;
    client->state         = SSH_STATE_CONNECTING;
    client->status[0]     = '\0';
    strlcpy(client->password, profile->password, sizeof(client->password));
    xStreamBufferReset(client->outgoing);

    if (xTaskCreate(session_task, "ssh", TASK_STACK, client, 5, &client->task) != pdPASS) {
        client->state = SSH_STATE_ERROR;
        snprintf(client->status, sizeof(client->status), "Cannot start the session task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ssh_client_connect(ssh_client_t* client, host_profile_t const* profile) {
    return start_session(client, profile, SSH_JOB_SHELL);
}

esp_err_t ssh_client_copy_id(ssh_client_t* client, host_profile_t const* profile) {
    // The key is what we are here to install, so do not let it gate the login.
    host_profile_t with_password = *profile;
    with_password.use_key        = false;
    esp_err_t err                = start_session(client, &with_password, SSH_JOB_COPY_ID);
    mbedtls_platform_zeroize(&with_password, sizeof(with_password));
    return err;
}

void ssh_client_queue_reply(ssh_client_t* client, void const* data, size_t len) {
    // Called from the terminal while the session task is feeding it, so this is
    // the session task's own buffer and needs no lock. Dropping a reply is
    // better than growing a queue for a host that asks for thousands.
    if (client->reply_len + len > sizeof(client->reply)) {
        return;
    }
    memcpy(client->reply + client->reply_len, data, len);
    client->reply_len += len;
}

bool ssh_client_copy_id_succeeded(ssh_client_t const* client) {
    return client->copy_id_ok;
}

void ssh_client_disconnect(ssh_client_t* client) {
    if (!client->task) {
        return;
    }
    client->stop         = true;
    client->answer_ready = true;  // Release anything waiting on the user
    for (int waited = 0; client->task && waited < 40; waited++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (client->task && client->socket >= 0) {
        // Still stuck in a blocking libssh2 call. Tearing the socket down under
        // it makes that call fail so the task can reach its own cleanup.
        shutdown(client->socket, SHUT_RDWR);
        for (int waited = 0; client->task && waited < 60; waited++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
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
