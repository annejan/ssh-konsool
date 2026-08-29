/* SPDX-License-Identifier: BSD-3-Clause
 *
 * See psa_backend.h for what this backend does and does not cover.
 */

#include "libssh2_priv.h"

#include <string.h>

#include "misc.h"
#include "tweetnacl.h"

/* ------------------------------------------------------------------ */
/* Generic                                                            */
/* ------------------------------------------------------------------ */

int _libssh2_psa_init(void)
{
    /* psa_crypto_init() is idempotent, and ESP-IDF may already have run it. */
    return psa_crypto_init() == PSA_SUCCESS ? 0 : -1;
}

void _libssh2_psa_exit(void)
{
    /* Other parts of the firmware share the PSA subsystem, so it is left
       running rather than torn down under them. */
}

int _libssh2_psa_random(unsigned char *buf, size_t len)
{
    return psa_generate_random(buf, len) == PSA_SUCCESS ? 0 : -1;
}

void _libssh2_init_aes_ctr(void)
{
    /* PSA implements CTR mode itself; nothing to register. */
}

/* TweetNaCl asks the platform for randomness under this name. */
void randombytes(unsigned char *buf, unsigned long long len)
{
    psa_generate_random(buf, (size_t)len);
}

/* ------------------------------------------------------------------ */
/* Hashes                                                             */
/* ------------------------------------------------------------------ */

int _libssh2_psa_hash_init(libssh2_psa_hash_ctx *ctx, psa_algorithm_t alg)
{
    psa_hash_operation_t fresh = PSA_HASH_OPERATION_INIT;
    ctx->operation = fresh;
    return psa_hash_setup(&ctx->operation, alg) == PSA_SUCCESS ? 1 : 0;
}

int _libssh2_psa_hash_update(libssh2_psa_hash_ctx *ctx,
                             const void *data, size_t len)
{
    return psa_hash_update(&ctx->operation,
                           (const unsigned char *)data, len) == PSA_SUCCESS
           ? 1 : 0;
}

int _libssh2_psa_hash_final(libssh2_psa_hash_ctx *ctx, unsigned char *out)
{
    size_t written = 0;
    /* PSA_HASH_MAX_SIZE covers every algorithm this backend uses. */
    psa_status_t status = psa_hash_finish(&ctx->operation, out,
                                          PSA_HASH_MAX_SIZE, &written);
    return status == PSA_SUCCESS ? 1 : 0;
}

int _libssh2_psa_hash(const void *data, size_t len,
                      psa_algorithm_t alg, unsigned char *out)
{
    size_t written = 0;
    psa_status_t status = psa_hash_compute(alg, (const unsigned char *)data,
                                           len, out, PSA_HASH_MAX_SIZE,
                                           &written);
    return status == PSA_SUCCESS ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* HMAC                                                               */
/* ------------------------------------------------------------------ */

int _libssh2_hmac_ctx_init(libssh2_hmac_ctx *ctx)
{
    psa_mac_operation_t fresh = PSA_MAC_OPERATION_INIT;
    ctx->operation = fresh;
    ctx->key       = PSA_KEY_ID_NULL;
    return 1;
}

static int hmac_start(libssh2_hmac_ctx *ctx, psa_algorithm_t hash,
                      void *key, size_t keylen)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_algorithm_t      alg        = PSA_ALG_HMAC(hash);

    _libssh2_hmac_ctx_init(ctx);

    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, alg);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);

    if(psa_import_key(&attributes, (const unsigned char *)key, keylen,
                      &ctx->key) != PSA_SUCCESS)
        return 0;

    if(psa_mac_sign_setup(&ctx->operation, ctx->key, alg) != PSA_SUCCESS) {
        psa_destroy_key(ctx->key);
        ctx->key = PSA_KEY_ID_NULL;
        return 0;
    }
    return 1;
}

int _libssh2_hmac_sha1_init(libssh2_hmac_ctx *ctx, void *key, size_t keylen)
{
    return hmac_start(ctx, PSA_ALG_SHA_1, key, keylen);
}

int _libssh2_hmac_sha256_init(libssh2_hmac_ctx *ctx, void *key, size_t keylen)
{
    return hmac_start(ctx, PSA_ALG_SHA_256, key, keylen);
}

int _libssh2_hmac_sha512_init(libssh2_hmac_ctx *ctx, void *key, size_t keylen)
{
    return hmac_start(ctx, PSA_ALG_SHA_512, key, keylen);
}

int _libssh2_hmac_update(libssh2_hmac_ctx *ctx, const void *data,
                         size_t datalen)
{
    return psa_mac_update(&ctx->operation, (const unsigned char *)data,
                          datalen) == PSA_SUCCESS ? 1 : 0;
}

