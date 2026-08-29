/* SPDX-License-Identifier: BSD-3-Clause
 *
 * A libssh2 crypto backend built on the PSA Crypto API, as shipped with
 * ESP-IDF's Mbed TLS 4.x, plus TweetNaCl for Ed25519 and X25519.
 *
 * Mbed TLS 4 removed the legacy mbedtls_rsa/ecp/bignum/pk-EC interfaces that
 * libssh2's own mbedTLS backend is written against, so this backend talks to
 * PSA directly. PSA has no big integer arithmetic and no Edwards curve
 * support, which shapes two decisions:
 *
 *   - The finite field Diffie-Hellman key exchanges need modular
 *     exponentiation over 2048 bit and larger groups. They are not supported;
 *     the application asks for ECDH key exchange instead.
 *   - Ed25519 signing and verification, and X25519, come from TweetNaCl.
 *
 * Only public keys are parsed here. Private keys the badge did not generate
 * itself are not supported: the application signs with its own key through
 * libssh2_userauth_publickey() and a signing callback.
 */

#ifndef LIBSSH2_PSA_BACKEND_H
#define LIBSSH2_PSA_BACKEND_H

#include <psa/crypto.h>
#include <stdint.h>
#include <stdlib.h>

/* libssh2_crypto_engine() reports which library is underneath. PSA here is
   Mbed TLS's implementation of it, which is what that enum value means. */
#define LIBSSH2_CRYPTO_ENGINE libssh2_mbedtls

/* Which algorithms this backend offers. The deliberate zeroes are the ones
   modern servers no longer want: MD5, RIPEMD, SHA-1 signatures, RC4, DES and
   Blowfish. */
#define LIBSSH2_MD5             0
#define LIBSSH2_HMAC_RIPEMD     0
#define LIBSSH2_HMAC_SHA256     1
#define LIBSSH2_HMAC_SHA512     1

#define LIBSSH2_AES_CBC         1
#define LIBSSH2_AES_CTR         1
#define LIBSSH2_AES_GCM         0
#define LIBSSH2_BLOWFISH        0
#define LIBSSH2_RC4             0
#define LIBSSH2_CAST            0
#define LIBSSH2_3DES            0

#define LIBSSH2_RSA             1
#define LIBSSH2_RSA_SHA1        0
#define LIBSSH2_RSA_SHA2        1
#define LIBSSH2_DSA             0
#define LIBSSH2_ECDSA           1
#define LIBSSH2_ED25519         1

#include "crypto_config.h"

#define SHA_DIGEST_LENGTH      20
#define SHA256_DIGEST_LENGTH   32
#define SHA384_DIGEST_LENGTH   48
#define SHA512_DIGEST_LENGTH   64

/* Room for an uncompressed point on the largest curve libssh2 knows (P-521) */
#define EC_MAX_POINT_LEN ((528 * 2 / 8) + 1)

/*******************************************************************/
/* Generic                                                         */

#define libssh2_crypto_init()   _libssh2_psa_init()
#define libssh2_crypto_exit()   _libssh2_psa_exit()
#define _libssh2_random(buf, len) _libssh2_psa_random(buf, len)
#define libssh2_prepare_iovec(vec, len)  /* Empty. */

int  _libssh2_psa_init(void);
void _libssh2_psa_exit(void);
int  _libssh2_psa_random(unsigned char *buf, size_t len);

/*******************************************************************/
/* Hashes                                                          */

typedef struct {
    psa_hash_operation_t operation;
} libssh2_psa_hash_ctx;

int _libssh2_psa_hash_init(libssh2_psa_hash_ctx *ctx, psa_algorithm_t alg);
int _libssh2_psa_hash_update(libssh2_psa_hash_ctx *ctx,
                             const void *data, size_t len);
int _libssh2_psa_hash_final(libssh2_psa_hash_ctx *ctx, unsigned char *out);
int _libssh2_psa_hash(const void *data, size_t len,
                      psa_algorithm_t alg, unsigned char *out);

#define libssh2_sha1_ctx libssh2_psa_hash_ctx
#define libssh2_sha1_init(pctx) _libssh2_psa_hash_init(pctx, PSA_ALG_SHA_1)
#define libssh2_sha1_update(ctx, data, len) \
    _libssh2_psa_hash_update(&(ctx), data, len)
#define libssh2_sha1_final(ctx, hash) _libssh2_psa_hash_final(&(ctx), hash)
#define libssh2_sha1(data, len, hash) \
    _libssh2_psa_hash(data, len, PSA_ALG_SHA_1, hash)

