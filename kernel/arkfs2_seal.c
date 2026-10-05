/* ArkFS2 volume seal: PBKDF2-HMAC-SHA256 + AES-128-GCM (BearSSL).
 * Layout and KDF wiring are original; not ported from other filesystems. */
#include "arkfs2_seal.h"
#include "arkfs2_lz.h"
#include "sha256.h"
#include "bearssl.h"

#ifdef ARK_STORAGE_HOST_TEST
static bool (*random_hook)(void *, size_t);
void arkfs2_set_random_hook(bool (*fn)(void *buf, size_t n)) { random_hook = fn; }
#endif

#ifndef ARK_STORAGE_HOST_TEST
bool platform_secure_random(void *output, size_t length);
#endif

bool arkfs2_random(void *buf, size_t n) {
#ifdef ARK_STORAGE_HOST_TEST
    if (random_hook)
        return random_hook(buf, n);
    return false;
#else
    return platform_secure_random(buf, n);
#endif
}

static void mem_set(void *d, int v, size_t n) {
    uint8_t *p = d;
    for (size_t i = 0; i < n; ++i)
        p[i] = (uint8_t)v;
}
static void mem_copy(void *d, const void *s, size_t n) {
    uint8_t *dd = d;
    const uint8_t *ss = s;
    for (size_t i = 0; i < n; ++i)
        dd[i] = ss[i];
}

static void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                        uint8_t out[32]) {
    uint8_t k[64], kipad[64], kopad[64], mid[32], buf[96];
    mem_set(k, 0, sizeof k);
    if (key_len > 64)
        ark_sha256(key, key_len, k);
    else
        mem_copy(k, key, key_len);
    for (unsigned i = 0; i < 64; ++i) {
        kipad[i] = (uint8_t)(k[i] ^ 0x36u);
        kopad[i] = (uint8_t)(k[i] ^ 0x5cu);
    }
    uint8_t inner[64 + 64];
    mem_copy(inner, kipad, 64);
    mem_copy(inner + 64, msg, msg_len > 64 ? 64 : msg_len);
    /* msg_len is 20 (salt+counter) or 32 (U) for PBKDF2 */
    ark_sha256(inner, 64 + msg_len, mid);
    mem_copy(buf, kopad, 64);
    mem_copy(buf + 64, mid, 32);
    ark_sha256(buf, 96, out);
}

void arkfs2_kdf(const char *pass, size_t pass_len, const uint8_t salt[ARKFS2_SALT_LEN],
                uint32_t iters, uint8_t out[ARKFS2_KEY_LEN]) {
    uint8_t u[32], t[32], block[ARKFS2_SALT_LEN + 4];
    mem_copy(block, salt, ARKFS2_SALT_LEN);
    block[ARKFS2_SALT_LEN] = 0;
    block[ARKFS2_SALT_LEN + 1] = 0;
    block[ARKFS2_SALT_LEN + 2] = 0;
    block[ARKFS2_SALT_LEN + 3] = 1;
    hmac_sha256((const uint8_t *)pass, pass_len, block, sizeof block, u);
    mem_copy(t, u, 32);
    for (uint32_t i = 1; i < iters; ++i) {
        hmac_sha256((const uint8_t *)pass, pass_len, u, 32, u);
        for (unsigned j = 0; j < 32; ++j)
            t[j] ^= u[j];
    }
    mem_copy(out, t, ARKFS2_KEY_LEN);
    mem_set(u, 0, sizeof u);
    mem_set(t, 0, sizeof t);
}

