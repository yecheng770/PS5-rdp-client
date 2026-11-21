/* Local session menu: no platform calls, so transitions are host-testable. */
#ifndef PSRDP_SESSION_MENU_H
#define PSRDP_SESSION_MENU_H
#include "psrdp_input.h"
typedef struct { int open, stopping; } psrdp_session_menu;
enum { SESSION_MENU_NONE, SESSION_MENU_OPEN, SESSION_MENU_CLOSE, SESSION_MENU_DISCONNECT };
static inline int session_menu_step(psrdp_session_menu *m,const input_frame *f)
{
    if (m->stopping) return SESSION_MENU_NONE;
    if (!m->open) {
        if (!f->options) return SESSION_MENU_NONE;
        m->open=1;
        return SESSION_MENU_OPEN;
    }
    if (f->options || f->cancel) {
        m->open=0;
        return SESSION_MENU_CLOSE;
    }
    if (!f->confirm) return SESSION_MENU_NONE;
    m->stopping=1;
    return SESSION_MENU_DISCONNECT;
}
#endif