#define libssh2_sha256_ctx libssh2_psa_hash_ctx
#define libssh2_sha256_init(pctx) _libssh2_psa_hash_init(pctx, PSA_ALG_SHA_256)
#define libssh2_sha256_update(ctx, data, len) \
    _libssh2_psa_hash_update(&(ctx), data, len)
#define libssh2_sha256_final(ctx, hash) _libssh2_psa_hash_final(&(ctx), hash)
#define libssh2_sha256(data, len, hash) \
    _libssh2_psa_hash(data, len, PSA_ALG_SHA_256, hash)

#define libssh2_sha384_ctx libssh2_psa_hash_ctx
#define libssh2_sha384_init(pctx) _libssh2_psa_hash_init(pctx, PSA_ALG_SHA_384)
#define libssh2_sha384_update(ctx, data, len) \
    _libssh2_psa_hash_update(&(ctx), data, len)
#define libssh2_sha384_final(ctx, hash) _libssh2_psa_hash_final(&(ctx), hash)
#define libssh2_sha384(data, len, hash) \
    _libssh2_psa_hash(data, len, PSA_ALG_SHA_384, hash)

#define libssh2_sha512_ctx libssh2_psa_hash_ctx
#define libssh2_sha512_init(pctx) _libssh2_psa_hash_init(pctx, PSA_ALG_SHA_512)
#define libssh2_sha512_update(ctx, data, len) \
    _libssh2_psa_hash_update(&(ctx), data, len)
#define libssh2_sha512_final(ctx, hash) _libssh2_psa_hash_final(&(ctx), hash)
#define libssh2_sha512(data, len, hash) \
    _libssh2_psa_hash(data, len, PSA_ALG_SHA_512, hash)

/*******************************************************************/
/* HMAC                                                            */

typedef struct {
    psa_mac_operation_t operation;
    psa_key_id_t        key;
} libssh2_psa_hmac_ctx;

#define libssh2_hmac_ctx libssh2_psa_hmac_ctx

/*******************************************************************/
/* Ciphers                                                         */

typedef enum {
    LIBSSH2_PSA_CIPHER_NONE = 0,
    LIBSSH2_PSA_CIPHER_AES128_CTR,
    LIBSSH2_PSA_CIPHER_AES192_CTR,
    LIBSSH2_PSA_CIPHER_AES256_CTR,
    LIBSSH2_PSA_CIPHER_AES128_CBC,
    LIBSSH2_PSA_CIPHER_AES192_CBC,
    LIBSSH2_PSA_CIPHER_AES256_CBC,
    /* libssh2 implements ChaCha20-Poly1305 itself; this only names the entry
       in its cipher table. */
    LIBSSH2_PSA_CIPHER_CHACHA20,
} libssh2_psa_cipher_t;

typedef struct {
    psa_cipher_operation_t operation;
    psa_key_id_t           key;
} libssh2_psa_cipher_ctx;

#define _libssh2_cipher_ctx        libssh2_psa_cipher_ctx
#define _libssh2_cipher_type(algo) libssh2_psa_cipher_t algo

#define _libssh2_cipher_aes128ctr LIBSSH2_PSA_CIPHER_AES128_CTR
#define _libssh2_cipher_aes192ctr LIBSSH2_PSA_CIPHER_AES192_CTR
#define _libssh2_cipher_aes256ctr LIBSSH2_PSA_CIPHER_AES256_CTR
#define _libssh2_cipher_aes128    LIBSSH2_PSA_CIPHER_AES128_CBC
#define _libssh2_cipher_aes192    LIBSSH2_PSA_CIPHER_AES192_CBC
#define _libssh2_cipher_aes256    LIBSSH2_PSA_CIPHER_AES256_CBC
#define _libssh2_cipher_chacha20  LIBSSH2_PSA_CIPHER_CHACHA20

#define _libssh2_cipher_dtor(ctx) _libssh2_psa_cipher_dtor(ctx)
void _libssh2_psa_cipher_dtor(_libssh2_cipher_ctx *ctx);

/*******************************************************************/
/* Keys                                                            */

typedef struct {
    psa_key_id_t key;
    size_t       bits;
} libssh2_psa_rsa_ctx;

#define libssh2_rsa_ctx      libssh2_psa_rsa_ctx
#define _libssh2_rsa_free(c) _libssh2_psa_rsa_free(c)
void _libssh2_psa_rsa_free(libssh2_rsa_ctx *ctx);

typedef enum {
    LIBSSH2_EC_CURVE_NISTP256 = 256,
    LIBSSH2_EC_CURVE_NISTP384 = 384,
    LIBSSH2_EC_CURVE_NISTP521 = 521,
} libssh2_curve_type;

typedef struct {
    psa_key_id_t       key;
    libssh2_curve_type curve;
    int                is_private;
} libssh2_psa_ec_ctx;

