/* SPDX-License-Identifier: LGPL-3.0-or-later OR GPL-3.0-or-later */
#ifndef MULTINET_CRYPTO_H
#define MULTINET_CRYPTO_H
#include <stddef.h>
#include <stdint.h>

int mn_crypto_backend_available(void);
int mn_crypto_aes256gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t *aad, size_t aad_size,
                                const uint8_t *plain, size_t plain_size,
                                uint8_t *cipher, uint8_t tag[16]);
int mn_crypto_aes256gcm_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t *aad, size_t aad_size,
                                const uint8_t *cipher, size_t cipher_size,
                                const uint8_t tag[16], uint8_t *plain);
#endif
