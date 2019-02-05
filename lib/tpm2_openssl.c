//**********************************************************************;
// Copyright (c) 2017, Intel Corporation
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//**********************************************************************

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <openssl/aes.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

#include <tss2/tss2_mu.h>
#include <tss2/tss2_sys.h>

#include "files.h"
#include "log.h"
#include "tpm2_alg_util.h"
#include "tpm2_kdfa.h"
#include "tpm2_openssl.h"
#include "tpm2_util.h"

int tpm2_openssl_halgid_from_tpmhalg(TPMI_ALG_HASH algorithm) {

    switch (algorithm) {
    case TPM2_ALG_SHA1:
        return NID_sha1;
    case TPM2_ALG_SHA256:
        return NID_sha256;
    case TPM2_ALG_SHA384:
        return NID_sha384;
    case TPM2_ALG_SHA512:
        return NID_sha512;
    default:
        return NID_sha256;
    }
    /* no return, not possible */
}

const EVP_MD *tpm2_openssl_halg_from_tpmhalg(TPMI_ALG_HASH algorithm) {

    switch (algorithm) {
    case TPM2_ALG_SHA1:
        return EVP_sha1();
    case TPM2_ALG_SHA256:
        return EVP_sha256();
    case TPM2_ALG_SHA384:
        return EVP_sha384();
    case TPM2_ALG_SHA512:
        return EVP_sha512();
    default:
        return NULL;
    }
    /* no return, not possible */
}

#if defined(LIB_TPM2_OPENSSL_OPENSSL_PRE11)
int RSA_set0_key(RSA *r, BIGNUM *n, BIGNUM *e, BIGNUM *d) {

    if ((r->n == NULL && n == NULL) || (r->e == NULL && e == NULL)) {
        return 0;
    }

    if (n != NULL) {
        BN_free(r->n);
        r->n = n;
    }

    if (e != NULL) {
        BN_free(r->e);
        r->e = e;
    }

    if (d != NULL) {
        BN_free(r->d);
        r->d = d;
    }

    return 1;
}
#endif

static inline const char *get_openssl_err(void) {
    return ERR_error_string(ERR_get_error(), NULL);
}


bool tpm2_openssl_hash_compute_data(TPMI_ALG_HASH halg,
        BYTE *buffer, UINT16 length, TPM2B_DIGEST *digest) {

    bool result = false;

    const EVP_MD *md = tpm2_openssl_halg_from_tpmhalg(halg);
    if (!md) {
        return false;
    }

    EVP_MD_CTX *mdctx = EVP_MD_CTX_create();
    if (!mdctx) {
        LOG_ERR("%s", get_openssl_err());
        return false;
    }

    int rc = EVP_DigestInit_ex(mdctx, md, NULL);
    if (!rc) {
        LOG_ERR("%s", get_openssl_err());
        goto out;
    }

    rc = EVP_DigestUpdate(mdctx, buffer, length);
    if (!rc) {
        LOG_ERR("%s", get_openssl_err());
        goto out;
    }

    unsigned size = EVP_MD_size(md);
    rc = EVP_DigestFinal_ex(mdctx, digest->buffer, &size);
    if (!rc) {
        LOG_ERR("%s", get_openssl_err());
        goto out;
    }

    digest->size = size;

    result = true;

out:
    EVP_MD_CTX_destroy(mdctx);
    return result;
}

bool tpm2_openssl_hash_pcr_values(TPMI_ALG_HASH halg,
        TPML_DIGEST *digests, TPM2B_DIGEST *digest) {

    bool result = false;

    const EVP_MD *md = tpm2_openssl_halg_from_tpmhalg(halg);
    if (!md) {
        return false;
    }

    EVP_MD_CTX *mdctx = EVP_MD_CTX_create();
    if (!mdctx) {
        LOG_ERR("%s", get_openssl_err());
        return false;
    }

    int rc = EVP_DigestInit_ex(mdctx, md, NULL);
    if (!rc) {
        LOG_ERR("%s", get_openssl_err());
        goto out;
    }

    size_t i;
    for (i=0; i < digests->count; i++) {

        TPM2B_DIGEST *b = &digests->digests[i];
        rc = EVP_DigestUpdate(mdctx, b->buffer, b->size);
        if (!rc) {
            LOG_ERR("%s", get_openssl_err());
            goto out;
        }
    }

    unsigned size = EVP_MD_size(EVP_sha256());

    rc = EVP_DigestFinal_ex(mdctx, digest->buffer, &size);
    if (!rc) {
        LOG_ERR("%s", get_openssl_err());
        goto out;
    }

    digest->size = size;

    result = true;

out:
    EVP_MD_CTX_destroy(mdctx);
    return result;
}

