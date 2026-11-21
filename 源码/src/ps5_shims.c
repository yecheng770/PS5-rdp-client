/*
 * ps5_shims.c - implementations for libc entry points that the PS5 sysroot
 * headers may declare but target/lib/libc.a does not export.
 *
 * The list below is closed, not guessed: it came from one full
 * --whole-archive link of libwinpr3.a + libfreerdp3.a against the PS5
 * sysroot (scripts/link-test.sh), which reported exactly these symbols as
 * undefined. Keep it in sync with that script rather than adding shims
 * "just in case".
 *
 * Rule: behave like a sane libc, never like a silent success. Anything we
 * cannot honestly implement returns a failure code so the caller takes its
 * documented fallback path.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * getline - winpr/crt/string.c:GetLine() calls it on every non-Windows
 * build. Straight POSIX semantics: grow by chunks, -1 on EOF-before-read
 * and on hard errors.
 * ------------------------------------------------------------------ */
ssize_t getline(char** lineptr, size_t* n, FILE* stream)
{
    char* buf;
    size_t cap;
    size_t used = 0;
    int c;

    if (!lineptr || !n || !stream)
    {
        errno = EINVAL;
        return -1;
    }

    buf = *lineptr;
    cap = buf ? *n : 0;

    for (;;)
    {
        c = fgetc(stream);

        if (c == EOF)
        {
            if (used == 0)
                return -1;
            break;
        }

        if (used + 2 > cap)
        {
            size_t want = cap ? cap * 2 : 128;
            char* grown = realloc(buf, want);

            if (!grown)
                return -1;

            buf = grown;
            cap = want;
        }

        buf[used++] = (char)c;

        if (c == '\n')
            break;
    }

    buf[used] = '\0';
    *lineptr = buf;
    *n = cap;
    return (ssize_t)used;
}

/* ------------------------------------------------------------------ *
 * ctermid - trivially answerable, no syscall needed.
 * ------------------------------------------------------------------ */
char* ctermid(char* s)
{
    static char fallback[] = "/dev/tty";

    if (!s)
        return fallback;

    strcpy(s, fallback);
    return s;
}

/* ------------------------------------------------------------------ *
 * getprotobynumber - protocol name lookup. PS5 has no protocol database;
 * returning NULL makes callers fall back to numeric formatting, which is
 * exactly what we want for RDP logging.
 * ------------------------------------------------------------------ */
struct protoent;

struct protoent* getprotobynumber(int proto)
{
    (void)proto;
    return NULL;
}

/* ------------------------------------------------------------------ *
 * initgroups - supplementary group setup. Meaningless in the PS5
 * environment (one fixed uid, no group files).
 * ------------------------------------------------------------------ */
int initgroups(const char* user, gid_t group)
{
    (void)user;
    (void)group;
    errno = ENOSYS;
    return -1;
}

/* ------------------------------------------------------------------ *
 * accept4 - accept() plus SOCK_NONBLOCK / SOCK_CLOEXEC. winpr reaches it
 * from pipe.c:ConnectNamedPipe(). Implemented for real rather than stubbed
 * so the rdpdr channel path stays usable later.
 * ------------------------------------------------------------------ */
