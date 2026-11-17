#include <setjmp.h>
#include <stdio.h>
jmp_buf env;
int main(void){
    printf("before\n"); fflush(stdout);
    int r = setjmp(env);
    printf("after setjmp r=%d\n", r); fflush(stdout);
    if (r == 0) { printf("calling longjmp 7\n"); fflush(stdout); longjmp(env, 7); }
    printf("done r=%d\n", r); fflush(stdout);
    return 0;
}
