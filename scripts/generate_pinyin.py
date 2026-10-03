#!/usr/bin/env python3
"""Build a native immutable dictionary from bundled Unicode Unihan 15.1 data."""
from pathlib import Path
import zipfile,unicodedata,json,hashlib
root=Path(__file__).resolve().parents[1]
source=root/'third_party/unicode/Unihan-15.1.0.zip'
text=zipfile.ZipFile(source).read('Unihan_Readings.txt').decode()
items={}
for line in text.splitlines():
 if line.startswith('#') or not line:continue
 cp,kind,value=line.split('\t',2)
 if kind!='kMandarin':continue
 number=int(cp[2:],16)
 if not 0x4e00<=number<=0x9fff:continue
 for reading in value.split():
  reading=unicodedata.normalize('NFD',reading.lower()).replace('u\u0308','v')
  syllable=''.join(c for c in reading if 'a'<=c<='z')
  if syllable:items.setdefault(syllable,[]).append(chr(number))
priority='的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成会可主发年动同工也能下过子说产种面而方后多定行学法所民得经进着等部度家电力里如水化高自二理起小物现实加量都两体制机当使点从业本去把性好应开它合还因由其些然前外天政四日那社义事平形相全表间样与关各重新线内数正心反你明看原又么利比或但质气第向道命此变条只没结解问意建月公无系军很情者最立代想已通并提直题程展五果料象员革位入常文总次品式活设及管特件长求老头基资边流路级少图山统接知较将组见计别她手角期根论运农指区强放决西被干做必战先回则任取据处理世车真海口东安工日记录系统设置文件截图设备应用程序工具你好中国北京上海电脑网络权限管理浏览器终端桌面安装硬盘屏幕时间用户多核线程学习生活欢迎测试保存取消删除复制粘贴'
rank={c:i for i,c in reversed(list(enumerate(priority)))}
rows=[];characters=''
for key,values in sorted(items.items()):
 values=sorted(set(values),key=lambda c:(rank.get(c,100000),ord(c)))
 rows.append((key,len(characters.encode()),len(values)))
 characters+=''.join(values)
lines=['/* Generated from Unicode Unihan 15.1.0; see third_party/unicode/LICENSE.txt. */','typedef struct {const char *key;unsigned offset,count;} PinyinEntry;','static const char pinyin_characters[] =']
for i in range(0,len(characters),100):lines.append(json.dumps(characters[i:i+100],ensure_ascii=False))
lines[-1]+=';';lines.append('static const PinyinEntry pinyin_entries[]={')
lines += ['{"%s",%d,%d},'%row for row in rows];lines.append('};\n')
(root/'user/pinyin_data.h').write_text('\n'.join(lines))
(root/'third_party/unicode/README.md').write_text(f'# Native Pinyin data\n\nSource: https://www.unicode.org/Public/15.1.0/ucd/Unihan.zip\n\nSHA-256: `{hashlib.sha256(source.read_bytes()).hexdigest()}`\n\nOnly kMandarin readings for U+4E00–U+9FFF are compiled. {len(rows)} syllables, {len(characters)} character/readings. Tone marks are removed; ü uses v. The original archive and Unicode license are included. ArkOS owns the query/segmentation engine; it needs no runtime process, network or host input method. The short common phrase list was written for ArkOS.\n')
print(len(rows),'syllables',len(characters),'characters/readings')
