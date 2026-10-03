#!/usr/bin/env python3
"""Pack native ELF load images with one byte-identical font payload.

Only file offsets change. Virtual addresses, permissions, entry points, BSS and
loaded bytes stay identical. The existing kernel ELF loader still copies every
segment into private pages. No decompressor, shared user pages or runtime host.
Standalone SDK ELFs remain intact. All input PT_LOAD bytes are verified below.
"""
from pathlib import Path
import hashlib,json,struct,sys,argparse
EH=struct.Struct('<16sHHIQQQIHHHHHH');PH=struct.Struct('<IIQQQQQQ')
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output-dir',type=Path,default=Path('build'))
parser.add_argument('images',nargs='+')
args=parser.parse_args()
FONT=Path('assets/fonts/wqy-microhei.ttc').read_bytes();output=args.output_dir
output.mkdir(parents=True,exist_ok=True)
font_hash=hashlib.sha256(FONT).hexdigest()
def aligned(n):return (n+4095)&~4095
def pad(b):b.extend(bytes(aligned(len(b))-len(b)))
archive=bytearray();images=[]
for name in args.images:
 source=Path(name);raw=source.read_bytes();h=list(EH.unpack_from(raw))
 assert h[0][:7]==b'\x7fELF\x02\x01\x01' and h[1:4]==[2,62,1],name
 assert h[8]==EH.size and h[9]==PH.size and 0<h[10]<=32,name
 assert h[5]+h[9]*h[10]<=len(raw),name
 # Section headers/symbols are unnecessary for runtime loading. Keep original
 # program-header placement, rebuilding each non-font load at a page boundary.
 h[6]=0;h[11]=h[12]=h[13]=0
 compact=bytearray(h[5]+h[9]*h[10]);compact[:EH.size]=EH.pack(*h)
 loads=[];font_index=None
 for i in range(h[10]):
  old=list(PH.unpack_from(raw,h[5]+i*PH.size));p=old.copy()
  assert p[0]==1 and p[5]<=p[6] and p[2]+p[5]<=len(raw),name
  if p[6]:assert p[2]%4096==0 and p[3]%4096==0 and p[7]==4096,name
  data=raw[p[2]:p[2]+p[5]]
  if p[1]==4 and p[5]==len(FONT) and data==FONT:
   assert font_index is None,name
   font_index=i;p[2]=0
  else:
   pad(compact);p[2]=len(compact);compact.extend(data)
  loads.append((old,p))
 assert font_index is not None,'missing dedicated font segment: '+name
 pad(archive);start=len(archive);archive.extend(compact)
 images.append({'source':name,'raw':raw,'header':h,'loads':loads,'font_index':font_index,'start':start,'compact_bytes':len(compact)})
assert images,'no user programs'
pad(archive);font_start=len(archive);archive.extend(FONT)
symbols=['.section .rodata.user_images,"a",@progbits','.balign 4096','ark_user_images:','.incbin "'+str(output/'programs.pack')+'"','ark_user_images_end:']
report=[]
for image in images:
 start=image['start'];h=image['header'];raw=image['raw']
 image['loads'][image['font_index']][1][2]=font_start-start
 for i,(old,p) in enumerate(image['loads']):
  PH.pack_into(archive,start+h[5]+i*PH.size,*p)
  assert p[5]<=p[6] and p[2]+p[5]<=len(archive)-start
  assert archive[start+p[2]:start+p[2]+p[5]]==raw[old[2]:old[2]+old[5]],image['source']
  assert p[:2]==old[:2] and p[3:]==old[3:],image['source']
 symbol='_binary_'+image['source'].replace('/','_').replace('.','_')
 assert all(c.isalnum() or c=='_' for c in symbol)
 # All views end after the common font. The validated program headers only
 # reference this application's own payload and the immutable common font.
 symbols+=['.global '+symbol+'_start','.set '+symbol+'_start, ark_user_images+'+str(start),'.global '+symbol+'_end','.set '+symbol+'_end, ark_user_images_end']
 report.append({'source':image['source'],'original_bytes':len(raw),'start':start,'compact_bytes':image['compact_bytes'],'font_offset':font_start-start,'view_bytes':len(archive)-start,'load_segments_byte_identical':True})
symbols+=['.section .note.GNU-stack,"",@progbits','']
(output/'programs.pack').write_bytes(archive)
(output/'programs.S').write_text('\n'.join(symbols))
result={'font_bytes':len(FONT),'font_sha256':font_hash,'copies_before':len(images),'copies_after':1,'input_bytes':sum(len(x['raw']) for x in images),'packed_bytes':len(archive),'packed_sha256':hashlib.sha256(archive).hexdigest(),'images':report}
(output/'programs.json').write_text(json.dumps(result,indent=2)+'\n')
print(f"Native ELF pack: {result['input_bytes']} -> {len(archive)} bytes; all {len(images)} load images byte-identical")
