/* psrdp_input.c - pad, keyboard and pointer.
 *
 * The SDK never ships a header for any of this, and libScePad.so /
 * libSceKeyboard.so are import stubs: every symbol resolves to the same
 * placeholder address and the loader patches it at run time. That means
 * none of it can be proven on the build machine - so every call that
 * probes hardware is wrapped, its result is kept, and the config page
 * reports exactly what opened. Guesswork is confined to the data layout,
 * which is the documented HID boot report for keyboards and the well
 * known 120-byte ScePadData for the controller.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>

#include "psrdp_input.h"

/* ------------------------------------------------------------------ */
/* Platform                                                            */
/* ------------------------------------------------------------------ */

/* Nothing here is linked directly any more. Linking libScePad.so or
 * libSceKeyboard.so puts the module in DT_NEEDED and the converter accepts
 * it, but on the console the import slot stays empty and the first call
 * lands on address 0 - that is the SIGSEGV with RIP=0, backtrace 0x400b02,
 * logged by 0.3.6. Resolving by name at run time leaves a missing symbol
 * NULL instead, so the page reports it and the app keeps running.
 *
 * 0.3.5 already knew this and used dlopen; 0.3.6 went back to direct
 * imports to add scePadInit and crashed on the first call. The fix keeps
 * the run-time route *and* the new init sequence. */

typedef int32_t (*fn_v)(void);
typedef int32_t (*fn_p)(const void *);
typedef int32_t (*fn_pi)(int32_t *);
typedef int32_t (*fn_i)(int32_t);
typedef int32_t (*fn_ii)(int32_t, void *);
typedef int32_t (*fn_iii)(int32_t, int32_t, int32_t);
typedef int32_t (*fn_open)(int32_t, int32_t, int32_t, const void *);
typedef int32_t (*fn_read)(int32_t, void *, int32_t);

static fn_p    p_user_init;
static fn_pi   p_user_initial;

static fn_v    p_pad_init;
static fn_open p_pad_open;
static fn_i    p_pad_close;
static fn_read p_pad_read;
static fn_ii   p_pad_read_state;
static fn_iii  p_pad_get_handle;
static fn_ii   p_pad_info;

static fn_v    p_kbd_init;
static fn_open p_kbd_open;
static fn_i    p_kbd_close;
static fn_read p_kbd_read;      /* sceKeyboardRead: three arguments */
static fn_pi   p_user_fguser;   /* sceUserServiceGetForegroundUser */

/* The link-only SDK facade resolves these imports to libSceMouse on PS5. */
extern int32_t sceMouseInit(void);
extern int32_t sceMouseOpen(int32_t user, int32_t type, int32_t index, const void *param);
extern int32_t sceMouseRead(int32_t handle, void *data, int32_t count);
extern int32_t sceMouseClose(int32_t handle);

typedef int32_t (*mouse_init_fn)(void);
typedef int32_t (*mouse_open_fn)(int32_t userId, int32_t type, int32_t index,
                                 const void *param);
typedef int32_t (*mouse_read_fn)(int32_t handle, void *data, int32_t num);
typedef int32_t (*mouse_close_fn)(int32_t handle);

/* 40 bytes as documented. Kept as bytes so the compiler cannot pad it. */
typedef struct {
    uint64_t timestamp;
    uint8_t  connected;
    uint8_t  pad[3];
    uint32_t buttons;
    int32_t  x_axis;
    int32_t  y_axis;
    int32_t  wheel;
    int32_t  tilt;
    uint8_t  reserve[8];
} orbis_mouse_data;

#define MOUSE_BTN_LEFT   0x00000001u
#define MOUSE_BTN_RIGHT  0x00000002u
#define MOUSE_BTN_MIDDLE 0x00000004u
#define MOUSE_BTN_SIDE1  0x00000008u
#define MOUSE_BTN_SIDE2  0x00000010u

/* ScePadData is 120 bytes; we only touch the bytes we know. */
#define PAD_SAMPLE 120
#define PAD_BATCH  4

/* Button bits, as documented by the community headers. */
#define PB_L3       0x00000002u
#define PB_R3       0x00000004u
#define PB_OPTIONS  0x00000008u
#define PB_UP       0x00000010u
#define PB_RIGHT    0x00000020u
#define PB_DOWN     0x00000040u
#define PB_LEFT     0x00000080u
#define PB_L2       0x00000100u
#define PB_R2       0x00000200u
#define PB_L1       0x00000400u
#define PB_R1       0x00000800u
#define PB_TRIANGLE 0x00001000u
#define PB_CIRCLE   0x00002000u
#define PB_CROSS    0x00004000u
#define PB_SQUARE   0x00008000u
#define PB_TOUCHPAD  0x00100000u
#define PB_INTERCEPT 0x80000000u

#define STICK_DEAD 24

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static int g_pad = -1;
static int g_user = -1;
static int g_pad_rc_open = -1;      /* scePadOpen result */
static int g_pad_rc_handle = -2;    /* scePadGetHandle result, -2 = not tried */
static int g_pad_rc_init = -1;      /* sceUserServiceInitialize result */
static int g_pad_sys_init = -1;     /* scePadInit result */
static int g_pad_sys_init_done;
static int g_pad_src;               /* 0 = scePadOpen, 1 = scePadGetHandle */
static int g_pad_info_rc = -1;
static int g_pad_reads;
static int g_pad_fail;
static int g_pad_idle;
static uint32_t g_pad_last_buttons;
static int g_pad_intercept;
static unsigned char g_pad_first[PAD_SAMPLE];
static int g_pad_first_have;

/* One packed record as sceKeyboardRead fills it: 96 bytes on the console,
 * the same shape the community headers and XashPS5 use. `connected` says
 * whether that slot currently has a device; `intercepted` means the system
 * already consumed the report (IME, system dialog) and it must be skipped
 * or every key would be typed twice. */
typedef struct {
    uint64_t timestamp;
    uint8_t  intercepted;
    uint8_t  reserved0[7];
    uint8_t  connected;
    uint8_t  reserved1[3];
    uint32_t length;
    uint32_t leds;
    uint32_t modifiers;
    uint16_t keycodes[16];
    uint8_t  reserved2[32];
} ps5_kb_record_t;
#define KBD_REC_SIZE 96
#define KBD_REC_MAX  16

_Static_assert(sizeof(ps5_kb_record_t) == KBD_REC_SIZE,
               "ps5_kb_record_t must match the console's 96-byte record");

/* Two keyboard slots, exactly as XashPS5 opens them: index 0 is the wired
 * USB keyboard, index 1 the Bluetooth/alternate one. A handle of -1 means
 * that slot was refused and is simply skipped. */
#define KBD_SLOTS 2
static int g_kbd[KBD_SLOTS] = { -1, -1 };
static int g_kbd_open_rc[KBD_SLOTS] = { -1, -1 };
static int g_kbd_rc_open = -1;      /* first successful open, for display */
static int g_kbd_rc_init = -1;
static int g_kbd_rc_fguser = -1;
static int g_kbd_sysmodule_id = -1;
static int g_kbd_sysmodule_rc = -1;
static int g_kbd_reads;
static int g_kbd_fail;
static unsigned char g_kbd_raw[96];
static int g_kbd_raw_len;
static int g_kbd_rec_ok;
static int g_kbd_rec_skip;

enum { MOUSE_SLOTS = 8, MOUSE_BATCH = 64 };
_Static_assert(sizeof(orbis_mouse_data) == 40, "PS5 mouse record size");
#define MOUSE_INTERCEPTED 0x80000000u
#define MOUSE_BUTTONS (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT | MOUSE_BTN_MIDDLE | MOUSE_BTN_SIDE1 | MOUSE_BTN_SIDE2)
#define SCE_SYSMODULE_MOUSE 0x00a9

static struct {
    int handle, open_rc, read_rc;
    unsigned errors, records, consecutive_errors;
    int connected;
    uint32_t sent, blocked;
    uint64_t retry_at;
} g_mouse_slots[MOUSE_SLOTS] = { { .handle = -1, .open_rc = -1 },
                                { .handle = -1, .open_rc = -1 },
                                { .handle = -1, .open_rc = -1 },
                                { .handle = -1, .open_rc = -1 },
                                { .handle = -1, .open_rc = -1 },
                                { .handle = -1, .open_rc = -1 },
                                { .handle = -1, .open_rc = -1 },
                                { .handle = -1, .open_rc = -1 } };
