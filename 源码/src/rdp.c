/* FreeRDP client glue. Only the worker owns the instance and its GDI.
 * rdp_stop requests cancellation; it never waits for network teardown.
 * Detached pthreads are reaped by pthreads. A new session is admitted only
 * after the old worker has released every session resource.
 */
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#include <freerdp/freerdp.h>
#include <freerdp/error.h>
#include <freerdp/settings.h>
#include <freerdp/codecs.h>
#include <freerdp/update.h>
#include <freerdp/input.h>
#include <freerdp/gdi/gdi.h>
#include <winpr/synch.h>
#include <winpr/error.h>
#include <winpr/ssl.h>
#include <winpr/custom-crypto.h>
#include <winpr/wlog.h>
#include <winpr/crt.h>

#include "psrdp_glue.h"
#include "psrdp_audio.h"

extern int g_psrdp_pipe2_stage;
extern int g_psrdp_pipe2_errno;

/* State is copied under g_lock; callers never borrow worker-owned memory. */
rdp_state g_rdp = { .running = 0, .connected = 0, .phase = RDP_IDLE, .err = "" };
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_active;
static int g_stopping;
static int g_failed;
static HANDLE g_stop;
/* Published only in PreConnect, AFTER freerdp_connect resets abortEvent.
 * Protected by g_lock, including cancellation and removal before destruction. */
static freerdp *g_abort_instance;
static freerdp *g_live_instance;
static int g_mouse_buttons;
static config_t g_snapshot;
/* Worker/callback thread only: freerdp_connect may disconnect internally. */
static int g_postdisconnected;
static unsigned long long g_frames;
static unsigned long g_session;
static char g_log[32768];
static size_t g_log_used;
static int g_log_full;
static char g_failure[256];
static FILE *g_trace_file;

/* Stage evidence must outlive an application crash. No credentials are logged.
 *
 * The launch context is not guaranteed to expose a writable /data, so try the
 * usual spots and report which one opened. */
static const char *g_trace_path = "";

/* Crash path only: called from a signal/abort context, so it must not take
 * g_lock. Writes straight to the trace file and to stderr. */
static void emergency_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_trace_file) {
        fputs(line, g_trace_file);
        fputc('\n', g_trace_file);
        fflush(g_trace_file);
        (void)fsync(fileno(g_trace_file));
    }
    (void)!write(2, line, strlen(line));
    (void)!write(2, "\n", 1);
}

static void trace_open_locked(void)
{
    static const char *const candidates[] = {
        "/data/psrdp-last-session.log",
        "/temp0/psrdp-last-session.log",
        NULL
    };
    int i;
    if (g_trace_file) fclose(g_trace_file);
    g_trace_file = NULL;
    g_trace_path = "";
    for (i = 0; candidates[i]; i++) {
        FILE *file = fopen(candidates[i], "w");
        if (!file) continue;
        g_trace_file = file;
        g_trace_path = candidates[i];
        /* FTP runs under a different identity from the title process. */
        (void)chmod(candidates[i], 0644);
        fprintf(file, "PSrdp v%s session trace\n", PSRDP_VERSION);
        fflush(file);
        break;
    }
}

/* Caller holds g_lock. Retain earlier stage evidence instead of rotating it
 * away on frame/handle churn. Reserve the final 4 KiB for failures/exit. */
static void log_locked(int important, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    size_t len;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_trace_file) {
        fputs(line, g_trace_file);
        fputc('\n', g_trace_file);
        fflush(g_trace_file);
    }
    len = strlen(line);
    if (g_log_used + len + 2 >= (important ? sizeof(g_log) : sizeof(g_log) - 4096)) {
        g_log_full = 1;
        return;
    }
    memcpy(g_log + g_log_used, line, len);
    g_log_used += len;
    g_log[g_log_used++] = '\n';
    g_log[g_log_used] = 0;
}

static void diag(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_lock);
    log_locked(0, "%s", line);
    pthread_mutex_unlock(&g_lock);
}

/* The aligned-allocation shim owns no logger; it reports through here so its
 * evidence lands in the same trace as everything else. */
void psrdp_diag_line(const char *line)
{
    diag("%s", line);
}

/* FreeRDP's POSIX helper calls the payload SDK's inline syscall umask.
 * Native titles must not enter that payload-only syscall trampoline. This
 * wrapper deliberately skips a process-global mask; it does NOT emulate it.
 * Application storage is confined to the directory checked in runtime_test. */
BOOL __wrap_utils_set_umask(void)
{
    diag("PS5: process umask skipped; application directory permissions checked explicitly");
    return TRUE;
}

/* Record source locations, not library message text: authentication and
 * transport messages can contain user-supplied values. WLOG_FATAL is the one
 * exception - WinPR reserves it for failed assertions, where the text is a
 * compile-time condition string and is the only thing that names a crash. */
static BOOL library_log(const wLogMessage *msg)
{
    if (!msg || msg->Level < WLOG_WARN) return TRUE;
    if (msg->TextString && strncmp(msg->TextString, "[psrdp-merge]", 13) == 0) {
        diag("%s", msg->TextString);
        return TRUE;
    }
    if (msg->Level == WLOG_FATAL) {
        emergency_log("Freerdp library FATAL %s:%lu %s: %s",
                      msg->FileName ? msg->FileName : "?",
                      (unsigned long)msg->LineNumber,
                      msg->FunctionName ? msg->FunctionName : "?",
                      msg->TextString ? msg->TextString : "(no text)");
        return TRUE;
    }
    diag("FreeRDP library level=%lu %s:%lu %s (message values omitted)",
         (unsigned long)msg->Level, msg->FileName ? msg->FileName : "?",
         (unsigned long)msg->LineNumber, msg->FunctionName ? msg->FunctionName : "?");
    return TRUE;
}

static int library_logging(void)
{
    wLog *root = WLog_GetRoot();
    wLogCallbacks callbacks = { 0 };
    callbacks.message = library_log;
    if (!root || !WLog_SetLogAppenderType(root, WLOG_APPENDER_CALLBACK)) return 0;
    wLogAppender *appender = WLog_GetLogAppender(root);
    return appender && WLog_ConfigureAppender(appender, "callbacks", &callbacks)
        && WLog_SetLogLevel(root, WLOG_WARN) && WLog_OpenAppender(root);
}

/* The library aborts through here, and the default handler sends the
 * condition string through the logger this title suppresses. Wrap the handler
 * itself: condstr/file/fkt/line are compile-time constants, so nothing
 * user-supplied can leak, and this is the only place a crash names itself.
 * Callers were compiled against a noreturn prototype, so this must not
 * return - flush and abort exactly like the original. */
void __wrap_winpr_int_assert(const char *condstr, const char *file,
                             const char *fkt, size_t line)
{
    emergency_log("ASSERT FAILED: %s | %s:%lu | %s",
                  condstr ? condstr : "?", file ? file : "?",
                  (unsigned long)line, fkt ? fkt : "?");
    abort();
}

