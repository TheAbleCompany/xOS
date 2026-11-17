#ifndef MESA_PORT_SIGNAL_H
#define MESA_PORT_SIGNAL_H
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*sighandler_t)(int);
#define SIGINT 2
#define SIGSEGV 11
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
sighandler_t signal(int sig, sighandler_t fn);
int raise(int sig);

#ifdef __cplusplus
}
#endif
#endif
