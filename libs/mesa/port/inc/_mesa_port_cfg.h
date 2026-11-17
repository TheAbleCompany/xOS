#ifndef __MESA_PORT_BUILD_CFG
#define __MESA_PORT_BUILD_CFG
#ifdef __cplusplus
extern "C" {
#endif
#define PACKAGE_VERSION "25.2.5"
#define PACKAGE_BUGREPORT "https://gitlab.freedesktop.org/mesa/mesa/-/issues"
#define MESA_GIT_SHA1 ""
#define HAVE_OPENGL 1
#define HAVE_OPENGL_ES_1 0
#define HAVE_OPENGL_ES_2 0
#define HAVE_OPENGL_ES_3 0
#define BLAKE3_NO_SSE2 1
#define BLAKE3_NO_SSE41 1
#define BLAKE3_NO_AVX2 1
#define BLAKE3_NO_AVX512 1

/* On glibc, src/util/compiler.h skips its `uint` typedef because
 * __USE_MISC is defined; provide the type here unconditionally.
 * A duplicate typedef of the same type is valid in gnu11/C++. */
typedef unsigned int uint;

#ifdef __cplusplus
}
#endif
#endif