// show all PCR banks according to g_pcrSelection & g_pcrs->
bool tpm2_openssl_hash_pcr_banks(TPMI_ALG_HASH hashAlg, 
                TPML_PCR_SELECTION *pcrSelect, 
                tpm2_pcrs *pcrs, TPM2B_DIGEST *digest) {

    UINT32 vi = 0, di = 0, i;
    bool result = false;

    const EVP_MD *md = tpm2_openssl_halg_from_tpmhalg(hashAlg);
    if (!md) {
        return false;
    }

    EVP_MD_CTX *mdctx = EVP_MD_CTX_create();
    if (!mdctx) {
        LOG_ERR("%s", get_openssl_err());
        return false;
    }

    int rc = EVP_DigestInit_ex(mdctx, md, NULL);
    if (!rc) {
        LOG_ERR("%s", get_openssl_err());
        goto out;
    }

    // Loop through all PCR/hash banks 
    for (i = 0; i < pcrSelect->count; i++) {

        // Loop through all PCRs in this bank
        UINT8 pcr_id;
        for (pcr_id = 0; pcr_id < pcrSelect->pcrSelections[i].sizeofSelect * 8; pcr_id++) {
            if (!tpm2_util_is_pcr_select_bit_set(&pcrSelect->pcrSelections[i],
                    pcr_id)) {
                // skip non-selected banks
                continue;
            }
            if (vi >= pcrs->count || di >= pcrs->pcr_values[vi].count) {
                LOG_ERR("Something wrong, trying to print but nothing more");
                goto out;
            }

            // Update running digest (to compare with quote)
            TPM2B_DIGEST *b = &pcrs->pcr_values[vi].digests[di];
            rc = EVP_DigestUpdate(mdctx, b->buffer, b->size);
            if (!rc) {
                LOG_ERR("%s", get_openssl_err());
                goto out;
            }

            if (++di < pcrs->pcr_values[vi].count) {
                continue;
            }

            di = 0;
            if (++vi < pcrs->count) {
                continue;
            }
        }
    }

    // Finalize running digest
    unsigned size = EVP_MD_size(md);
    rc = EVP_DigestFinal_ex(mdctx, digest->buffer, &size);
    if (!rc) {
        LOG_ERR("%s", get_openssl_err());
        goto out;
    }

    digest->size = size;

    result = true;

out:
    EVP_MD_CTX_destroy(mdctx);
    return result;
}

HMAC_CTX *tpm2_openssl_hmac_new() {
    HMAC_CTX *ctx;
#if defined(LIB_TPM2_OPENSSL_OPENSSL_PRE11)
    ctx = malloc(sizeof(*ctx));
#else
    ctx = HMAC_CTX_new();
#endif
    if (!ctx)
        return NULL;

#if defined(LIB_TPM2_OPENSSL_OPENSSL_PRE11)
    HMAC_CTX_init(ctx);
#endif

    return ctx;
}

void tpm2_openssl_hmac_free(HMAC_CTX *ctx) {
#if defined(LIB_TPM2_OPENSSL_OPENSSL_PRE11)
    HMAC_CTX_cleanup(ctx);
    free(ctx);
#else
    HMAC_CTX_free(ctx);
#endif
}