static int g_mouse_sys_rc = -1, g_mouse_init_rc = -1;
static int g_mouse_resolve_tried, g_mouse_user = -1;
static unsigned g_mouse_full, g_mouse_filtered;
static mouse_init_fn p_mouse_init;
static mouse_open_fn p_mouse_open;
static mouse_read_fn p_mouse_read;
static mouse_close_fn p_mouse_close;

static input_sink g_sink;
static int g_sink_ready;

static uint32_t g_prev_buttons;
static int g_prev_have;
static int g_touch_w = 1920, g_touch_h = 1080;
static int g_touch_have, g_touch_x, g_touch_y, g_touch_id;
static int g_touch_rem_x, g_touch_rem_y, g_touch_button;
static int g_touch_logged;
static int g_touch_blocked;
static int g_stick_rem_x, g_stick_rem_y;
static int g_pad_scale_rem_x, g_pad_scale_rem_y;
/* Merged remote left button: X and a touchpad press share one state. */
static int g_left_down;
static int g_left_blocked;   /* held across a menu: require a release first */
static int stick_motion(int v, int *remainder)
{
    if (!v) { *remainder=0; return 0; }
    int n=v*(v<0?-v:v)+*remainder;
    int out=n/1024;
    *remainder=n%1024;
    return out;
}

static void touch_reset(input_frame *f)
{
    if (g_touch_button && f && f->touch_event_count < 8)
        f->touch_events[f->touch_event_count++] = 0;
    g_touch_button = g_touch_have = 0;
    g_touch_rem_x = g_touch_rem_y = 0;
}

/* ScePadData: count at 52, first point x/y at 60/62, contact ID at 64.
 * Relative motion allows lifting/repositioning without moving the pointer. */
static void feed_touch(const unsigned char *s, uint32_t buttons, input_frame *f)
{
    int held = (buttons & PB_TOUCHPAD) != 0;
    /* A drag held while returning from a local menu must be released before
     * it can become a fresh remote press. */
    if (g_touch_blocked) {
        touch_reset(f);
        if (held) return;
        g_touch_blocked=0;
    }
    if (held != g_touch_button) {
        if (f->touch_event_count < 8)
            f->touch_events[f->touch_event_count++] = held;
        g_touch_button = held;
    }
    if (s[52] != 1) {
        g_touch_have = 0;
        g_touch_rem_x = g_touch_rem_y = 0;
        return;
    }
    int x = s[60] | (s[61] << 8), y = s[62] | (s[63] << 8);
    int id = s[64];
    if (x >= g_touch_w || y >= g_touch_h) {
        g_touch_have = 0;
        g_touch_rem_x = g_touch_rem_y = 0;
        return;
    }
    if (g_touch_have && id == g_touch_id) {
        int nx = (x - g_touch_x) * 1920 + g_touch_rem_x;
        int ny = (y - g_touch_y) * 1080 + g_touch_rem_y;
        f->touch_dx += nx / (g_touch_w * 2);
        f->touch_dy += ny / (g_touch_h * 2);
        g_touch_rem_x = nx % (g_touch_w * 2);
        g_touch_rem_y = ny % (g_touch_h * 2);
    } else {
        g_touch_rem_x = g_touch_rem_y = 0;
    }
    g_touch_x = x; g_touch_y = y; g_touch_id = id; g_touch_have = 1;
}

/* Per slot: two keyboards must not diff against each other, or each
 * alternating record would look like a release and a fresh press. */
static uint16_t g_prev_keys[KBD_SLOTS][KBD_REC_MAX];
static uint32_t g_prev_kbd_mod[KBD_SLOTS];
static int g_prev_kbd_have[KBD_SLOTS];
static int g_kbd_connected[KBD_SLOTS];
/* Auto-repeat: the ordinary key currently repeating and its owning slot. */
static uint16_t g_rep_key;
static int g_rep_slot = -1;
static uint64_t g_rep_next_ms;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Which names came back empty. Written once, read by the page. */
static char g_sym_missing[96];

static void note_sym(const char *tag)
{
    size_t o = strlen(g_sym_missing);
    if (strstr(g_sym_missing, tag))
        return;
    if (o + strlen(tag) + 2 >= sizeof(g_sym_missing))
        return;
    if (o)
        g_sym_missing[o++] = ',';
    memcpy(g_sym_missing + o, tag, strlen(tag) + 1);
}

static char g_lib_try[192];

