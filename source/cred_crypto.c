#include "cred_crypto.h"
#include "debug_log.h"

#include <3ds.h>
#include <3ds/services/cfgu.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include <string.h>

#define GCM_IV_LEN   12
#define GCM_TAG_LEN  16
#define PREFIX       "v1:"
/* Config password field is 64 bytes including NUL; max stored secret length */
#define MAX_PW_LEN   63
#define MAX_BIN_LEN  (GCM_IV_LEN + MAX_PW_LEN + GCM_TAG_LEN)
#define MAX_HEX_LEN  (MAX_BIN_LEN * 2)

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool bytes_to_hex(const unsigned char* bin, size_t bin_len,
                         char* out, size_t out_sz) {
    static const char xd[] = "0123456789abcdef";
    if (out_sz < bin_len * 2 + 1)
        return false;
    for (size_t i = 0; i < bin_len; i++) {
        out[i * 2]     = xd[bin[i] >> 4];
        out[i * 2 + 1] = xd[bin[i] & 0x0f];
    }
    out[bin_len * 2] = '\0';
    return true;
}

static bool hex_to_bytes(const char* hex, unsigned char* out, size_t* out_len) {
    size_t n = strlen(hex);
    if (n % 2 != 0 || n > MAX_HEX_LEN)
        return false;
    for (size_t i = 0; i < n; i += 2) {
        int hi = hex_nibble((unsigned char)hex[i]);
        int lo = hex_nibble((unsigned char)hex[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i / 2] = (unsigned char)((hi << 4) | lo);
    }
    *out_len = n / 2;
    return true;
}

static bool derive_key(unsigned char key[32]) {
    static const char label[] = "kavita-3ds/cred/v1";
    u8 serial[18];
    memset(serial, 0, sizeof(serial));

    Result r = CFGU_GetConfigInfoBlk2((u32)sizeof(serial), 0x000B0000, serial);
    if (R_FAILED(r)) {
        dlog("[cred] serial block failed 0x%08lX, using fallback",
             (unsigned long)r);
        memcpy(serial, "NO-SERIAL-BLOCK", 15);
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts_ret(&sha, 0) != 0)
        goto fail;
    if (mbedtls_sha256_update_ret(&sha, (const unsigned char*)label, sizeof(label) - 1) != 0)
        goto fail;
    if (mbedtls_sha256_update_ret(&sha, serial, sizeof(serial)) != 0)
        goto fail;
    if (mbedtls_sha256_finish_ret(&sha, key) != 0)
        goto fail;
    mbedtls_sha256_free(&sha);
    return true;
fail:
    mbedtls_sha256_free(&sha);
    return false;
}

static bool random_iv(unsigned char iv[GCM_IV_LEN]) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr);
    int ret = mbedtls_ctr_drbg_seed(&ctr, mbedtls_entropy_func, &entropy,
                                    (const unsigned char*)"kv3ds-pw", 8);
    if (ret == 0)
        ret = mbedtls_ctr_drbg_random(&ctr, iv, GCM_IV_LEN);
    mbedtls_ctr_drbg_free(&ctr);
    mbedtls_entropy_free(&entropy);
    return ret == 0;
}

bool cred_encrypt_password(const char* plaintext, char* out_hex, size_t out_hex_sz) {
    if (!plaintext || !out_hex || out_hex_sz == 0)
        return false;
    size_t pt_len = strlen(plaintext);
    if (pt_len == 0) {
        out_hex[0] = '\0';
        return true;
    }
    if (pt_len > MAX_PW_LEN)
        return false;

    unsigned char key[32];
    if (!derive_key(key))
        return false;

    unsigned char iv[GCM_IV_LEN];
    if (!random_iv(iv)) {
        memset(key, 0, sizeof(key));
        return false;
    }

    unsigned char ct[MAX_PW_LEN];
    unsigned char tag[GCM_TAG_LEN];
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (ret == 0) {
        ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, pt_len,
                                        iv, GCM_IV_LEN, NULL, 0,
                                        (const unsigned char*)plaintext, ct,
                                        GCM_TAG_LEN, tag);
    }
    mbedtls_gcm_free(&gcm);
    memset(key, 0, sizeof(key));

    if (ret != 0) {
        dlog("[cred] gcm encrypt failed -0x%04X", (unsigned)-ret);
        return false;
    }

    unsigned char bin[MAX_BIN_LEN];
    memcpy(bin, iv, GCM_IV_LEN);
    memcpy(bin + GCM_IV_LEN, ct, pt_len);
    memcpy(bin + GCM_IV_LEN + pt_len, tag, GCM_TAG_LEN);
    size_t bin_len = GCM_IV_LEN + pt_len + GCM_TAG_LEN;

    size_t need = strlen(PREFIX) + bin_len * 2 + 1;
    if (out_hex_sz < need)
        return false;
    memcpy(out_hex, PREFIX, strlen(PREFIX));
    if (!bytes_to_hex(bin, bin_len, out_hex + strlen(PREFIX), out_hex_sz - strlen(PREFIX)))
        return false;
    memset(bin, 0, sizeof(bin));
    return true;
}

bool cred_decrypt_password(const char* stored, char* plaintext, size_t plaintext_sz) {
    if (!stored || !plaintext || plaintext_sz == 0)
        return false;
    if (strncmp(stored, PREFIX, strlen(PREFIX)) != 0)
        return false;

    const char* hex = stored + strlen(PREFIX);
    if (strlen(hex) > MAX_HEX_LEN)
        return false;

    unsigned char bin[MAX_BIN_LEN];
    size_t bin_len = 0;
    if (!hex_to_bytes(hex, bin, &bin_len))
        return false;
    if (bin_len < GCM_IV_LEN + GCM_TAG_LEN)
        return false;

    size_t ct_len = bin_len - GCM_IV_LEN - GCM_TAG_LEN;
    if (ct_len >= plaintext_sz)
        return false;

    const unsigned char* iv  = bin;
    const unsigned char* ct  = bin + GCM_IV_LEN;
    const unsigned char* tag = bin + GCM_IV_LEN + ct_len;

    unsigned char key[32];
    if (!derive_key(key))
        return false;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    memset(key, 0, sizeof(key));
    if (ret == 0) {
        ret = mbedtls_gcm_auth_decrypt(&gcm, ct_len, iv, GCM_IV_LEN,
                                       NULL, 0, tag, GCM_TAG_LEN, ct,
                                       (unsigned char*)plaintext);
    }
    mbedtls_gcm_free(&gcm);

    if (ret != 0) {
        dlog("[cred] gcm decrypt failed -0x%04X", (unsigned)-ret);
        return false;
    }
    plaintext[ct_len] = '\0';
    return true;
}
