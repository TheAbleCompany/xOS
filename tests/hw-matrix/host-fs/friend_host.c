#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ix_fs_fat.h"
#include "ix_fs_ext.h"
#include "ix_fs_ntfs.h"
static FILE *g_img; static uint32_t g_ss = 512;
int ixk_storage_read_device(const ixk_storage_device_t*d, uint64_t lba, uint32_t count, void *buf){ (void)d; fseek(g_img, lba*g_ss, SEEK_SET); return fread(buf,g_ss,count,g_img)==count; }
int ixk_storage_write_device(const ixk_storage_device_t*d, uint64_t lba, uint32_t count, const void *buf){ (void)d;(void)lba;(void)count;(void)buf; return 0; }
uint32_t ixk_storage_sector_size(void){return g_ss;} uint64_t ixk_storage_total_sectors(void){return 0;}
#define READMODE 1
static unsigned long long g_sum=0, g_len=0;
int main(int argc,char**argv){
  g_img=fopen(argv[1],"rb"); fseek(g_img,0,SEEK_END); uint64_t SECT=ftell(g_img)/512; fseek(g_img,0,SEEK_SET); const char*kind=argv[2]; const char*path=argv[3]; ixk_storage_device_t dev={0,0,0,512,0}; dev.total_sectors=SECT;
  int n=0; ix_dirent_t e;
  int rd = argc>4;
  static unsigned char RB[65536]; int fh,k;
  if(!strcmp(kind,"ext")){ static ext_context_t c; if(!ext_mount(&c,0,SECT,&dev)){puts("mount failed");return 1;} if(rd){ fh=ext_open(&c,path,IX_FS_READ); if(fh<0){puts("open failed");return 1;} while((k=ext_read(&c,fh,RB,sizeof RB))>0){ for(int i=0;i<k;i++) g_sum+=RB[i]*(1+((g_len+i)%251)); g_len+=k; } printf("READ len=%llu sum=%llu\n",g_len,g_sum); return 0; } int dh=ext_opendir(&c,path); if(dh<0){puts("opendir failed");return 1;} while(ext_readdir(&c,dh,&e)>0)n++; }
  else if(!strcmp(kind,"ntfs")){ static ntfs_context_t c; if(!ntfs_mount(&c,0,SECT,&dev)){puts("mount failed");return 1;} if(rd){ fh=ntfs_open(&c,path,IX_FS_READ); if(fh<0){puts("open failed");return 1;} while((k=ntfs_read(&c,fh,RB,sizeof RB))>0){ for(int i=0;i<k;i++) g_sum+=RB[i]*(1+((g_len+i)%251)); g_len+=k; } printf("READ len=%llu sum=%llu\n",g_len,g_sum); return 0; } int dh=ntfs_opendir(&c,path); if(dh<0){puts("opendir failed");return 1;} while(ntfs_readdir(&c,dh,&e)>0)n++; }
  else { static fat_context_t c; ix_fs_type_t t=!strcmp(kind,"fat32")?IX_FS_TYPE_FAT32:IX_FS_TYPE_FAT16; if(!fat_mount(&c,0,SECT,t,&dev)){puts("mount failed");return 1;} if(rd){ fh=fat_open(&c,path,IX_FS_READ); if(fh<0){puts("open failed");return 1;} while((k=fat_read(&c,fh,RB,sizeof RB))>0){ for(int i=0;i<k;i++) g_sum+=RB[i]*(1+((g_len+i)%251)); g_len+=k; } printf("READ len=%llu sum=%llu\n",g_len,g_sum); return 0; } int dh=fat_opendir(&c,path); if(dh<0){puts("opendir failed");return 1;} while(fat_readdir(&c,dh,&e)>0)n++; }
  printf("TOTAL %d\n",n); return 0; }