static void log_lib(const char *line)
{
    FILE *f = fopen("/data/psrdp_diag.txt", "a");

    if (!f)
        return;
    fprintf(f, "[lib] %s\n", line);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Direct imports (0.4.0)                                              */
/* ------------------------------------------------------------------ */
/* dlopen cannot load the console's own SPRX modules: the loader only
 * resolves them through the import table of a signed app, and every path
 * under /system* answers EPERM/"unsupported" to a plain dlopen. The route
 * every other PS5 project takes is a link-time reference: name the
 * function, let the converter write the NID import into eboot.bin, and let
 * the SDK's libSce*.so import stubs (--stub-dir $SDK/target/lib) supply
 * the symbol. The dlopen path below stays only as a fallback and as the
 * diagnostic that records why a library refused. */

extern int32_t sceUserServiceInitialize(const void *params);
extern int32_t sceUserServiceGetInitialUser(int32_t *user_id);
extern int32_t sceUserServiceGetForegroundUser(int32_t *user_id);
extern int32_t scePadInit(void);
extern int32_t scePadOpen(int32_t user_id, int32_t type, int32_t index,
                          const void *param);
extern int32_t scePadClose(int32_t handle);
extern int32_t scePadRead(int32_t handle, void *data, int32_t count);
extern int32_t scePadReadState(int32_t handle, void *data);
extern int32_t scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
extern int32_t scePadGetControllerInformation(int32_t handle, void *info);
extern int32_t sceKeyboardInit(void);
extern int32_t sceKeyboardOpen(int32_t user_id, int32_t type, int32_t index,
                               const void *param);
extern int32_t sceKeyboardReadState(int32_t handle, void *data);
extern int32_t sceKeyboardRead(int32_t handle, void *data, int32_t count);
extern int32_t sceKeyboardClose(int32_t handle);
extern int32_t sceKeyboardConnectPort(int32_t index);
extern int32_t sceKeyboardDeviceOpen(int32_t user_id, int32_t type);
extern int32_t sceKeyboardSetProcessPrivilege(uint32_t privilege);

static int32_t w_user_init(const void *p) { return sceUserServiceInitialize(p); }
static int32_t w_user_initial(int32_t *id) { return sceUserServiceGetInitialUser(id); }
static int32_t w_pad_init(void) { return scePadInit(); }
static int32_t w_pad_open(int32_t u, int32_t t, int32_t i, const void *p)
{
    return scePadOpen(u, t, i, p);
}
static int32_t w_pad_close(int32_t h) { return scePadClose(h); }
static int32_t w_pad_read(int32_t h, void *d, int32_t n) { return scePadRead(h, d, n); }
static int32_t w_pad_read_state(int32_t h, void *d) { return scePadReadState(h, d); }
static int32_t w_pad_get_handle(int32_t u, int32_t t, int32_t i)
{
    return scePadGetHandle(u, t, i);
}
static int32_t w_pad_info(int32_t h, void *i)
{
    return scePadGetControllerInformation(h, i);
}
static int32_t w_kbd_init(void) { return sceKeyboardInit(); }
static int32_t w_kbd_open(int32_t u, int32_t t, int32_t i, const void *p)
{
    return sceKeyboardOpen(u, t, i, p);
}
/* The queue variant, not ReadState: one call returns up to `count` packed
 * records and a keyboard that is not connected simply yields 0. ReadState
 * only ever hands back the current HID boot report, which the PS5 keyboard
 * service does not populate for wired devices. */
static int32_t w_kbd_read(int32_t h, void *d, int32_t n)
{
    return sceKeyboardRead(h, d, n);
}
static int32_t w_kbd_close(int32_t h) { return sceKeyboardClose(h); }
static int32_t w_user_fguser(int32_t *id)
{
    return sceUserServiceGetForegroundUser(id);
}

/* One dlopen attempt that records the outcome. dlopen is the only thing that
 * can say *why* a system library refused to load, so keep the hit path and
 * the miss reason instead of returning a bare NULL. */
static void *try_dlopen(const char *path)
{
    void *lib = dlopen(path, RTLD_LAZY);

    if (lib) {
        snprintf(g_lib_try, sizeof(g_lib_try), "hit %s", path);
        log_lib(g_lib_try);
        return lib;
    }
    {
        const char *err = dlerror();

        snprintf(g_lib_try, sizeof(g_lib_try), "miss %s: %s",
                 path, err ? err : "no-error-string");
    }
    log_lib(g_lib_try);
    return NULL;
}

static void *open_input_lib(const char *name, const char *alt)
{
    /* The sandbox cannot see /system*, so the console libraries live in
     * /data/psrdp_lib and are tried first. */
    static const char *dirs[] = {
        "/data/psrdp_lib/",
        "/system_ex/system/lib/",
        "/system_ex/common/lib/",
        "/system/common/lib/",
        NULL
    };
    char path[192];
    void *lib;
    int i;

    for (i = 0; dirs[i]; ++i) {
        snprintf(path, sizeof(path), "%s%s", dirs[i], name);
        lib = try_dlopen(path);
        if (lib)
            return lib;
        if (alt) {
            snprintf(path, sizeof(path), "%s%s", dirs[i], alt);
            lib = try_dlopen(path);
            if (lib)
                return lib;
        }
    }
    lib = try_dlopen(name);
    if (lib)
        return lib;
    if (alt) {
        lib = try_dlopen(alt);
        if (lib)
            return lib;
    }
    return NULL;
}

/* One pass, once per process. Every pointer stays NULL when its library or
 * symbol is absent, and no call site below is allowed to run on NULL. */
static void *g_lib_user;
static void *g_lib_pad;
static void *g_lib_kbd;

/* Console libraries export NIDs, not names. dlsym(handle, "scePadOpen")
 * returns NULL even with the module loaded, which is why 0.3.5 and 0.3.7
 * both opened nothing and the pad stayed dead. Try the NID first, then the
 * plain name (a few libraries do export both). */
static void *sym_nid(void *lib, const char *nid, const char *name)
{
    void *p;

    if (!lib)
        return NULL;
    p = dlsym(lib, nid);
    if (!p && name)
        p = dlsym(lib, name);
    return p;
}

static void write_input_line(const char *line)
{
    FILE *f = fopen("/data/psrdp_diag.txt", "a");

    if (!f)
        return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

static void write_input_diag(const char *phase)
{
    FILE *f = fopen("/data/psrdp_diag.txt", "a");

    if (!f)
        return;
    fprintf(f,
            "[%s] lib user=%p pad=%p kbd=%p "
            "sym padinit=%p open=%p read=%p readstate=%p geth=%p info=%p "
            "kbdopen=%p sysmod_id=%d sysmod_rc=0x%08x syms=%s\n",
            phase, g_lib_user, g_lib_pad, g_lib_kbd,
            (void *)p_pad_init, (void *)p_pad_open, (void *)p_pad_read,
            (void *)p_pad_read_state, (void *)p_pad_get_handle,
            (void *)p_pad_info, (void *)p_kbd_open,
            g_kbd_sysmodule_id, (unsigned)g_kbd_sysmodule_rc,
            g_sym_missing[0] ? g_sym_missing : "-");
    fclose(f);
}

/* 0.5.0 - Reading &fn loads the GOT slot of an imported function without
 * jumping to it. A slot the loader never resolved reads as 0: that is
 * exactly the RIP=0 crash of 0.3.6 and 0.4.0. Record every slot value and
 * only report an unresolved one - never call through it. */
static int got_live(const void *slot, const char *tag)
{
    char line[256];
    int live = slot != NULL && (uintptr_t)slot != (uintptr_t)-1;

    snprintf(line, sizeof(line), "[got] %-16s %p %s", tag, slot,
             live ? "live" : "UNRESOLVED");
    write_input_line(line);
    if (!live)
        note_sym(tag);
    return live;
}

static void load_input_imports(void)
{
    static int done;

    if (done)
        return;
    done = 1;

    /* 0.5.0: guarded direct imports. Wire a wrapper only when the GOT
     * slot is already filled; an empty slot stays NULL and is reported
     * through note_sym(), so the app runs no matter what the loader did.
     * A successful dlopen below can still overwrite these. */
    p_user_init      = got_live((const void *)&sceUserServiceInitialize,
                                "user:init") ? w_user_init : NULL;
    p_user_initial   = got_live((const void *)&sceUserServiceGetInitialUser,
                                "user:initial") ? w_user_initial : NULL;
    p_user_fguser    = got_live((const void *)&sceUserServiceGetForegroundUser,
                                "user:fguser") ? w_user_fguser : NULL;
    p_pad_init       = got_live((const void *)&scePadInit,
                                "pad:init") ? w_pad_init : NULL;
    p_pad_open       = got_live((const void *)&scePadOpen,
                                "pad:open") ? w_pad_open : NULL;
    p_pad_close      = got_live((const void *)&scePadClose,
                                "pad:close") ? w_pad_close : NULL;
    p_pad_read       = got_live((const void *)&scePadRead,
                                "pad:read") ? w_pad_read : NULL;
    p_pad_read_state = got_live((const void *)&scePadReadState,
                                "pad:readstate") ? w_pad_read_state : NULL;
    p_pad_get_handle = got_live((const void *)&scePadGetHandle,
                                "pad:geth") ? w_pad_get_handle : NULL;
    p_pad_info       = got_live((const void *)&scePadGetControllerInformation,
                                "pad:info") ? w_pad_info : NULL;
    p_kbd_init       = got_live((const void *)&sceKeyboardInit,
                                "kbd:init") ? w_kbd_init : NULL;
    p_kbd_open       = got_live((const void *)&sceKeyboardOpen,
                                "kbd:open") ? w_kbd_open : NULL;
    p_kbd_read       = got_live((const void *)&sceKeyboardRead,
                                "kbd:read") ? w_kbd_read : NULL;
    p_kbd_close      = got_live((const void *)&sceKeyboardClose,
                                "kbd:close") ? w_kbd_close : NULL;

    g_lib_user = open_input_lib("libSceUserService.sprx",
                               "libSceUserService.prx");
    if (g_lib_user) {
        p_user_init    = (fn_p)sym_nid(g_lib_user, "j3YMu1MVNNo",
                                      "sceUserServiceInitialize");
        p_user_initial = (fn_pi)sym_nid(g_lib_user, "CdWp0oHWGr0",
                                       "sceUserServiceGetInitialUser");
    }
    if (!p_user_init || !p_user_initial)
        note_sym("user");

    g_lib_pad = open_input_lib("libScePad.sprx", "libScePad.prx");
    if (g_lib_pad) {
        p_pad_init       = (fn_v)sym_nid(g_lib_pad, "hv1luiJrqQM",
                                        "scePadInit");
        p_pad_open       = (fn_open)sym_nid(g_lib_pad, "xk0AcarP3V4",
                                           "scePadOpen");
        p_pad_close      = (fn_i)sym_nid(g_lib_pad, "6ncge5+l5Qs",
                                        "scePadClose");
        p_pad_read       = (fn_read)sym_nid(g_lib_pad, "q1cHNfGycLI",
                                           "scePadRead");
        p_pad_read_state = (fn_ii)sym_nid(g_lib_pad, "YndgXqQVV7c",
                                         "scePadReadState");
        p_pad_get_handle = (fn_iii)sym_nid(g_lib_pad, "u1GRHp+oWoY",
                                          "scePadGetHandle");
        p_pad_info       = (fn_ii)sym_nid(g_lib_pad, "gjP9-KQzoUk",
                                         "scePadGetControllerInformation");
    }
    if (!p_pad_open || !p_pad_read)
        note_sym("pad");
    if (!p_pad_init)
        note_sym("padinit");

    /* No dlopen for the keyboard: /system* refuses open() from a homebrew
     * process, so every earlier attempt only logged "miss", and the module's
     * dlsym table exposes ReadState under a different entry than the queue
     * read - overwriting the working pointers with that one is how the
     * two-argument call slipped in. The guarded direct imports above are
     * the route that actually resolves on the console. */
    if (!p_kbd_open || !p_kbd_read)
        note_sym("kbd");

    write_input_diag("load");
}

/* Opening the pad takes three separate steps. scePadInit() has to run once
 * before any open, otherwise the open fails with NOT_INITIALISED - that is
 * why 0.3.5 saw no input at all. If our own open still fails, take the
 * handle the system already holds for this user. */
static int open_pad(void)
{
    int32_t uid;
    int32_t h;

    load_input_imports();

    if (g_user < 0) {
        if (p_user_init && p_user_initial) {
            int32_t id = -1;
            g_pad_rc_init = p_user_init(NULL);
            if (p_user_initial(&id) == 0 && id >= 0)
                g_user = id;
        } else {
            g_pad_rc_init = -1;
        }
    }
    /* 0xff is the "current user" convention the 0.3.5 path used. */
    uid = (g_user >= 0) ? (int32_t)g_user : 0xff;

    if (!g_pad_sys_init_done) {
        g_pad_sys_init = p_pad_init ? p_pad_init() : -1;
        g_pad_sys_init_done = 1;
    }

    /* 1. our own handle. */
    if (g_pad < 0 && g_pad_rc_handle == -2 && g_pad_rc_open < 0 && p_pad_open) {
        g_pad_rc_open = p_pad_open(uid, 0, 0, NULL);
        if (g_pad_rc_open >= 0) {
            g_pad = g_pad_rc_open;
            g_pad_src = 0;
        }
    }

    /* 2. the handle the system opened for this user. */
    if (g_pad < 0 && g_pad_rc_handle == -2 && p_pad_get_handle) {
        h = p_pad_get_handle(uid, 0, 0);
        if (h < 0 && uid != 0)
            h = p_pad_get_handle(0, 0, 0);
        g_pad_rc_handle = h;
        if (h >= 0) {
            g_pad = h;
            g_pad_src = 1;
        }
    }

    if (g_pad >= 0 && g_pad_info_rc == -1 && p_pad_info) {
        unsigned char info[64];
        memset(info, 0, sizeof(info));
        g_pad_info_rc = p_pad_info(g_pad, info);
        if (g_pad_info_rc == 0) {
            int w = info[4] | (info[5] << 8), h = info[6] | (info[7] << 8);
            if (w >= 100 && w <= 8192 && h >= 100 && h <= 8192) {
                g_touch_w = w; g_touch_h = h;
            }
        }
    }
    {
        static int dumped;

        if (!dumped) {
            char line[256];

            dumped = 1;
            snprintf(line, sizeof(line),
                     "open uid=%d usersvc=0x%08x padinit=0x%08x open=0x%08x "
                     "geth=0x%08x handle=%d src=%d info=0x%08x",
                     uid, (unsigned)g_pad_rc_init,
                     (unsigned)g_pad_sys_init, (unsigned)g_pad_rc_open,
                     (unsigned)g_pad_rc_handle, g_pad, g_pad_src,
                     g_pad_info_rc);
            write_input_line(line);
        }
    }
    return g_pad;
}

/* The keyboard module is the only input module the on-device loader refuses
 * to fill automatically (pad and user service resolve, keyboard reads 0 in
 * every session log). Load it explicitly through the official sysmodule
 * service before the first call, the same route Jelly5 uses for its video
 * and audio decoders.
 *
 * Id evidence, taken from libSceSysmodule.sprx itself rather than from
 * guesswork: the module table (file 0x1de80, 0x40-byte rows, u32 id + 0xffffffff
 * tail) holds two rows that both carry the name "libSceKeyboard" - 0x00a3 sits
 * inside the contiguous run 0x9f 0xa0 0xa2 [a3] a4 a5 a6, while 0x0106 is an
 * out-of-sequence insert between 0xa3 and 0xa4. 0x00a3 is therefore the primary
 * id and is the one tried first here; 0x0106 stays listed as the alternate so a
 * single log line tells us which one the loader accepts.
 *
 * 14..18 came from guessing, every one answered 0x805A1000 (INVALID_VALUE). */
extern int sceSysmoduleLoadModule(unsigned short id);
extern int sceSysmoduleIsLoaded(unsigned short id) __attribute__((weak));

#define SCE_SYSMODULE_KEYBOARD 0x00a3
#define SCE_SYSMODULE_KEYBOARD_ALT 0x0106

/* Read-only probe: sceSysmoduleIsLoaded answers 0 for "already loaded",
 * 0x805A1001 for "known id, not loaded" and 0x805A1000 for "no such id", so one
 * line of it maps the neighbourhood without loading anything. */
static void kbd_sysmodule_probe(void)
{
    static const unsigned short ids[] = {
        0x009f, 0x00a0, 0x00a2, 0x00a3, 0x00a4, 0x00a5, 0x00a6, 0x00a9, 0x0106
    };
    char line[256];
    size_t i, n = 0;

    if (!sceSysmoduleIsLoaded) {
        write_input_line("[kbd] isloaded UNRESOLVED");
        return;
    }
    n += (size_t)snprintf(line + n, sizeof(line) - n, "[kbd] isloaded");
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]) && n < sizeof(line) - 16; ++i)
        n += (size_t)snprintf(line + n, sizeof(line) - n, " %04x:0x%08x",
                              (unsigned)ids[i],
                              (unsigned)sceSysmoduleIsLoaded(ids[i]));
    write_input_line(line);
}

