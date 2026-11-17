#include <stdio.h>
#include <stdlib.h>
#include "ix_fs_detect.h"
static FILE *g_img;
int ixk_storage_read_device(const ixk_storage_device_t*d, uint64_t lba, uint32_t count, void *buf){ (void)d; fseek(g_img, lba*512, SEEK_SET); return fread(buf,512,count,g_img)==count; }
int ixk_storage_write_device(const ixk_storage_device_t*d, uint64_t l, uint32_t c, const void*b){(void)d;(void)l;(void)c;(void)b;return 0;}
uint32_t ixk_storage_sector_size(void){return 512;} uint64_t ixk_storage_total_sectors(void){return 0;}
int main(int argc,char**argv){ g_img=fopen(argv[1],"rb"); fseek(g_img,0,SEEK_END); uint64_t n=ftell(g_img)/512; ixk_storage_device_t d={0,0,0,512,n};
  printf("%-14s -> %s\n", argv[1], ix_fs_type_name(ix_fs_detect_device(&d,0,n))); return 0; }
