#include <internal/types.h>
#include <internal/mm/mem_list.h>
#include <unistd.h>

static void dbg(const char *s, int n) { write(1, s, n); }
static void dbgx(unsigned long v) {
    char b[19]; b[0]='0'; b[1]='x';
    for (int i=0;i<16;i++) b[2+i] = "0123456789abcdef"[(v >> (60-4*i)) & 0xF];
    b[18]='\n'; write(1,b,19);
}

static void init(void)    { mem_list_init(); }
static void cleanup(void) { mem_list_cleanup(); }

int __libc_start_main(
    int (*main_fn)(int argc, char *argv[]),
    int argc,
    char *argv[]
)
{
    int exit_code;
    dbg("argc=",5); dbgx((unsigned long)argc);
    dbg("argv=",5); dbgx((unsigned long)argv);
    if (argv) {
        dbg("argv[0]=",8); dbgx((unsigned long)argv[0]);
        dbg("argv[1]=",8); dbgx((unsigned long)argv[1]);
    }
    init();
    dbg("2",1);
    exit_code = main_fn(argc, argv);
    dbg("3",1);
    cleanup();
    dbg("4",1);
    return exit_code;
}