static bool gcm_crypt(int encrypt, const uint8_t key[ARKFS2_KEY_LEN], const uint8_t *nonce,
                      const uint8_t *aad, size_t aad_len, uint8_t *data, size_t data_len,
                      uint8_t tag[ARKFS2_TAG_LEN]) {
    br_aes_ct_ctr_keys kc;
    br_gcm_context gc;
    br_aes_ct_ctr_init(&kc, key, ARKFS2_KEY_LEN);
    br_gcm_init(&gc, &kc.vtable, br_ghash_ctmul64);
    br_gcm_reset(&gc, nonce, ARKFS2_NONCE_LEN);
    if (aad_len)
        br_gcm_aad_inject(&gc, aad, aad_len);
    br_gcm_flip(&gc);
    br_gcm_run(&gc, encrypt, data, data_len);
    if (encrypt) {
        br_gcm_get_tag(&gc, tag);
        return true;
    }
    return br_gcm_check_tag(&gc, tag) != 0;
}

bool arkfs2_wrap_key(const uint8_t pass_key[ARKFS2_KEY_LEN], const uint8_t vol_key[ARKFS2_KEY_LEN],
                     Arkfs2CryptoHeader *hdr) {
    uint8_t buf[ARKFS2_KEY_LEN];
    uint8_t tag[ARKFS2_TAG_LEN];
    if (!arkfs2_random(hdr->wrap_nonce, ARKFS2_NONCE_LEN))
        return false;
    mem_copy(buf, vol_key, ARKFS2_KEY_LEN);
    if (!gcm_crypt(1, pass_key, hdr->wrap_nonce, (const uint8_t *)"ARKWRAP", 7, buf, ARKFS2_KEY_LEN, tag))
        return false;
    mem_copy(hdr->wrapped, buf, ARKFS2_KEY_LEN);
    mem_copy(hdr->wrapped + ARKFS2_KEY_LEN, tag, ARKFS2_TAG_LEN);
    mem_set(buf, 0, sizeof buf);
    return true;
}

bool arkfs2_unwrap_key(const uint8_t pass_key[ARKFS2_KEY_LEN], const Arkfs2CryptoHeader *hdr,
                       uint8_t vol_key[ARKFS2_KEY_LEN]) {
    uint8_t buf[ARKFS2_KEY_LEN];
    uint8_t tag[ARKFS2_TAG_LEN];
    mem_copy(buf, hdr->wrapped, ARKFS2_KEY_LEN);
    mem_copy(tag, hdr->wrapped + ARKFS2_KEY_LEN, ARKFS2_TAG_LEN);
    if (!gcm_crypt(0, pass_key, hdr->wrap_nonce, (const uint8_t *)"ARKWRAP", 7, buf, ARKFS2_KEY_LEN, tag))
        return false;
    mem_copy(vol_key, buf, ARKFS2_KEY_LEN);
    mem_set(buf, 0, sizeof buf);
    return true;
}

/* On-disk sealed block (4096):
 * [0] flags: bit0=compressed bit1=encrypted
 * [1..2] plain_len LE
 * [3..4] body_len LE
 * if encrypted: [5..16] nonce, [17..32] tag, [33..] cipher body
 * else: [5..] body
 */
