// SPDX-License-Identifier: MIT
//
// Tests for the known-host store in main/hosts.c. That code decides whether a
// server's key is the one we saw last time, so a mistake there is a silent
// downgrade rather than a visible failure — and it is worth testing off-device.

#include "shims.h"

// The code under test, compiled straight into this program so the tests cannot
// drift away from it.
#include "../../main/hosts.c"

void nvs_test_reset(void);

static int failures = 0;

#define CHECK(condition, ...)                        \
    do {                                             \
        if (!(condition)) {                          \
            printf("FAIL %s:%d: ", __func__, __LINE__); \
            printf(__VA_ARGS__);                     \
            printf("\n");                            \
            failures++;                              \
        }                                            \
    } while (0)

// Every fingerprint the app produces starts "SHA256:", so the record contains a
// colon after the one separating host from port. Getting that wrong makes every
// lookup miss and quietly turns host key pinning off.
static char const FINGERPRINT[] = "SHA256:aBcD3fGh+Ij/KlMnOpQrStUvWxYz0123456789abcd";

static void test_round_trip(void) {
    nvs_test_reset();
    char stored[80];

    CHECK(knownhost_set("example.com", 22, FINGERPRINT) == ESP_OK, "set failed");
    CHECK(knownhost_get("example.com", 22, stored, sizeof(stored)), "lookup missed");
    CHECK(strcmp(stored, FINGERPRINT) == 0, "got '%s'", stored);
}

static void test_unknown_host_misses(void) {
    nvs_test_reset();
    char stored[80];

    CHECK(!knownhost_get("example.com", 22, stored, sizeof(stored)), "empty store returned a hit");
}

static void test_port_is_part_of_the_identity(void) {
    nvs_test_reset();
    char stored[80];

    knownhost_set("example.com", 22, FINGERPRINT);
    CHECK(!knownhost_get("example.com", 2222, stored, sizeof(stored)), "port was ignored");
}

static void test_host_is_part_of_the_identity(void) {
    nvs_test_reset();
    char stored[80];

    knownhost_set("example.com", 22, FINGERPRINT);
    CHECK(!knownhost_get("other.example.com", 22, stored, sizeof(stored)), "host was ignored");
    CHECK(!knownhost_get("example.co", 22, stored, sizeof(stored)), "a prefix matched");
    CHECK(!knownhost_get("example.comm", 22, stored, sizeof(stored)), "a longer name matched");
}

// Host names are case insensitive, so a key pinned for one spelling must be
// found under any other. Otherwise a changed key at a differently-cased name
// would show as a brand new host rather than a warning.
static void test_host_case_is_normalized(void) {
    nvs_test_reset();
    char stored[80];

    knownhost_set("Example.COM", 22, FINGERPRINT);
    CHECK(knownhost_get("example.com", 22, stored, sizeof(stored)), "lower case spelling missed the pin");
    CHECK(knownhost_get("EXAMPLE.COM", 22, stored, sizeof(stored)), "upper case spelling missed the pin");

    // And the same key name is produced regardless of case.
    char a[NVS_KEY_NAME_MAX_SIZE];
    char b[NVS_KEY_NAME_MAX_SIZE];
    known_key("Host.Example", 22, a, sizeof(a));
    known_key("host.example", 22, b, sizeof(b));
    CHECK(strcmp(a, b) == 0, "case changed the key name: '%s' vs '%s'", a, b);
}

// A host name can itself be full of colons.
static void test_ipv6_hosts(void) {
    nvs_test_reset();
    char stored[80];

    CHECK(knownhost_set("::1", 22, FINGERPRINT) == ESP_OK, "set failed for ::1");
    CHECK(knownhost_get("::1", 22, stored, sizeof(stored)), "::1 lookup missed");
    CHECK(strcmp(stored, FINGERPRINT) == 0, "::1 gave '%s'", stored);

    CHECK(knownhost_set("[2001:db8::1]", 2222, FINGERPRINT) == ESP_OK, "set failed for a bracketed address");
    CHECK(knownhost_get("[2001:db8::1]", 2222, stored, sizeof(stored)), "bracketed lookup missed");
    CHECK(!knownhost_get("[2001:db8::2]", 2222, stored, sizeof(stored)), "a different address matched");
}

static void test_replacing_a_key_for_the_same_host(void) {
    nvs_test_reset();
    char stored[80];
    char const other[] = "SHA256:ZZZZ3fGh+Ij/KlMnOpQrStUvWxYz0123456789abcd";

    knownhost_set("example.com", 22, FINGERPRINT);
    CHECK(knownhost_set("example.com", 22, other) == ESP_OK, "could not replace its own record");
    CHECK(knownhost_get("example.com", 22, stored, sizeof(stored)), "lookup missed after replacement");
    CHECK(strcmp(stored, other) == 0, "replacement not stored");
}

