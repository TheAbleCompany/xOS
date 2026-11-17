#include <stdio.h>
#include <unistd.h>
int main(void){
    FILE *f = fopen("/tmp/m7.bin", "w+b");
    if (!f) { write(2,"fopen fail\n",11); return 2; }
    fwrite("0123456789ABCDEF", 1, 16, f);
    fflush(f);
    rewind(f);
    int c = fgetc(f);
    printf("first char = %c (0x%x)\n", c, c);
    fclose(f);
    return 0;
}