int _libssh2_hmac_final(libssh2_hmac_ctx *ctx, void *data)
{
    size_t written = 0;
    psa_status_t status = psa_mac_sign_finish(&ctx->operation,
                                              (unsigned char *)data,
                                              PSA_MAC_MAX_SIZE, &written);
    return status == PSA_SUCCESS ? 1 : 0;
}

void _libssh2_hmac_cleanup(libssh2_hmac_ctx *ctx)
{
    psa_mac_abort(&ctx->operation);
    if(ctx->key != PSA_KEY_ID_NULL) {
        psa_destroy_key(ctx->key);
        ctx->key = PSA_KEY_ID_NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Ciphers                                                            */
/* ------------------------------------------------------------------ */

static int cipher_parameters(libssh2_psa_cipher_t type,
                             psa_algorithm_t *alg, size_t *key_bits)
{
    switch(type) {
    case LIBSSH2_PSA_CIPHER_AES128_CTR: *alg = PSA_ALG_CTR;
        *key_bits = 128; return 0;
    case LIBSSH2_PSA_CIPHER_AES192_CTR: *alg = PSA_ALG_CTR;
        *key_bits = 192; return 0;
    case LIBSSH2_PSA_CIPHER_AES256_CTR: *alg = PSA_ALG_CTR;
        *key_bits = 256; return 0;
    case LIBSSH2_PSA_CIPHER_AES128_CBC: *alg = PSA_ALG_CBC_NO_PADDING;
        *key_bits = 128; return 0;
    case LIBSSH2_PSA_CIPHER_AES192_CBC: *alg = PSA_ALG_CBC_NO_PADDING;
        *key_bits = 192; return 0;
    case LIBSSH2_PSA_CIPHER_AES256_CBC: *alg = PSA_ALG_CBC_NO_PADDING;
        *key_bits = 256; return 0;
    default: return -1;
    }
}

int _libssh2_cipher_init(_libssh2_cipher_ctx *ctx,
                         _libssh2_cipher_type(type),
                         unsigned char *iv, unsigned char *secret,
                         int encrypt)
{
    psa_key_attributes_t   attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_cipher_operation_t fresh      = PSA_CIPHER_OPERATION_INIT;
    psa_algorithm_t        alg        = 0;
    size_t                 key_bits   = 0;
    psa_status_t           status;

    if(cipher_parameters(type, &alg, &key_bits))
        return -1;

    ctx->operation = fresh;
    ctx->key       = PSA_KEY_ID_NULL;

    psa_set_key_usage_flags(&attributes, encrypt ? PSA_KEY_USAGE_ENCRYPT
                                                 : PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, alg);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, key_bits);

    if(psa_import_key(&attributes, secret, key_bits / 8,
                      &ctx->key) != PSA_SUCCESS)
        return -1;

    status = encrypt ? psa_cipher_encrypt_setup(&ctx->operation, ctx->key, alg)
                     : psa_cipher_decrypt_setup(&ctx->operation, ctx->key, alg);
    if(status != PSA_SUCCESS) {
        psa_destroy_key(ctx->key);
        ctx->key = PSA_KEY_ID_NULL;
        return -1;
    }

    /* Both modes take a 16 byte block as the starting state: the IV for CBC,
       the initial counter block for CTR. */
    if(psa_cipher_set_iv(&ctx->operation, iv, 16) != PSA_SUCCESS) {
        _libssh2_psa_cipher_dtor(ctx);
        return -1;
    }
    return 0;
}

int _libssh2_cipher_crypt(_libssh2_cipher_ctx *ctx,
                          _libssh2_cipher_type(type),
                          int encrypt, unsigned char *block,
                          size_t blocksize, int firstlast)
{
    /* PSA will not encrypt in place, so the block makes a short round trip
       through scratch memory. Every packet passes through here, so the common
       small sizes stay on the stack. */
    unsigned char  stack_buffer[256];
    unsigned char *output  = stack_buffer;
    size_t         written = 0;
    psa_status_t   status;
    int            result  = -1;

    (void)type;
    (void)encrypt;
    (void)firstlast;

    if(blocksize + 16 > sizeof(stack_buffer)) {
        output = malloc(blocksize + 16);
        if(!output)
            return -1;
    }

    status = psa_cipher_update(&ctx->operation, block, blocksize,
                               output, blocksize + 16, &written);
    if(status == PSA_SUCCESS && written == blocksize) {
        memcpy(block, output, blocksize);
        result = 0;
    }

    if(output != stack_buffer)
        free(output);
    return result;
}

void _libssh2_psa_cipher_dtor(_libssh2_cipher_ctx *ctx)
{
    psa_cipher_abort(&ctx->operation);
    if(ctx->key != PSA_KEY_ID_NULL) {
        psa_destroy_key(ctx->key);
        ctx->key = PSA_KEY_ID_NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Big numbers                                                        */
/* ------------------------------------------------------------------ */

_libssh2_bn *_libssh2_psa_bn_init(void)
{
    _libssh2_bn *bn = calloc(1, sizeof(*bn));
    return bn;
}

void _libssh2_psa_bn_free(_libssh2_bn *bn)
{
    if(!bn)
        return;
    if(bn->bytes) {
        _libssh2_explicit_zero(bn->bytes, bn->len);
        free(bn->bytes);
    }
    free(bn);
}

static int bn_assign(_libssh2_bn *bn, const unsigned char *data, size_t len)
{
    size_t         offset = 0;
    unsigned char *copy;

    /* Keep the value canonical: no leading zero bytes. */
    while(offset < len && data[offset] == 0)
        offset++;

    copy = malloc(len - offset ? len - offset : 1);
    if(!copy)
        return -1;
    memcpy(copy, data + offset, len - offset);

    if(bn->bytes) {
        _libssh2_explicit_zero(bn->bytes, bn->len);
        free(bn->bytes);
    }
    bn->bytes = copy;
    bn->len   = len - offset;
    return 0;
}

int _libssh2_psa_bn_set_word(_libssh2_bn *bn, unsigned long word)
{
    unsigned char buffer[sizeof(unsigned long)];
    size_t        i;

    for(i = 0; i < sizeof(buffer); i++)
        buffer[i] = (unsigned char)(word >> ((sizeof(buffer) - 1 - i) * 8));

    return bn_assign(bn, buffer, sizeof(buffer));
}

int _libssh2_psa_bn_from_bin(_libssh2_bn *bn, size_t len,
                             const unsigned char *bin)
{
    return bn_assign(bn, bin, len);
}

int _libssh2_psa_bn_to_bin(const _libssh2_bn *bn, unsigned char *bin)
{
    if(bn->len)
        memcpy(bin, bn->bytes, bn->len);
    return 0;
}

size_t _libssh2_psa_bn_bytes(const _libssh2_bn *bn)
{
    return bn->len;
}

size_t _libssh2_psa_bn_bits(const _libssh2_bn *bn)
{
    unsigned char top;
    size_t        bits;

    if(!bn->len)
        return 0;

    top  = bn->bytes[0];
    bits = (bn->len - 1) * 8;
    while(top) {
        bits++;
        top >>= 1;
    }
    return bits;
}

/* ------------------------------------------------------------------ */
/* Diffie-Hellman: refused, see the header                            */
/* ------------------------------------------------------------------ */

void _libssh2_psa_dh_init(_libssh2_dh_ctx *dhctx)
{
    dhctx->unused = 0;
}

int _libssh2_psa_dh_key_pair(_libssh2_dh_ctx *dhctx, _libssh2_bn *public,
                             _libssh2_bn *g, _libssh2_bn *p, int group_order)
{
    (void)dhctx; (void)public; (void)g; (void)p; (void)group_order;
    return -1;
}

int _libssh2_psa_dh_secret(_libssh2_dh_ctx *dhctx, _libssh2_bn *secret,
                           _libssh2_bn *f, _libssh2_bn *p)
{
    (void)dhctx; (void)secret; (void)f; (void)p;
    return -1;
}

void _libssh2_psa_dh_dtor(_libssh2_dh_ctx *dhctx)
{
    (void)dhctx;
}

/* ------------------------------------------------------------------ */
/* DER helpers, for handing RSA public keys to PSA                    */
/* ------------------------------------------------------------------ */

/* Length of a DER INTEGER holding this unsigned big endian value, including
   the tag and length bytes. */
static size_t der_integer_len(const unsigned char *value, size_t len)
{
    size_t offset  = 0;
    size_t content;

    while(offset < len && value[offset] == 0)
        offset++;
    content = len - offset;
    if(!content)
        content = 1;                       /* The value zero is one byte */
    else if(value[offset] & 0x80)
        content++;                         /* Leading zero keeps it positive */

    if(content < 0x80)
        return 2 + content;
    if(content < 0x100)
        return 3 + content;
    return 4 + content;
}

static unsigned char *der_put_len(unsigned char *out, size_t len)
{
    if(len < 0x80) {
        *out++ = (unsigned char)len;
    }
    else if(len < 0x100) {
        *out++ = 0x81;
        *out++ = (unsigned char)len;
    }
    else {
        *out++ = 0x82;
        *out++ = (unsigned char)(len >> 8);
        *out++ = (unsigned char)len;
    }
    return out;
}

static unsigned char *der_put_integer(unsigned char *out,
                                      const unsigned char *value, size_t len)
{
    size_t offset = 0;
    size_t content;
    int    pad    = 0;

    while(offset < len && value[offset] == 0)
        offset++;
    content = len - offset;

    *out++ = 0x02;
    if(!content) {
        out    = der_put_len(out, 1);
        *out++ = 0x00;
        return out;
    }
    if(value[offset] & 0x80)
        pad = 1;

    out = der_put_len(out, content + pad);
    if(pad)
        *out++ = 0x00;
    memcpy(out, value + offset, content);
    return out + content;
}

/* ------------------------------------------------------------------ */
/* RSA                                                                */
/* ------------------------------------------------------------------ */

void _libssh2_psa_rsa_free(libssh2_rsa_ctx *ctx)
{
    if(!ctx)
        return;
    if(ctx->key != PSA_KEY_ID_NULL)
        psa_destroy_key(ctx->key);
    free(ctx);
}

int _libssh2_rsa_new(libssh2_rsa_ctx **rsa,
                     const unsigned char *edata, unsigned long elen,
                     const unsigned char *ndata, unsigned long nlen,
                     const unsigned char *ddata, unsigned long dlen,
                     const unsigned char *pdata, unsigned long plen,
                     const unsigned char *qdata, unsigned long qlen,
                     const unsigned char *e1data, unsigned long e1len,
                     const unsigned char *e2data, unsigned long e2len,
                     const unsigned char *coeffdata, unsigned long coefflen)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    libssh2_rsa_ctx     *ctx;
    unsigned char       *der;
    unsigned char       *write;
    size_t               body;
    size_t               total;

    /* Private key components are only ever passed when libssh2 loads a private
       key file, which this backend does not do. */
    (void)ddata; (void)dlen; (void)pdata; (void)plen; (void)qdata; (void)qlen;
    (void)e1data; (void)e1len; (void)e2data; (void)e2len;
    (void)coeffdata; (void)coefflen;

    if(ddata)
        return -1;

    ctx = calloc(1, sizeof(*ctx));
    if(!ctx)
        return -1;
    ctx->key = PSA_KEY_ID_NULL;

    body  = der_integer_len(ndata, nlen) + der_integer_len(edata, elen);
    total = 1 + (body < 0x80 ? 1 : (body < 0x100 ? 2 : 3)) + body;

    der = malloc(total);
    if(!der) {
        free(ctx);
        return -1;
    }

    write  = der;
    *write++ = 0x30;                        /* SEQUENCE */
    write  = der_put_len(write, body);
    write  = der_put_integer(write, ndata, nlen);
    write  = der_put_integer(write, edata, elen);

    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes,
                          PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_ANY_HASH));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_PUBLIC_KEY);

    if(psa_import_key(&attributes, der, (size_t)(write - der),
                      &ctx->key) != PSA_SUCCESS) {
        free(der);
        free(ctx);
        return -1;
    }
    free(der);

    ctx->bits = nlen * 8;
    *rsa      = ctx;
    return 0;
}

