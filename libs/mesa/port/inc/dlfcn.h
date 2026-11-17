#ifndef MESA_PORT_DLFCN_H
#define MESA_PORT_DLFCN_H
#ifdef __cplusplus
extern "C" {
#endif
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100
void *dlopen(const char *f, int m);
void *dlsym(void *h, const char *s);
int dlclose(void *h);
char *dlerror(void);

#ifdef __cplusplus
}
#endif
#endif
