/* hello.c — проверка компилятора tcc на ArtyomX64.
 *
 * Скомпилировать и запустить на ОС:
 *
 *     exec tcc hello.c -o hello2
 *     exec hello2
 */

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
        char buf[64];

        strcpy(buf, "Hello from tcc on ArtyomX64!");
        printf("%s\n", buf);
        printf("argc = %d\n", argc);

        return 0;
}
