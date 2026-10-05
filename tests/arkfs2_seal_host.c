#include "arkfs2_lz.h"
#include "arkfs2_seal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t rng;
static bool fake_random(void *buf, size_t n) {
    uint8_t *p = buf;
    for (size_t i = 0; i < n; ++i) {
        rng = rng * 6364136223846793005ull + 1ull;
        p[i] = (uint8_t)(rng >> 56);
    }
    return true;
}

static void test_lz(void) {
    uint8_t src[2000], dst[2000], round[2000];
    for (int i = 0; i < 2000; ++i)
        src[i] = (uint8_t)('A' + (i % 3));
    uint32_t cl = arkfs2_lz_compress(src, 2000, dst, sizeof dst);
    assert(cl && cl < 2000);
    uint32_t got = 0;
    assert(arkfs2_lz_decompress(dst, cl, round, sizeof round, &got));
    assert(got == 2000 && !memcmp(src, round, 2000));
    puts("PASS: lz round-trip");
}

static void test_seal(void) {
    arkfs2_set_random_hook(fake_random);
    Arkfs2SealState st = {0};
    st.features = ARKFS2_FEAT_ENCRYPT | ARKFS2_FEAT_COMPRESS;
    st.unlocked = true;
    for (int i = 0; i < ARKFS2_KEY_LEN; ++i)
        st.volume_key[i] = (uint8_t)(i * 17 + 3);
    uint8_t plain[400], out1[4096], out2[4096], back[400];
    memset(plain, 'Z', sizeof plain);
    memcpy(plain, "hello sealed block", 18);
    assert(arkfs2_seal_block(&st, 7, plain, sizeof plain, out1));
    assert(arkfs2_seal_block(&st, 7, plain, sizeof plain, out2));
    assert(memcmp(out1 + 5, out2 + 5, ARKFS2_NONCE_LEN) != 0); /* new nonce each write */
    uint32_t got = 0;
    assert(arkfs2_unseal_block(&st, 7, out1, back, sizeof back, &got));
    assert(got == sizeof plain && !memcmp(plain, back, sizeof plain));
    out1[40] ^= 1;
    assert(!arkfs2_unseal_block(&st, 7, out1, back, sizeof back, &got));
    puts("PASS: seal nonce+auth");
}

static void test_wrap(void) {
    arkfs2_set_random_hook(fake_random);
    uint8_t pass_key[ARKFS2_KEY_LEN], vol[ARKFS2_KEY_LEN], out[ARKFS2_KEY_LEN];
    uint8_t salt[ARKFS2_SALT_LEN];
    memset(salt, 9, sizeof salt);
    arkfs2_kdf("secret", 6, salt, 1000, pass_key);
    for (int i = 0; i < ARKFS2_KEY_LEN; ++i)
        vol[i] = (uint8_t)i;
    Arkfs2CryptoHeader hdr = {0};
    hdr.features = ARKFS2_FEAT_ENCRYPT;
    hdr.kdf_iters = 1000;
    memcpy(hdr.salt, salt, sizeof salt);
    assert(arkfs2_wrap_key(pass_key, vol, &hdr));
    assert(arkfs2_unwrap_key(pass_key, &hdr, out));
    assert(!memcmp(vol, out, ARKFS2_KEY_LEN));
    pass_key[0] ^= 1;
    assert(!arkfs2_unwrap_key(pass_key, &hdr, out));
    puts("PASS: wrap/unwrap");
}

int main(void) {
    test_lz();
    test_seal();
    test_wrap();
    return 0;
}
