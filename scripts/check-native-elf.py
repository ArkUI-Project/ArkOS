#!/usr/bin/env python3
"""Reject host/dynamic/TLS dependencies that ArkOS ABI v1 cannot load."""
import struct,sys
from pathlib import Path
for name in sys.argv[1:]:
 b=Path(name).read_bytes();assert b[:6]==b'\x7fELF\x02\x01',(name,'ELF64 LE required')
 phoff,shoff=struct.unpack_from('<QQ',b,32);phsize,phnum,shsize,shnum=struct.unpack_from('<HHHH',b,54)
 for i in range(phnum):
  typ,flags=struct.unpack_from('<II',b,phoff+i*phsize)
  assert typ not in (2,3,7),(name,'dynamic loader / TLS unsupported')
  assert typ!=1 or flags&3!=3,(name,'W+X segment forbidden')
 for i in range(shnum):
  typ,flags=struct.unpack_from('<IQ',b,shoff+i*shsize+4)
  assert not flags&0x400,(name,'TLS sections unsupported; use explicit per-thread storage')
  assert typ!=11,(name,'dynamic symbols forbidden')
 print('PASS native ELF: static, no TLS/dynamic loader, W^X:',name)
