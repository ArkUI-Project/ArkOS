# stb_truetype native port

Upstream stb_truetype 1.26, fixed repository commit `5736b15f7ea0ffb08dd38af21067c314d6a3aae9`:
https://github.com/nothings/stb/blob/5736b15f7ea0ffb08dd38af21067c314d6a3aae9/stb_truetype.h

Unmodified header SHA-256: `a34d8d536ce7c11b9163ab2d524721c1f4df1452cce6595c4f11d3048384f925`.
ArkOS uses its MIT license, reproduced in LICENSE.txt and at the end of the header.

ArkOS's original adapter in user/unicode.c supplies freestanding allocation/math,
clips the glyph mask, keeps legacy text metrics and caches 256 glyphs at actual
pixel sizes. The sole accepted font is the immutable hash-pinned WQY Micro Hei
Mono TTC linked into each executable. stb_truetype is not a safe parser for
untrusted fonts; user-provided fonts are not accepted. Builds are offline.
