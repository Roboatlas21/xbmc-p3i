/*
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "mkcert.h"

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/conf.h>
#include <openssl/pkcs12.h>
#include <openssl/rsa.h>

static const int NUM_BITS = 2048;
static const int SERIAL = 0;
static const int NUM_YEARS = 10;

int mkcert(X509 **x509p, EVP_PKEY **pkeyp, int bits, int serial, int years);

CERT_KEY_PAIR mkcert_generate() {
    X509 *x509 = NULL;
    EVP_PKEY *pkey = NULL;
    PKCS12 *p12 = NULL;
    if (mkcert(&x509, &pkey, NUM_BITS, SERIAL, NUM_YEARS) != 1 ||
        !x509 || !pkey)
        goto fail;
    p12 = PKCS12_create("limelight", "GameStream", pkey, x509,
                       NULL, 0, 0, 0, 0, 0);
    if (!p12)
        goto fail;
    return (CERT_KEY_PAIR) {x509, pkey, p12};
fail:
    X509_free(x509);
    EVP_PKEY_free(pkey);
    PKCS12_free(p12);
    return (CERT_KEY_PAIR) {NULL, NULL, NULL};
}

void mkcert_free(CERT_KEY_PAIR certKeyPair) {
    X509_free(certKeyPair.x509);
    EVP_PKEY_free(certKeyPair.pkey);
    PKCS12_free(certKeyPair.p12);
}

/* Each file is fully written, restricted and synced before publication.
 * Publishing client.pem LAST makes an interrupted first-time generation
 * discoverable rather than treating an incomplete pair as ready. */
static int save_credential(const char *path, CERT_KEY_PAIR pair, int kind) {
    char temporary[PATH_MAX];
    if (!path || snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path) >=
                     (int)sizeof(temporary))
        return 0;
    int fd = mkstemp(temporary);
    if (fd < 0) return 0;
    FILE *stream = NULL;
    int ok = 0;
    if (fchmod(fd, 0600) != 0) goto cleanup;
    stream = fdopen(fd, "wb");
    if (!stream) goto cleanup;
    fd = -1;
    if (kind == 0)
        ok = PEM_write_PrivateKey(stream, pair.pkey, NULL, NULL, 0, NULL, NULL);
    else if (kind == 1)
        ok = i2d_PKCS12_fp(stream, pair.p12);
    else
        ok = PEM_write_X509(stream, pair.x509);
    if (!ok || fflush(stream) != 0 ||
        fsync(fileno(stream)) != 0) goto cleanup;
    if (fclose(stream) != 0) { stream = NULL; goto cleanup; }
    stream = NULL;
    /* Unlike rename(), link() cannot overwrite an existing identity. */
    if (link(temporary, path) != 0) goto cleanup;
    unlink(temporary);
    return 1;
cleanup:
    if (stream) fclose(stream);
    if (fd >= 0) close(fd);
    unlink(temporary);
    return 0;
}

int mkcert_save(const char* certFile, const char* p12File,
                const char* keyPairFile, CERT_KEY_PAIR pair) {
    if (!pair.x509 || !pair.pkey || !pair.p12 ||
        !certFile || !p12File || !keyPairFile)
        return 0;
    if (!save_credential(keyPairFile, pair, 0))
        return 0;
    if (!save_credential(p12File, pair, 1)) {
        unlink(keyPairFile);
        return 0;
    }
    if (!save_credential(certFile, pair, 2)) {
        unlink(p12File);
        unlink(keyPairFile);
        return 0;
    }
    return 1;
}

int mkcert(X509 **x509p, EVP_PKEY **pkeyp, int bits, int serial, int years) {
    if (!x509p || !pkeyp) return 0;
    *x509p = NULL;
    *pkeyp = NULL;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    EVP_PKEY *pk = NULL;
    X509 *cert = NULL;
    X509_NAME *name = NULL;
    if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) <= 0 ||
        EVP_PKEY_keygen(ctx, &pk) <= 0)
        goto fail;
    cert = X509_new();
    if (!cert || X509_set_version(cert, 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(cert), serial) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(cert), 0) ||
        !X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60 * 24 * 365 * years) ||
        X509_set_pubkey(cert, pk) != 1)
        goto fail;
    name = X509_NAME_new();
    if (!name || X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            (const unsigned char *)"NVIDIA GameStream Client", -1, -1, 0) != 1 ||
        X509_set_subject_name(cert, name) != 1 ||
        X509_set_issuer_name(cert, name) != 1 ||
        X509_sign(cert, pk, EVP_sha256()) <= 0)
        goto fail;
    X509_NAME_free(name);
    EVP_PKEY_CTX_free(ctx);
    *x509p = cert;
    *pkeyp = pk;
    return 1;
fail:
    X509_NAME_free(name);
    X509_free(cert);
    EVP_PKEY_free(pk);
    EVP_PKEY_CTX_free(ctx);
    return 0;
}
