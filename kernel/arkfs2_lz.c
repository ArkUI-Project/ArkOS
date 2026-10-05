/* Original ArkFS2 small-window LZ. Not derived from zlib/lz4/other FS codecs. */
#include "arkfs2_lz.h"

uint32_t arkfs2_lz_compress(const uint8_t *src, uint32_t src_len, uint8_t *dst,
                            uint32_t dst_cap) {
    if (!src || !dst || src_len == 0 || dst_cap < 2)
        return 0;
    uint32_t di = 0, si = 0;
    while (si < src_len) {
        uint32_t best_len = 0, best_off = 0;
        uint32_t start = si > ARKFS2_LZ_WINDOW ? si - ARKFS2_LZ_WINDOW : 0;
        for (uint32_t j = start; j < si; ++j) {
            uint32_t n = 0;
            while (n < ARKFS2_LZ_MAX && si + n < src_len && src[j + n] == src[si + n])
                ++n;
            if (n >= ARKFS2_LZ_MIN && n > best_len) {
                best_len = n;
                best_off = si - j;
            }
        }
        if (best_len >= ARKFS2_LZ_MIN) {
            /* match: 1xxxxxxx | len-3 in low 4, then 12-bit offset big-endian-ish */
            if (di + 3 > dst_cap)
                return 0;
            uint32_t len_code = best_len - ARKFS2_LZ_MIN;
            dst[di++] = (uint8_t)(0x80u | ((len_code & 0xfu) << 3) | ((best_off >> 8) & 0x7u));
            dst[di++] = (uint8_t)(best_off & 0xffu);
            si += best_len;
        } else {
            /* literal run: 0lllllll then bytes; max 127 */
            uint32_t run = 1;
            while (run < 127 && si + run < src_len) {
                /* stop if a match of MIN would start here */
                uint32_t probe = si + run;
                uint32_t st = probe > ARKFS2_LZ_WINDOW ? probe - ARKFS2_LZ_WINDOW : 0;
                uint32_t m = 0;
                for (uint32_t j = st; j < probe; ++j) {
                    uint32_t n = 0;
                    while (n < ARKFS2_LZ_MIN && probe + n < src_len && src[j + n] == src[probe + n])
                        ++n;
                    if (n >= ARKFS2_LZ_MIN) {
                        m = n;
                        break;
                    }
                }
                if (m)
                    break;
                ++run;
            }
            if (di + 1 + run > dst_cap)
                return 0;
            dst[di++] = (uint8_t)run;
            for (uint32_t k = 0; k < run; ++k)
                dst[di++] = src[si++];
        }
    }
    if (di >= src_len)
        return 0; /* not smaller */
    return di;
}

bool arkfs2_lz_decompress(const uint8_t *src, uint32_t src_len, uint8_t *dst,
                          uint32_t dst_cap, uint32_t *out_len) {
    if (!src || !dst || !out_len)
        return false;
    uint32_t di = 0, si = 0;
    while (si < src_len) {
        uint8_t op = src[si++];
        if (op & 0x80u) {
            if (si >= src_len)
                return false;
            uint32_t len = ARKFS2_LZ_MIN + ((op >> 3) & 0xfu);
            uint32_t off = ((uint32_t)(op & 0x7u) << 8) | src[si++];
            if (!off || off > di || di + len > dst_cap)
                return false;
            for (uint32_t k = 0; k < len; ++k)
                dst[di + k] = dst[di - off + k];
            di += len;
        } else {
            uint32_t run = op;
            if (!run || si + run > src_len || di + run > dst_cap)
                return false;
            for (uint32_t k = 0; k < run; ++k)
                dst[di++] = src[si++];
        }
    }
    *out_len = di;
    return true;
}
