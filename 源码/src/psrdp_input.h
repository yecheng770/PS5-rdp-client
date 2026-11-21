/* psrdp_input.h - pad, keyboard and pointer, behind one poll.
 *
 * The console hands us three unrelated APIs: scePad* for the wireless
 * controller, sceKeyboard* for USB/Bluetooth keyboards, and nothing at all
 * for a mouse. This module hides that behind a single input_frame the
 * display loop consumes, and reports what actually opened so the config
 * page can say which devices are live instead of guessing.
 *
 * A sink is used for the remote session: while an RDP session is live the
 * events go to the server rather than to our own menus.
 */
#ifndef PSRDP_INPUT_H
#define PSRDP_INPUT_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    /* Edge-triggered, one frame only. */
    int up, down, left, right;      /* D-pad / left stick, debounced */
    int confirm, cancel;            /* Cross, Circle */
    int options;                    /* Options */
    int l1, r1;
    int square, triangle;           /* reserved for later mappings */

    /* Level, each frame. */
    int l2, r2;                     /* 1 while held */
    int lx, ly, rx, ry;             /* sticks, -128..127, dead zone applied */

    /* Level: the two remote left-button sources. X and a touchpad press are
     * merged, so either one held keeps the button down and a stick or
     * touchpad move drags until both are released. */
    int cross_held, touch_held;

    /* Accumulated across every pad sample in this poll. */
    int touch_dx, touch_dy;
    int touch_events[8], touch_event_count;

    int any;                        /* any of the edge flags is set */
} input_frame;

typedef struct {
    /* HID usage codes (as they arrive from the keyboard, or synthesised
     * from the pad). */
    void (*key)(int down, unsigned short hid_usage);
    void (*mouse_rel)(int dx, int dy);
    void (*mouse_button)(int button, int down);   /* 0 left, 1 right, 2 middle, 3/4 side */
    void (*mouse_wheel)(int notches);             /* +1 up, -1 down */
} input_sink;

int  input_init(void);
void input_shutdown(void);

/* Which devices are actually usable, for the diagnostics page. */
int  input_pad_count(void);
int  input_keyboard_handle(void);
int  input_keyboard_connected(void);
int  input_mouse_connected(void);
int  input_mouse_source(void);      /* 0 none, 1 /dev/uhid-ish, 2 dlsym */

void input_set_sink(const input_sink *sink);   /* NULL detaches */
void input_poll(input_frame *f);

/* Up to `cap` bytes of the last raw keyboard report, hex, for the page. */
void input_kbd_dump(char *out, size_t cap);

/* Same for the pointer, plus what the loader actually found. */
void input_mouse_dump(char *out, size_t cap);

/* Every step of the pad open and its first raw sample, hex. */
void input_pad_dump(char *out, size_t cap);

/* Pad-only convenience mapping used while an RDP session is live: the
 * stick moves the pointer, the face buttons click. Mouse and keyboard
 * feed the sink directly and do not come through here. */
void input_pad_to_remote(const input_frame *f, int *acc_x, int *acc_y,
                         int *wheel_accum, int display_w, int display_h);

#endif /* PSRDP_INPUT_H */
