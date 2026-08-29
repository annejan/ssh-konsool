/* SPDX-License-Identifier: MIT */

#include "badge_ed25519.h"
#include <stdlib.h>
#include <string.h>
#include "tweetnacl.h"

int badge_ed25519_keypair(uint8_t public_key[BADGE_ED25519_PUBLIC_LEN],
                          uint8_t secret_key[BADGE_ED25519_SECRET_LEN])
{
    return crypto_sign_keypair(public_key, secret_key);
}

int badge_ed25519_sign(const uint8_t secret_key[BADGE_ED25519_SECRET_LEN],
                       const void *message, size_t message_len,
                       uint8_t signature[BADGE_ED25519_SIGNATURE_LEN])
{
    /* TweetNaCl signs by prefixing the signature to a copy of the message. */
    unsigned char     *signed_message = malloc(message_len +
                                               BADGE_ED25519_SIGNATURE_LEN);
    unsigned long long signed_len     = 0;
    int                result;

    if(!signed_message)
        return -1;

    result = crypto_sign(signed_message, &signed_len, message,
                         (unsigned long long)message_len, secret_key);
    if(result == 0)
        memcpy(signature, signed_message, BADGE_ED25519_SIGNATURE_LEN);

    free(signed_message);
    return result;
}