int _libssh2_rsa_sha2_verify(libssh2_rsa_ctx *rsa, size_t hash_len,
                             const unsigned char *sig, size_t sig_len,
                             const unsigned char *m, size_t m_len)
{
    unsigned char   hash[64];
    psa_algorithm_t hash_alg;
    size_t          written = 0;

    switch(hash_len) {
    case 32: hash_alg = PSA_ALG_SHA_256; break;
    case 48: hash_alg = PSA_ALG_SHA_384; break;
    case 64: hash_alg = PSA_ALG_SHA_512; break;
    default: return -1;
    }

    if(psa_hash_compute(hash_alg, m, m_len, hash, sizeof(hash),
                        &written) != PSA_SUCCESS)
        return -1;

    return psa_verify_hash(rsa->key, PSA_ALG_RSA_PKCS1V15_SIGN(hash_alg),
                           hash, written, sig, sig_len) == PSA_SUCCESS ? 0 : -1;
}

int _libssh2_rsa_sha2_sign(LIBSSH2_SESSION *session, libssh2_rsa_ctx *rsactx,
                           const unsigned char *hash, size_t hash_len,
                           unsigned char **signature, size_t *signature_len)
{
    (void)session; (void)rsactx; (void)hash; (void)hash_len;
    (void)signature; (void)signature_len;
    return -1;  /* No RSA private keys, see the header */
}