EVP_CIPHER_CTX *tpm2_openssl_cipher_new(void) {
    EVP_CIPHER_CTX *ctx;
#if defined(LIB_TPM2_OPENSSL_OPENSSL_PRE11)
    ctx = malloc(sizeof(*ctx));
#else
    ctx = EVP_CIPHER_CTX_new();
#endif
    if (!ctx)
        return NULL;

#if defined(LIB_TPM2_OPENSSL_OPENSSL_PRE11)
    EVP_CIPHER_CTX_init(ctx);
#endif

    return ctx;
}

void tpm2_openssl_cipher_free(EVP_CIPHER_CTX *ctx) {
#if defined(LIB_TPM2_OPENSSL_OPENSSL_PRE11)
    EVP_CIPHER_CTX_cleanup(ctx);
    free(ctx);
#else
    EVP_CIPHER_CTX_free(ctx);
#endif
}

digester tpm2_openssl_halg_to_digester(TPMI_ALG_HASH halg) {

    switch(halg) {
    case TPM2_ALG_SHA1:
        return SHA1;
    case TPM2_ALG_SHA256:
        return SHA256;
    case TPM2_ALG_SHA384:
        return SHA384;
    case TPM2_ALG_SHA512:
        return SHA512;
    /* no default */
    }

    return NULL;
}

/*
 * Per man openssl(1), handle the following --passin formats:
 *     pass:password
 *             the actual password is password. Since the password is visible to utilities (like 'ps' under Unix) this form should only be used where security is not
 *             important.
 *
 *   env:var   obtain the password from the environment variable var. Since the environment of other processes is visible on certain platforms (e.g. ps under certain
 *             Unix OSes) this option should be used with caution.
 *
 *   file:pathname
 *             the first line of pathname is the password. If the same pathname argument is supplied to -passin and -passout arguments then the first line will be used
 *             for the input password and the next line for the output password. pathname need not refer to a regular file: it could for example refer to a device or
 *             named pipe.
 *
 *   fd:number read the password from the file descriptor number. This can be used to send the data via a pipe for example.
 *
 *   stdin     read the password from standard input.
 *
 */

typedef bool (*pfn_ossl_pw_handler)(const char *passin, char **pass);

static bool do_pass(const char *passin, char **pass) {

    char *tmp = strdup(passin);
    if (!tmp) {
        LOG_ERR("oom");
        return false;
    }

    *pass = tmp;
    return true;
}

static bool do_env(const char *envvar, char **pass) {

    char *tmp = getenv(envvar);
    if (!tmp) {
        LOG_ERR("Environment variable \"%s\" not found", envvar);
        return false;
    }

    tmp = strdup(tmp);
    if (!tmp) {
        LOG_ERR("oom");
        return false;
    }

    *pass = tmp;
    return true;
}

static bool do_open_file(FILE *f, const char *path, char **pass) {

    bool rc = false;

    unsigned long file_size = 0;
    bool result = files_get_file_size(f, &file_size, path);
    if (!result) {
        goto out;
    }

    if (file_size + 1 <= file_size) {
        LOG_ERR("overflow: file_size too large");
        goto out;
    }

    char *tmp = calloc(sizeof(char), file_size + 1);
    if (!tmp) {
        LOG_ERR("oom");
        goto out;
    }

    result = files_read_bytes(f, (UINT8 *)tmp, file_size);
    if (!result) {
        goto out;
    }

    *pass = tmp;

    rc = true;

out:
    fclose(f);

    return rc;
}

static bool do_file(const char *path, char **pass) {

    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_ERR("could not open file \"%s\" error: %s",
                path, strerror(errno));
        return false;
    }

    return do_open_file(f, path, pass);
}

static bool do_fd(const char *passin, char **pass) {

    char *end_ptr = NULL;
    int fd = strtoul(passin, &end_ptr, 0);
    if (passin[0] != '\0' && end_ptr[0] != '\0') {
        LOG_ERR("Invalid fd, got: \"%s\"", passin);
        return false;
    }

    FILE *f = fdopen(fd, "rb");
    if (!f) {
        LOG_ERR("could not open fd \"%d\" error: %s",
                fd, strerror(errno));
        return false;
    }

    return do_open_file(f, "fd", pass);
}

