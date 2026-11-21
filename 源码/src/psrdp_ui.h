/* psrdp_ui.h - 2D primitives and the on-screen pages.
 *
 * Everything is laid out on a 1920x1080 canvas and scaled to the live
 * surface, so 720p and 4K share one layout. */
#ifndef PSRDP_UI_H
#define PSRDP_UI_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int width;
    int height;
    int pitch;           /* pixels per row, not bytes */
    uint32_t *pixels;    /* 0x00RRGGBB */
} ui_surface;

enum { UI_ZH, UI_EN, UI_JA, UI_FR, UI_RU, UI_LANG_COUNT };
enum { UI_READY, UI_CONNECTING, UI_CONNECTED, UI_DISCONNECTED, UI_FAILED };
enum { UI_LANG_X=1510, UI_LANG_Y=58, UI_LANG_W=324, UI_LANG_H=60,
       UI_LANG_LIST_Y=124, UI_LANG_ROW_H=55 };
typedef enum {
    UI_HOME_LANGUAGE, UI_HOME_RESOLUTION, UI_HOME_SCALE,
    UI_HOME_SETTINGS, UI_HOME_CONNECT, UI_HOME_COUNT
} ui_home_focus;
typedef struct { int x, y, w, h; } ui_home_rect;
extern const ui_home_rect ui_home_controls[UI_HOME_COUNT];
extern const char *const ui_language_codes[UI_LANG_COUNT];
void ui_set_language(int language);
const char *ui_localize(const char *english);

typedef struct {
    const char *host;
    int port;
    const char *user;
    const char *res;
    unsigned desktop_scale;
    int status;
    int status_bad;
    int failure;
    ui_home_focus focus;
    int mouse_connected, keyboard_connected;
    int busy;                /* connect greyed out */
    int custom;             /* 0 preset, 1 custom, 2 width, 3 height */
    int language_open, language_focus;
    const char *output;
} ui_home;

enum {
    UI_CONN_HOST, UI_CONN_PORT, UI_CONN_USER, UI_CONN_PASS,
    UI_CONN_REMEMBER, UI_CONN_HISTORY, UI_CONN_SAVE, UI_CONN_COUNT
};
enum { UI_KEY_COLS = 12, UI_KEY_ROWS = 4, UI_KEY_PAGES = 3 };
typedef struct {
    const char *host, *port, *user, *history, *message, *entry;
    int has_password, remember, focus, has_history, key_open, key_index, key_page;
} ui_connection;
char ui_keyboard_char(int page, int index);
void ui_draw_connection(const ui_connection *m);

/* Decode the embedded font. Returns 0 when the face is usable. */
int  ui_init(void);
int  ui_ready(void);
void ui_begin(ui_surface *s);

void ui_clear(uint32_t color);
void ui_fill(int x, int y, int w, int h, uint32_t color);
void ui_fill_a(int x, int y, int w, int h, uint32_t color, int alpha);
void ui_round(int x, int y, int w, int h, int r, uint32_t color, int alpha);
void ui_border(int x, int y, int w, int h, int r, uint32_t color);

void ui_text(int x, int y, int px, uint32_t color, const char *text);
void ui_text_center(int cx, int y, int px, uint32_t color, const char *text);
int  ui_width(int px, const char *text);

void ui_draw_home(const ui_home *m);
void ui_draw_session_menu(int stopping);
/* Full-width translucent plate with a headline; used for disconnect and
 * fatal-display states that sit on top of a live frame. */
void ui_draw_banner(const char *title, const char *detail, const char *hint);
void ui_draw_cursor(int x, int y);

#endif /* PSRDP_UI_H */