static void kbd_sysmodule_load(void)
{
    static int done;
    int rc;

    if (done)
        return;
    done = 1;

    kbd_sysmodule_probe();

    rc = sceSysmoduleLoadModule(SCE_SYSMODULE_KEYBOARD);
    g_kbd_sysmodule_id = SCE_SYSMODULE_KEYBOARD;
    g_kbd_sysmodule_rc = rc;
    {
        char line[160];
        int rc2;

        rc2 = sceSysmoduleLoadModule(SCE_SYSMODULE_KEYBOARD_ALT);
        snprintf(line, sizeof(line),
                 "[kbd] sysmodule id=0x%04x rc=0x%08x alt=0x%04x rc=0x%08x",
                 (unsigned)SCE_SYSMODULE_KEYBOARD, (unsigned)rc,
                 (unsigned)SCE_SYSMODULE_KEYBOARD_ALT, (unsigned)rc2);
        write_input_line(line);
    }
}

/* 1.7.9 - Import slots are bound when the process starts, and at that moment
 * the keyboard is not part of the picture: 1.7.8 logged every pad slot live
 * (0x8009f0xxx) and all four keyboard slots as 0. sceSysmoduleLoadModule()
 * then answers rc=0, so one question is still open - does the loader fill
 * those slots retroactively? Re-reading them costs nothing and calls
 * nothing: a slot that stays 0 leaves p_kbd_* NULL exactly as in 1.7.8. */
static void kbd_recheck_imports(const char *phase)
{
    char tag[40];

    snprintf(tag, sizeof(tag), "kbd@%s:init", phase);
    if (got_live((const void *)&sceKeyboardInit, tag))
        p_kbd_init = w_kbd_init;
    snprintf(tag, sizeof(tag), "kbd@%s:open", phase);
    if (got_live((const void *)&sceKeyboardOpen, tag))
        p_kbd_open = w_kbd_open;
    snprintf(tag, sizeof(tag), "kbd@%s:read", phase);
    if (got_live((const void *)&sceKeyboardRead, tag))
        p_kbd_read = w_kbd_read;
    snprintf(tag, sizeof(tag), "kbd@%s:close", phase);
    if (got_live((const void *)&sceKeyboardClose, tag))
        p_kbd_close = w_kbd_close;
}