int accept4(int s, struct sockaddr* addr, socklen_t* addrlen, int flags)
{
    int fd = accept(s, addr, addrlen);

    if (fd < 0)
        return -1;

    if (flags & SOCK_CLOEXEC)
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

    if (flags & SOCK_NONBLOCK)
    {
        int fl = fcntl(fd, F_GETFL, 0);

        if (fl >= 0)
            (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }

    return fd;
}

/* ------------------------------------------------------------------ *
 * sysinfo - <sys/sysinfo.h> does not exist in the sysroot; see
 * ps5-compat/sys/sysinfo.h. winpr reads only info.uptime and already
 * falls back to gettimeofday() when the call fails, so failing is the
 * honest answer. Returning 0 with a zeroed uptime would freeze winpr's
 * uptime clock at zero instead.
 * ------------------------------------------------------------------ */
int sysinfo(struct sysinfo* info)
{
    if (info)
        memset(info, 0, sizeof(*info));

    errno = ENOSYS;
    return -1;
}

/* ------------------------------------------------------------------ *
 * Optional runtime facilities used through weak imports. Returning a
 * clear failure keeps OpenSSL and WinPR on their documented fallbacks.
 * ------------------------------------------------------------------ */
int getentropy(void* buffer, size_t length)
{
    (void)buffer;
    (void)length;
    errno = ENOSYS;
    return -1;
}

void* __dlopen(const char* path, int mode)
{
    (void)path;
    (void)mode;
    return NULL;
}

void* __dlsym(void* handle, const char* symbol)
{
    (void)handle;
    (void)symbol;
    return NULL;
}

int __dlclose(void* handle)
{
    (void)handle;
    return -1;
}

char* __dlerror(void)
{
    return (char*)"unsupported";
}

int __dladdr(const void* address, void* info)
{
    (void)address;
    (void)info;
    return 0;
}

int kernel_mprotect(pid_t pid, intptr_t address, size_t size, int protection)
{
    (void)pid;
    return mprotect((void*)address, size, protection);
}

/* Exposed only for the local diagnostics page. Zero means success. */
int g_psrdp_pipe2_stage;
int g_psrdp_pipe2_errno;

/* ------------------------------------------------------------------ *
 * pipe2 - PS5 app processes reject both pipe() and AF_UNIX socketpair.
 * Build an equivalent full-duplex descriptor pair through the proven
 * AF_INET loopback stack. The listener uses port 0 and is closed before
 * returning, so no fixed port remains allocated.
 * ------------------------------------------------------------------ */
int pipe2(int fds[2], int flags)
{
    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);
    int listener = -1;
    int client = -1;
    int server = -1;
    int saved;

    g_psrdp_pipe2_stage = 0;
    g_psrdp_pipe2_errno = 0;

    if (!fds || (flags & ~(O_CLOEXEC | O_NONBLOCK)))
    {
        errno = EINVAL;
        g_psrdp_pipe2_stage = 1;
        goto fail;
    }

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
    {
        g_psrdp_pipe2_stage = 2;
        goto fail;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (bind(listener, (struct sockaddr*)&address, sizeof(address)) != 0)
    {
        g_psrdp_pipe2_stage = 3;
        goto fail;
    }
    if (listen(listener, 1) != 0)
    {
        g_psrdp_pipe2_stage = 4;
        goto fail;
    }
    if (getsockname(listener, (struct sockaddr*)&address, &address_length) != 0)
    {
        g_psrdp_pipe2_stage = 5;
        goto fail;
    }

    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0)
    {
        g_psrdp_pipe2_stage = 6;
        goto fail;
    }
    if (connect(client, (struct sockaddr*)&address, sizeof(address)) != 0)
    {
        g_psrdp_pipe2_stage = 7;
        goto fail;
    }

    server = accept(listener, NULL, NULL);
    if (server < 0)
    {
        g_psrdp_pipe2_stage = 8;
        goto fail;
    }
    close(listener);
    listener = -1;

    fds[0] = server;
    fds[1] = client;

    /* PS5 rejects F_SETFD/FD_CLOEXEC on socket descriptors (v0.2.1,
     * stage 9, EINVAL). This application never execs a child image, so
     * inheritance across exec cannot occur. Keep the attempt for targets
     * that support it; accept only this verified unsupported operation.
     * Nonblocking, read/write and wait semantics remain mandatory.
     */
    if (flags & O_CLOEXEC)
    {
        for (int i = 0; i < 2; ++i)
        {
            if (fcntl(fds[i], F_SETFD, FD_CLOEXEC) < 0 && errno != EINVAL)
            {
                g_psrdp_pipe2_stage = 9;
                goto fail_pair;
            }
        }
    }

    if (flags & O_NONBLOCK)
    {
        for (int i = 0; i < 2; ++i)
        {
            int fl = fcntl(fds[i], F_GETFL, 0);
            if (fl < 0) {
                g_psrdp_pipe2_stage = 10;
                goto fail_pair;
            }
            int rc = fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
            int set_errno = errno;
            int actual = fcntl(fds[i], F_GETFL, 0);
            if (actual < 0 || !(actual & O_NONBLOCK)) {
                errno = rc < 0 ? set_errno : EIO;
                g_psrdp_pipe2_stage = 10;
                goto fail_pair;
            }
        }
    }

    return 0;

fail_pair:
    saved = errno;
    close(server);
    close(client);
    g_psrdp_pipe2_errno = saved;
    errno = saved;
    return -1;

fail:
    saved = errno;
    if (server >= 0)
        close(server);
    if (client >= 0)
        close(client);
    if (listener >= 0)
        close(listener);
    g_psrdp_pipe2_errno = saved;
    errno = saved;
    return -1;
}

/* ------------------------------------------------------------------ *
 * __cpu_model - libgcc's CPU feature bitmap, normally supplied by
 * compiler-rt builtins, which this SDK does not ship. __builtin_cpu_supports()
 * lowers to a load at offset 12 of this object, so it must be laid out as
 * {vendor, type, subtype, features[0]}.
 *
 * All-zero means "no SIMD features": FreeRDP's primitives dispatch then
 * takes its portable C paths. That is the correctness-first default - flip
 * individual bits (SSE=bit3, SSE2=bit4, SSE3=bit5, SSSE3=bit6, SSE4.1=bit7,
 * SSE4.2=bit8) only after a pixel-level comparison says the fast paths are
 * actually clean.
 * ------------------------------------------------------------------ */
unsigned int __cpu_model[4] = { 0u, 0u, 0u, 0u };

#ifdef __cplusplus
}
#endif
