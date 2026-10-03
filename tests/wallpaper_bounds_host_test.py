#!/usr/bin/env python3
"""Exercise the production wallpaper at supported display bounds under UBSan."""
import os,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/test-wallpaper-bounds';OUT.mkdir(exist_ok=True)
source=(ROOT/'user/desktop.c').read_text()
def function(name):
 m=re.search(r'static[^\n;]*\b'+name+r'\([^;]*?\)\s*\{',source);assert m
 start=m.start();end=m.end();depth=1
 while depth:
  if source[end]=='{':depth+=1
  if source[end]=='}':depth-=1
  end+=1
 return source[start:end]+'\n'
preamble='''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static int sw,sh,wall;static bool night,dirty;static uint32_t *wallpaper;
'''
main='''int main(void){
 const int sizes[][2]={{1024,720},{1280,800},{2560,1440},{3840,2160}};
 for(unsigned i=0;i<4;i++){sw=sizes[i][0];sh=sizes[i][1];size_t count=(size_t)sw*sh;uint32_t *p=malloc((count+2)*4);assert(p);p[0]=0x13579bdf;p[count+1]=0x2468ace0;wallpaper=p+1;
  for(int style=0;style<4;style++){wall=style&1;night=style&2;dirty=false;make_wallpaper();assert(dirty);assert(p[0]==0x13579bdf&&p[count+1]==0x2468ace0);for(size_t x=0;x<count;x++)assert(wallpaper[x]<=0xffffff);
   /* The far top-left lies outside the circular light gradient at every size. */
   uint32_t expected=wall?0x33265c:0x213f82;if(night)expected=mix(expected,0x081327,90);assert(wallpaper[0]==expected);
  }free(p);
 }puts("PASS production wallpaper at 1024p/1280p/1440p/4K bounds, themes, corner value and frame guards; UBSan no signed overflow");
}
'''
file=OUT/'test.c';file.write_text(preamble+''.join(function(n) for n in ('max','min','clamp','mix','make_wallpaper'))+main)
exe=OUT/'run';subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(file),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
