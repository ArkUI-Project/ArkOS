#ifndef ARKFS2_LZ_H
#define ARKFS2_LZ_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Original small-window LZ for ArkFS2 file payloads. Window 4095, match
 * length 3..18. Not derived from zlib, lz4, or other filesystem codecs. */
enum { ARKFS2_LZ_WINDOW = 4095, ARKFS2_LZ_MIN = 3, ARKFS2_LZ_MAX = 18 };

/* Compress src[src_len] into dst[dst_cap]. Returns compressed size, or 0 if
 * compression does not shrink or the output cannot fit. */
uint32_t arkfs2_lz_compress(const uint8_t *src, uint32_t src_len, uint8_t *dst,
                            uint32_t dst_cap);

/* Decompress src[src_len] into dst[dst_cap]. Returns true and sets *out_len on
 * success. Rejects malformed streams without writing past dst_cap. */
bool arkfs2_lz_decompress(const uint8_t *src, uint32_t src_len, uint8_t *dst,
                          uint32_t dst_cap, uint32_t *out_len);
#endif