static int open_keyboard(void)
{
    int32_t uid = -1;
    int i, opened = 0;

    load_input_imports();
    kbd_sysmodule_load();
    kbd_recheck_imports("load");
    if (!p_kbd_init || !p_kbd_open)
        return -1;

    if (g_kbd_rc_init < 0)
        g_kbd_rc_init = p_kbd_init();

    /* Prefer the foreground user: that is the profile whose keyboard the
     * panel is attached to. The initial user is only right when the title
     * started before anyone signed in. */
    if (g_kbd_rc_fguser == -1 && p_user_fguser)
        g_kbd_rc_fguser = p_user_fguser(&uid);
    if (uid < 0 && g_user >= 0)
        uid = g_user;

    /* Both slots, once. Index 0 is the wired USB keyboard; a slot that is
     * refused keeps its rc so the page can show it instead of retrying. */
    for (i = 0; i < KBD_SLOTS; ++i) {
        if (g_kbd[i] >= 0)
            continue;
        g_kbd_open_rc[i] = p_kbd_open(uid < 0 ? 0xff : uid, 0, i, NULL);
        if (g_kbd_open_rc[i] >= 0) {
            g_kbd[i] = g_kbd_open_rc[i];
            opened++;
        }
    }
    if (g_kbd_rc_open < 0) {
        for (i = 0; i < KBD_SLOTS; ++i)
            if (g_kbd[i] >= 0) { g_kbd_rc_open = g_kbd[i]; break; }
    }
    {
        char line[160];

        snprintf(line, sizeof(line),
                 "[kbd] open uid=%d init=0x%08x fg=0x%08x slot0=%d slot1=%d",
                 (int)uid, (unsigned)g_kbd_rc_init, (unsigned)g_kbd_rc_fguser,
                 g_kbd_open_rc[0], g_kbd_open_rc[1]);
        write_input_line(line);
    }
    return opened ? 0 : -1;
}

static uint64_t kbd_now_ms(void);

static int mouse_source(void)
{
    for (int i = 0; i < MOUSE_SLOTS; ++i)
        if (g_mouse_slots[i].handle >= 0)
            return 2;
    return 0;
}

static void open_mouse(void)
{
    if (!g_mouse_resolve_tried) {
        g_mouse_resolve_tried = 1;
        g_mouse_sys_rc = sceSysmoduleLoadModule(SCE_SYSMODULE_MOUSE);
        if (got_live((const void *)&sceMouseInit, "MouseInit") &&
            got_live((const void *)&sceMouseOpen, "MouseOpen") &&
            got_live((const void *)&sceMouseRead, "MouseRead") &&
            got_live((const void *)&sceMouseClose, "MouseClose")) {
            p_mouse_init = sceMouseInit;
            p_mouse_open = sceMouseOpen;
            p_mouse_read = sceMouseRead;
            p_mouse_close = sceMouseClose;
            g_mouse_init_rc = p_mouse_init();
        }
        g_mouse_user = g_user;
        if (p_user_fguser) {
            int32_t user = -1;
            if (p_user_fguser(&user) >= 0 && user >= 0)
                g_mouse_user = user;
        }
        if (g_mouse_user < 0)
            g_mouse_user = 0xff;
    }
    if (!p_mouse_open)
        return;
    uint64_t now = kbd_now_ms();
    for (int i = 0; i < MOUSE_SLOTS; ++i) {
        if (g_mouse_slots[i].handle >= 0 || now < g_mouse_slots[i].retry_at)
            continue;
        uint8_t param[8] = {0};
        int rc = p_mouse_open(g_mouse_user, 0, i, param);
        g_mouse_slots[i].open_rc = rc;
        g_mouse_slots[i].retry_at = now + 2000;
        if (rc >= 0)
            g_mouse_slots[i].handle = rc;
    }
}

/* ------------------------------------------------------------------ */
/* Pad                                                                 */
/* ------------------------------------------------------------------ */

static void feed_pad(const unsigned char *s, input_frame *f)
{
    uint32_t b = (uint32_t)s[0] | ((uint32_t)s[1] << 8) |
                 ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
    int lx = (int)s[4] - 128;
    int ly = (int)s[5] - 128;
    int rx = (int)s[6] - 128;
    int ry = (int)s[7] - 128;

    if (!g_pad_first_have) {
        memcpy(g_pad_first, s, PAD_SAMPLE);
        g_pad_first_have = 1;
    }
    g_pad_last_buttons = b;

    if (b & PB_INTERCEPT) {
        g_pad_intercept++;
        touch_reset(f);
        return;
    }

    /* Sample the drag sources every frame; the edge flags above are for
     * menus, these levels drive the merged remote left button. Latch
     * within the frame: a press and release inside one poll must still
     * produce one click, and a held source must stay held. */
    if (b & PB_CROSS)    f->cross_held = 1;
    if (b & PB_TOUCHPAD) f->touch_held = 1;

    if (g_sink_ready) {
        feed_touch(s, b, f);
        if (!g_touch_logged && s[52] == 1) {
            char line[128];
            snprintf(line, sizeof(line), "touch: first contact; resolution=%dx%d id=%u", g_touch_w, g_touch_h, s[64]);
            write_input_line(line);
            g_touch_logged = 1;
        }
    }
    if (g_prev_have) {
        uint32_t now = b & ~g_prev_buttons;
        if (now & PB_UP)       f->up = 1;
        if (now & PB_DOWN)     f->down = 1;
        if (now & PB_LEFT)     f->left = 1;
        if (now & PB_RIGHT)    f->right = 1;
        if (now & PB_CROSS)    f->confirm = 1;
        if (now & PB_CIRCLE)   f->cancel = 1;
        if (now & PB_OPTIONS)  f->options = 1;
        if (now & PB_L1)       f->l1 = 1;
        if (now & PB_R1)       f->r1 = 1;
        if (now & PB_SQUARE)   f->square = 1;
        if (now & PB_TRIANGLE) f->triangle = 1;
    }
    g_prev_buttons = b;
    g_prev_have = 1;

    f->l2 = (b & PB_L2) != 0;
    f->r2 = (b & PB_R2) != 0;

    /* Left stick also walks the menus, with the dead zone applied so a
     * worn stick does not scroll the page on its own. */
    if (lx < -STICK_DEAD || lx > STICK_DEAD) f->lx = lx;
    if (ly < -STICK_DEAD || ly > STICK_DEAD) f->ly = ly;
    if (rx < -STICK_DEAD || rx > STICK_DEAD) f->rx = rx;
    if (ry < -STICK_DEAD || ry > STICK_DEAD) f->ry = ry;

    if (!g_prev_have && b)
        g_prev_have = 1;
    if (f->up || f->down || f->left || f->right ||
        f->confirm || f->cancel || f->options || f->l1 || f->r1 ||
        f->square || f->triangle)
        f->any = 1;
}

