#ifndef MESA_PORT_ALLOCA_H
#define MESA_PORT_ALLOCA_H
#ifdef __cplusplus
extern "C" {
#endif
#define alloca(n) __builtin_alloca(n)

#ifdef __cplusplus
}
#endif
#endif
