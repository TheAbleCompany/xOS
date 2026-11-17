#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint16_t u16int; typedef uint64_t uint64_t_;
int blkdev_ata_parse_identify(const u16int *id, uint64_t *native_sectors,
                              uint32_t *lss, int *lba48, char model[41])
{
    /* Слово 0 бит 15 -- ATAPI/не-ATA. */
    if (id[0] & 0x8000u)
        return 0;

    int l48 = (id[83] & (1u << 10)) != 0 && (id[83] & 0xC000u) == 0x4000u;

    uint64_t sectors;
    if (l48)
        sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                  ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    else
        sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    if (sectors == 0)
        return 0;

    /*
     * Логический сектор: слово 106 валидно при бите 14=1 и 15=0;
     * бит 12 -- логический сектор больше 512, размер в СЛОВАХ
     * в 117..118.  Без этого 4Kn-диски читались бы неверно.
     */
    uint32_t ss = 512;
    u16int   w106 = id[106];
    if ((w106 & 0xC000u) == 0x4000u && (w106 & (1u << 12))) {
        uint32_t words = (uint32_t)id[117] | ((uint32_t)id[118] << 16);
        if (words >= 256 && words <= 2048)
            ss = words * 2u;
    }
    if (ss != 512 && ss != 1024 && ss != 2048 && ss != 4096)
        ss = 512;

    /* Модель: слова 27..46, байты в слове переставлены. */
    int k = 0;
    for (int i = 27; i <= 46; i++) {
        model[k++] = (char)(id[i] >> 8);
        model[k++] = (char)(id[i] & 0xFF);
    }
    model[k] = 0;
    while (k > 0 && (model[k - 1] == ' ' || model[k - 1] == 0))
        model[--k] = 0;
    for (int i = 0; i < k; i++)
        if ((unsigned char)model[i] < 0x20 || (unsigned char)model[i] > 0x7E)
            model[i] = '?';

    *native_sectors = sectors;
    *lss   = ss;
    *lba48 = l48;
    return 1;
}


static void setmodel(uint16_t*id,const char*m){ char p[41]; memset(p,' ',40); p[40]=0; memcpy(p,m,strlen(m)); for(int i=0;i<20;i++) id[27+i]=(uint16_t)((p[2*i]<<8)|p[2*i+1]); }
int main(void){
  uint16_t id[256]; uint64_t sec; uint32_t lss; int l48; char model[41]; int fails=0;
  #define CHECK(c) do{ if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c); fails++;} }while(0)
  /* 1. 4Kn LBA48 */
  memset(id,0,sizeof id); id[83]=0x4000|(1<<10); id[100]=0x1000; id[101]=0x0001; id[106]=0x4000|(1<<12); id[117]=2048; id[118]=0; setmodel(id,"WDC 4KN TEST");
  CHECK(blkdev_ata_parse_identify(id,&sec,&lss,&l48,model)); CHECK(lss==4096); CHECK(l48==1); CHECK(sec==0x11000); printf("model=[%s]\n",model); CHECK(strcmp(model,"WDC 4KN TEST")==0);
  /* 2. 512e (physical 4K, logical 512): слово 106 бит13 (physical>logical), бит12=0 */
  memset(id,0,sizeof id); id[83]=0x4000|(1<<10); id[100]=100; id[106]=0x4000|(1<<13)|3;
  CHECK(blkdev_ata_parse_identify(id,&sec,&lss,&l48,model)); CHECK(lss==512);
  /* 3. LBA28 old disk */
  memset(id,0,sizeof id); id[60]=0x1234; id[61]=0x0002;
  CHECK(blkdev_ata_parse_identify(id,&sec,&lss,&l48,model)); CHECK(l48==0); CHECK(sec==0x21234); CHECK(lss==512);
  /* 4. ATAPI отвергается */
  memset(id,0,sizeof id); id[0]=0x8000|0x0500; id[60]=1000;
  CHECK(!blkdev_ata_parse_identify(id,&sec,&lss,&l48,model));
  /* 5. ёмкость 0 */
  memset(id,0,sizeof id); CHECK(!blkdev_ata_parse_identify(id,&sec,&lss,&l48,model));
  /* 6. мусор в слове 117 (не 4K) при бите12 -> откат на 512 */
  memset(id,0,sizeof id); id[60]=100; id[106]=0x4000|(1<<12); id[117]=7;
  CHECK(blkdev_ata_parse_identify(id,&sec,&lss,&l48,model)); CHECK(lss==512);
  /* 7. слово 83 без бита валидности 14 -> LBA48 не считаем */
  memset(id,0,sizeof id); id[83]=(1<<10); id[60]=50; id[100]=999;
  CHECK(blkdev_ata_parse_identify(id,&sec,&lss,&l48,model)); CHECK(l48==0); CHECK(sec==50);
  printf(fails?"IDENTIFY tests: %d FAILED\n":"IDENTIFY tests: all passed\n",fails); return fails;
}