#define libssh2_ecdsa_ctx      libssh2_psa_ec_ctx
#define _libssh2_ec_key        libssh2_psa_ec_ctx
#define _libssh2_ecdsa_free(c) _libssh2_psa_ecdsa_free(c)
void _libssh2_psa_ecdsa_free(libssh2_ecdsa_ctx *ctx);

typedef struct {
    unsigned char public_key[32];
    unsigned char private_key[64];  /* TweetNaCl keeps the public half here */
    int           has_private;
} libssh2_psa_ed25519_ctx;

#define libssh2_ed25519_ctx      libssh2_psa_ed25519_ctx
#define _libssh2_ed25519_free(c) _libssh2_psa_ed25519_free(c)
void _libssh2_psa_ed25519_free(libssh2_ed25519_ctx *ctx);

/*******************************************************************/
/* Big numbers                                                     */
/*
 * Only what the key exchange needs: hold a big endian integer, hand it back.
 * No arithmetic, because the exchanges that need arithmetic are not offered.
 */

typedef struct {
    unsigned char *bytes;  /* Big endian, no leading zero bytes */
    size_t         len;
} libssh2_psa_bn;

#define _libssh2_bn_ctx           int  /* not used */
#define _libssh2_bn_ctx_new()     0    /* not used */
#define _libssh2_bn_ctx_free(c)   ((void)0)

#define _libssh2_bn                    libssh2_psa_bn
#define _libssh2_bn_init()             _libssh2_psa_bn_init()
#define _libssh2_bn_init_from_bin()    _libssh2_psa_bn_init()
#define _libssh2_bn_free(bn)           _libssh2_psa_bn_free(bn)
#define _libssh2_bn_set_word(bn, word) _libssh2_psa_bn_set_word(bn, word)
#define _libssh2_bn_from_bin(bn, len, bin) \
    _libssh2_psa_bn_from_bin(bn, len, bin)
#define _libssh2_bn_to_bin(bn, bin)    _libssh2_psa_bn_to_bin(bn, bin)
#define _libssh2_bn_bytes(bn)          _libssh2_psa_bn_bytes(bn)
#define _libssh2_bn_bits(bn)           _libssh2_psa_bn_bits(bn)

_libssh2_bn *_libssh2_psa_bn_init(void);
void         _libssh2_psa_bn_free(_libssh2_bn *bn);
int          _libssh2_psa_bn_set_word(_libssh2_bn *bn, unsigned long word);
int          _libssh2_psa_bn_from_bin(_libssh2_bn *bn, size_t len,
                                      const unsigned char *bin);
int          _libssh2_psa_bn_to_bin(const _libssh2_bn *bn, unsigned char *bin);
size_t       _libssh2_psa_bn_bytes(const _libssh2_bn *bn);
size_t       _libssh2_psa_bn_bits(const _libssh2_bn *bn);

/*******************************************************************/
/* Diffie-Hellman                                                  */
/*
 * Present only because libssh2's key exchange table refers to it. Every entry
 * point fails, and the application restricts the offered key exchanges to the
 * elliptic curve ones so these are never reached.
 */

#define LIBSSH2_DH_GEX_MINGROUP     2048
#define LIBSSH2_DH_GEX_OPTGROUP     4096
#define LIBSSH2_DH_GEX_MAXGROUP     8192
#define LIBSSH2_DH_MAX_MODULUS_BITS 16384

typedef struct {
    int unused;
} libssh2_psa_dh_ctx;

#define _libssh2_dh_ctx libssh2_psa_dh_ctx

#define libssh2_dh_init(dhctx) _libssh2_psa_dh_init(dhctx)
#define libssh2_dh_key_pair(dhctx, public, g, p, group_order, bnctx) \
    _libssh2_psa_dh_key_pair(dhctx, public, g, p, group_order)
#define libssh2_dh_secret(dhctx, secret, f, p, bnctx) \
    _libssh2_psa_dh_secret(dhctx, secret, f, p)
#define libssh2_dh_dtor(dhctx) _libssh2_psa_dh_dtor(dhctx)

void _libssh2_psa_dh_init(_libssh2_dh_ctx *dhctx);
int  _libssh2_psa_dh_key_pair(_libssh2_dh_ctx *dhctx, _libssh2_bn *public,
                              _libssh2_bn *g, _libssh2_bn *p, int group_order);
int  _libssh2_psa_dh_secret(_libssh2_dh_ctx *dhctx, _libssh2_bn *secret,
                            _libssh2_bn *f, _libssh2_bn *p);
void _libssh2_psa_dh_dtor(_libssh2_dh_ctx *dhctx);

extern void _libssh2_init_aes_ctr(void);

#endif /* LIBSSH2_PSA_BACKEND_H */