int _libssh2_rsa_new_private(libssh2_rsa_ctx **rsa, LIBSSH2_SESSION *session,
                             const char *filename,
                             unsigned const char *passphrase)
{
    (void)rsa; (void)session; (void)filename; (void)passphrase;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "Loading private keys is not supported");
}

int _libssh2_rsa_new_private_frommemory(libssh2_rsa_ctx **rsa,
                                        LIBSSH2_SESSION *session,
                                        const char *filedata,
                                        size_t filedata_len,
                                        unsigned const char *passphrase)
{
    (void)rsa; (void)filedata; (void)filedata_len; (void)passphrase;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "Loading private keys is not supported");
}

/* ------------------------------------------------------------------ */
/* ECDSA and ECDH over the NIST curves                                */
/* ------------------------------------------------------------------ */

void _libssh2_psa_ecdsa_free(libssh2_ecdsa_ctx *ctx)
{
    if(!ctx)
        return;
    if(ctx->key != PSA_KEY_ID_NULL)
        psa_destroy_key(ctx->key);
    free(ctx);
}

static size_t curve_bytes(libssh2_curve_type curve)
{
    switch(curve) {
    case LIBSSH2_EC_CURVE_NISTP256: return 32;
    case LIBSSH2_EC_CURVE_NISTP384: return 48;
    case LIBSSH2_EC_CURVE_NISTP521: return 66;
    default: return 0;
    }
}