static bool do_stdin(const char *passin, char **pass) {

    UNUSED(passin);

    void *buf = calloc(sizeof(BYTE), UINT16_MAX + 1);
    if (!buf) {
        LOG_ERR("oom");
        return false;
    }

    UINT16 size = UINT16_MAX;

    bool result = files_load_bytes_from_buffer_or_file_or_stdin(NULL,NULL, &size,
        buf);
    if (!result) {
        free(buf);
        return false;
    }

    *pass = buf;
    return true;
}

static bool handle_ossl_pass(const char *passin, char **pass) {

    pfn_ossl_pw_handler pfn = NULL;

    if (!passin) {
        *pass = NULL;
        return true;
    }

    if (!strncmp("pass:", passin, 5)) {
        passin += 5;
        pfn = do_pass;
    } else if (!strncmp("env:", passin, 4)) {
        pfn = do_env;
        passin += 4;
    } else if (!strncmp("file:", passin, 5)) {
        pfn = do_file;
        passin += 5;
    } else if (!strncmp("fd:", passin, 3)) {
        pfn = do_fd;
        passin += 3;
    } else if (!strcmp("stdin", passin)) {
        pfn = do_stdin;
    } else {
        LOG_ERR("Unknown OSSL style password argument, got: \"%s\"", passin);
        return false;
    }

    return pfn(passin, pass);
}

static bool load_public_RSA_from_key(RSA *k, TPM2B_PUBLIC *pub) {

    TPMT_PUBLIC *pt = &pub->publicArea;
    pt->type = TPM2_ALG_RSA;

    TPMS_RSA_PARMS *rdetail = &pub->publicArea.parameters.rsaDetail;
    rdetail->scheme.scheme = TPM2_ALG_NULL;
    rdetail->symmetric.algorithm = TPM2_ALG_NULL;
    rdetail->scheme.details.anySig.hashAlg = TPM2_ALG_NULL;

    /* NULL out sym details */
    TPMT_SYM_DEF_OBJECT *sym = &rdetail->symmetric;
    sym->algorithm = TPM2_ALG_NULL;
    sym->keyBits.sym = 0;
    sym->mode.sym = TPM2_ALG_NULL;

    const BIGNUM *n; /* modulus */
    const BIGNUM *e; /* public key exponent */

#if defined(LIB_TPM2_OPENSSL_OPENSSL_PRE11)
    n = k->n;
    e = k->e;
#else
    RSA_get0_key(k, &n, &e, NULL);
#endif

    /*
     * The size of the modulus is the key size in RSA, store this as the
     * keyBits in the RSA details.
     */
    rdetail->keyBits = BN_num_bytes(n) * 8;
    switch (rdetail->keyBits) {
    case 1024: /* falls-through */
    case 2048: /* falls-through */
    case 4096: /* falls-through */
        break;
    default:
        LOG_ERR("RSA key-size %u is not supported", rdetail->keyBits);
        return false;
    }

    /* copy the modulus to the unique RSA field */
    pt->unique.rsa.size = rdetail->keyBits/8;
    int success = BN_bn2bin(n, pt->unique.rsa.buffer);
    if (!success) {
        LOG_ERR("Could not copy public modulus N");
        return false;
    }

    /*Make sure that we can fit the exponent into a UINT32 */
    unsigned e_size = BN_num_bytes(e);
    if (e_size > sizeof(rdetail->exponent)) {
        LOG_ERR("Exponent is too big. Got %d expected less than or equal to %zu",
                e_size, sizeof(rdetail->exponent));
        return false;
    }

    /*
     * Copy the exponent into the field.
     * Returns 1 on success false on error.
     */
    return BN_bn2bin(e, (unsigned char *)&rdetail->exponent);
}

RSA *tpm2_openssl_get_public_RSA_from_pem(FILE *f, const char *path) {

    /*
     * Public PEM files appear in two formats:
     * 1. PEM format, read with PEM_read_RSA_PUBKEY
     * 2. PKCS#1 format, read with PEM_read_RSAPublicKey
     *
     * See:
     *  - https://stackoverflow.com/questions/7818117/why-i-cant-read-openssl-generated-rsa-pub-key-with-pem-read-rsapublickey
     */
    RSA *pub = PEM_read_RSA_PUBKEY(f, NULL, NULL, NULL);
    if (!pub) {
        pub = PEM_read_RSAPublicKey(f, NULL, NULL, NULL);
    }

    if (!pub) {
         ERR_print_errors_fp (stderr);
         LOG_ERR("Reading public PEM file \"%s\" failed", path);
         return NULL;
    }

    return pub;
}