extern BOOL __real_freerdp_client_codecs_prepare(rdpCodecs *, UINT32, UINT32, UINT32);
extern void psrdp_aligned_summary(void);
BOOL __wrap_freerdp_client_codecs_prepare(rdpCodecs *codecs, UINT32 flags, UINT32 width, UINT32 height)
{
    /* 32bpp bitmap updates arrive planar-encoded, so the planar context is
     * mandatory: without it the first update calls
     * freerdp_planar_switch_bgr(NULL, ...) and aborts the title. 0.3.1 lost
     * that context because WinPR's aligned allocator refuses the 8.3+ MiB
     * planes on this platform (src/ps5_aligned.c carries the measurement and
     * the replacement). Keep the full set first and the narrower stages only
     * as a diagnosable fallback. */
    const UINT32 requested = flags;
    static const UINT32 ladder[] = {
        FREERDP_CODEC_PLANAR | FREERDP_CODEC_INTERLEAVED,
        FREERDP_CODEC_PLANAR,
        FREERDP_CODEC_INTERLEAVED,
        0
    };
    const size_t stages = sizeof(ladder) / sizeof(ladder[0]);

    diag("codecs prepare: requested=0x%08X size=%ux%u", requested, width, height);
    for (size_t i = 0; i < stages; i++)
    {
        diag("codecs prepare: try flags=0x%08X", ladder[i]);
        if (__real_freerdp_client_codecs_prepare(codecs, ladder[i], width, height))
        {
            diag("codecs prepare: OK flags=0x%08X planar=%p interleaved=%p",
                 ladder[i], (void *)codecs->planar, (void *)codecs->interleaved);
            psrdp_aligned_summary();
            return TRUE;
        }
        diag("codecs prepare: stage failed flags=0x%08X", ladder[i]);
    }
    diag("codecs prepare: FAILED all stages");
    psrdp_aligned_summary();
    return FALSE;
}

static void fail_v(rdp_failure failure, const char *fmt, va_list ap)
{
    char line[256];
    vsnprintf(line, sizeof(line), fmt, ap);
    pthread_mutex_lock(&g_lock);
    g_failed = 1;
    g_rdp.phase = RDP_ERROR;
    /* Keep the first failure as the cause; subsequent cleanup errors stay
     * in the multiline log rather than replacing it with 'disconnected'. */
    if (!g_failure[0]) {
        g_rdp.failure = failure;
        snprintf(g_failure, sizeof(g_failure), "%s", line);
        snprintf(g_rdp.err, sizeof(g_rdp.err), "%.159s", line);
    }
    log_locked(1, "ERROR: %s", line);
    pthread_mutex_unlock(&g_lock);
}

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fail_v(RDP_FAILURE_UNKNOWN, fmt, ap);
    va_end(ap);
}

static void fail_as(rdp_failure failure, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fail_v(failure, fmt, ap);
    va_end(ap);
}

static rdp_failure connect_failure(UINT32 error)
{
    switch (error) {
    case FREERDP_ERROR_DNS_ERROR:
    case FREERDP_ERROR_DNS_NAME_NOT_FOUND:
        return RDP_FAILURE_HOST;
    case FREERDP_ERROR_CONNECT_FAILED:
        return RDP_FAILURE_PORT;
    case FREERDP_ERROR_CONNECT_WRONG_PASSWORD:
        return RDP_FAILURE_PASSWORD;
    case FREERDP_ERROR_AUTHENTICATION_FAILED:
    case FREERDP_ERROR_CONNECT_LOGON_FAILURE:
    case FREERDP_ERROR_CONNECT_ACCESS_DENIED:
    case FREERDP_ERROR_INSUFFICIENT_PRIVILEGES:
    case FREERDP_ERROR_CONNECT_ACCOUNT_RESTRICTION:
    case FREERDP_ERROR_CONNECT_ACCOUNT_LOCKED_OUT:
    case FREERDP_ERROR_CONNECT_ACCOUNT_DISABLED:
    case FREERDP_ERROR_CONNECT_ACCOUNT_EXPIRED:
    case FREERDP_ERROR_CONNECT_LOGON_TYPE_NOT_GRANTED:
    case FREERDP_ERROR_CONNECT_NO_OR_MISSING_CREDENTIALS:
    case FREERDP_ERROR_CONNECT_PASSWORD_EXPIRED:
    case FREERDP_ERROR_CONNECT_PASSWORD_CERTAINLY_EXPIRED:
    case FREERDP_ERROR_CONNECT_PASSWORD_MUST_CHANGE:
        return RDP_FAILURE_LOGIN;
    case FREERDP_ERROR_POST_CONNECT_FAILED:
    case FREERDP_ERROR_CONNECT_ACTIVATION_TIMEOUT:
    case FREERDP_ERROR_MCS_CONNECT_INITIAL_ERROR:
    case FREERDP_ERROR_TLS_CONNECT_FAILED:
    case FREERDP_ERROR_SECURITY_NEGO_CONNECT_FAILED:
    case FREERDP_ERROR_CONNECT_TRANSPORT_FAILED:
        return RDP_FAILURE_SETUP;
    default:
        return RDP_FAILURE_UNKNOWN;
    }
}

static int normal_remote_exit(UINT32 info)
{
    return info == ERRINFO_RPC_INITIATED_LOGOFF ||
           info == ERRINFO_RPC_INITIATED_DISCONNECT_BY_USER ||
           info == ERRINFO_LOGOFF_BY_USER;
}

static int stopping(void);

static void session_exit(freerdp *inst, const char *stage, int requested)
{
    UINT32 error = freerdp_get_last_error(inst->context);
    UINT32 info = freerdp_error_info(inst);
    UINT channel = getChannelError(inst->context);
    if (stopping() || (!channel && normal_remote_exit(info)) ||
        (!channel && requested && !error && !info && g_frames)) {
        diag("%s: exit (rdp=0x%08X info=0x%08X channel=0x%08X)", stage, error, info, channel);
        return;
    }
    rdp_failure failure = connect_failure(error);
    if (failure != RDP_FAILURE_LOGIN && failure != RDP_FAILURE_PASSWORD)
        failure = g_frames ? RDP_FAILURE_INTERRUPTED : RDP_FAILURE_SETUP;
    fail_as(failure, "%s (rdp=0x%08X info=0x%08X channel=0x%08X)",
            stage, error, info, channel);
}

/* Public API: copies a NUL-terminated snapshot, no borrowed pointers.
 * Intended header declaration: void rdp_diagnostics(char *dst, size_t size);
 * Contains no host, user, password, or FreeRDP settings dumps. */
void rdp_diagnostics(char *dst, size_t size);
void rdp_diagnostics(char *dst, size_t size)
{
    if (!dst || !size)
        return;
    pthread_mutex_lock(&g_lock);
    snprintf(dst, size,
             "session=%lu active=%d stopping=%d frames=%llu\n"
             "first failure: %s\n%s%s",
             g_session, g_active, g_stopping, g_frames,
             g_failure[0] ? g_failure : "none", g_log,
             g_log_full ? "[log capacity reached; earlier evidence retained]\n" : "");
    pthread_mutex_unlock(&g_lock);
}

void psrdp_get_state(rdp_state *out)
{
    if (!out) return;
    pthread_mutex_lock(&g_lock);
    *out = g_rdp;
    pthread_mutex_unlock(&g_lock);
}

static int stopping(void)
{
    int stop;
    pthread_mutex_lock(&g_lock);
    stop = g_stopping;
    pthread_mutex_unlock(&g_lock);
    return stop;
}

static int wait_test(HANDLE event, const char *kind, const char *step, DWORD expected)
{
    DWORD result, error;
    int saved_errno;
    diag("event %s %s: entering WaitForSingleObject", kind, step);
    errno = 0;
    result = WaitForSingleObject(event, 1);
    error = GetLastError();
    saved_errno = errno;
    diag("event %s %s: wait=0x%08lX expected=0x%08lX%s error=%lu errno=%d",
         kind, step, (unsigned long)result, (unsigned long)expected,
         result == WAIT_FAILED ? " WAIT_FAILED" : "", (unsigned long)error, saved_errno);
    return result == expected;
}

