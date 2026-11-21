#define _WITH_GETLINE 1
#ifndef PS5_COMPAT_H
#define PS5_COMPAT_H
/*
 * Forced-include shim for the PS5 payload SDK sysroot.
 *
 * The sysroot is FreeBSD-derived but trimmed: some POSIX/BSD declarations the
 * upstream sources expect are absent. Declaring them here keeps the sources
 * untouched; the actual implementations live in the payload (ps5_shims.c) and
 * return a failure code, so the delegate is safe.
 *
 * Only add a declaration here after nm(1) proves the symbol is missing from
 * target/lib/libc.a and libpthread.a.
 */
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Missing from target/include/pthread.h; winpr/thread/thread.c uses it. */
int pthread_setschedprio(pthread_t thread, int prio);

#ifdef __cplusplus
}
#endif

#endif /* PS5_COMPAT_H */