static bool load_public_RSA_from_pem(FILE *f, const char *path, TPM2B_PUBLIC *pub) {

    /*
     * Public PEM files appear in two formats:
     * 1. PEM format, read with PEM_read_RSA_PUBKEY
     * 2. PKCS#1 format, read with PEM_read_RSAPublicKey
     *
     * See:
     *  - https://stackoverflow.com/questions/7818117/why-i-cant-read-openssl-generated-rsa-pub-key-with-pem-read-rsapublickey
     */
    RSA *k = tpm2_openssl_get_public_RSA_from_pem(f, path);
    if (!k) {
        /* tpm2_openssl_get_public_RSA_from_pem() should already log errors */
        return false;
    }

    bool result = load_public_RSA_from_key(k, pub);

    RSA_free(k);

    return result;
}

static const struct {
    TPMI_ECC_CURVE curve;
    int nid;
} nid_curve_map[] = {
    { TPM2_ECC_NIST_P192, NID_X9_62_prime192v1 },
    { TPM2_ECC_NIST_P224, NID_secp224r1        },
    { TPM2_ECC_NIST_P256, NID_X9_62_prime256v1 },
    { TPM2_ECC_NIST_P384, NID_secp384r1        },
    { TPM2_ECC_NIST_P521, NID_secp521r1        }
    /*
     * XXX
     * See if it's possible to support the other curves, I didn't see the
     * mapping in OSSL:
     *  - TPM2_ECC_BN_P256
     *  - TPM2_ECC_BN_P638
     *  - TPM2_ECC_SM2_P256
     */
};

/**
 * Maps an OSSL nid as defined obj_mac.h to a TPM2 ECC curve id.
 * @param nid
 *  The nid to map.
 * @return
 *  A valid TPM2_ECC_* or TPM2_ALG_ERROR on error.
 */
static TPMI_ECC_CURVE ossl_nid_to_curve(int nid) {

    unsigned i;
    for (i=0; i < ARRAY_LEN(nid_curve_map); i++) {
        TPMI_ECC_CURVE c = nid_curve_map[i].curve;
        int n = nid_curve_map[i].nid;

        if (n == nid) {
            return c;
        }
    }

    LOG_ERR("Cannot map nid \"%d\" to TPM ECC curve", nid);
    return TPM2_ALG_ERROR;
}

int tpm2_ossl_curve_to_nid(TPMI_ECC_CURVE curve) {

    unsigned i;
    for (i=0; i < ARRAY_LEN(nid_curve_map); i++) {
        TPMI_ECC_CURVE c = nid_curve_map[i].curve;
        int n = nid_curve_map[i].nid;

        if (c == curve) {
            return n;
        }
    }

    LOG_ERR("Cannot map TPM ECC curve \"%u\" to nid", curve);
    return -1;
}

