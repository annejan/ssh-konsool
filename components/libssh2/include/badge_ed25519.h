/* SPDX-License-Identifier: MIT
 *
 * Ed25519 for the rest of the firmware. PSA has no Edwards curve support, so
 * this is TweetNaCl, which the libssh2 backend already carries.
 */

#ifndef BADGE_ED25519_H
#define BADGE_ED25519_H

#include <stddef.h>
#include <stdint.h>

#define BADGE_ED25519_PUBLIC_LEN    32
#define BADGE_ED25519_SECRET_LEN    64  /* seed || public key */
#define BADGE_ED25519_SIGNATURE_LEN 64

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0 on success. */
int badge_ed25519_keypair(uint8_t public_key[BADGE_ED25519_PUBLIC_LEN],
                          uint8_t secret_key[BADGE_ED25519_SECRET_LEN]);

int badge_ed25519_sign(const uint8_t secret_key[BADGE_ED25519_SECRET_LEN],
                       const void *message, size_t message_len,
                       uint8_t signature[BADGE_ED25519_SIGNATURE_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* BADGE_ED25519_H */