bool arkfs2_seal_block(const Arkfs2SealState *st, uint32_t logical_index, const uint8_t *plain,
                       uint32_t plain_len, uint8_t out[4096]) {
    uint8_t body[ARKFS2_PLAIN_MAX];
    uint8_t comp[ARKFS2_PLAIN_MAX];
    uint32_t body_len;
    uint8_t flags = 0;
    if (!st || !out || (!plain && plain_len) || plain_len > ARKFS2_PLAIN_MAX)
        return false;
    mem_set(out, 0, 4096);
    mem_set(body, 0, sizeof body);
    if (plain_len)
        mem_copy(body, plain, plain_len);
    body_len = plain_len;
    if ((st->features & ARKFS2_FEAT_COMPRESS) && plain_len) {
        uint32_t cl = arkfs2_lz_compress(plain, plain_len, comp, sizeof comp);
        if (cl && cl < plain_len) {
            mem_set(body, 0, sizeof body);
            mem_copy(body, comp, cl);
            body_len = cl;
            flags |= 1u;
        }
    }
    out[1] = (uint8_t)plain_len;
    out[2] = (uint8_t)(plain_len >> 8);
    out[3] = (uint8_t)body_len;
    out[4] = (uint8_t)(body_len >> 8);
    if (!(st->features & ARKFS2_FEAT_ENCRYPT)) {
        out[0] = flags;
        mem_copy(out + 5, body, body_len);
        return true;
    }
    if (!st->unlocked)
        return false;
    flags |= 2u;
    uint8_t nonce[ARKFS2_NONCE_LEN];
    if (!arkfs2_random(nonce, sizeof nonce))
        return false;
    uint8_t aad[8];
    aad[0] = (uint8_t)logical_index;
    aad[1] = (uint8_t)(logical_index >> 8);
    aad[2] = (uint8_t)(logical_index >> 16);
    aad[3] = (uint8_t)(logical_index >> 24);
    aad[4] = flags;
    aad[5] = out[1];
    aad[6] = out[2];
    aad[7] = out[3];
    uint8_t tag[ARKFS2_TAG_LEN];
    if (!gcm_crypt(1, st->volume_key, nonce, aad, sizeof aad, body, body_len, tag))
        return false;
    out[0] = flags;
    mem_copy(out + 5, nonce, ARKFS2_NONCE_LEN);
    mem_copy(out + 5 + ARKFS2_NONCE_LEN, tag, ARKFS2_TAG_LEN);
    mem_copy(out + 5 + ARKFS2_NONCE_LEN + ARKFS2_TAG_LEN, body, body_len);
    mem_set(body, 0, sizeof body);
    return true;
}

bool arkfs2_unseal_block(const Arkfs2SealState *st, uint32_t logical_index, const uint8_t in[4096],
                         uint8_t *plain, uint32_t plain_cap, uint32_t *plain_len_out) {
    if (!st || !in || !plain || !plain_len_out)
        return false;
    uint8_t flags = in[0];
    uint32_t plain_len = (uint32_t)in[1] | ((uint32_t)in[2] << 8);
    uint32_t body_len = (uint32_t)in[3] | ((uint32_t)in[4] << 8);
    if (plain_len > ARKFS2_PLAIN_MAX || body_len > ARKFS2_PLAIN_MAX || plain_len > plain_cap)
        return false;
    uint8_t body[ARKFS2_PLAIN_MAX];
    mem_set(body, 0, sizeof body);
    if (flags & 2u) {
        if (!(st->features & ARKFS2_FEAT_ENCRYPT) || !st->unlocked)
            return false;
        if (5u + ARKFS2_NONCE_LEN + ARKFS2_TAG_LEN + body_len > 4096u)
            return false;
        const uint8_t *nonce = in + 5;
        const uint8_t *tag = in + 5 + ARKFS2_NONCE_LEN;
        mem_copy(body, in + 5 + ARKFS2_NONCE_LEN + ARKFS2_TAG_LEN, body_len);
        uint8_t aad[8];
        aad[0] = (uint8_t)logical_index;
        aad[1] = (uint8_t)(logical_index >> 8);
        aad[2] = (uint8_t)(logical_index >> 16);
        aad[3] = (uint8_t)(logical_index >> 24);
        aad[4] = flags;
        aad[5] = in[1];
        aad[6] = in[2];
        aad[7] = in[3];
        uint8_t tag_copy[ARKFS2_TAG_LEN];
        mem_copy(tag_copy, tag, ARKFS2_TAG_LEN);
        if (!gcm_crypt(0, st->volume_key, nonce, aad, sizeof aad, body, body_len, tag_copy))
            return false;
    } else {
        if (5u + body_len > 4096u)
            return false;
        mem_copy(body, in + 5, body_len);
    }
    if (flags & 1u) {
        uint32_t got = 0;
        if (!arkfs2_lz_decompress(body, body_len, plain, plain_cap, &got) || got != plain_len)
            return false;
    } else {
        if (body_len != plain_len)
            return false;
        mem_copy(plain, body, plain_len);
    }
    *plain_len_out = plain_len;
    return true;
}
