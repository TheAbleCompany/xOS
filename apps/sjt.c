/* sjt.c — setjmp/longjmp demo (mini-libc + tcc).
 *
 * exec tcc sjt.c -o sjt2
 * exec sjt2
 */

#include <stdio.h>
#include <setjmp.h>

static jmp_buf env;

static int deep(int n)
{
        if (n == 0)
                longjmp(env, 777);
        return deep(n - 1);
}

int main(void)
{
        volatile int stage = 0;
        int r = setjmp(env);

        if (r == 0) {
                stage = 1;
                printf("first pass, jumping...\n");
                deep(100);
        } else {
                printf("longjmp returned %d, stage=%d\n", r, stage);
        }

        return 0;
}