static bool load_public_ECC_from_key(EC_KEY *k, TPM2B_PUBLIC *pub) {

    bool result = false;

    BIGNUM *y = BN_new();
    BIGNUM *x = BN_new();
    if (!x || !y) {
        LOG_ERR("oom");
        goto out;
    }

    /*
     * Set the algorithm type
     */
    pub->publicArea.type = TPM2_ALG_ECC;

    /*
     * Get the curve type
     */
    const EC_GROUP *group = EC_KEY_get0_group(k);
    int nid = EC_GROUP_get_curve_name(group);

    TPMS_ECC_PARMS *pp = &pub->publicArea.parameters.eccDetail;
    TPM2_ECC_CURVE curve_id = ossl_nid_to_curve(nid); // Not sure what lines up with NIST 256...
    if (curve_id == TPM2_ALG_ERROR) {
        goto out;
    }

    pp->curveID = curve_id;

    /*
     * Set the unique data to the public key.
     */
    const EC_POINT *point = EC_KEY_get0_public_key(k);

    int ret = EC_POINT_get_affine_coordinates_GFp(group, point, x, y, NULL);
    if (!ret) {
        LOG_ERR("Could not get X and Y affine coordinates");
        goto out;
    }

    /*
     * Copy the X and Y coordinate data into the ECC unique field,
     * ensuring that it fits along the way.
     */
    TPM2B_ECC_PARAMETER *X = &pub->publicArea.unique.ecc.x;
    TPM2B_ECC_PARAMETER *Y = &pub->publicArea.unique.ecc.y;

    unsigned x_size = BN_num_bytes(x);
    if (x_size > sizeof(X->buffer)) {
        LOG_ERR("X coordinate is too big. Got %u expected less than or equal to %zu",
                x_size, sizeof(X->buffer));
        goto out;
    }

    unsigned y_size = BN_num_bytes(y);
    if (y_size > sizeof(Y->buffer)) {
        LOG_ERR("X coordinate is too big. Got %u expected less than or equal to %zu",
                y_size, sizeof(Y->buffer));
        goto out;
    }

    X->size = BN_bn2bin(x, X->buffer);
    if (X->size != x_size) {
        LOG_ERR("Error converting X point BN to binary");
        goto out;
    }

    Y->size = BN_bn2bin(y, Y->buffer);
    if (Y->size != y_size) {
        LOG_ERR("Error converting Y point BN to binary");
        goto out;
    }

    /*
     * no kdf - not sure what this should be
     */
    pp->kdf.scheme = TPM2_ALG_NULL;
    pp->scheme.scheme = TPM2_ALG_NULL;
    pp->symmetric.algorithm = TPM2_ALG_NULL;
    pp->scheme.details.anySig.hashAlg = TPM2_ALG_NULL;

    /* NULL out sym details */
    TPMT_SYM_DEF_OBJECT *sym = &pp->symmetric;
    sym->algorithm = TPM2_ALG_NULL;
    sym->keyBits.sym = 0;
    sym->mode.sym = TPM2_ALG_NULL;

    result = true;

out:
    if (x) {
        BN_free(x);
    }
    if (y) {
        BN_free(y);
    }

    return result;
}

EC_KEY *tpm2_openssl_get_public_ECC_from_pem(FILE *f, const char *path) {

    EC_KEY *pub = PEM_read_EC_PUBKEY(f, NULL, NULL, NULL);
    if (!pub) {
         ERR_print_errors_fp (stderr);
         LOG_ERR("Reading public PEM file \"%s\" failed", path);
         return NULL;
    }

    return pub;
}

static bool load_public_ECC_from_pem(FILE *f, const char *path, TPM2B_PUBLIC *pub) {

    EC_KEY *k = tpm2_openssl_get_public_ECC_from_pem(f, path);
    if (!k) {
         ERR_print_errors_fp (stderr);
         LOG_ERR("Reading PEM file \"%s\" failed", path);
         return false;
    }

    bool result = load_public_ECC_from_key(k, pub);

    EC_KEY_free(k);

    return result;
}

static bool load_public_AES_from_file(FILE *f, const char *path, TPM2B_PUBLIC *pub, TPM2B_SENSITIVE *priv) {

    /*
     * Get the file size and validate that it is the proper AES keysize.
     */
    unsigned long file_size = 0;
    bool result = files_get_file_size(f, &file_size, path);
    if (!result) {
        return false;
    }

    result = tpm2_alg_util_is_aes_size_valid(file_size);
    if (!result) {
        return false;
    }

    pub->publicArea.type = TPM2_ALG_SYMCIPHER;
    TPMT_SYM_DEF_OBJECT *s = &pub->publicArea.parameters.symDetail.sym;
    s->algorithm = TPM2_ALG_AES;
    s->keyBits.aes = file_size * 8;

    /* allow any mode later on */
    s->mode.aes = TPM2_ALG_NULL;

    /*
     * Calculate the unique field with is the
     * is HMAC(sensitive->seedValue, sensitive->sensitive(key itself))
     * Where:
     *   - HMAC Key is the seed
     *   - Hash algorithm is the name algorithm
     */
    TPM2B_DIGEST *unique = &pub->publicArea.unique.sym;
    TPM2B_DIGEST *seed = &priv->sensitiveArea.seedValue;
    TPM2B_PRIVATE_VENDOR_SPECIFIC *key = &priv->sensitiveArea.sensitive.any;
    TPMI_ALG_HASH name_alg = pub->publicArea.nameAlg;

    return tpm2_util_calc_unique(name_alg, key, seed, unique);
}


