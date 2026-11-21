#ifndef PS5_COMPAT_SYS_SYSINFO_H
#define PS5_COMPAT_SYS_SYSINFO_H
/*
 * <sys/sysinfo.h> does not exist in the PS5 sysroot.
 *
 * winpr/sysinfo/sysinfo.c includes it unconditionally on the non-Windows,
 * non-Apple path and reads exactly one field: info.uptime. Two ways out were
 * tried; defining _POSIX_C_SOURCE made that file take its clock_gettime()
 * branch but then broke half a dozen other translation units by hiding XSI and
 * BSD declarations, so the cheaper fix is this shim plus an error-returning
 * sysinfo() in the payload. When the call fails winpr leaves uptime at 0 and
 * falls back to raw gettimeofday() nanoseconds, which is still monotonic.
 */
struct sysinfo
{
    long uptime;
};

int sysinfo(struct sysinfo* info);

#endif /* PS5_COMPAT_SYS_SYSINFO_H */