static void poll_pad(input_frame *f)
{
    unsigned char buf[PAD_SAMPLE * PAD_BATCH];
    int32_t n;
    int i;

    if (g_pad < 0) {
        /* Retry the whole sequence now and then: a controller switched on
         * after the title started has to be picked up. */
        if (open_pad() < 0) {
            if ((++g_pad_idle % 300) == 0) {
                g_pad_rc_open = -1;
                g_pad_rc_handle = -2;
            }
            return;
        }
    }

    n = p_pad_read ? p_pad_read(g_pad, buf, PAD_BATCH) : -1;
    if (n <= 0) {
        /* Zero means "no new sample" on some builds and a hard failure on
         * others; ask for the current state before calling it a loss. */
        memset(buf, 0, PAD_SAMPLE);
        if (p_pad_read_state && p_pad_read_state(g_pad, buf) >= 0) {
            g_pad_idle = 0;
            g_pad_reads++;
            feed_pad(buf, f);
            return;
        }
        g_pad_fail++;
        touch_reset(f);
        /* A controller that went away comes back: reopen and try once. */
        if ((g_pad_fail % 120) == 0) {
            if (g_pad_src == 0 && p_pad_close)
                p_pad_close(g_pad);
            g_pad = -1;
            g_pad_rc_open = -1;
            g_pad_rc_handle = -2;
            g_pad_info_rc = -1;
            g_prev_have = 0;
            open_pad();
        }
        return;
    }
    g_pad_idle = 0;
    if (n > PAD_BATCH)
        n = PAD_BATCH;
    g_pad_reads += n;
    for (i = 0; i < n; ++i)
        feed_pad(buf + (size_t)i * PAD_SAMPLE, f);
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */

/* Auto-repeat for ordinary keys: Windows waits about half a second, then
 * repeats roughly thirty times a second. The console record only reports
 * changes, so the repeat itself has to be driven from poll_keyboard().
 * Modifiers never repeat, and only the last key pressed owns the repeat. */
#define KBD_REPEAT_DELAY_MS  500u
#define KBD_REPEAT_PERIOD_MS 33u

static void emit_key(int down, unsigned short hid)
{
    static const unsigned short map[256] = {
        /* 0x04..0x1d : a-z */
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24,
        0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14,
        0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
        /* 0x1e..0x27 : 1-0 */
        0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
        /* 0x28..0x39 */
        0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B,
        0x00, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35, 0x3A,
        /* 0x3a..0x45 : F1-F12 */
        0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44,
        0x57, 0x58,
        /* 0x46..0x52 */
        0x137, 0x46, 0x45, 0x152, 0x147, 0x149, 0x153, 0x14F, 0x151,
        0x14D, 0x14B, 0x150, 0x148,
        /* 0x53..0x63 */
        0x45, 0x135, 0x37, 0x4A, 0x4E, 0x11C, 0x4F, 0x50, 0x51, 0x4B,
        0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52, 0x53,
        /* 0xe0..0xe7 modifiers: explicit index hid - 0x04 (0xDC..0xE3);
         * sequential placement would land at 96..103 and be dropped. */
        [0xE0 - 0x04] = 0x1D,  /* L Ctrl */
        [0xE1 - 0x04] = 0x2A,  /* L Shift */
        [0xE2 - 0x04] = 0x38,  /* L Alt */
        [0xE3 - 0x04] = 0x15B, /* L GUI (E0 5B) */
        [0xE4 - 0x04] = 0x11D, /* R Ctrl (E0 1D) */
        [0xE5 - 0x04] = 0x36,  /* R Shift */
        [0xE6 - 0x04] = 0x138, /* R Alt (E0 38) */
        [0xE7 - 0x04] = 0x15C, /* R GUI (E0 5C) */
    };
    unsigned short code;
    int sc;

    /* Table is indexed by hid - 0x04. */
    if (hid < 0x04 || hid > 0xE7)
        return;
    code = map[hid - 0x04];
    if (code == 0)
        return;
    /* Values above 0xff in the table are extended: keep the low byte and
     * set the RDP extended bit. */
    sc = (int)(code & 0xff);
    if (code & 0x100)
        sc |= 0x100;
    if (g_sink.key)
        g_sink.key(down, (unsigned short)sc);
}

static uint64_t kbd_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void kbd_repeat_stop(void)
{
    g_rep_key = 0;
    g_rep_slot = -1;
    g_rep_next_ms = 0;
}

static void kbd_repeat_start(int slot, uint16_t hid)
{
    g_rep_key = hid;
    g_rep_slot = slot;
    g_rep_next_ms = kbd_now_ms() + KBD_REPEAT_DELAY_MS;
}

/* True while another slot still reports the same usage, so one keyboard
 * letting go cannot lift a key the second one is holding. */
static int kbd_slot_holds(int except_slot, uint16_t hid)
{
    int slot, i;

    for (slot = 0; slot < KBD_SLOTS; ++slot) {
        if (slot == except_slot || !g_prev_kbd_have[slot])
            continue;
        for (i = 0; i < KBD_REC_MAX; ++i)
            if (g_prev_keys[slot][i] == hid)
                return 1;
    }
    return 0;
}

/* Drop everything a slot was holding. Used when that keyboard disappears
 * or its handle keeps failing; the other slot keeps its own keys. */
static void kbd_slot_release(int slot)
{
    int i, bit;

    if (!g_prev_kbd_have[slot])
        return;
    for (i = 0; i < KBD_REC_MAX; ++i) {
        uint16_t k = g_prev_keys[slot][i];

        if (!k || (k >= 0xE0 && k <= 0xE7))
            continue;
        if (!kbd_slot_holds(slot, k))
            emit_key(0, k);
    }
    for (bit = 0; bit < 8; ++bit) {
        int other = 0, s;

        if (!((g_prev_kbd_mod[slot] >> bit) & 1u))
            continue;
        for (s = 0; s < KBD_SLOTS; ++s)
            if (s != slot && g_prev_kbd_have[s] &&
                ((g_prev_kbd_mod[s] >> bit) & 1u))
                other = 1;
        if (!other)
            emit_key(0, (unsigned short)(0xE0 + bit));
    }
    if (g_rep_slot == slot)
        kbd_repeat_stop();
    memset(g_prev_keys[slot], 0, sizeof(g_prev_keys[slot]));
    g_prev_kbd_mod[slot] = 0;
    g_prev_kbd_have[slot] = 0;
}

/* Called once per input frame. At most one repeat per tick, so a stalled
 * frame cannot flood the remote with keydowns. */
static void kbd_repeat_tick(void)
{
    uint64_t now;

    if (!g_rep_key)
        return;
    for (int slot = 0; slot < KBD_SLOTS; ++slot)
        if (g_prev_kbd_mod[slot] & 0xDDu) {
            kbd_repeat_stop();
            return;
        }
    if (!g_sink_ready) {
        kbd_repeat_stop();
        return;
    }
    now = kbd_now_ms();
    if (now < g_rep_next_ms)
        return;
    emit_key(1, g_rep_key);
    g_rep_next_ms = now + KBD_REPEAT_PERIOD_MS;
}

/* One 96-byte sceKeyboardRead record: `modifiers` is the HID modifier
 * bitmap (bit 0 = left ctrl .. bit 7 = right GUI) and `keycodes` holds up
 * to 16 simultaneous usages. A record that is not connected, or that the
 * system already consumed, is skipped - otherwise every key would be typed
 * twice while the IME is up. */
static void feed_keyboard(int slot, const unsigned char *rec, input_frame *f)
{
    static const unsigned short mod_hid[8] = {
        0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7
    };
    const ps5_kb_record_t *r = (const ps5_kb_record_t *)rec;
    uint16_t *prev = g_prev_keys[slot];
    uint32_t mod;
    int i, j, new_key = 0;

    (void)f;
    g_kbd_connected[slot] = r->connected != 0;
    if (!r->connected || r->intercepted) {
        g_kbd_rec_skip++;
        /* An unplugged keyboard must not leave its last keys held, and a
         * key that the system just took over must stop repeating. */
        if (!r->connected)
            kbd_slot_release(slot);
        else if (g_rep_slot == slot)
            kbd_repeat_stop();
        return;
    }
    g_kbd_rec_ok++;

    mod = r->modifiers;

    if (g_prev_kbd_have[slot]) {
        for (i = 0; i < 8; ++i) {
            int was = (int)((g_prev_kbd_mod[slot] >> i) & 1u);
            int is  = (int)((mod >> i) & 1u);
            if (is && !was)
                emit_key(1, mod_hid[i]);
            else if (!is && was)
                emit_key(0, mod_hid[i]);
        }
        /* Released keys: present in the previous record, gone from this one. */
        for (i = 0; i < KBD_REC_MAX; ++i) {
            uint16_t k = prev[i];

            if (!k || (k >= 0xE0 && k <= 0xE7))
                continue;
            for (j = 0; j < KBD_REC_MAX; ++j)
                if (r->keycodes[j] == k)
                    break;
            if (j == KBD_REC_MAX) {
                if (!kbd_slot_holds(slot, k))
                    emit_key(0, k);
                if (g_rep_key == k && g_rep_slot == slot)
                    kbd_repeat_stop();
            }
        }
        /* Newly pressed keys. */
        for (i = 0; i < KBD_REC_MAX; ++i) {
            uint16_t k = r->keycodes[i];

            if (!k || (k >= 0xE0 && k <= 0xE7))
                continue;
            for (j = 0; j < KBD_REC_MAX; ++j)
                if (prev[j] == k)
                    break;
            if (j == KBD_REC_MAX) {
                emit_key(1, k);
                new_key = (int)k;   /* last press wins the repeat */
            }
        }
        if (new_key)
            kbd_repeat_start(slot, (uint16_t)new_key);
    } else {
        for (i = 0; i < 8; ++i)
            if ((mod >> i) & 1u)
                emit_key(1, mod_hid[i]);
        for (i = 0; i < KBD_REC_MAX; ++i) {
            uint16_t k = r->keycodes[i];

            if (!k || (k >= 0xE0 && k <= 0xE7))
                continue;
            emit_key(1, k);
        }
    }

    memcpy(prev, r->keycodes, sizeof(g_prev_keys[slot]));
    g_prev_kbd_mod[slot] = mod;
    g_prev_kbd_have[slot] = 1;
}

/* A hot-plugged keyboard has to be picked up, but calling open() every
 * frame is what made the log unreadable in 1.7.x: one attempt per ~300
 * frames (about 5 s at 60 fps) is enough for a device the user just
 * plugged in. */
static int g_kbd_retry_idle;

static void poll_keyboard(input_frame *f)
{
    unsigned char buf[KBD_REC_SIZE * KBD_REC_MAX];
    int32_t n;
    int slot, i, any = 0;

    for (slot = 0; slot < KBD_SLOTS; ++slot)
        if (g_kbd[slot] >= 0)
            any = 1;
    if (!any) {
        kbd_repeat_stop();
        if ((++g_kbd_retry_idle % 300) != 0)
            return;
        if (open_keyboard() < 0)
            return;
    }
    if (!p_kbd_read)
        return;

    for (slot = 0; slot < KBD_SLOTS; ++slot) {
        if (g_kbd[slot] < 0)
            continue;

        memset(buf, 0, sizeof(buf));
        n = p_kbd_read(g_kbd[slot], buf, KBD_REC_MAX);
        if (n < 0) {
            if (g_rep_slot == slot)
                kbd_repeat_stop();
            g_kbd_fail++;
            /* A slot that keeps failing is closed and retried later; the
             * other slot keeps running either way. */
            if ((g_kbd_fail % 120) == 0) {
                /* The slot dies while keys are down: lift them here, or the
                 * remote would keep them held forever. */
                kbd_slot_release(slot);
                if (p_kbd_close)
                    p_kbd_close(g_kbd[slot]);
                g_kbd[slot] = -1;
                g_kbd_connected[slot] = 0;
                g_kbd_open_rc[slot] = -1;
            }
            continue;
        }
        if (n == 0)
            continue;       /* slot open, nothing typed */
        g_kbd_reads += n;
        if (n > KBD_REC_MAX)
            n = KBD_REC_MAX;
        for (i = 0; i < n; ++i)
            feed_keyboard(slot, buf + (size_t)i * KBD_REC_SIZE, f);
        /* Keep the first record around for /diagnostics. */
        memcpy(g_kbd_raw, buf, sizeof(g_kbd_raw));
        g_kbd_raw_len = KBD_REC_SIZE;
    }
    kbd_repeat_tick();
}

/* ------------------------------------------------------------------ */
/* Mouse                                                               */
/* ------------------------------------------------------------------ */

/* The pointer never lands in input_frame: it goes straight to the sink.
 * That is deliberate - a menu can ignore the pointer simply by leaving
 * its sink detached. */
static void mouse_buttons(int slot, uint32_t next)
{
    static const uint32_t bits[] = { MOUSE_BTN_LEFT, MOUSE_BTN_RIGHT, MOUSE_BTN_MIDDLE,
                                     MOUSE_BTN_SIDE1, MOUSE_BTN_SIDE2 };
    uint32_t changed = next ^ g_mouse_slots[slot].sent;
    g_mouse_slots[slot].sent = next;
    if (g_sink.mouse_button)
        for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i)
            if (changed & bits[i])
                g_sink.mouse_button(i, (next & bits[i]) != 0);
}

