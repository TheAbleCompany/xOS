#ifndef MESA_PORT_PTHREAD_H
#define MESA_PORT_PTHREAD_H
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned long pthread_t;
typedef struct { int v; } pthread_mutex_t;
typedef struct { int v; } pthread_cond_t;
typedef struct { int v; } pthread_once_t;
typedef struct { int v; } pthread_rwlock_t;
typedef unsigned int pthread_key_t;
typedef struct { int v; } pthread_barrier_t;
typedef struct { int v; } pthread_spinlock_t;
int pthread_barrier_init(pthread_barrier_t *b, const void *a, unsigned c);
int pthread_barrier_destroy(pthread_barrier_t *b);
int pthread_barrier_wait(pthread_barrier_t *b);
int pthread_spin_init(pthread_spinlock_t *l, int p);
int pthread_spin_destroy(pthread_spinlock_t *l);
int pthread_spin_lock(pthread_spinlock_t *l);
int pthread_spin_unlock(pthread_spinlock_t *l);
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
int pthread_key_create(pthread_key_t *k, void (*dtor)(void *));
int pthread_key_delete(pthread_key_t k);
void *pthread_getspecific(pthread_key_t k);
int pthread_setspecific(pthread_key_t k, const void *v);
int pthread_detach(pthread_t t);
int pthread_mutex_trylock(pthread_mutex_t *m);
#define PTHREAD_MUTEX_INITIALIZER {0}
#define PTHREAD_COND_INITIALIZER {0}
#define PTHREAD_ONCE_INIT {0}
#define PTHREAD_RWLOCK_INITIALIZER {0}
int pthread_mutex_init(pthread_mutex_t *m, const void *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_cond_init(pthread_cond_t *c, const void *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_once(pthread_once_t *o, void (*fn)(void));
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_create(pthread_t *t, const void *a, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);
int pthread_rwlock_init(pthread_rwlock_t *l, const void *a);
int pthread_rwlock_destroy(pthread_rwlock_t *l);
int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
int pthread_rwlock_unlock(pthread_rwlock_t *l);

#ifdef __cplusplus
}
#endif
#endif