static psa_algorithm_t curve_hash(libssh2_curve_type curve)
{
    switch(curve) {
    case LIBSSH2_EC_CURVE_NISTP256: return PSA_ALG_SHA_256;
    case LIBSSH2_EC_CURVE_NISTP384: return PSA_ALG_SHA_384;
    default: return PSA_ALG_SHA_512;
    }
}

libssh2_curve_type _libssh2_ecdsa_get_curve_type(libssh2_ecdsa_ctx *ctx)
{
    return ctx->curve;
}

int _libssh2_ecdsa_curve_type_from_name(const char *name,
                                        libssh2_curve_type *out_type)
{
    if(!name || strlen(name) != 19 || strncmp(name, "ecdsa-sha2-nistp", 16))
        return -1;

    if(!strcmp(name, "ecdsa-sha2-nistp256"))
        *out_type = LIBSSH2_EC_CURVE_NISTP256;
    else if(!strcmp(name, "ecdsa-sha2-nistp384"))
        *out_type = LIBSSH2_EC_CURVE_NISTP384;
    else if(!strcmp(name, "ecdsa-sha2-nistp521"))
        *out_type = LIBSSH2_EC_CURVE_NISTP521;
    else
        return -1;

    return 0;
}

int _libssh2_ecdsa_curve_name_with_octal_new(libssh2_ecdsa_ctx **out_ctx,
                                             const unsigned char *k,
                                             size_t k_len,
                                             libssh2_curve_type curve)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    libssh2_ecdsa_ctx   *ctx;
    size_t               bits = (curve == LIBSSH2_EC_CURVE_NISTP521) ? 521
                                                                     : (size_t)curve;

    ctx = calloc(1, sizeof(*ctx));
    if(!ctx)
        return -1;
    ctx->key   = PSA_KEY_ID_NULL;
    ctx->curve = curve;

    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(curve_hash(curve)));
    psa_set_key_type(&attributes,
                     PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, bits);

    if(psa_import_key(&attributes, k, k_len, &ctx->key) != PSA_SUCCESS) {
        free(ctx);
        return -1;
    }

    *out_ctx = ctx;
    return 0;
}

/* SSH sends r and s as separate integers with the leading zeros stripped; PSA
   wants them fixed width and concatenated. */
static int pad_signature(libssh2_curve_type curve,
                         const unsigned char *r, size_t r_len,
                         const unsigned char *s, size_t s_len,
                         unsigned char *out, size_t *out_len)
{
    size_t width = curve_bytes(curve);

    if(!width)
        return -1;

    while(r_len > width && *r == 0) {
        r++;
        r_len--;
    }
    while(s_len > width && *s == 0) {
        s++;
        s_len--;
    }
    if(r_len > width || s_len > width)
        return -1;

    memset(out, 0, width * 2);
    memcpy(out + width - r_len, r, r_len);
    memcpy(out + width * 2 - s_len, s, s_len);
    *out_len = width * 2;
    return 0;
}

int _libssh2_ecdsa_verify(libssh2_ecdsa_ctx *ctx,
                          const unsigned char *r, size_t r_len,
                          const unsigned char *s, size_t s_len,
                          const unsigned char *m, size_t m_len)
{
    unsigned char signature[132];
    unsigned char hash[64];
    size_t        signature_len = 0;
    size_t        hash_len      = 0;

    if(pad_signature(ctx->curve, r, r_len, s, s_len,
                     signature, &signature_len))
        return -1;

    if(psa_hash_compute(curve_hash(ctx->curve), m, m_len, hash, sizeof(hash),
                        &hash_len) != PSA_SUCCESS)
        return -1;

    return psa_verify_hash(ctx->key, PSA_ALG_ECDSA(curve_hash(ctx->curve)),
                           hash, hash_len, signature,
                           signature_len) == PSA_SUCCESS ? 0 : -1;
}

