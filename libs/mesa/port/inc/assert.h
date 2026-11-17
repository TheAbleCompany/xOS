#ifndef MESA_PORT_ASSERT_H
#define MESA_PORT_ASSERT_H
#ifdef __cplusplus
extern "C" {
#endif
void mesa_port_assert_fail(const char *expr, const char *file, int line) __attribute__((noreturn));
#define assert(e) ((e) ? (void)0 : mesa_port_assert_fail(#e, __FILE__, __LINE__))
#ifndef __cplusplus
#define static_assert _Static_assert
#endif

#ifdef __cplusplus
}
#endif
#endif