static void poll_mouse(void)
{
    orbis_mouse_data batch[MOUSE_BATCH];
    open_mouse();
    for (int i = 0; i < MOUSE_SLOTS; ++i) {
        int handle = g_mouse_slots[i].handle;
        if (handle < 0)
            continue;
        int rc = p_mouse_read(handle, batch, MOUSE_BATCH);
        g_mouse_slots[i].read_rc = rc;
        if (rc < 0) {
            ++g_mouse_slots[i].errors;
            if (++g_mouse_slots[i].consecutive_errors >= 3) {
                mouse_buttons(i, 0);
                p_mouse_close(handle);
                g_mouse_slots[i].handle = -1;
                g_mouse_slots[i].connected = 0;
                g_mouse_slots[i].blocked = MOUSE_BUTTONS;
                g_mouse_slots[i].consecutive_errors = 0;
                g_mouse_slots[i].retry_at = kbd_now_ms() + 2000;
            }
            continue;
        }
        g_mouse_slots[i].consecutive_errors = 0;
        if (rc >= MOUSE_BATCH)
            ++g_mouse_full;
        if (rc > MOUSE_BATCH)
            rc = MOUSE_BATCH;
        for (int n = 0; n < rc; ++n) {
            const orbis_mouse_data *d = &batch[n];
            if (!d->connected || (d->buttons & MOUSE_INTERCEPTED)) {
                ++g_mouse_filtered;
                g_mouse_slots[i].connected = d->connected != 0;
                mouse_buttons(i, 0);
                g_mouse_slots[i].blocked = MOUSE_BUTTONS;
                continue;
            }
            ++g_mouse_slots[i].records;
            g_mouse_slots[i].connected = 1;
            uint32_t down = d->buttons & MOUSE_BUTTONS;
            if (!g_sink_ready) {
                g_mouse_slots[i].blocked = down;
                continue;
            }
            g_mouse_slots[i].blocked &= down;
            if ((d->x_axis || d->y_axis) && g_sink.mouse_rel)
                g_sink.mouse_rel(d->x_axis, d->y_axis);
            mouse_buttons(i, down & ~g_mouse_slots[i].blocked);
            if (d->wheel && g_sink.mouse_wheel) {
                int steps = d->wheel;
                if (steps > 4) steps = 4;
                if (steps < -4) steps = -4;
                g_sink.mouse_wheel(steps);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

int input_init(void)
{
    g_prev_have = 0;
    memset(g_prev_keys, 0, sizeof(g_prev_keys));
    memset(g_prev_kbd_mod, 0, sizeof(g_prev_kbd_mod));
    memset(g_prev_kbd_have, 0, sizeof(g_prev_kbd_have));
    memset(g_kbd_connected, 0, sizeof(g_kbd_connected));
    kbd_repeat_stop();
    load_input_imports();
    open_pad();
    open_keyboard();
    open_mouse();
    return (g_pad >= 0) ? 0 : -1;
}

void input_shutdown(void)
{
    int i;

    if (g_pad >= 0) {
        /* Only close what we opened ourselves; a handle taken from the
         * system belongs to the system. */
        if (g_pad_src == 0 && p_pad_close)
            p_pad_close(g_pad);
        g_pad = -1;
    }
    for (i = 0; i < KBD_SLOTS; ++i) {
        if (g_kbd[i] >= 0) {
            if (p_kbd_close)
                p_kbd_close(g_kbd[i]);
            g_kbd[i] = -1;
        }
        g_kbd_open_rc[i] = -1;
    }
    g_kbd_rc_open = -1;
    for (int slot = 0; slot < MOUSE_SLOTS; ++slot) {
        if (g_mouse_slots[slot].handle >= 0)
            p_mouse_close(g_mouse_slots[slot].handle);
        g_mouse_slots[slot].handle = -1;
        g_mouse_slots[slot].connected = 0;
        g_mouse_slots[slot].sent = 0;
        g_mouse_slots[slot].retry_at = 0;
    }
    g_left_down = 0;
    g_left_blocked = 0;
}

int input_pad_count(void)
{
    return g_pad >= 0 ? 1 : 0;
}

int input_keyboard_handle(void)
{
    return g_kbd_rc_open;
}

int input_keyboard_connected(void)
{
    for (int i = 0; i < KBD_SLOTS; ++i)
        if (g_kbd[i] >= 0 && g_kbd_connected[i]) return 1;
    return 0;
}

int input_mouse_connected(void)
{
    for (int i = 0; i < MOUSE_SLOTS; ++i)
        if (g_mouse_slots[i].handle >= 0 && g_mouse_slots[i].connected) return 1;
    return 0;
}

int input_mouse_source(void)
{
    return mouse_source();
}

void input_set_sink(const input_sink *sink)
{
    /* A repeat belongs to the session that owned the sink; never carry it
     * into a menu or a new session. */
    kbd_repeat_stop();
    touch_reset(NULL);
    g_stick_rem_x=g_stick_rem_y=0;
    g_pad_scale_rem_x=g_pad_scale_rem_y=0;
    /* main() releases the remote buttons before detaching. Forget the
     * merged state too, so a re-attach cannot inherit a stale press. If a
     * source is still held, the next press has to start with a release. */
    if (!sink) {
        for (int slot = 0; slot < MOUSE_SLOTS; ++slot) {
            g_mouse_slots[slot].blocked |= g_mouse_slots[slot].sent;
            g_mouse_slots[slot].sent = 0;
        }
    }
    g_left_down = 0;
    g_left_blocked = 0;
    if (sink) {
        g_touch_blocked=(g_prev_buttons & PB_TOUCHPAD)!=0;
        if (g_prev_buttons & (PB_CROSS|PB_TOUCHPAD))
            g_left_blocked = 1;
        g_sink = *sink;
        g_sink_ready = 1;
    } else {
        memset(&g_sink, 0, sizeof(g_sink));
        g_sink_ready = 0;
    }
}

void input_poll(input_frame *f)
{
    memset(f, 0, sizeof(*f));
    poll_pad(f);
    /* Options is reserved for the local session menu. Do not send keyboard
     * or mouse events from the frame that takes local input ownership. */
    if (!f->options) {
        poll_keyboard(f);
        poll_mouse();
    }
}

void input_kbd_dump(char *out, size_t cap)
{
    size_t o = 0;
    int i;
    if (!out || cap < 8)
        return;
    if (g_kbd_rc_open < 0) {
        snprintf(out, cap,
                 "keyboard: not open (init rc=%d fg rc=%d sysmod=0x%04x/0x%08x "
                 "slot0 rc=%d slot1 rc=%d)",
                 g_kbd_rc_init, g_kbd_rc_fguser,
                 (unsigned)(g_kbd_sysmodule_id < 0 ? 0 : g_kbd_sysmodule_id),
                 (unsigned)g_kbd_sysmodule_rc,
                 g_kbd_open_rc[0], g_kbd_open_rc[1]);
        return;
    }
    o += (size_t)snprintf(out + o, cap - o,
                          "keyboard: h=%d slot0=%d slot1=%d reads=%d fail=%d "
                          "ok=%d skip=%d notes=%d hex=",
                          g_kbd_rc_open, g_kbd[0], g_kbd[1],
                          g_kbd_reads, g_kbd_fail,
                          g_kbd_rec_ok, g_kbd_rec_skip, g_kbd_raw_len);
    for (i = 0; i < 16 && i < g_kbd_raw_len && o + 3 < cap; ++i)
        o += (size_t)snprintf(out + o, cap - o, "%02x", g_kbd_raw[i]);
    if (o + 1 < cap)
        out[o] = 0;
}

void input_mouse_dump(char *out, size_t cap)
{
    if (!out || cap < 8)
        return;
    size_t used = (size_t)snprintf(out, cap,
        "mouse: src=%d mod=%08x init=%08x uid=%d full=%u filtered=%u",
        mouse_source(), (unsigned)g_mouse_sys_rc, (unsigned)g_mouse_init_rc,
        g_mouse_user, g_mouse_full, g_mouse_filtered);
    for (int i = 0; i < MOUSE_SLOTS && used < cap; ++i) {
        used += (size_t)snprintf(out + used, cap - used,
            " s%d=%08x/%08x/%d/%u/%u", i,
            (unsigned)g_mouse_slots[i].open_rc, (unsigned)g_mouse_slots[i].read_rc,
            g_mouse_slots[i].connected, g_mouse_slots[i].records, g_mouse_slots[i].errors);
    }
}

/* Every step of the pad path, so one diagnostics fetch settles whether the
 * open failed, the handle came from the system, or reads simply never
 * deliver a sample. */
void input_pad_dump(char *out, size_t cap)
{
    size_t o;
    int i;

    if (!out || cap < 16)
        return;
    o = (size_t)snprintf(out, cap,
        "pad: h=%d via=%s usersvc=0x%08x padinit=0x%08x uid=%d "
        "open=0x%08x geth=0x%08x info=0x%08x reads=%d fail=%d "
        "btn=0x%08x intercept=%d syms=%s",
        g_pad, g_pad_src ? "gethandle" : "open",
        (unsigned)g_pad_rc_init, (unsigned)g_pad_sys_init, g_user,
        (unsigned)g_pad_rc_open, (unsigned)g_pad_rc_handle,
        (unsigned)g_pad_info_rc, g_pad_reads, g_pad_fail,
        (unsigned)g_pad_last_buttons, g_pad_intercept,
        g_sym_missing[0] ? g_sym_missing : "-");
    if (g_pad_first_have && o + 40 < cap) {
        o += (size_t)snprintf(out + o, cap - o, " first=");
        for (i = 0; i < 16 && o + 3 < cap; ++i)
            o += (size_t)snprintf(out + o, cap - o, "%02x", g_pad_first[i]);
    }
    if (o + 1 < cap)
        out[o] = 0;
}

/* Pad deltas use the original 1080p feel as a reference. Accumulate
 * fractional display pixels so small movements remain usable below 1080p. */
static int pad_scale(int delta, int extent, int reference, int *remainder)
{
    int n = delta * extent + *remainder;
    *remainder = n % reference;
    return n / reference;
}

/* Both pad sources feed one remote left button. Holding X or a touchpad
 * press keeps it down, so a stick or touchpad move drags; the button is
 * lifted only after both sources are released. */
static void left_button_apply(int want)
{
    if (g_left_blocked) {
        if (!want)
            g_left_blocked = 0;
        return;
    }
    if (want == g_left_down || !g_sink.mouse_button)
        return;
    g_sink.mouse_button(0, want);
    g_left_down = want;
}

/* Called by the pad path when a session is live: the stick drives the
 * pointer, the face buttons click, the d-pad and a few keys type. */
void input_pad_to_remote(const input_frame *f, int *acc_x, int *acc_y,
                         int *wheel_accum, int display_w, int display_h)
{
    if (!g_sink_ready)
        return;

    int dx = f->touch_dx + stick_motion(f->lx, &g_stick_rem_x);
    int dy = f->touch_dy + stick_motion(f->ly, &g_stick_rem_y);
    *acc_x += pad_scale(dx, display_w, 1920, &g_pad_scale_rem_x);
    *acc_y += pad_scale(dy, display_h, 1080, &g_pad_scale_rem_y);
    /* Pad Y grows downward; RDP positive wheel means scroll upward. */
    if (f->ry) {
        *wheel_accum -= f->ry / 64;
    }
    if (f->l1 && g_sink.mouse_wheel)
        g_sink.mouse_wheel(1);
    if (f->r1 && g_sink.mouse_wheel)
        g_sink.mouse_wheel(-1);

    if (g_sink.mouse_button) {
        /* A tap still becomes down+up; a hold stays down while the stick
         * or the touchpad moves the pointer, which is the drag we want. */
        left_button_apply(f->touch_held || f->cross_held);
    }
    if (f->cancel && g_sink.mouse_button) {
        g_sink.mouse_button(1, 1);
        g_sink.mouse_button(1, 0);
    }
    if (f->square && g_sink.key) {
        g_sink.key(1, 0x0E);   /* backspace */
        g_sink.key(0, 0x0E);
    }
    if (f->triangle && g_sink.key) {
        g_sink.key(1, 0x2B);   /* tab */
        g_sink.key(0, 0x2B);
    }
    if (f->up && g_sink.key)    { g_sink.key(1, 0x148); g_sink.key(0, 0x148); }
    if (f->down && g_sink.key)  { g_sink.key(1, 0x150); g_sink.key(0, 0x150); }
    if (f->left && g_sink.key)  { g_sink.key(1, 0x14B); g_sink.key(0, 0x14B); }
    if (f->right && g_sink.key) { g_sink.key(1, 0x14D); g_sink.key(0, 0x14D); }
}
