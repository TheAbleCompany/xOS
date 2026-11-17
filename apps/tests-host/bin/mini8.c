#include <stdio.h>
#include <unistd.h>
int main(void){
    FILE *f = fopen("/tmp/m8.bin", "w+b");
    if (!f) { write(2,"fopen fail\n",11); return 2; }
    fwrite("0123456789ABCDEF", 1, 16, f);
    int r1 = fflush(f);
    long t1 = ftell(f);
    rewind(f);
    long t2 = ftell(f);
    long raw = lseek(f->_fileno, 0, SEEK_CUR);
    int c1 = fgetc(f);
    long raw2 = lseek(f->_fileno, 0, SEEK_CUR);
    printf("fflush=%d ftell1=%ld ftell2=%ld raw=%ld c1=%c raw2=%ld\n", r1, t1, t2, raw, c1>0?c1:'?', raw2);
    fclose(f);
    return 0;
}