int _libssh2_ecdsa_create_key(LIBSSH2_SESSION *session,
                              _libssh2_ec_key **out_private_key,
                              unsigned char **out_public_key_octal,
                              size_t *out_public_key_octal_len,
                              libssh2_curve_type curve)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    libssh2_ecdsa_ctx   *ctx;
    unsigned char       *octal;
    size_t               octal_len = 0;
    size_t               bits      = (curve == LIBSSH2_EC_CURVE_NISTP521)
                                     ? 521 : (size_t)curve;

    ctx = calloc(1, sizeof(*ctx));
    if(!ctx)
        return -1;
    ctx->key        = PSA_KEY_ID_NULL;
    ctx->curve      = curve;
    ctx->is_private = 1;

    psa_set_key_usage_flags(&attributes,
                            PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_SIGN_HASH |
                            PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
    psa_set_key_type(&attributes,
                     PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, bits);

    if(psa_generate_key(&attributes, &ctx->key) != PSA_SUCCESS) {
        free(ctx);
        return -1;
    }

    octal = LIBSSH2_ALLOC(session, EC_MAX_POINT_LEN);
    if(!octal) {
        _libssh2_psa_ecdsa_free(ctx);
        return -1;
    }
    if(psa_export_public_key(ctx->key, octal, EC_MAX_POINT_LEN,
                             &octal_len) != PSA_SUCCESS) {
        LIBSSH2_FREE(session, octal);
        _libssh2_psa_ecdsa_free(ctx);
        return -1;
    }

    *out_private_key          = ctx;
    *out_public_key_octal     = octal;
    *out_public_key_octal_len = octal_len;
    return 0;
}

int _libssh2_ecdh_gen_k(_libssh2_bn **k, _libssh2_ec_key *private_key,
                        const unsigned char *server_public_key,
                        size_t server_public_key_len)
{
    unsigned char secret[66];
    size_t        secret_len = 0;

    if(!k || !*k)
        return -1;

    if(psa_raw_key_agreement(PSA_ALG_ECDH, private_key->key,
                             server_public_key, server_public_key_len,
                             secret, sizeof(secret),
                             &secret_len) != PSA_SUCCESS)
        return -1;

    if(_libssh2_psa_bn_from_bin(*k, secret_len, secret)) {
        _libssh2_explicit_zero(secret, sizeof(secret));
        return -1;
    }
    _libssh2_explicit_zero(secret, sizeof(secret));
    return 0;
}

int _libssh2_ecdsa_sign(LIBSSH2_SESSION *session, libssh2_ecdsa_ctx *ctx,
                        const unsigned char *hash, size_t hash_len,
                        unsigned char **signature, size_t *signature_len)
{
    unsigned char  raw[132];
    size_t         raw_len = 0;
    size_t         width   = curve_bytes(ctx->curve);
    unsigned char *out;
    size_t         out_len;
    unsigned char *write;

    if(!width)
        return -1;

    if(psa_sign_hash(ctx->key, PSA_ALG_ECDSA(curve_hash(ctx->curve)),
                     hash, hash_len, raw, sizeof(raw),
                     &raw_len) != PSA_SUCCESS)
        return -1;

    /* SSH wants the pair as two mpints inside one string. */
    out_len = 4 + width + 1 + 4 + width + 1;
    out     = LIBSSH2_ALLOC(session, out_len);
    if(!out)
        return -1;

    write = out;
    _libssh2_store_bignum2_bytes(&write, raw, width);
    _libssh2_store_bignum2_bytes(&write, raw + width, width);

    *signature     = out;
    *signature_len = (size_t)(write - out);
    return 0;
}

int _libssh2_ecdsa_new_private(libssh2_ecdsa_ctx **ctx,
                               LIBSSH2_SESSION *session, const char *filename,
                               unsigned const char *passphrase)
{
    (void)ctx; (void)filename; (void)passphrase;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "Loading private keys is not supported");
}

int _libssh2_ecdsa_new_private_frommemory(libssh2_ecdsa_ctx **ctx,
                                          LIBSSH2_SESSION *session,
                                          const char *filedata,
                                          size_t filedata_len,
                                          unsigned const char *passphrase)
{
    (void)ctx; (void)filedata; (void)filedata_len; (void)passphrase;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "Loading private keys is not supported");
}

int _libssh2_ecdsa_new_private_sk(libssh2_ecdsa_ctx **ctx,
                                  unsigned char *flags,
                                  const char **application,
                                  const unsigned char **key_handle,
                                  size_t *handle_len,
                                  LIBSSH2_SESSION *session,
                                  const char *filename,
                                  unsigned const char *passphrase)
{
    (void)ctx; (void)flags; (void)application; (void)key_handle;
    (void)handle_len; (void)filename; (void)passphrase;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "Security keys are not supported");
}

int _libssh2_ecdsa_new_private_frommemory_sk(libssh2_ecdsa_ctx **ctx,
                                             unsigned char *flags,
                                             const char **application,
                                             const unsigned char **key_handle,
                                             size_t *handle_len,
                                             LIBSSH2_SESSION *session,
                                             const char *filedata,
                                             size_t filedata_len,
                                             unsigned const char *passphrase)
{
    (void)ctx; (void)flags; (void)application; (void)key_handle;
    (void)handle_len; (void)filedata; (void)filedata_len; (void)passphrase;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "Security keys are not supported");
}