static bool load_private_RSA_from_key(RSA *k, TPM2B_SENSITIVE *priv) {

    const BIGNUM *p; /* the private key exponent */

#if defined(LIB_TPM2_OPENSSL_OPENSSL_PRE11)
    p = k->p;
#else
    RSA_get0_factors(k, &p, NULL);
#endif

    TPMT_SENSITIVE *sa = &priv->sensitiveArea;

    sa->sensitiveType = TPM2_ALG_RSA;

    TPM2B_PRIVATE_KEY_RSA *pkr = &sa->sensitive.rsa;

    unsigned priv_bytes = BN_num_bytes(p);
    if (priv_bytes > sizeof(pkr->buffer)) {
        LOG_ERR("Expected prime \"d\" to be less than or equal to %zu,"
                " got: %u", sizeof(pkr->buffer), priv_bytes);
        return false;
    }

    pkr->size = priv_bytes;

    int success = BN_bn2bin(p, pkr->buffer);
    if (!success) {
        ERR_print_errors_fp (stderr);
        LOG_ERR("Could not copy private exponent \"d\"");
        return false;
    }

    return true;
}

bool tpm2_openssl_load_public(const char *path, TPMI_ALG_PUBLIC alg, TPM2B_PUBLIC *pub) {

    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_ERR("Could not open file \"%s\" error: %s", path, strerror(errno));
        return false;
    }

    bool result = false;

    switch (alg) {
    case TPM2_ALG_RSA:
        result = load_public_RSA_from_pem(f, path, pub);
        break;
    case TPM2_ALG_ECC:
        result = load_public_ECC_from_pem(f, path, pub);
        break;
    /* Skip AES here, as we can only load this one from a private file */
    default:
        /* default try TSS */
        result = files_load_public(path, pub);
    }

    fclose(f);

    return result;
}

 static bool load_private_ECC_from_key(EC_KEY *k, TPM2B_SENSITIVE *priv) {

     /*
      * private data
      */
     priv->sensitiveArea.sensitiveType = TPM2_ALG_ECC;

     TPM2B_ECC_PARAMETER *p = &priv->sensitiveArea.sensitive.ecc;

     const BIGNUM *b = EC_KEY_get0_private_key(k);

     unsigned priv_bytes = BN_num_bytes(b);
     if (priv_bytes > sizeof(p->buffer)) {
         LOG_ERR("Expected ECC private portion to be less than or equal to %zu,"
                 " got: %u", sizeof(p->buffer), priv_bytes);
         return false;
     }

     p->size = priv_bytes;
     int success = BN_bn2bin(b, p->buffer);
     if (!success) {
         return false;
     }

     return true;
}

static tpm2_openssl_load_rc load_private_ECC_from_pem(FILE *f, const char *path, const char *passin, TPM2B_PUBLIC *pub, TPM2B_SENSITIVE *priv) {

    tpm2_openssl_load_rc rc = lprc_error;

    char *pass = NULL;
    bool result = handle_ossl_pass(passin, &pass);
    if (!result) {
        return lprc_error;
    }

    EC_KEY *k = PEM_read_ECPrivateKey(f, NULL,
        NULL, (void *)pass);
    free(pass);
    fclose(f);
    if (!k) {
         ERR_print_errors_fp (stderr);
         LOG_ERR("Reading PEM file \"%s\" failed", path);
         return lprc_error;
    }

    result = load_private_ECC_from_key(k, priv);
    if (!result) {
        rc = lprc_error;
        goto out;
    }

    rc |= lprc_private;

    result = load_public_ECC_from_key(k, pub);
    if (!result) {
        rc = lprc_error;
        goto out;
    }

    rc |= lprc_public;

out:
    EC_KEY_free(k);
    return rc;
}

