#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Encrypt password for config.ini (AES-256-GCM, device-bound key). */
bool cred_encrypt_password(const char* plaintext, char* out_hex, size_t out_hex_sz);

/* Decrypt value written as v1:<hex>. */
bool cred_decrypt_password(const char* stored_v1_hex, char* plaintext, size_t plaintext_sz);
