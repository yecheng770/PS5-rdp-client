/* psrdp_glue.h - shared surface between main.c (display) and rdp.c (client). */
#ifndef PSRDP_GLUE_H
#define PSRDP_GLUE_H

/* Single source of truth: the config page prints it, the session trace
 * records it, and the param.json contentVersion follows it. */
#define PSRDP_VERSION "1.7.23"

#include <stdint.h>
#include <stdio.h>

/* Limits of the current FreeRDP decoder surface. */
#define PSRDP_REMOTE_MIN_W 200
#define PSRDP_REMOTE_MIN_H 200
#define PSRDP_REMOTE_MAX_W 3840
#define PSRDP_REMOTE_MAX_H 2160

static inline int psrdp_parse_dimension(const char **text, int max)
{
    const char *p = *text;
    int value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p++ - '0');
        if (value > max) return 0;
    }
    *text = p;
    return value;
}

static inline int psrdp_resolution(const char *name, int *w, int *h)
{
    if (!name || !w || !h) return 0;
    int width = psrdp_parse_dimension(&name, PSRDP_REMOTE_MAX_W);
    if (width < PSRDP_REMOTE_MIN_W || (*name != 'x' && *name != 'X')) return 0;
    ++name;
    int height = psrdp_parse_dimension(&name, PSRDP_REMOTE_MAX_H);
    if (height < PSRDP_REMOTE_MIN_H || *name) return 0;
    *w = width;
    *h = height;
    return 1;
}

static inline int psrdp_map_coordinate(int value, int from, int to)
{
    if (from <= 1 || to <= 1 || value <= 0) return 0;
    if (value >= from-1) return to-1;
    return (int)(((int64_t)value*(to-1)+(from-1)/2)/(from-1));
}


typedef enum {
    RDP_IDLE = 0,
    RDP_CONNECTING,
    RDP_LIVE,
    RDP_ERROR
} rdp_phase;

typedef enum {
    RDP_FAILURE_UNKNOWN = 0, RDP_FAILURE_HOST, RDP_FAILURE_PORT,
    RDP_FAILURE_PASSWORD, RDP_FAILURE_LOGIN, RDP_FAILURE_SETUP,
    RDP_FAILURE_INTERRUPTED, RDP_FAILURE_COUNT
} rdp_failure;

typedef struct {
    int running;   /* thread alive */
    int connected; /* session up   */
    rdp_phase phase;
    rdp_failure failure;
    char err[160];
} rdp_state;

typedef struct {
    char host[128];
    char user[64];
    char pass[64];
    char res[16];
    char codec[16];
    int  port;
    unsigned desktop_scale;
} config_t;

/* Defined in main.c (the config page already owns one). */
extern config_t g_cfg;

/* Thread-safe copy; the caller owns the returned state. */
void psrdp_get_state(rdp_state *out);

/* Half-open source bounds in a BGRA32 surface. The worker keeps ownership. */
void psrdp_blit(const void *pix, int w, int h, int pitch,
                int x0, int y0, int x1, int y1);
void psrdp_picture_reset(void);
void psrdp_diag_line(const char *line);

/* main.c offers this for diagnostics surfaced on the page. */
void psrdp_note(const char *fmt, ...);

int  rdp_start(void);
void rdp_stop(void);
/* Complete, password-free trace for one connection attempt. */
void rdp_diagnostics(char *out, size_t capacity);

/* ---- input forwarding ------------------------------------------------
 * All of these are no-ops that return 0 when no session is live, so the
 * display loop can call them unconditionally. They never block for longer
 * than the session lock, and they never take ownership of their arguments.
 *
 * Coordinates are absolute on the negotiated desktop: the caller keeps the
 * pointer position, the transport only clips it to the desktop it knows. */
int rdp_send_mouse(int x, int y, int buttons);   /* bit0 left, bit1 right, bit2 middle */
int rdp_send_wheel(int x, int y, int notches);   /* +1 = up, -1 = down */
int rdp_send_key(int down, int scancode);        /* PS/2 set-1 make code, |0x100 extended */
int rdp_send_flags_ready(void);                  /* 1 once the session accepts input */
int rdp_desktop_width(void);
int rdp_desktop_height(void);

#endif /* PSRDP_GLUE_H */
