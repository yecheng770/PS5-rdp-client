/* PS5 socket flags use SO_NBIO, as documented by the starter's console_curl.
 * One linker wrapper covers the app, WinPR, FreeRDP and TLS static archives.
 * Regular files retain libc semantics. Never consume missing varargs. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/socket.h>

#ifndef PSRDP_SOCKET_NBIO
#define PSRDP_SOCKET_NBIO 0x1200
#endif
extern int __real_fcntl(int fd, int cmd, ...);

int __wrap_fcntl(int fd, int cmd, ...)
{
    int intarg = 0;
    void *ptrarg = 0;
    enum { NONE, INTEGER, POINTER } kind = NONE;
    switch (cmd) {
    case F_GETFD: case F_GETFL: case F_GETOWN: break;
    case F_DUPFD: case F_SETFD: case F_SETFL: case F_SETOWN:
#ifdef F_DUPFD_CLOEXEC
    case F_DUPFD_CLOEXEC:
#endif
#ifdef F_DUP2FD
    case F_DUP2FD:
#endif
        kind = INTEGER; break;
    case F_GETLK: case F_SETLK: case F_SETLKW:
        kind = POINTER; break;
    default: errno = EINVAL; return -1;
    }
    if (kind != NONE) {
        va_list ap; va_start(ap, cmd);
        if (kind == INTEGER) intarg = va_arg(ap, int);
        else ptrarg = va_arg(ap, void *);
        va_end(ap);
    }
    int socktype = 0;
    socklen_t len = sizeof(socktype);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &socktype, &len) == 0) {
        if (cmd == F_GETFD || cmd == F_SETFD) return 0; /* title never execs */
        if (cmd == F_SETFL) {
            int on = (intarg & O_NONBLOCK) != 0;
            if (setsockopt(fd, SOL_SOCKET, PSRDP_SOCKET_NBIO, &on, sizeof(on)) != 0)
                return -1;
            int actual = -1; len = sizeof(actual);
            if (getsockopt(fd, SOL_SOCKET, PSRDP_SOCKET_NBIO, &actual, &len) != 0)
                return -1;
            if (!!actual != on) { errno = EIO; return -1; }
            return 0;
        }
        if (cmd == F_GETFL) {
            int on = 0; len = sizeof(on);
            if (getsockopt(fd, SOL_SOCKET, PSRDP_SOCKET_NBIO, &on, &len) != 0)
                return -1;
            return O_RDWR | (on ? O_NONBLOCK : 0);
        }
    }
    if (kind == POINTER) return __real_fcntl(fd, cmd, ptrarg);
    if (kind == INTEGER) return __real_fcntl(fd, cmd, intarg);
    return __real_fcntl(fd, cmd);
}