static tpm2_openssl_load_rc load_private_RSA_from_pem(FILE *f, const char *path, const char *passin,
        TPM2B_PUBLIC *pub, TPM2B_SENSITIVE *priv) {

    RSA *k = NULL;

    tpm2_openssl_load_rc rc = lprc_error;

    char *pass = NULL;
    bool result = handle_ossl_pass(passin, &pass);
    if (!result) {
        return lprc_error;
    }

    k = PEM_read_RSAPrivateKey(f, NULL,
        NULL, (void *)pass);
    free(pass);
    fclose(f);
    if (!k) {
         ERR_print_errors_fp (stderr);
         LOG_ERR("Reading PEM file \"%s\" failed", path);
         return lprc_error;
    }

    bool loaded_priv = load_private_RSA_from_key(k, priv);
    if (!loaded_priv) {
        return lprc_error;
    } else {
        rc |= lprc_private;
    }

    bool loaded_pub = load_public_RSA_from_key(k, pub);
    if (!loaded_pub) {
        goto out;
    } else {
        rc |= lprc_public;
    }
out:
    RSA_free(k);
    return rc;
}

static tpm2_openssl_load_rc load_private_AES_from_file(FILE *f, const char *path,
        TPM2B_PUBLIC *pub, TPM2B_SENSITIVE *priv) {

    unsigned long file_size = 0;
    bool result = files_get_file_size(f, &file_size, path);
    if (!result) {
        return lprc_error;
    }

    result = tpm2_alg_util_is_aes_size_valid(file_size);
    if (!result) {
        return lprc_error;
    }

    priv->sensitiveArea.sensitiveType = TPM2_ALG_SYMCIPHER;

    TPM2B_SYM_KEY *s = &priv->sensitiveArea.sensitive.sym;
    s->size = file_size;

    result = files_read_bytes(f, s->buffer, s->size);
    if (!result) {
        return lprc_error;
    }

    result = load_public_AES_from_file(f, path, pub, priv);
    if (!result) {
        return lprc_error;
    }

    return lprc_private | lprc_public;
}

/**
 * Loads a private portion of a key, and possibly the public portion, as for RSA the public data is in
 * a private pem file.
 *
 * @param path
 *  The path to load from.
 * @param alg
 *  algorithm type to import.
 * @param pub
 *  The public structure to populate. Note that nameAlg must be populated.
 * @param priv
 *  The sensitive structure to populate.
 *
 * @returns
 *  A private object loading status
 */
tpm2_openssl_load_rc tpm2_openssl_load_private(const char *path, const char *pass, TPMI_ALG_PUBLIC alg, TPM2B_PUBLIC *pub, TPM2B_SENSITIVE *priv) {

    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_ERR("Could not open file \"%s\", error: %s",
                path, strerror(errno));
        return 0;
    }

    /* set the seed */
    TPM2B_DIGEST *seed = &priv->sensitiveArea.seedValue;
    seed->size = tpm2_alg_util_get_hash_size(pub->publicArea.nameAlg);
    RAND_bytes(seed->buffer, seed->size);

    tpm2_openssl_load_rc rc = lprc_error;

    switch (alg) {
    case TPM2_ALG_RSA:
        rc = load_private_RSA_from_pem(f, path, pass, pub, priv);
        break;
    case TPM2_ALG_AES:
        if (pass) {
            LOG_ERR("No password can be used for protecting AES key");
            return lprc_error;
        }
        rc = load_private_AES_from_file(f, path, pub, priv);
        break;
    case TPM2_ALG_ECC:
        rc =load_private_ECC_from_pem(f, path, pass, pub, priv);
        break;
    default:
        LOG_ERR("Cannot handle algorithm, got: %s", tpm2_alg_util_algtostr(alg,
            tpm2_alg_util_flags_any));
        return lprc_error;
    }

    return rc;
}
