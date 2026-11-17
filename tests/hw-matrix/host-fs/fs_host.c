#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ix_fs_fat.h"
#include "ix_fs_ext.h"
static FILE *g_img;
int ixk_storage_read(uint64_t lba, uint32_t count, void *buf){ fseek(g_img, lba*512, SEEK_SET); return fread(buf,512,count,g_img)==count; }
int ixk_storage_write(uint64_t lba, uint32_t count, const void *buf){ (void)lba;(void)count;(void)buf; return 0; }
uint32_t ixk_storage_native_sector(void){return 512;}
uint64_t ixk_storage_capacity(void){return 0;}
int ixk_storage_sync(void){return 1;}
void debug_puts(const char*s){ fputs(s,stderr); }
#define READMODE 1
static unsigned long long g_sum=0, g_len=0;
int main(int argc,char**argv){
  g_img=fopen(argv[1],"rb"); const char *kind=argv[2]; const char *path=argv[3];
  int n=0; ix_dirent_t e; int rd=argc>4; static unsigned char RB[65536]; int fh,k;
  if(!strcmp(kind,"ext")){ static ext_context_t c; if(!ext_mount(&c,0,0)){puts("mount failed");return 1;}
    if(rd){ fh=ext_open(&c,path,IX_FS_READ); if(fh<0){puts("open failed");return 1;} while((k=ext_read(&c,fh,RB,sizeof RB))>0){ for(int i=0;i<k;i++) g_sum+=RB[i]*(1+((g_len+i)%251)); g_len+=k; } printf("READ len=%llu sum=%llu\n",g_len,g_sum); return 0; } int dh=ext_opendir(&c,path); if(dh<0){puts("opendir failed");return 1;}
    while(ext_readdir(&c,dh,&e)>0){ printf("%s\n",e.name); n++; } }
  else { static fat_context_t c; int t = !strcmp(kind,"fat32")?IX_FS_TYPE_FAT32:!strcmp(kind,"fat16")?IX_FS_TYPE_FAT16:IX_FS_TYPE_FAT12;
    if(!fat_mount(&c,0,0,t)){puts("mount failed");return 1;}
    if(rd){ fh=fat_open(&c,path,IX_FS_READ); if(fh<0){puts("open failed");return 1;} while((k=fat_read(&c,fh,RB,sizeof RB))>0){ for(int i=0;i<k;i++) g_sum+=RB[i]*(1+((g_len+i)%251)); g_len+=k; } printf("READ len=%llu sum=%llu\n",g_len,g_sum); return 0; } int dh=fat_opendir(&c,path); if(dh<0){puts("opendir failed");return 1;}
    while(fat_readdir(&c,dh,&e)>0){ printf("%s\n",e.name); n++; } }
  fprintf(stderr,"TOTAL %d\n",n); return 0; }
