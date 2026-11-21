/* PS5 libc workaround: WinPR's aligned allocator refuses large blocks here.
 *
 * Measured in the 0.3.1 device trace: winpr_aligned_calloc(8.3 MiB, 16) and
 * the equivalent winpr_aligned_malloc(8.3 MiB, *) return NULL, while plain
 * calloc of the same size succeeds and 4 KiB / 256 KiB aligned blocks succeed
 * too. WinPR builds _aligned_* on the platform aligned_alloc/posix_memalign,
 * so the rejection is in the libc primitive for large sizes, not in the heap.
 *
 * What that cost us: freerdp_bitmap_planar_context_reset() allocates four
 * planes (8.3 / 12.4 / 8.3 / 8.3 MiB at 1920x1080), which could never succeed.
 * freerdp_client_codecs_prepare() then reported failure, the client degraded
 * to the interleaved codec only, and the first 32bpp planar bitmap update
 * called freerdp_planar_switch_bgr(NULL, ...) and aborted the title with no
 * picture drawn. That abort is the silent exit.
 *
 * Fix: keep the WinPR contract, drop the libc primitive. Align inside a plain
 * malloc block and keep the block address in a header placed immediately
 * before the returned pointer, so the matching free can recover it. Every
 * entry point of the family is redirected with --wrap (see APP_WRAP_SYMBOLS)
 * because both sides must agree on that header layout.
 *
 * This file logs through a counter snapshot, never through the logger from
 * inside the allocator: allocation happens deep inside the library and the
 * logger takes a mutex the caller may already hold.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSRDP_ALIGN_MAGIC ((size_t)0x50535244414C4E31ULL)

extern void psrdp_diag_line(const char *line);

typedef struct
{
    void *base;
    size_t size;
    size_t magic;
} psrdp_align_hdr;

typedef struct
{
    size_t size;
    size_t alignment;
    int ok;
} psrdp_align_event;

#define PSRDP_ALIGN_EVENTS 24

static size_t g_live_blocks;
static size_t g_live_bytes;
static size_t g_live_peak;
static psrdp_align_event g_events[PSRDP_ALIGN_EVENTS];
static size_t g_event_next;
static size_t g_event_total;

static void record(size_t size, size_t alignment, int ok)
{
    if (size < (size_t)(1u << 20)) return;
    g_events[g_event_next].size = size;
    g_events[g_event_next].alignment = alignment;
    g_events[g_event_next].ok = ok;
    g_event_next = (g_event_next + 1) % PSRDP_ALIGN_EVENTS;
    if (g_event_total < PSRDP_ALIGN_EVENTS) g_event_total++;
}

/* Safe point only: called from application code, never from the allocator. */
void psrdp_aligned_summary(void)
{
    char line[256];
    size_t i;
    size_t start = (g_event_total == PSRDP_ALIGN_EVENTS) ? g_event_next : 0;
    snprintf(line, sizeof(line),
             "aligned pool: live=%lu blocks %lu B, peak=%lu B, events=%lu",
             (unsigned long)g_live_blocks, (unsigned long)g_live_bytes,
             (unsigned long)g_live_peak, (unsigned long)g_event_total);
    psrdp_diag_line(line);
    for (i = 0; i < g_event_total; i++)
    {
        const psrdp_align_event *event = &g_events[(start + i) % PSRDP_ALIGN_EVENTS];
        snprintf(line, sizeof(line), "aligned event: %lu B align=%lu -> %s",
                 (unsigned long)event->size, (unsigned long)event->alignment,
                 event->ok ? "OK" : "NULL");
        psrdp_diag_line(line);
    }
}

static int is_pow2(size_t value)
{
    return (value != 0) && ((value & (value - 1)) == 0);
}

static psrdp_align_hdr *hdr_from(void *ptr)
{
    psrdp_align_hdr *hdr;
    if (!ptr) return NULL;
    hdr = (psrdp_align_hdr *)((char *)ptr - sizeof(psrdp_align_hdr));
    return (hdr->magic == PSRDP_ALIGN_MAGIC) ? hdr : NULL;
}

static void *aligned_alloc_ex(size_t size, size_t alignment, size_t offset)
{
    psrdp_align_hdr *hdr;
    char *base;
    size_t total;
    size_t start;
    size_t aligned;

    if ((size == 0) || (offset >= size)) return NULL;
    if (alignment == 0) alignment = sizeof(void *);
    if (!is_pow2(alignment)) return NULL;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    if (size > SIZE_MAX - alignment - sizeof(psrdp_align_hdr) - offset) return NULL;

    total = size + alignment + sizeof(psrdp_align_hdr) + offset;
    base = (char *)malloc(total);
    if (!base)
    {
        record(size, alignment, 0);
        return NULL;
    }

    start = (size_t)base + sizeof(psrdp_align_hdr) + offset;
    aligned = (start + (alignment - 1)) & ~(alignment - 1);
    aligned -= offset;
    if (aligned < (size_t)base + sizeof(psrdp_align_hdr)) aligned += alignment;

    hdr = (psrdp_align_hdr *)(aligned - sizeof(psrdp_align_hdr));
    hdr->base = base;
    hdr->size = size;
    hdr->magic = PSRDP_ALIGN_MAGIC;

    g_live_blocks++;
    g_live_bytes += size;
    if (g_live_bytes > g_live_peak) g_live_peak = g_live_bytes;
    record(size, alignment, 1);
    return (void *)aligned;
}