static int event_test(BOOL manual)
{
    const char *kind = manual ? "manual-reset" : "auto-reset";
    HANDLE event;
    BOOL set1, reset1, set2, reset2, closed;
    int ok = 1;
    DWORD error;
    errno = 0;
    diag("event %s: entering CreateEvent", kind);
    event = CreateEvent(NULL, manual, FALSE, NULL);
    error = GetLastError();
    diag("event %s CreateEvent=%s error=%lu errno=%d pipe-stage=%d/%d",
         kind, event ? "OK" : "FAILED", (unsigned long)error, errno,
         g_psrdp_pipe2_stage, g_psrdp_pipe2_errno);
    if (!event)
        return 0;
    if (!wait_test(event, kind, "initial timeout", WAIT_TIMEOUT)) ok = 0;
    set1 = SetEvent(event);
    diag("event %s SetEvent=%d error=%lu", kind, set1, (unsigned long)GetLastError());
    if (!set1) ok = 0;
    if (!wait_test(event, kind, "after set", WAIT_OBJECT_0)) ok = 0;
    if (!wait_test(event, kind, "second wait (reset semantics)",
                   manual ? WAIT_OBJECT_0 : WAIT_TIMEOUT)) ok = 0;
    reset1 = ResetEvent(event);
    diag("event %s ResetEvent=%d error=%lu", kind, reset1, (unsigned long)GetLastError());
    if (!reset1) ok = 0;
    if (!wait_test(event, kind, "after reset", WAIT_TIMEOUT)) ok = 0;
    /* Also reset a signalled event BEFORE consuming it. */
    set2 = SetEvent(event);
    diag("event %s second SetEvent=%d error=%lu", kind, set2, (unsigned long)GetLastError());
    reset2 = ResetEvent(event);
    diag("event %s signalled ResetEvent=%d error=%lu", kind, reset2, (unsigned long)GetLastError());
    if (!set2 || !reset2) ok = 0;
    if (!wait_test(event, kind, "set/reset/wait", WAIT_TIMEOUT)) ok = 0;
    closed = CloseHandle(event);
    diag("event %s CloseHandle=%d error=%lu", kind, closed, (unsigned long)GetLastError());
    return ok && closed;
}