/* ------------------------------------------------------------------ */
/* Ed25519 and X25519, through TweetNaCl                              */
/* ------------------------------------------------------------------ */

void _libssh2_psa_ed25519_free(libssh2_ed25519_ctx *ctx)
{
    if(!ctx)
        return;
    _libssh2_explicit_zero(ctx->private_key, sizeof(ctx->private_key));
    free(ctx);
}

int _libssh2_curve25519_new(LIBSSH2_SESSION *session, uint8_t **out_public_key,
                            uint8_t **out_private_key)
{
    uint8_t *private_key = LIBSSH2_ALLOC(session, LIBSSH2_ED25519_KEY_LEN);
    uint8_t *public_key  = LIBSSH2_ALLOC(session, LIBSSH2_ED25519_KEY_LEN);

    if(!private_key || !public_key) {
        if(private_key)
            LIBSSH2_FREE(session, private_key);
        if(public_key)
            LIBSSH2_FREE(session, public_key);
        return -1;
    }

    if(_libssh2_psa_random(private_key, LIBSSH2_ED25519_KEY_LEN)) {
        LIBSSH2_FREE(session, private_key);
        LIBSSH2_FREE(session, public_key);
        return -1;
    }
    crypto_scalarmult_base(public_key, private_key);

    if(out_private_key)
        *out_private_key = private_key;
    else
        LIBSSH2_FREE(session, private_key);

    if(out_public_key)
        *out_public_key = public_key;
    else
        LIBSSH2_FREE(session, public_key);

    return 0;
}

int _libssh2_curve25519_gen_k(_libssh2_bn **k,
                              uint8_t private_key[LIBSSH2_ED25519_KEY_LEN],
                              uint8_t server_public_key[LIBSSH2_ED25519_KEY_LEN])
{
    uint8_t secret[LIBSSH2_ED25519_KEY_LEN];

    if(!k || !*k)
        return -1;

    if(crypto_scalarmult(secret, private_key, server_public_key) != 0)
        return -1;

    if(_libssh2_psa_bn_from_bin(*k, sizeof(secret), secret)) {
        _libssh2_explicit_zero(secret, sizeof(secret));
        return -1;
    }
    _libssh2_explicit_zero(secret, sizeof(secret));
    return 0;
}

int _libssh2_ed25519_new_public(libssh2_ed25519_ctx **out_ctx,
                                LIBSSH2_SESSION *session,
                                const unsigned char *raw_pub_key,
                                const size_t key_len)
{
    libssh2_ed25519_ctx *ctx;

    (void)session;

    if(key_len != LIBSSH2_ED25519_KEY_LEN)
        return -1;

    ctx = calloc(1, sizeof(*ctx));
    if(!ctx)
        return -1;
    memcpy(ctx->public_key, raw_pub_key, LIBSSH2_ED25519_KEY_LEN);

    *out_ctx = ctx;
    return 0;
}

int _libssh2_ed25519_verify(libssh2_ed25519_ctx *ctx, const uint8_t *s,
                            size_t s_len, const uint8_t *m, size_t m_len)
{
    unsigned char     *signed_message;
    unsigned char     *recovered;
    unsigned long long recovered_len = 0;
    int                result;

    if(s_len != LIBSSH2_ED25519_SIG_LEN)
        return -1;

    /* TweetNaCl only verifies a signature attached to its message, so the two
       are stitched together and the copy it hands back is discarded. */
    signed_message = malloc(s_len + m_len);
    recovered      = malloc(s_len + m_len);
    if(!signed_message || !recovered) {
        free(signed_message);
        free(recovered);
        return -1;
    }

    memcpy(signed_message, s, s_len);
    memcpy(signed_message + s_len, m, m_len);

    result = crypto_sign_open(recovered, &recovered_len, signed_message,
                              (unsigned long long)(s_len + m_len),
                              ctx->public_key);

    free(signed_message);
    free(recovered);
    return result == 0 ? 0 : -1;
}

int _libssh2_ed25519_sign(libssh2_ed25519_ctx *ctx, LIBSSH2_SESSION *session,
                          uint8_t **out_sig, size_t *out_sig_len,
                          const uint8_t *message, size_t message_len)
{
    unsigned char     *signed_message;
    unsigned long long signed_len = 0;
    uint8_t           *signature;

    if(!ctx->has_private)
        return -1;

    signed_message = malloc(message_len + LIBSSH2_ED25519_SIG_LEN);
    if(!signed_message)
        return -1;

    if(crypto_sign(signed_message, &signed_len, message,
                   (unsigned long long)message_len, ctx->private_key) != 0) {
        free(signed_message);
        return -1;
    }

    signature = LIBSSH2_ALLOC(session, LIBSSH2_ED25519_SIG_LEN);
    if(!signature) {
        free(signed_message);
        return -1;
    }
    memcpy(signature, signed_message, LIBSSH2_ED25519_SIG_LEN);
    free(signed_message);

    *out_sig     = signature;
    *out_sig_len = LIBSSH2_ED25519_SIG_LEN;
    return 0;
}

