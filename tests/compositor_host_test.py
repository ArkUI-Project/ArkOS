#!/usr/bin/env python3
"""Exercise the actual live native glass compositor: worker equivalence,
clip bounds and identical visible pixels after opaque-interior elision."""
import os,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/test-compositor-013';OUT.mkdir(exist_ok=True)
def function(source,name):
 m=re.search(r'static[^\n;]*\b'+name+r'\([^;]*?\)\s*\{',source);assert m,name
 at=m.end();depth=1
 while depth:
  depth+=(source[at]=='{')-(source[at]=='}');at+=1
 return source[m.start():at]+'\n'
current=(ROOT/'user/desktop.c').read_text()
shared=''.join(function(current,n) for n in ['max','min','clamp','rounded_coverage','compositor_mix','compositor_radius','compositor_span','compositor_round_region','rr','stroke','shadow_color','shadow'])
preamble='''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ARK_KERNEL
#include "ark_api.h"
#include "raster.h"
#include "liquid_glass.h"
#define MAX_W 3840
#define MAX_H 2160
typedef struct {int x,y,w,h;} GpuRect;
static int sw=640,sh=480;static uint32_t *canvas;
static bool night,reduced_transparency;
static uint8_t shadow_channels[3][256];static bool shadow_channels_ready,shadow_pending;static int shadow_x0,shadow_x1,shadow_y0,shadow_y1;
static unsigned workers=1;static bool reference_mode;
static void *ark_memory(size_t n){return calloc(1,n);}
static uint64_t platform_millis(void){return 0;}
static int ark_memory_release(void*p,size_t n){(void)n;free(p);return 0;}
static void notice(const char*s){fprintf(stderr,"%s",s);assert(false);}
/* CPU-path oracle: this host fixture has no GPU device or Ring3 syscall. */
static int64_t ark_call(uint64_t n,void*p,size_t bytes){(void)n;(void)p;(void)bytes;return -19;}
static void serial_write(const char*s){(void)s;}
static void compositor_glass_stage(unsigned);
static void compositor_glass_parallel(GpuRect);
'''
parallel='''static void reference_expand_rows(int first,int last){
 for(int y=first;y<last;y++)for(int x=glass_work.x0;x<glass_work.x1;x++){
  int ax=x-glass_work.x,ay=y-glass_work.y,w=glass_work.w,h=glass_work.h,r=glass_work.r,pad=glass_work.pad;unsigned cover=rounded_coverage(ax,ay,w,h,r);if(!cover)continue;
  LiquidLens lens;liquid_glass_lens(ax+.5f,ay+.5f,w,h,r,12,24,false,glass_work.map->dispersion,&lens);
  uint32_t c=liquid_glass_dispersion(glass_blur,glass_work.bw,glass_work.bw,glass_work.bh,(ax+pad)*256+(int)(lens.dx*256),(ay+pad)*256+(int)(lens.dy*256),(int)(lens.dispersion_x*256),(int)(lens.dispersion_y*256));
  unsigned inner=rounded_coverage(ax-1,ay-1,w-2,h-2,max(0,r-1)),light=(unsigned)((cover>inner?cover-inner:0)*lens.highlight*.5f);c=glass_plus(c,light*glass_work.edge/255);canvas[(size_t)y*sw+x]=cover==255?c:compositor_mix(canvas[(size_t)y*sw+x],c,cover);
 }
}
static void compositor_glass_stage(unsigned stage){for(unsigned i=0;i<workers;i++)glass_process_rows(stage,i,glass_work.bh*(int)i/(int)workers,glass_work.bh*(int)(i+1)/(int)workers);}
static void compositor_glass_parallel(GpuRect r){if(reference_mode){reference_expand_rows(r.y,r.y+r.h);return;}for(unsigned i=0;i<workers;i++)glass_expand_rows(r.y+r.h*(int)i/(int)workers,r.y+r.h*(int)(i+1)/(int)workers);}
'''
main='''int main(void){size_t size=(size_t)sw*sh*4;uint32_t *before=malloc(size),*expected=malloc(size),*actual=malloc(size);assert(before&&expected&&actual);uint32_t random=42;
 for(unsigned i=0;i<(unsigned)sw*sh;i++){random=random*1664525u+1013904223u;before[i]=random&0xffffff;}
 for(int scenario=0;scenario<24;scenario++){int x=scenario%4==0?-57:scenario%4==1?sw-20:13+scenario*3,y=scenario%3==0?-12:24,w=scenario%7==0?35:240+scenario*11,h=scenario%5==0?72:140+scenario*5,r=scenario%4==0?34:scenario%4==1?24:scenario%4==2?20:0;night=scenario&1;reduced_transparency=scenario&2;
  for(unsigned opaque=0;opaque<2;opaque++){
   memcpy(expected,before,size);canvas=expected;workers=1;reference_mode=true;glass_opaque_client=false;shadow(x,y,w,h);glass_edge(x,y,w,h,r,170+scenario,80);
   memcpy(actual,before,size);canvas=actual;workers=4;reference_mode=false;glass_opaque_client=opaque;shadow(x,y,w,h);glass_edge(x,y,w,h,r,170+scenario,80);
   if(opaque){canvas=expected;rr(x+15,y+68,w-30,h-83,0,0x324567,255);canvas=actual;rr(x+15,y+68,w-30,h-83,0,0x324567,255);}
   for(unsigned i=0;i<(unsigned)sw*sh;i++)if(actual[i]!=expected[i]){fprintf(stderr,"Mismatch scene=%d opaque=%u x=%u y=%u %06x/%06x\\n",scenario,opaque,i%(unsigned)sw,i/(unsigned)sw,actual[i],expected[i]);return 1;}
  }
 }
 canvas=actual;glass_opaque_client=false;glass(3,4,36,36,18,150);
 free(actual);free(expected);free(before);free(glass_blur);for(unsigned i=0;i<4;i++)free(glass_maps[i].pixels);
 puts("PASS production live glass: 1/4 worker equality and opaque-client elision across offscreen clipping, tiny panels, light/dark, dispersion and transparency modes");}
'''
source=OUT/'test.c';source.write_text(preamble+shared+(ROOT/'user/desktop_glass.inc').read_text().split('/* LiquidButton interaction')[0]+parallel+main)
cc=os.environ.get('CC','cc');exe=OUT/'run'
subprocess.run([cc,'-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(source),str(ROOT/'user/raster.c'),str(ROOT/'user/liquid_glass.c'),'-lm','-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
