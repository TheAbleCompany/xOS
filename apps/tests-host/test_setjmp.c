/*
 * tests-host/test_setjmp.c — тест setjmp/longjmp mini-libc (хост-шим).
 *
 * Проверяем то, что важно для tcc (libtcc.c использует setjmp как
 * механизм отката ошибок компиляции):
 *   1) первый возврат setjmp = 0;
 *   2) longjmp(env, N) => setjmp вернёт N;
 *   3) longjmp(env, 0) => setjmp вернёт 1;
 *   4) callee-saved регистры восстановлены (глубокие вызовы);
 *   5) rsp восстановлен (longjmp из глубокого стека);
 *   6) цикл setjmp/longjmp (как в tcc error_jmp_buf).
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
        if (cond) { printf("OK   %s\n", (msg)); } \
        else { printf("FAIL %s\n", (msg)); failures++; } \
} while (0)

/* 4) изменяем ВСЕ callee-saved регистры, потом longjmp */
static void deep_clobber(jmp_buf env, int depth, int val)
{
        volatile long a = 1, b = 2, c = 3, d = 4, e = 5, f = 6;

        a += depth; b *= depth; c -= depth; d += depth; e *= depth; f -= depth;

        if (depth > 0)
                deep_clobber(env, depth - 1, val);

        /* Используем переменные, чтобы регистры реально занимались. */
        asm volatile("" : : "r"(a), "r"(b), "r"(c),
                          "r"(d), "r"(e), "r"(f));
        longjmp(env, val);
}

/* 6) цикл: setjmp -> работа -> ошибка -> longjmp (паттерн libtcc.c) */
static jmp_buf err_jmp;
static int work_passes = 0;
static const char *work_log = "";

static void fail_compile(const char *what)
{
        work_log = what;
        longjmp(err_jmp, 1);
}

static int try_compile(int pass)
{
        jmp_buf saved;

        /* Паттерн libtcc.c: setjmp(...) == 0 -> полезли компилировать */
        if (setjmp(err_jmp) == 0) {
                if (pass == 0) {
                        work_passes++;
                        return 0;       /* "успешная компиляция" */
                }
                work_passes++;
                fail_compile("syntax error in pass");
                return 0;
        } else {
                /* откат после longjmp — как tcc_error_noabort */
                (void)saved;
                return -1;
        }
}

int main(void)
{
        jmp_buf env;
        volatile int stage = 0;
        int ret;

        /* 1) первый возврат */
        ret = setjmp(env);
        if (ret == 0) {
                CHECK(1, "setjmp first return == 0");
                stage = 42;
                longjmp(env, 5);
                CHECK(0, "longjmp returned (must not!)");
                return 1;
        }

        /* 2) значение из longjmp; stage (volatile) сохранился */
        CHECK(ret == 5, "longjmp(env,5) -> setjmp == 5");
        CHECK(stage == 42, "volatile local preserved across longjmp");

        /* 3) longjmp(env, 0) == 1 */
        ret = setjmp(env);
        if (ret == 0)
                longjmp(env, 0);
        CHECK(ret == 1, "longjmp(env,0) -> setjmp == 1");

        /* 4,5) глубокий стек + забитые callee-saved */
        ret = setjmp(env);
        if (ret == 0) {
                deep_clobber(env, 50, 77);
                CHECK(0, "deep_clobber returned (must not!)");
                return 1;
        }
        CHECK(ret == 77, "longjmp from depth=50, all callee-saved restored");

        /* 6) цикл ошибок как в libtcc */
        CHECK(try_compile(0) == 0, "try_compile: success pass");
        CHECK(try_compile(1) == -1, "try_compile: error pass -> rollback");
        CHECK(work_passes == 2, "both passes executed");
        CHECK(work_log[0] != 0, "state visible after longjmp");

        if (failures == 0) {
                printf("test_setjmp: ALL PASSED\n");
                return 0;
        }
        printf("test_setjmp: %d FAILURES\n", failures);
        return 1;
}