static void test_longest_host_name(void) {
    nvs_test_reset();
    char host[HOST_NAME_MAX];
    char stored[80];

    memset(host, 'a', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';

    CHECK(knownhost_set(host, 22, FINGERPRINT) == ESP_OK, "set failed for a maximum length host");
    CHECK(knownhost_get(host, 22, stored, sizeof(stored)), "lookup missed for a maximum length host");
}

static void test_malformed_records_are_rejected(void) {
    char stored[80];

    CHECK(!parse_record("no newline here", "example.com", 22, stored, sizeof(stored)), "accepted no newline");
    CHECK(!parse_record("\nSHA256:x", "example.com", 22, stored, sizeof(stored)), "accepted no host");
    CHECK(!parse_record("example.com\nSHA256:x", "example.com", 22, stored, sizeof(stored)),
          "accepted a missing port");
    CHECK(!parse_record("", "example.com", 22, stored, sizeof(stored)), "accepted an empty record");
    CHECK(parse_record("example.com:22\nSHA256:x", "example.com", 22, stored, sizeof(stored)),
          "rejected a good record");
    CHECK(strcmp(stored, "SHA256:x") == 0, "fingerprint mis-parsed as '%s'", stored);
}

// The key name is what NVS is handed; too long and every write fails.
static void test_key_name_fits_nvs(void) {
    char host[HOST_NAME_MAX];
    char key[NVS_KEY_NAME_MAX_SIZE];

    memset(host, 'z', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';

    known_key(host, 65535, key, sizeof(key));
    CHECK(strlen(key) < NVS_KEY_NAME_MAX_SIZE, "key '%s' is %zu characters", key, strlen(key));
    CHECK(key[0] == 'h', "key does not start with the prefix");
}

// Host and port must not be able to trade characters between them.
static void test_host_and_port_are_delimited(void) {
    char a[NVS_KEY_NAME_MAX_SIZE];
    char b[NVS_KEY_NAME_MAX_SIZE];

    known_key("a", 2258, a, sizeof(a));
    known_key("a:22", 58, b, sizeof(b));
    CHECK(strcmp(a, b) != 0, "host and port ran together");
}

// A pin written before the slot name was case folded lives under a name this
// build no longer derives. If the lookup cannot find it the host reads as never
// seen, and the next key offered is pinned in its place with the benign prompt —
// so the old entry has to be found, and moved.
static void test_pin_under_the_legacy_slot_is_found_and_migrated(void) {
    nvs_test_reset();

    char legacy_key[NVS_KEY_NAME_MAX_SIZE];
    known_key_variant("Example.com", 22, legacy_key, sizeof(legacy_key), false);
    char record[KNOWN_RECORD_MAX];
    snprintf(record, sizeof(record), "Example.com:22\n%s", FINGERPRINT);

    nvs_handle_t handle;
    CHECK(nvs_open(NVS_KNOWN_NS, NVS_READWRITE, &handle) == ESP_OK, "nvs_open failed");
    CHECK(nvs_set_str(handle, legacy_key, record) == ESP_OK, "seeding the legacy slot failed");
    nvs_close(handle);

    char stored[80];
    CHECK(knownhost_get("Example.com", 22, stored, sizeof(stored)), "the legacy pin was not found");
    CHECK(strcmp(stored, FINGERPRINT) == 0, "got '%s'", stored);

    // Moved rather than copied: readable under the folded spelling, and the old
    // slot is gone so this does not run again for the same host.
    CHECK(knownhost_get("example.com", 22, stored, sizeof(stored)), "not readable under the folded name");
    CHECK(nvs_open(NVS_KNOWN_NS, NVS_READONLY, &handle) == ESP_OK, "nvs_open failed");
    size_t size = sizeof(record);
    CHECK(nvs_get_str(handle, legacy_key, record, &size) != ESP_OK, "the legacy slot survived the migration");
    nvs_close(handle);
}

// A host with no upper-case letter derives the same slot either way, so a miss
// is a real miss: it must not be reported as a hit from somewhere else.
static void test_lowercase_host_still_misses(void) {
    nvs_test_reset();
    char stored[80];
    CHECK(!knownhost_get("example.com", 22, stored, sizeof(stored)), "an unpinned host reported a pin");
}

int main(void) {
    test_round_trip();
    test_unknown_host_misses();
    test_port_is_part_of_the_identity();
    test_host_is_part_of_the_identity();
    test_host_case_is_normalized();
    test_ipv6_hosts();
    test_replacing_a_key_for_the_same_host();
    test_longest_host_name();
    test_malformed_records_are_rejected();
    test_key_name_fits_nvs();
    test_host_and_port_are_delimited();
    test_pin_under_the_legacy_slot_is_found_and_migrated();
    test_lowercase_host_still_misses();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
