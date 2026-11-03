/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#include "multinet_crypto.h"

#ifdef MN_ENABLE_CRYPTO
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdlib.h>
#include <limits.h>

static int mn_bcrypt_crypt(int encrypt, const uint8_t key_bytes[32],
                           const uint8_t nonce[12], const uint8_t *aad,
                           size_t aad_size, const uint8_t *input,
                           size_t input_size, const uint8_t input_tag[16],
                           uint8_t *output, uint8_t output_tag[16]) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    PUCHAR key_object = NULL;
    DWORD object_size = 0, result_size = 0;
    NTSTATUS status;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
    int ok = 0;

    if (aad_size > ULONG_MAX || input_size > ULONG_MAX) return 0;
    status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (status < 0) goto cleanup;
    status = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                               (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                               (ULONG)sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (status < 0) goto cleanup;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                               (PUCHAR)&object_size, sizeof(object_size),
                               &result_size, 0);
    if (status < 0 || object_size == 0) goto cleanup;
    key_object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, object_size);
    if (!key_object) goto cleanup;
    status = BCryptGenerateSymmetricKey(algorithm, &key, key_object,
                                         object_size, (PUCHAR)key_bytes, 32, 0);
    if (status < 0) goto cleanup;

    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = (PUCHAR)nonce;
    auth.cbNonce = 12;
    auth.pbAuthData = (PUCHAR)aad;
    auth.cbAuthData = (ULONG)aad_size;
    auth.pbTag = encrypt ? output_tag : (PUCHAR)input_tag;
    auth.cbTag = 16;

    if (encrypt) {
        status = BCryptEncrypt(key, (PUCHAR)input, (ULONG)input_size, &auth,
                               NULL, 0, output, (ULONG)input_size,
                               &result_size, 0);
    } else {
        status = BCryptDecrypt(key, (PUCHAR)input, (ULONG)input_size, &auth,
                               NULL, 0, output, (ULONG)input_size,
                               &result_size, 0);
    }
    ok = status >= 0 && result_size == input_size;

cleanup:
    if (key) BCryptDestroyKey(key);
    if (key_object) {
        SecureZeroMemory(key_object, object_size);
        HeapFree(GetProcessHeap(), 0, key_object);
    }
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

int mn_crypto_backend_available(void) { return 1; }

int mn_crypto_hmac_sha256(const uint8_t *key_bytes, size_t key_size,
                          const uint8_t *data, size_t data_size,
                          uint8_t output[32]) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR object = NULL;
    DWORD object_size = 0, result_size = 0;
    NTSTATUS status;
    int ok = 0;
    if (!key_bytes || key_size == 0 || key_size > ULONG_MAX ||
        (!data && data_size != 0) || data_size > ULONG_MAX || !output)
        return 0;
    status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                         NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status < 0) goto cleanup;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                               (PUCHAR)&object_size, sizeof(object_size),
                               &result_size, 0);
    if (status < 0 || object_size == 0) goto cleanup;
    object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, object_size);
    if (!object) goto cleanup;
    status = BCryptCreateHash(algorithm, &hash, object, object_size,
                              (PUCHAR)key_bytes, (ULONG)key_size, 0);
    if (status < 0) goto cleanup;
    if (data_size != 0) {
        status = BCryptHashData(hash, (PUCHAR)data, (ULONG)data_size, 0);
        if (status < 0) goto cleanup;
    }
    status = BCryptFinishHash(hash, output, 32, 0);
    ok = status >= 0;
cleanup:
    if (hash) BCryptDestroyHash(hash);
    if (object) {
        SecureZeroMemory(object, object_size);
        HeapFree(GetProcessHeap(), 0, object);
    }
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

int mn_crypto_aes256gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t *aad, size_t aad_size,
                                const uint8_t *plain, size_t plain_size,
                                uint8_t *cipher, uint8_t tag[16]) {
    return mn_bcrypt_crypt(1, key, nonce, aad, aad_size, plain, plain_size,
                           NULL, cipher, tag);
}
int mn_crypto_aes256gcm_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t *aad, size_t aad_size,
                                const uint8_t *cipher, size_t cipher_size,
                                const uint8_t tag[16], uint8_t *plain) {
    return mn_bcrypt_crypt(0, key, nonce, aad, aad_size, cipher, cipher_size,
                           tag, plain, NULL);
}

#else
#include <limits.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
int mn_crypto_backend_available(void) { return 1; }

int mn_crypto_hmac_sha256(const uint8_t *key, size_t key_size,
                          const uint8_t *data, size_t data_size,
                          uint8_t output[32]) {
    unsigned int output_size = 0;
    if (!key || key_size == 0 || key_size > INT_MAX ||
        (!data && data_size != 0) || !output)
        return 0;
    return HMAC(EVP_sha256(), key, (int)key_size, data, data_size,
                output, &output_size) != NULL && output_size == 32u;
}

int mn_crypto_aes256gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t *aad, size_t aad_size,
                                const uint8_t *plain, size_t plain_size,
                                uint8_t *cipher, uint8_t tag[16]) {
    EVP_CIPHER_CTX *ctx;
    int length = 0, final_length = 0, ok = 0;
    if (aad_size > INT_MAX || plain_size > INT_MAX) return 0;
    ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return 0;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) goto done;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto done;
    if (aad_size && EVP_EncryptUpdate(ctx, NULL, &length, aad, (int)aad_size) != 1) goto done;
    if (plain_size && EVP_EncryptUpdate(ctx, cipher, &length, plain, (int)plain_size) != 1) goto done;
    if (EVP_EncryptFinal_ex(ctx, cipher + length, &final_length) != 1) goto done;
    if ((size_t)(length + final_length) != plain_size) goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) goto done;
    ok = 1;
done:
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

int mn_crypto_aes256gcm_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t *aad, size_t aad_size,
                                const uint8_t *cipher, size_t cipher_size,
                                const uint8_t tag[16], uint8_t *plain) {
    EVP_CIPHER_CTX *ctx;
    int length = 0, final_length = 0, ok = 0;
    if (aad_size > INT_MAX || cipher_size > INT_MAX) return 0;
    ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return 0;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) goto done;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto done;
    if (aad_size && EVP_DecryptUpdate(ctx, NULL, &length, aad, (int)aad_size) != 1) goto done;
    if (cipher_size && EVP_DecryptUpdate(ctx, plain, &length, cipher, (int)cipher_size) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) != 1) goto done;
    if (EVP_DecryptFinal_ex(ctx, plain + length, &final_length) != 1) goto done;
    if ((size_t)(length + final_length) != cipher_size) goto done;
    ok = 1;
done:
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}
#endif
#else
int mn_crypto_backend_available(void) { return 0; }
int mn_crypto_hmac_sha256(const uint8_t *key, size_t key_size,
                          const uint8_t *data, size_t data_size,
                          uint8_t output[32]) {
    (void)key; (void)key_size; (void)data; (void)data_size; (void)output;
    return 0;
}
int mn_crypto_aes256gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t *aad, size_t aad_size,
                                const uint8_t *plain, size_t plain_size,
                                uint8_t *cipher, uint8_t tag[16]) {
    (void)key; (void)nonce; (void)aad; (void)aad_size; (void)plain;
    (void)plain_size; (void)cipher; (void)tag; return 0;
}
int mn_crypto_aes256gcm_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t *aad, size_t aad_size,
                                const uint8_t *cipher, size_t cipher_size,
                                const uint8_t tag[16], uint8_t *plain) {
    (void)key; (void)nonce; (void)aad; (void)aad_size; (void)cipher;
    (void)cipher_size; (void)tag; (void)plain; return 0;
}
#endif