static void aligned_free_ex(void *ptr)
{
    psrdp_align_hdr *hdr = hdr_from(ptr);
    if (!hdr) return;
    hdr->magic = 0;
    if (g_live_blocks > 0) g_live_blocks--;
    if (g_live_bytes >= hdr->size) g_live_bytes -= hdr->size;
    free(hdr->base);
}

void *__wrap_winpr_aligned_malloc(size_t size, size_t alignment)
{
    return aligned_alloc_ex(size, alignment, 0);
}

void *__wrap_winpr_aligned_calloc(size_t count, size_t size, size_t alignment)
{
    size_t total;
    void *ptr;

    if ((count == 0) || (size == 0)) return NULL;
    if (count > SIZE_MAX / size) return NULL;
    total = count * size;
    ptr = aligned_alloc_ex(total, alignment, 0);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void __wrap_winpr_aligned_free(void *ptr)
{
    aligned_free_ex(ptr);
}

void *__wrap_winpr_aligned_realloc(void *ptr, size_t size, size_t alignment)
{
    psrdp_align_hdr *hdr;
    void *next;
    size_t copy;

    if (!ptr) return aligned_alloc_ex(size, alignment, 0);
    hdr = hdr_from(ptr);
    if (!hdr) return NULL;
    if (size == 0)
    {
        aligned_free_ex(ptr);
        return NULL;
    }
    next = aligned_alloc_ex(size, alignment, 0);
    if (!next) return NULL;
    copy = (hdr->size < size) ? hdr->size : size;
    memcpy(next, ptr, copy);
    aligned_free_ex(ptr);
    return next;
}

void *__wrap_winpr_aligned_recalloc(void *ptr, size_t count, size_t size,
                                    size_t alignment)
{
    size_t total;
    psrdp_align_hdr *hdr;
    void *next;
    size_t copy;

    if ((count == 0) || (size == 0))
    {
        aligned_free_ex(ptr);
        return NULL;
    }
    if (count > SIZE_MAX / size) return NULL;
    total = count * size;

    if (!ptr)
    {
        next = aligned_alloc_ex(total, alignment, 0);
        if (next) memset(next, 0, total);
        return next;
    }

    hdr = hdr_from(ptr);
    if (!hdr) return NULL;
    /* Match WinPR: a block that is already large enough is kept as is. */
    if (hdr->size > total + alignment) return ptr;

    next = aligned_alloc_ex(total, alignment, 0);
    if (!next) return NULL;
    copy = (hdr->size < total) ? hdr->size : total;
    memcpy(next, ptr, copy);
    memset((char *)next + copy, 0, total - copy);
    aligned_free_ex(ptr);
    return next;
}

void *__wrap_winpr_aligned_offset_malloc(size_t size, size_t alignment, size_t offset)
{
    return aligned_alloc_ex(size, alignment, offset);
}

void *__wrap_winpr_aligned_offset_realloc(void *ptr, size_t size, size_t alignment,
                                          size_t offset)
{
    psrdp_align_hdr *hdr;
    void *next;
    size_t copy;

    if (!ptr) return aligned_alloc_ex(size, alignment, offset);
    hdr = hdr_from(ptr);
    if (!hdr) return NULL;
    if (size == 0)
    {
        aligned_free_ex(ptr);
        return NULL;
    }
    next = aligned_alloc_ex(size, alignment, offset);
    if (!next) return NULL;
    copy = (hdr->size < size) ? hdr->size : size;
    memcpy(next, ptr, copy);
    aligned_free_ex(ptr);
    return next;
}

void *__wrap_winpr_aligned_offset_recalloc(void *ptr, size_t count, size_t size,
                                           size_t alignment, size_t offset)
{
    size_t total;
    psrdp_align_hdr *hdr;
    void *next;
    size_t copy;

    if ((count == 0) || (size == 0))
    {
        aligned_free_ex(ptr);
        return NULL;
    }
    if (count > SIZE_MAX / size) return NULL;
    total = count * size;

    if (!ptr)
    {
        next = aligned_alloc_ex(total, alignment, offset);
        if (next) memset(next, 0, total);
        return next;
    }

    hdr = hdr_from(ptr);
    if (!hdr) return NULL;
    if (hdr->size > total + alignment) return ptr;

    next = aligned_alloc_ex(total, alignment, offset);
    if (!next) return NULL;
    copy = (hdr->size < total) ? hdr->size : total;
    memcpy(next, ptr, copy);
    memset((char *)next + copy, 0, total - copy);
    aligned_free_ex(ptr);
    return next;
}

size_t __wrap_winpr_aligned_msize(void *ptr, size_t alignment, size_t offset)
{
    psrdp_align_hdr *hdr = hdr_from(ptr);
    (void)alignment;
    (void)offset;
    return hdr ? hdr->size : 0;
}
