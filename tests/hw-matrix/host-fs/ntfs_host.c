#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ix_fs_ntfs.h"
static FILE *g_img;
int ixk_storage_read(uint64_t lba, uint32_t count, void *buf){ fseek(g_img, lba*512, SEEK_SET); return fread(buf,512,count,g_img)==count; }
int ixk_storage_write(uint64_t lba, uint32_t count, const void *buf){ (void)lba;(void)count;(void)buf; return 0; }
uint32_t ixk_storage_native_sector(void){return 512;}
uint64_t ixk_storage_capacity(void){return 0;}
int ixk_storage_sync(void){return 1;}
void debug_puts(const char*s){ fputs(s,stderr); }
int main(int argc,char**argv){
  g_img=fopen(argv[1],"rb"); static ntfs_context_t ctx;
  uint64_t off = argc>2 ? strtoull(argv[2],0,10) : 0;
  if(!ntfs_mount(&ctx, off, 0)){ puts("mount failed"); return 1; }
  int dh=ntfs_opendir(&ctx,"/"); if(dh<0){puts("opendir failed");return 1;}
  ix_dirent_t e; int n=0;
  while(ntfs_readdir(&ctx,dh,&e)>0){ printf("%s\n", e.name); n++; }
  fprintf(stderr,"TOTAL %d\n", n);
  return 0;
}