int _libssh2_ed25519_new_private(libssh2_ed25519_ctx **ctx,
                                 LIBSSH2_SESSION *session,
                                 const char *filename, const uint8_t *passphrase)
{
    (void)ctx; (void)filename; (void)passphrase;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "Loading private keys is not supported");
}

int _libssh2_ed25519_new_private_frommemory(libssh2_ed25519_ctx **ctx,
                                            LIBSSH2_SESSION *session,
                                            const char *filedata,
                                            size_t filedata_len,
                                            unsigned const char *passphrase)
{
    (void)ctx; (void)filedata; (void)filedata_len; (void)passphrase;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "Loading private keys is not supported");
}

int _libssh2_ed25519_new_private_sk(libssh2_ed25519_ctx **ctx,
                                    unsigned char *flags,
                                    const char **application,
                                    const unsigned char **key_handle,
                                    size_t *handle_len,
                                    LIBSSH2_SESSION *session,
                                    const char *filename,
                                    const uint8_t *passphrase)
{
    (void)ctx; (void)flags; (void)application; (void)key_handle;
    (void)handle_len; (void)filename; (void)passphrase;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "Security keys are not supported");
}

int _libssh2_ed25519_new_private_frommemory_sk(libssh2_ed25519_ctx **ctx,
                                               unsigned char *flags,
                                               const char **application,
                                               const unsigned char **key_handle,
                                               size_t *handle_len,
                                               LIBSSH2_SESSION *session,
                                               const char *filedata,
                                               size_t filedata_len,
                                               unsigned const char *passphrase)
{
    (void)ctx; (void)flags; (void)application; (void)key_handle;
    (void)handle_len; (void)filedata; (void)filedata_len; (void)passphrase;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "Security keys are not supported");
}

/* ------------------------------------------------------------------ */
/* Private key files                                                  */
/* ------------------------------------------------------------------ */

int _libssh2_pub_priv_keyfile(LIBSSH2_SESSION *session,
                              unsigned char **method, size_t *method_len,
                              unsigned char **pubkeydata,
                              size_t *pubkeydata_len,
                              const char *privatekey, const char *passphrase)
{
    (void)method; (void)method_len; (void)pubkeydata; (void)pubkeydata_len;
    (void)privatekey; (void)passphrase;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "Loading private keys is not supported; the badge "
                          "signs with its own key");
}

int _libssh2_pub_priv_keyfilememory(LIBSSH2_SESSION *session,
                                    unsigned char **method, size_t *method_len,
                                    unsigned char **pubkeydata,
                                    size_t *pubkeydata_len,
                                    const char *privatekeydata,
                                    size_t privatekeydata_len,
                                    const char *passphrase)
{
    (void)method; (void)method_len; (void)pubkeydata; (void)pubkeydata_len;
    (void)privatekeydata; (void)privatekeydata_len; (void)passphrase;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "Loading private keys is not supported; the badge "
                          "signs with its own key");
}

int _libssh2_sk_pub_keyfilememory(LIBSSH2_SESSION *session,
                                  unsigned char **method, size_t *method_len,
                                  unsigned char **pubkeydata,
                                  size_t *pubkeydata_len,
                                  int *algorithm, unsigned char *flags,
                                  const char **application,
                                  const unsigned char **key_handle,
                                  size_t *handle_len,
                                  const char *privatekeydata,
                                  size_t privatekeydata_len,
                                  const char *passphrase)
{
    (void)method; (void)method_len; (void)pubkeydata; (void)pubkeydata_len;
    (void)algorithm; (void)flags; (void)application; (void)key_handle;
    (void)handle_len; (void)privatekeydata; (void)privatekeydata_len;
    (void)passphrase;
    return _libssh2_error(session, LIBSSH2_ERROR_METHOD_NOT_SUPPORTED,
                          "Security keys are not supported");
}

const char *
_libssh2_supported_key_sign_algorithms(LIBSSH2_SESSION *session,
                                       unsigned char *key_method,
                                       size_t key_method_len)
{
    (void)session;

    /* RFC 8332: an "ssh-rsa" key may sign with SHA-2 instead of SHA-1. */
    if(key_method_len == 7 && !memcmp(key_method, "ssh-rsa", 7))
        return "rsa-sha2-512,rsa-sha2-256";

    return NULL;
}
