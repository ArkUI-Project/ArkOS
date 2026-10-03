#!/usr/bin/env python3
"""Generate immutable font metadata from the pinned licensed outline font."""
from pathlib import Path
import argparse,hashlib
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--font',type=Path,default=ROOT/'assets/fonts/wqy-microhei.ttc')
p.add_argument('--header',type=Path,default=ROOT/'user/unicode_font.h')
a=p.parse_args();data=a.font.read_bytes();digest=hashlib.sha256(data).hexdigest()
assert digest=='2420e8078af796b19a3f6ef13de527a1a91c1e7171eea115926c614ced1009b3'
a.header.write_text('/* Bundled WenQuanYi Micro Hei Mono outline font. See assets/fonts/LICENSE.txt. */\n#ifndef ARK_UNICODE_FONT_DATA_H\n#define ARK_UNICODE_FONT_DATA_H\n#define UNICODE_FONT_BINARY_BYTES '+str(len(data))+'u\n#define UNICODE_FONT_SHA256 "'+digest+'"\n#endif\n')
print(f'Outline font: {len(data)} bytes; SHA-256 {digest}')
