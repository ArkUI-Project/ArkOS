#ifndef ARKFS2_SEAL_H
#define ARKFS2_SEAL_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum {
    ARKFS2_FEAT_ENCRYPT = 1u,
    ARKFS2_FEAT_COMPRESS = 2u,
    ARKFS2_NONCE_LEN = 12,
    ARKFS2_TAG_LEN = 16,
    ARKFS2_SALT_LEN = 16,
    ARKFS2_KEY_LEN = 16,
    ARKFS2_KDF_ITERS_DEFAULT = 100000u,
    ARKFS2_KDF_ITERS_MAX = 500000u,
    /* flags + 2 lens + nonce + tag; logical payload per sealed block. */
    ARKFS2_SEAL_OVERHEAD = 5 + ARKFS2_NONCE_LEN + ARKFS2_TAG_LEN,
    ARKFS2_PLAIN_MAX = 4096u - ARKFS2_SEAL_OVERHEAD
};

typedef struct {
    uint32_t features; /* ARKFS2_FEAT_* */
    uint32_t kdf_iters;
    uint8_t salt[ARKFS2_SALT_LEN];
    uint8_t wrapped[ARKFS2_KEY_LEN + ARKFS2_TAG_LEN]; /* AES-GCM wrap of volume key */
    uint8_t wrap_nonce[ARKFS2_NONCE_LEN];
} Arkfs2CryptoHeader;

typedef struct {
    uint32_t features;
    uint8_t volume_key[ARKFS2_KEY_LEN];
    bool unlocked;
} Arkfs2SealState;

/* PBKDF2-HMAC-SHA256 (original ArkOS SHA-256). */
void arkfs2_kdf(const char *pass, size_t pass_len, const uint8_t salt[ARKFS2_SALT_LEN],
                uint32_t iters, uint8_t out[ARKFS2_KEY_LEN]);

bool arkfs2_wrap_key(const uint8_t pass_key[ARKFS2_KEY_LEN], const uint8_t vol_key[ARKFS2_KEY_LEN],
                     Arkfs2CryptoHeader *hdr);
bool arkfs2_unwrap_key(const uint8_t pass_key[ARKFS2_KEY_LEN], const Arkfs2CryptoHeader *hdr,
                       uint8_t vol_key[ARKFS2_KEY_LEN]);

/* Seal one file-data block. plain_len <= ARKFS2_PLAIN_MAX. out is 4096 bytes.
 * logical_index is authenticated. Uses compression when feat bit set and helpful. */
bool arkfs2_seal_block(const Arkfs2SealState *st, uint32_t logical_index, const uint8_t *plain,
                       uint32_t plain_len, uint8_t out[4096]);

/* Inverse. On auth/decompress failure returns false (caller maps to user string). */
bool arkfs2_unseal_block(const Arkfs2SealState *st, uint32_t logical_index, const uint8_t in[4096],
                         uint8_t *plain, uint32_t plain_cap, uint32_t *plain_len);

bool arkfs2_random(void *buf, size_t n);
bool arkfs2_random_available(void);

#ifdef ARK_STORAGE_HOST_TEST
void arkfs2_set_random_hook(bool (*fn)(void *buf, size_t n));
#endif
#endif