static int pipe_test(void)
{
    int fd[2] = { -1, -1 };
    int ok = 1, rc, saved_errno;
    errno = 0;
    rc = pipe2(fd, O_CLOEXEC);
    saved_errno = errno;
    diag("pipe2: rc=%d errno=%d stage=%d stage-errno=%d", rc, saved_errno,
         g_psrdp_pipe2_stage, g_psrdp_pipe2_errno);
    if (rc < 0)
        return 0;
    for (int i = 0; i < 2; ++i) {
        int flags, setrc, verified, cloexec;
        errno = 0;
        flags = fcntl(fd[i], F_GETFL);
        saved_errno = errno;
        diag("pipe fd[%d] F_GETFL=%d errno=%d", i, flags, saved_errno);
        errno = 0;
        setrc = flags < 0 ? -1 : fcntl(fd[i], F_SETFL, flags | O_NONBLOCK);
        saved_errno = errno;
        diag("pipe fd[%d] F_SETFL(nonblock)=%d errno=%d", i, setrc, saved_errno);
        errno = 0;
        verified = fcntl(fd[i], F_GETFL);
        saved_errno = errno;
        diag("pipe fd[%d] verify F_GETFL=%d errno=%d", i, verified, saved_errno);
        errno = 0;
        cloexec = fcntl(fd[i], F_GETFD);
        saved_errno = errno;
        diag("pipe fd[%d] F_GETFD=%d errno=%d", i, cloexec, saved_errno);
        if (flags < 0 || setrc < 0 || verified < 0 || !(verified & O_NONBLOCK) ||
            (cloexec < 0 && saved_errno != EINVAL)) ok = 0;
        if (cloexec < 0 || !(cloexec & FD_CLOEXEC))
            diag("pipe fd[%d]: CLOEXEC unavailable; allowed (application never execs)", i);
    }
    if (ok) {
        struct timeval limit = { .tv_sec = 1, .tv_usec = 0 };
        for (int i = 0; i < 2; ++i) {
            int rr = setsockopt(fd[i], SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
            int wr = setsockopt(fd[i], SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
            diag("pipe fd[%d]: probe timeouts receive=%d send=%d", i, rr, wr);
            if (rr != 0 || wr != 0) ok = 0;
        }
    }
    if (ok) {
        char value = 0;
        struct pollfd ready = { .fd = fd[0], .events = POLLIN };
        struct timespec t0 = { 0 }, t1 = { 0 };
        diag("pipe empty probe: entering clock_gettime/recv");
        int clock0 = clock_gettime(CLOCK_MONOTONIC, &t0);
        errno = 0;
        rc = (int)recv(fd[0], &value, 1, 0);
        saved_errno = errno;
        int clock1 = clock_gettime(CLOCK_MONOTONIC, &t1);
        long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        diag("pipe empty probe elapsed=%ldms (must be <250) clocks=%d/%d",
             elapsed_ms, clock0, clock1);
        if (clock0 != 0 || clock1 != 0 || elapsed_ms < 0 || elapsed_ms >= 250) ok = 0;
        diag("pipe empty read=%d errno=%d (must be EAGAIN)", rc, saved_errno);
        if (rc != -1 || (saved_errno != EAGAIN && saved_errno != EWOULDBLOCK)) ok = 0;
        rc = (int)send(fd[1], "x", 1, 0);
        diag("pipe write=%d errno=%d", rc, errno);
        if (rc != 1) ok = 0;
        rc = poll(&ready, 1, 100);
        diag("pipe poll=%d revents=0x%X errno=%d", rc, ready.revents, errno);
        if (rc != 1 || !(ready.revents & POLLIN)) ok = 0;
        rc = (int)recv(fd[0], &value, 1, 0);
        if (rc != 1 || value != 'x') ok = 0;
        rc = poll(&ready, 1, 0);
        diag("pipe drained poll=%d (must be 0)", rc);
        if (rc != 0) ok = 0;
    }
    for (int i = 0; i < 2; ++i) {
        rc = close(fd[i]);
        diag("pipe fd[%d] close=%d errno=%d", i, rc, errno);
        if (rc) ok = 0;
    }
    return ok;
}

static int multiwait_test(void)
{
    HANDLE events[2] = { NULL, NULL };
    int ok = 1;
    DWORD result;
    for (int i = 0; i < 2; ++i) {
        events[i] = CreateEvent(NULL, TRUE, FALSE, NULL);
        diag("multiwait CreateEvent[%d]=%s error=%lu", i,
             events[i] ? "OK" : "FAILED", (unsigned long)GetLastError());
        if (!events[i]) ok = 0;
    }
    if (ok) {
        result = WaitForMultipleObjects(2, events, FALSE, 1);
        diag("multiwait initial=0x%08lX error=%lu", (unsigned long)result,
             (unsigned long)GetLastError());
        if (result != WAIT_TIMEOUT) ok = 0;
        if (!SetEvent(events[1])) ok = 0;
        result = WaitForMultipleObjects(2, events, FALSE, 1);
        diag("multiwait set second=0x%08lX expected=0x%08lX error=%lu",
             (unsigned long)result, (unsigned long)(WAIT_OBJECT_0 + 1),
             (unsigned long)GetLastError());
        if (result != WAIT_OBJECT_0 + 1) ok = 0;
        if (!ResetEvent(events[1])) ok = 0;
        result = WaitForMultipleObjects(2, events, FALSE, 1);
        diag("multiwait reset=0x%08lX error=%lu", (unsigned long)result,
             (unsigned long)GetLastError());
        if (result != WAIT_TIMEOUT) ok = 0;
    }
    for (int i = 0; i < 2; ++i)
        if (events[i] && !CloseHandle(events[i])) ok = 0;
    return ok;
}

/* 0.2.9 died inside gdi_init with nothing but "initialization failed" left
 * behind: every step of gdi_init_ex except one allocation is already proven
 * on device, and that allocation is a single 8.3 MB winpr_aligned_calloc for
 * the primary bitmap. 0.3.0 therefore does two things.
 *
 * 1. It measures the exact request first, so the trace says whether the
 *    allocator refuses 8 MB - that answer is the diagnosis either way.
 * 2. It retries GDI with a framebuffer this title already owns, which skips
 *    the allocation entirely and leaves the connect path with no 8 MB ask.
 *
 * The buffer is static BSS, 64-byte aligned (one cache line) so the decode
 * path can write whole rows. GDI never frees it: pfree is NULL. */
static BYTE g_gdi_fb[PSRDP_REMOTE_MAX_W * PSRDP_REMOTE_MAX_H * 4] __attribute__((aligned(64)));

static void gdi_alloc_probe(void)
{
    /* Size matters: a 4 KiB refusal and an 8 MB refusal have different causes,
     * and the answer decides whether GDI can ever own a bitmap here. */
    void *small = winpr_aligned_malloc(4096, 16);
    void *mid = winpr_aligned_malloc(256 * 1024, 16);
    void *big = winpr_aligned_malloc(1920 * 1080 * 4, 16);
    void *plain = calloc(1, 1920 * 1080 * 4);
    void *aligned = winpr_aligned_calloc(1920 * 4, 1080, 16);
    diag("probe GDI bitmap: calloc(8.3MB)=%s aligned_calloc(8.3MB,16)=%s",
         plain ? "OK" : "NULL", aligned ? "OK" : "NULL");
    diag("probe aligned_malloc: 4KiB=%s 256KiB=%s 8.3MB=%s",
         small ? "OK" : "NULL", mid ? "OK" : "NULL", big ? "OK" : "NULL");
    if (small) winpr_aligned_free(small);
    if (mid) winpr_aligned_free(mid);
    if (big) winpr_aligned_free(big);
    if (plain) free(plain);
    if (aligned) winpr_aligned_free(aligned);
}

static BOOL cb_begin_paint(rdpContext *ctx)
{
    rdpGdi *gdi = ctx ? ctx->gdi : NULL;
    if (!gdi || !gdi->primary || !gdi->primary->hdc ||
        !gdi->primary->hdc->hwnd || !gdi->primary->hdc->hwnd->invalid) {
        fail("BeginPaint: missing GDI invalid region");
        return FALSE;
    }
    gdi->primary->hdc->hwnd->invalid->null = TRUE;
    gdi->primary->hdc->hwnd->ninvalid = 0;
    return TRUE;
}

/* Decoded pixels before tiling, for separating decoder colour faults from
 * scanout faults. Two 480x270 snapshots per session, bounded at 389 KiB each. */
static void save_color_sample(const rdpGdi *gdi, const char *path)
{
    FILE *fp=fopen(path,"wb");
    if (!fp) return;
    fprintf(fp,"P6\n480 270\n255\n");
    for (int y=0; y<270; ++y) {
        unsigned char row[480*3];
        int sy=psrdp_map_coordinate(y,270,gdi->height);
        const unsigned char *src=gdi->primary_buffer+(size_t)sy*gdi->stride;
        for (int x=0; x<480; ++x) {
            int sx=psrdp_map_coordinate(x,480,gdi->width);
            row[x*3]=src[sx*4+2]; row[x*3+1]=src[sx*4+1]; row[x*3+2]=src[sx*4];
        }
        if (fwrite(row,1,sizeof(row),fp)!=sizeof(row)) break;
    }
    fclose(fp);
}

/* Track exact coverage until the first desktop is ready. A bounding box
 * can span the screen while leaving untouched pixels between rectangles. */
#define COVER_WORDS ((PSRDP_REMOTE_MAX_W + 63) / 64)
#define FIRST_FRAME_BUDGET_MS 1000
static uint64_t g_first_coverage[PSRDP_REMOTE_MAX_H][COVER_WORDS];
static unsigned g_first_remaining;
static uint64_t g_first_started;
static int g_first_pending;

static uint64_t frame_clock_ms(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void first_frame_reset(rdpGdi *gdi)
{
    psrdp_picture_reset();
    memset(gdi->primary_buffer, 0, (size_t)gdi->stride * gdi->height);
    memset(g_first_coverage, 0, sizeof(g_first_coverage));
    g_first_remaining = (unsigned)gdi->width * (unsigned)gdi->height;
    g_first_started = 0;
    g_first_pending = 1;
}

static void first_frame_cover(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1;) {
            int offset = x & 63;
            int count = 64 - offset;
            if (count > x1 - x) count = x1 - x;
            uint64_t mask = (UINT64_MAX >> (64-count)) << offset;
            uint64_t *word = &g_first_coverage[y][x >> 6];
            g_first_remaining -= (unsigned)__builtin_popcountll(mask & ~*word);
            *word |= mask;
            x += count;
        }
    }
}

static void first_frame_publish(rdpGdi *gdi)
{
    if (!g_first_pending || !g_first_started) return;
    uint64_t elapsed = frame_clock_ms() - g_first_started;
    if (g_first_remaining && elapsed < FIRST_FRAME_BUDGET_MS) return;
    g_first_pending = 0;
    psrdp_blit(gdi->primary_buffer, gdi->width, gdi->height, (int)gdi->stride,
               0, 0, gdi->width, gdi->height);
    diag("first desktop: GDI=%dx%d stride=%u after %llu ms; uncovered=%u%s",
         gdi->width, gdi->height, gdi->stride, (unsigned long long)elapsed,
         g_first_remaining, g_first_remaining ? " (wait limit)" : "");
}

static BOOL cb_end_paint(rdpContext *ctx)
{
    rdpGdi *gdi = ctx ? ctx->gdi : NULL;
    unsigned long long frames;
    if (!gdi || !gdi->primary_buffer || gdi->width <= 0 || gdi->height <= 0 ||
        gdi->width > PSRDP_REMOTE_MAX_W || gdi->height > PSRDP_REMOTE_MAX_H ||
        gdi->stride > INT_MAX ||
        gdi->stride < (UINT32)gdi->width * 4 ||
        (size_t)gdi->height > SIZE_MAX / gdi->stride) {
        fail("EndPaint: invalid decoded GDI surface");
        return FALSE;
    }
    HGDI_WND wnd = gdi->primary->hdc->hwnd;
    if (wnd->invalid->null || wnd->ninvalid <= 0) return TRUE;
    int x0 = gdi->width, y0 = gdi->height, x1 = 0, y1 = 0;
    for (int i = 0; i < wnd->ninvalid; ++i) {
        const GDI_RGN *r = &wnd->cinvalid[i];
        int l = r->x < 0 ? 0 : r->x;
        int t = r->y < 0 ? 0 : r->y;
        int64_t right = (int64_t)r->x + r->w;
        int64_t bottom = (int64_t)r->y + r->h;
        if (l >= gdi->width || t >= gdi->height || right <= l || bottom <= t) continue;
        int rr = right > gdi->width ? gdi->width : (int)right;
        int bb = bottom > gdi->height ? gdi->height : (int)bottom;
        if (l < x0) x0 = l;
        if (t < y0) y0 = t;
        if (rr > x1) x1 = rr;
        if (bb > y1) y1 = bb;
        if (g_first_pending) first_frame_cover(l, t, rr, bb);
    }
    if (x0 >= x1 || y0 >= y1) return TRUE;
    if (g_first_pending) {
        if (!g_first_started) g_first_started = frame_clock_ms();
        first_frame_publish(gdi);
        if (g_first_pending) return TRUE;
    } else {
        psrdp_blit(gdi->primary_buffer, gdi->width, gdi->height, (int)gdi->stride,
                   x0, y0, x1, y1);
    }
    pthread_mutex_lock(&g_lock);
    frames = ++g_frames;
    pthread_mutex_unlock(&g_lock);
    if (frames==30) save_color_sample(gdi,"/data/psrdp-color-early.ppm");
    if (frames==300) save_color_sample(gdi,"/data/psrdp-color-later.ppm");
    return TRUE;
}

static BOOL cb_desktop(rdpContext *ctx)
{
    UINT32 width, height;
    BOOL rc;
    if (!ctx || !ctx->settings || !ctx->gdi) {
        fail("DesktopResize: missing settings/GDI");
        return FALSE;
    }
    width = freerdp_settings_get_uint32(ctx->settings, FreeRDP_DesktopWidth);
    height = freerdp_settings_get_uint32(ctx->settings, FreeRDP_DesktopHeight);
    if (!width || !height || width > PSRDP_REMOTE_MAX_W || height > PSRDP_REMOTE_MAX_H) {
        fail("DesktopResize: invalid size %ux%u", width, height);
        return FALSE;
    }
    if (ctx->gdi->primary_buffer == g_gdi_fb) {
        /* Our buffer is the surface: resize in place, never let GDI swap in
         * a library-allocated bitmap behind our back. */
        if ((size_t)width * (size_t)height * 4u > sizeof(g_gdi_fb)) {
            fail("DesktopResize: %ux%u exceeds the 3840x2160 framebuffer", width, height);
            return FALSE;
        }
        rc = gdi_resize_ex(ctx->gdi, width, height, width * 4, PIXEL_FORMAT_BGRA32,
                           g_gdi_fb, NULL);
    } else {
        rc = gdi_resize(ctx->gdi, width, height);
    }
    diag("DesktopResize: %ux%u gdi_resize=%d", width, height, rc);
    if (!rc) fail("DesktopResize: gdi_resize failed");
    else first_frame_reset(ctx->gdi);
    return rc;
}

static BOOL cb_load_channels(freerdp *inst)
{
    /* FreeRDP recreates the channel manager before invoking this callback. */
    if (!psrdp_audio_attach(inst))
        diag("audio: unavailable; continuing with desktop and input");
    return TRUE;
}

static BOOL cb_preconnect(freerdp *inst)
{
    int stop;
    /* freerdp_connect_begin resets abortEvent before calling PreConnect.
     * Publishing here closes the stop-before-connect/reset race. */
    pthread_mutex_lock(&g_lock);
    stop = g_stopping;
    if (!stop) g_abort_instance = inst;
    log_locked(0, "PreConnect: stop=%d; cancellation context %s", stop,
               stop ? "not published" : "published");
    pthread_mutex_unlock(&g_lock);
    return stop ? FALSE : TRUE;
}

/* Record the server's actual bitmap depth once per depth, then delegate
 * unchanged to GDI. A 32-bit destination alone does not prove a 32-bit source. */
static pBitmapUpdate g_bitmap_update;
static UINT32 g_bitmap_depth_seen;
static UINT32 g_planar_header_seen;

static BOOL cb_bitmap_update(rdpContext *ctx, const BITMAP_UPDATE *update)
{
    if (update && update->rectangles) {
        for (UINT32 i = 0; i < update->number; ++i) {
            const BITMAP_DATA *bmp=&update->rectangles[i];
            UINT32 bpp = bmp->bitsPerPixel;
            if (bpp==32 && bmp->compressed && bmp->bitmapLength && bmp->bitmapDataStream) {
                unsigned header=bmp->bitmapDataStream[0];
                unsigned key=header&15u;
                if (!(g_planar_header_seen&(1u<<key))) {
                    g_planar_header_seen|=1u<<key;
                    diag("color planar: header=0x%02x loss=%u subsampling=%u rect=%ux%u at=%u,%u",
                         header,header&7u,(header>>3)&1u,bmp->width,bmp->height,
                         bmp->destLeft,bmp->destTop);
                }
            }
            UINT32 bit = bpp < 32 ? (1u << bpp) : bpp == 32 ? 1u : 0u;
            if (bit && !(g_bitmap_depth_seen & bit)) {
                g_bitmap_depth_seen |= bit;
                diag("color source: server bitmap=%u bpp; destination BGRA32 (8 bits/channel)", bpp);
            }
        }
    }
    return g_bitmap_update ? g_bitmap_update(ctx, update) : FALSE;
}

static BOOL cb_postconnect(freerdp *inst)
{
    BOOL ok;
    rdpGdi *gdi;
    g_postdisconnected = 0;
    if (stopping()) return FALSE;
    gdi_alloc_probe();
    /* First the exact 0.2.9 call, so the trace still answers "was it the
     * library bitmap?". Then the same init on our own buffer. */
    UINT32 width=freerdp_settings_get_uint32(inst->context->settings,FreeRDP_DesktopWidth);
    UINT32 height=freerdp_settings_get_uint32(inst->context->settings,FreeRDP_DesktopHeight);
    if (!width || !height || width>PSRDP_REMOTE_MAX_W || height>PSRDP_REMOTE_MAX_H) {
        fail("PostConnect: negotiated size outside supported bounds"); return FALSE;
    }
    ok = gdi_init_ex(inst, PIXEL_FORMAT_BGRA32, 0, NULL, winpr_aligned_free);
    diag("PostConnect: gdi_init_ex(library bitmap)=%d", ok);
    if (!ok) {
        if (stopping()) return FALSE;
        ok = gdi_init_ex(inst, PIXEL_FORMAT_BGRA32, width * 4, g_gdi_fb, NULL);
        diag("PostConnect: gdi_init_ex(own framebuffer stride=%u)=%d", width*4,ok);
    }
    if (!ok) {
        fail("PostConnect: gdi_init failed with library bitmap and own framebuffer");
        return FALSE;
    }
    gdi = inst->context->gdi;
    first_frame_reset(gdi);
    diag("color negotiated: requested=32 effective=%u; GDI format=0x%08x; VideoOut BGRA8 sRGB",
         freerdp_settings_get_uint32(inst->context->settings, FreeRDP_ColorDepth),
         gdi ? gdi->dstFormat : 0);
    diag("Windows DPI client settings at PostConnect: desktop=%u%% device=%u%%",
         freerdp_settings_get_uint32(inst->context->settings, FreeRDP_DesktopScaleFactor),
         freerdp_settings_get_uint32(inst->context->settings, FreeRDP_DeviceScaleFactor));
    g_bitmap_depth_seen = g_planar_header_seen = 0;
    g_bitmap_update = inst->context->update->BitmapUpdate;
    inst->context->update->BitmapUpdate = cb_bitmap_update;
    inst->context->update->BeginPaint = cb_begin_paint;
    inst->context->update->EndPaint = cb_end_paint;
    inst->context->update->DesktopResize = cb_desktop;
    diag("PostConnect: GDI BGRA32 ready; buffer=%s %dx%d stride=%u",
         gdi && gdi->primary_buffer == g_gdi_fb ? "own" : "library",
         gdi ? gdi->width : 0, gdi ? gdi->height : 0, gdi ? gdi->stride : 0);
    /* Input only becomes reachable here: the client input channel exists as
     * soon as the instance is connected, and nothing may touch it earlier. */
    pthread_mutex_lock(&g_lock);
    g_mouse_buttons = 0;
    g_live_instance = inst;
    pthread_mutex_unlock(&g_lock);
    diag("PostConnect: input channel published");
    return TRUE;
}

static void cb_postdisconnect(freerdp *inst)
{
    pthread_mutex_lock(&g_lock);
    if (g_live_instance == inst)
        g_live_instance = NULL;
    pthread_mutex_unlock(&g_lock);
    g_postdisconnected = 1;
    gdi_free(inst);
    diag("PostDisconnect: gdi_free complete");
}

/* ---- input forwarding ------------------------------------------------
 * The transport is shared with the frame callback, so it lives behind the
 * session lock. Every entry point tolerates "no session": the display loop
 * calls these on every poll, connected or not. Nothing here logs, because a
 * bad input mapping must not turn into a log storm at 60 Hz. */

static freerdp *lock_live(void)
{
    freerdp *inst = g_live_instance;
    if (inst && (!inst->context || !inst->context->input))
        inst = NULL;
    return inst;
}

int rdp_send_flags_ready(void)
{
    int ready;
    pthread_mutex_lock(&g_lock);
    ready = lock_live() != NULL;
    pthread_mutex_unlock(&g_lock);
    return ready;
}

int rdp_desktop_width(void)
{
    int w = 0;
    pthread_mutex_lock(&g_lock);
    {
        freerdp *inst = lock_live();
        if (inst && inst->context->gdi)
            w = (int)inst->context->gdi->width;
    }
    pthread_mutex_unlock(&g_lock);
    return w;
}

int rdp_desktop_height(void)
{
    int h = 0;
    pthread_mutex_lock(&g_lock);
    {
        freerdp *inst = lock_live();
        if (inst && inst->context->gdi)
            h = (int)inst->context->gdi->height;
    }
    pthread_mutex_unlock(&g_lock);
    return h;
}

int rdp_send_mouse(int x, int y, int buttons)
{
    int rc = 0;
    static const UINT16 button_flags[3] = {
        PTR_FLAGS_BUTTON1, PTR_FLAGS_BUTTON2, PTR_FLAGS_BUTTON3
    };
    pthread_mutex_lock(&g_lock);
    {
        freerdp *inst = lock_live();
        if (inst) {
            rdpGdi *gdi = inst->context->gdi;
            int w = (gdi && gdi->width > 1) ? (int)gdi->width : 1920;
            int h = (gdi && gdi->height > 1) ? (int)gdi->height : 1080;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x > w - 1) x = w - 1;
            if (y > h - 1) y = h - 1;
            buttons &= 0x1f;
            rc = freerdp_input_send_mouse_event(inst->context->input, PTR_FLAGS_MOVE,
                                               (UINT16)x, (UINT16)y) ? 1 : 0;
            for (int i = 0; i < 5 && rc; ++i) {
                int bit = 1 << i;
                if ((buttons ^ g_mouse_buttons) & bit) {
                    if (i < 3) {
                        UINT16 flags = button_flags[i];
                        if (buttons & bit) flags |= PTR_FLAGS_DOWN;
                        rc = freerdp_input_send_mouse_event(inst->context->input, flags,
                                                           (UINT16)x, (UINT16)y) ? 1 : 0;
                    } else {
                        UINT16 flags = i == 3 ? PTR_XFLAGS_BUTTON1 : PTR_XFLAGS_BUTTON2;
                        if (buttons & bit) flags |= PTR_XFLAGS_DOWN;
                        rc = freerdp_input_send_extended_mouse_event(inst->context->input, flags,
                                                                    (UINT16)x, (UINT16)y) ? 1 : 0;
                    }
                    if (rc) g_mouse_buttons = (g_mouse_buttons & ~bit) | (buttons & bit);
                }
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int rdp_send_wheel(int x, int y, int notches)
{
    int rc = 0;
    INT32 rot = (INT32)notches * 120;
    UINT16 flags = PTR_FLAGS_WHEEL;
    unsigned mag;

    if (rot < 0) {
        mag = (unsigned)(-rot);
        flags |= PTR_FLAGS_WHEEL_NEGATIVE;
    } else {
        mag = (unsigned)rot;
    }
    if (mag > 0xFFu) mag = 0xFFu;
    flags |= (UINT16)(mag & 0xFFu);

    pthread_mutex_lock(&g_lock);
    {
        freerdp *inst = lock_live();
        if (inst) {
            rdpGdi *gdi = inst->context->gdi;
            int w = (gdi && gdi->width > 1) ? (int)gdi->width : 1920;
            int h = (gdi && gdi->height > 1) ? (int)gdi->height : 1080;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x > w - 1) x = w - 1;
            if (y > h - 1) y = h - 1;
            rc = freerdp_input_send_mouse_event(inst->context->input, flags,
                                                (UINT16)x, (UINT16)y) ? 1 : 0;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int rdp_send_key(int down, int scancode)
{
    int rc = 0;
    if (scancode <= 0)
        return 0;

    pthread_mutex_lock(&g_lock);
    {
        freerdp *inst = lock_live();
        if (inst) {
            rc = freerdp_input_send_keyboard_event_ex(inst->context->input,
                                                      down ? TRUE : FALSE,
                                                      FALSE,
                                                      (UINT32)scancode) ? 1 : 0;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

static int setting_result(BOOL rc, const char *key)
{
    diag("settings %s: %s", key, rc ? "OK" : "FAILED");
    if (!rc) fail("settings rejected: %s", key);
    return rc ? 1 : 0;
}

static int configure(rdpSettings *settings, const config_t *cfg)
{
    int ok = 1;
    int desktop_w=1920, desktop_h=1080;
    psrdp_resolution(cfg->res,&desktop_w,&desktop_h);
    UINT32 port = cfg->port > 0 && cfg->port <= 65535 ? (UINT32)cfg->port : 3389;
    /* Evaluate EVERY setter, even when an earlier setter failed. No values
     * (particularly the password) are ever copied into diagnostic messages. */
#define SET_SETTING(type, key, value) \
    do { if (!setting_result(freerdp_settings_set_##type(settings, key, value), #key)) ok = 0; } while (0)
    SET_SETTING(string, FreeRDP_ServerHostname, cfg->host);
    SET_SETTING(string, FreeRDP_Username, cfg->user[0] ? cfg->user : "user");
    SET_SETTING(string, FreeRDP_Password, cfg->pass);
    SET_SETTING(uint32, FreeRDP_ServerPort, port);
    SET_SETTING(uint32, FreeRDP_DesktopWidth, (UINT32)desktop_w);
    SET_SETTING(uint32, FreeRDP_DesktopHeight, (UINT32)desktop_h);
    SET_SETTING(uint32, FreeRDP_ColorDepth, 32);
    SET_SETTING(uint32, FreeRDP_DesktopScaleFactor, cfg->desktop_scale);
    SET_SETTING(uint32, FreeRDP_DeviceScaleFactor, 100);
    const UINT64 monitor_flags = freerdp_settings_get_uint64(settings, FreeRDP_MonitorOverrideFlags);
    SET_SETTING(uint64, FreeRDP_MonitorOverrideFlags, monitor_flags |
                FREERDP_MONITOR_OVERRIDE_DESKTOP_SCALE | FREERDP_MONITOR_OVERRIDE_DEVICE_SCALE);
    /* Verified 0.6.4 server bitmap header 0x33 used YCoCg color loss 3.
     * Request lossless planar RGB; retain the decoder for server compatibility.
     * FreeRDP's capability intersection preserves an explicit FALSE. */
    SET_SETTING(bool, FreeRDP_DrawAllowDynamicColorFidelity, FALSE);
    SET_SETTING(bool, FreeRDP_DrawAllowColorSubsampling, FALSE);
    SET_SETTING(bool, FreeRDP_IgnoreCertificate, TRUE);
    SET_SETTING(bool, FreeRDP_DeactivateClientDecoding, FALSE);
    SET_SETTING(bool, FreeRDP_SupportGraphicsPipeline, FALSE);
    SET_SETTING(bool, FreeRDP_RemoteFxCodec, FALSE);
    SET_SETTING(bool, FreeRDP_RemoteFxImageCodec, FALSE);
    SET_SETTING(bool, FreeRDP_NSCodec, FALSE);
    SET_SETTING(bool, FreeRDP_JpegCodec, FALSE);
    SET_SETTING(bool, FreeRDP_GfxProgressive, FALSE);
    SET_SETTING(bool, FreeRDP_GfxProgressiveV2, FALSE);
    SET_SETTING(bool, FreeRDP_GfxH264, FALSE);
    SET_SETTING(bool, FreeRDP_GfxAVC444, FALSE);
    SET_SETTING(bool, FreeRDP_GfxAVC444v2, FALSE);
#undef SET_SETTING
    diag("settings complete: %s; requested remote=%dx%d/32bpp",
         ok ? "OK" : "FAILED",desktop_w,desktop_h);
    diag("Windows DPI request: desktop=%u%% device=100%%", cfg->desktop_scale);
    diag("color policy: lossless bitmap requested; dynamic fidelity=0 subsampling=0");
    return ok;
}

static int runtime_test(void)
{
    unsigned char random_bytes[32];
    const char *home = getenv("HOME");
    int path_ok = 1;
    if (!home || !home[0]) {
        static const char *const dirs[] = { "/data/psrdp", "/temp0/psrdp", NULL };
        struct stat info;
        int i;
        path_ok = 0;
        diag("runtime HOME: absent; probing private directories");
        for (i = 0; dirs[i] && !path_ok; i++) {
            int rc = mkdir(dirs[i], 0700);
            int saved = errno;
            if (rc != 0 && saved != EEXIST) {
                diag("runtime dir %s: mkdir failed errno=%d", dirs[i], saved);
                continue;
            }
            if (stat(dirs[i], &info) != 0 || !S_ISDIR(info.st_mode)) {
                diag("runtime dir %s: not a directory", dirs[i]);
                continue;
            }
            (void)chmod(dirs[i], 0700);
            if (setenv("HOME", dirs[i], 1) != 0) {
                diag("runtime dir %s: setenv HOME failed", dirs[i]);
                continue;
            }
            diag("runtime dir %s: usable; HOME set", dirs[i]);
            path_ok = 1;
        }
        if (!path_ok)
            diag("runtime HOME: absent and no usable private directory (not fatal)");
    } else {
        diag("runtime HOME: provided by application environment");
    }
    BOOL ssl_ok = winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);
    int random_rc = winpr_RAND(random_bytes, sizeof(random_bytes));
    memset(random_bytes, 0, sizeof(random_bytes));
    diag("runtime SSL init=%d secure random=%d (0 is success); values omitted", ssl_ok, random_rc);
    return path_ok && ssl_ok && random_rc == 0;
}

static void *rdp_thread(void *arg)
{
    freerdp *inst = NULL;
    HANDLE stop = NULL;
    config_t cfg;
    (void)arg;
    int connect_attempted = 0;
    int pipe_ok = 0, manual_ok = 0, multi_ok = 0, runtime_ok = 0;
    DWORD previous_count = (DWORD)-1;
    pthread_mutex_lock(&g_lock);
    cfg = g_snapshot;
    /* Remove the extra long-lived password copy after taking ownership. */
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    log_locked(0, "thread: entered (detached; no HTTP join)");
    pthread_mutex_unlock(&g_lock);

    /* Event reset drains the pipe until EAGAIN. Never enter WinPR event
     * code after the raw pipe probe has rejected nonblocking behaviour. */
    diag("probe pipe: entering");
    pipe_ok = pipe_test();
    if (!pipe_ok) {
        fail("pipe self-test failed; unsafe event probes and connection skipped");
        goto cleanup;
    }
    diag("probe manual event: entering");
    manual_ok = event_test(TRUE);
    if (!manual_ok) {
        fail("manual event self-test failed; multiwait and connection skipped");
        goto cleanup;
    }
    diag("auto-reset: skipped; upstream WinPR does not implement it; core uses manual-reset");
    diag("probe multiwait: entering");
    multi_ok = multiwait_test();
    diag("probe runtime: entering");
    runtime_ok = runtime_test();
    diag("self-test: pipe=%d manual=%d multiwait=%d runtime=%d",
         pipe_ok, manual_ok, multi_ok, runtime_ok);
    if (!pipe_ok || !manual_ok || !multi_ok) {
        fail("self-test failed (pipe=%d manual=%d multi=%d runtime=%d)",
             pipe_ok, manual_ok, multi_ok, runtime_ok);
        goto cleanup;
    }
    /* HOME and a writable private directory are not needed to connect or to
     * decode frames, so they are evidence, not a gate. */
    if (!runtime_ok)
        diag("runtime self-test inconclusive (runtime=%d); continuing anyway", runtime_ok);
    if (stopping()) goto cleanup;
    pthread_mutex_lock(&g_lock);
    errno = 0;
    g_stop = stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    log_locked(0, "stop CreateEvent=%s error=%lu errno=%d pipe-stage=%d/%d",
               stop ? "OK" : "FAILED", (unsigned long)GetLastError(), errno,
               g_psrdp_pipe2_stage, g_psrdp_pipe2_errno);
    pthread_mutex_unlock(&g_lock);
    if (!stop) {
        fail("stop CreateEvent failed after self-test; connection blocked");
        goto cleanup;
    }
    if (stopping()) goto cleanup;
    diag("FreeRDP library diagnostic callback: %s", library_logging() ? "OK" : "FAILED");
    diag("freerdp_new: entering");
    inst = freerdp_new();
    diag("freerdp_new: %s", inst ? "OK" : "FAILED");
    if (!inst) {
        fail("freerdp_new failed");
        goto cleanup;
    }
    diag("freerdp_context_new: entering");
    if (!freerdp_context_new(inst)) {
        fail("freerdp_context_new failed");
        goto cleanup;
    }
    diag("freerdp_context_new: OK");
    inst->PreConnect = cb_preconnect;
    inst->LoadChannels = cb_load_channels;
    inst->PostConnect = cb_postconnect;
    inst->PostDisconnect = cb_postdisconnect;
    if (!configure(inst->context->settings, &cfg)) goto cleanup;
    memset(cfg.pass, 0, sizeof(cfg.pass));
    if (stopping()) goto cleanup;
    g_postdisconnected = 0;
    connect_attempted = 1;
    diag("connect: freerdp_connect entered");
    if (!freerdp_connect(inst)) {
        UINT32 error = freerdp_get_last_error(inst->context);
        const char *name = freerdp_get_last_error_name(error);
        const char *text = freerdp_get_error_connect_string(error);
        if (stopping() || error == FREERDP_ERROR_CONNECT_CANCELLED)
            diag("connect: cancelled (0x%08X %s)", error, name ? name : "?");
        else
            fail_as(connect_failure(error), "connect failed (0x%08X %s: %.96s)",
                    error, name ? name : "?", text ? text : "?");
        goto cleanup;
    }
    pthread_mutex_lock(&g_lock);
    if (!g_stopping) {
        g_rdp.connected = 1;
        g_rdp.phase = RDP_LIVE;
    }
    log_locked(0, "connect: succeeded; stop=%d", g_stopping);
    pthread_mutex_unlock(&g_lock);

    for (;;) {
        /* The inspected FreeRDP implementation appends two internal channel
         * handles with an overly loose bound. Give it spare backing storage,
         * but advertise 63 and never WAIT on more than 64 including stop. */
        HANDLE handles[66] = { NULL };
        DWORD count, result;
        if (stopping()) break;
        result = WaitForSingleObject(stop, 0);
        if (result == WAIT_OBJECT_0) break;
        if (result != WAIT_TIMEOUT) {
            fail("stop wait: %s result=0x%08lX error=%lu errno=%d",
                 result == WAIT_FAILED ? "WAIT_FAILED" : "unexpected", (unsigned long)result,
                 (unsigned long)GetLastError(), errno);
            break;
        }
        if (freerdp_shall_disconnect_context(inst->context)) {
            session_exit(inst, "session requested exit", 1);
            break;
        }
        handles[0] = stop;
        count = freerdp_get_event_handles(inst->context, handles + 1, 63);
        if (count != previous_count) {
            diag("event handles: FreeRDP=%lu total=%lu limit=64",
                 (unsigned long)count, (unsigned long)(count + 1));
            previous_count = count;
        }
        if (!count || count > 63) {
            fail("event handles invalid: FreeRDP=%lu (allowed 1..63)", (unsigned long)count);
            break;
        }
        result = WaitForMultipleObjects(count + 1, handles, FALSE, 100);
        if (result == WAIT_FAILED) {
            fail("event wait WAIT_FAILED error=%lu errno=%d handles=%lu",
                 (unsigned long)GetLastError(), errno, (unsigned long)(count + 1));
            break;
        }
        if (result == WAIT_OBJECT_0 || stopping()) break;
        if (result != WAIT_TIMEOUT && result >= WAIT_OBJECT_0 + count + 1) {
            fail("event wait unexpected result=0x%08lX", (unsigned long)result);
            break;
        }
        /* Also service timers/housekeeping after finite wait timeouts. */
        if (!freerdp_check_event_handles(inst->context)) {
            session_exit(inst, "freerdp_check_event_handles failed", 0);
            break;
        }
        first_frame_publish(inst->context->gdi);
    }

cleanup:
    /* Unpublish before disconnect/free. A concurrent HTTP stop can no longer
     * dereference the context; disconnect and all callbacks run unlocked. */
    pthread_mutex_lock(&g_lock);
    g_abort_instance = NULL;
    g_live_instance = NULL;
    log_locked(1, "exit: cleanup entered; stop=%d frames=%llu", g_stopping, g_frames);
    pthread_mutex_unlock(&g_lock);
    if (inst) {
        if (connect_attempted && !g_postdisconnected) {
            BOOL rc = freerdp_disconnect(inst);
            diag("freerdp_disconnect: %d", rc);
            if (!rc) diag("freerdp_disconnect failed during cleanup");
        }
        /* Defensive fallback for partial PostConnect/failed connect paths. */
        if (inst->context && inst->context->gdi) cb_postdisconnect(inst);
        freerdp_context_free(inst);
        diag("freerdp_context_free: complete");
        freerdp_free(inst);
        diag("freerdp_free: complete");
    }
    memset(&cfg, 0, sizeof(cfg));
    pthread_mutex_lock(&g_lock);
    if (g_stop) {
        if (!CloseHandle(g_stop)) {
            g_failed = 1;
            if (!g_failure[0]) {
                snprintf(g_failure, sizeof(g_failure), "stop CloseHandle failed");
                snprintf(g_rdp.err, sizeof(g_rdp.err), "stop CloseHandle failed");
            }
            log_locked(1, "ERROR: stop CloseHandle failed error=%lu", (unsigned long)GetLastError());
        }
        g_stop = NULL;
    }
    g_rdp.connected = 0;
    g_rdp.running = 0;
    g_rdp.phase = g_failed ? RDP_ERROR : RDP_IDLE;
    g_active = 0;
    log_locked(1, "exit: resources released; running=0; detached thread returning");
    if (g_trace_file) { fclose(g_trace_file); g_trace_file = NULL; }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int rdp_start(void)
{
    pthread_attr_t attr;
    pthread_t thread;
    int rc, destroy_rc;
    pthread_mutex_lock(&g_lock);
    if (g_active) {
        log_locked(0, "start rejected: previous worker still active (stop=%d)", g_stopping);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    trace_open_locked();
    log_locked(0, "trace file: %s",
               g_trace_path[0] ? g_trace_path : "unavailable (memory buffer only)");
    ++g_session;
    g_log_used = 0;
    g_log[0] = 0;
    g_log_full = 0;
    g_failure[0] = 0;
    g_failed = 0;
    g_stopping = 0;
    g_frames = 0;
    g_rdp.running = 0;
    g_rdp.connected = 0;
    g_rdp.phase = RDP_IDLE;
    g_rdp.err[0] = 0;
    g_rdp.failure = RDP_FAILURE_UNKNOWN;
    g_snapshot = g_cfg;
    g_snapshot.host[sizeof(g_snapshot.host) - 1] = 0;
    g_snapshot.user[sizeof(g_snapshot.user) - 1] = 0;
    g_snapshot.pass[sizeof(g_snapshot.pass) - 1] = 0;
    log_locked(0, "start: configuration snapshotted; diagnostics reset for session %lu", g_session);
    g_stop = NULL;
    rc = pthread_attr_init(&attr);
    log_locked(0, "thread pthread_attr_init=%d", rc);
    if (rc) goto start_failed;
    rc = pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
    log_locked(0, "thread pthread_attr_setstacksize(4MiB)=%d", rc);
    if (rc) { pthread_attr_destroy(&attr); goto start_failed; }
    rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    log_locked(0, "thread pthread_attr_setdetachstate=%d", rc);
    if (!rc) {
        /* Publish running BEFORE create; worker cannot finish before start
         * publishes state because it must first acquire this same mutex. */
        g_active = 1;
        g_rdp.running = 1;
        g_rdp.phase = RDP_CONNECTING;
        rc = pthread_create(&thread, &attr, rdp_thread, NULL);
        log_locked(0, "thread pthread_create=%d", rc);
    }
    destroy_rc = pthread_attr_destroy(&attr);
    log_locked(0, "thread pthread_attr_destroy=%d", destroy_rc);
    if (rc) goto start_failed;
    pthread_mutex_unlock(&g_lock);
    return 0;

start_failed:
    g_failed = 1;
    g_active = 0;
    g_rdp.running = 0;
    g_rdp.connected = 0;
    g_rdp.phase = RDP_ERROR;
    snprintf(g_failure, sizeof(g_failure), "start failed (%d); see stage log", rc);
    snprintf(g_rdp.err, sizeof(g_rdp.err), "%.159s", g_failure);
    log_locked(1, "ERROR: %s", g_failure);
    if (g_stop) {
        BOOL closed = CloseHandle(g_stop);
        log_locked(1, "start cleanup stop CloseHandle=%d error=%lu", closed,
                   (unsigned long)GetLastError());
        g_stop = NULL;
    }
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    pthread_mutex_unlock(&g_lock);
    return rc == -2 ? -2 : -3;
}

void rdp_stop(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_active && !g_stopping) {
        BOOL rc;
        g_stopping = 1;
        log_locked(1, "stop: requested; HTTP returns without join; worker owns cleanup");
        if (g_stop) {
            rc = SetEvent(g_stop);
            log_locked(1, "stop: SetEvent=%d error=%lu", rc, (unsigned long)GetLastError());
        } else {
            log_locked(1, "stop: self-test/preparation in progress; no stop handle yet");
        }
        if (g_abort_instance) {
            rc = freerdp_abort_connect_context(g_abort_instance->context);
            log_locked(1, "stop: freerdp_abort_connect_context=%d error=%lu",
                       rc, (unsigned long)GetLastError());
        } else {
            log_locked(1, "stop: context not published; worker/PreConnect will observe request");
        }
    }
    pthread_mutex_unlock(&g_lock);
}
