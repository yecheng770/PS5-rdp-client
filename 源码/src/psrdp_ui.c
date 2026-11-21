/* psrdp_ui.c - software 2D and the two on-screen pages.
 *
 * Authoring space is 1920x1080; every coordinate goes through S() so the
 * same layout lands correctly on 720p, 1440p and 4K surfaces. Text uses the
 * embedded Inter-Regular, decoded from hex at startup: no asset directory,
 * no filesystem permission to negotiate.
 *
 * Glyphs are cached per (pixel size, codepoint) with round-robin eviction.
 * The cache is sized for the handful of strings the two pages carry, not for
 * general typesetting.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "psrdp_glue.h"
#include "psrdp_ui.h"
#include "psrdp_dualsense.inc"
#include "psrdp_globe.inc"
#include "psrdp_github.inc"

/* stb_truetype is vendored C with a lot of static helpers we never call;
 * keep the project's strict warning set from turning those into errors. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wpedantic"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma GCC diagnostic pop

/* The embedded face is one 823 kB string literal; C99 only guarantees 4095
 * bytes of string literal, so the pedantic warning has to be switched off
 * around it. Every toolchain we build with handles far larger. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#include "psrdp_font_hex.h"
#include "psrdp_cjk_hex.h"
#pragma GCC diagnostic pop

/* Palette: one dark surface, one accent, two semantic colours. */
#define C_BG      0x000E1116u
#define C_PANEL   0x00171B21u
#define C_EDGE    0x00262C34u
#define C_TEXT    0x00E8ECF1u
#define C_DIM     0x008A93A0u
#define C_ACCENT  0x003B82F6u
#define C_OK      0x004ADE80u
#define C_BAD     0x00EF4444u
#define C_WHITE   0x00FFFFFFu

/* ------------------------------------------------------------------ */
/* Font                                                                */
/* ------------------------------------------------------------------ */

static unsigned char g_face[PSRDP_FONT_BYTES];
static stbtt_fontinfo g_font, g_cjk_zh, g_cjk_ja;
static int g_language = UI_ZH;
static unsigned char g_face_zh[sizeof(psrdp_cjk_zh_hex)/2];
static unsigned char g_face_ja[sizeof(psrdp_cjk_ja_hex)/2];
static int g_ok;

static int hex_nib(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int load_face(stbtt_fontinfo *font, unsigned char *dest, size_t size, const char *hex)
{
    for (size_t i = 0; i < size; ++i) {
        int hi = hex_nib((unsigned char)hex[i * 2]);
        int lo = hex_nib((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        dest[i] = (unsigned char)((hi << 4) | lo);
    }
    return stbtt_InitFont(font, dest, 0) ? 0 : -2;
}

int ui_init(void)
{
    g_ok = 0;
    if (load_face(&g_font, g_face, sizeof(g_face), psrdp_font_hex) ||
        load_face(&g_cjk_zh, g_face_zh, sizeof(g_face_zh), psrdp_cjk_zh_hex) ||
        load_face(&g_cjk_ja, g_face_ja, sizeof(g_face_ja), psrdp_cjk_ja_hex))
        return -2;
    g_ok = 1;
    return 0;
}

int ui_ready(void)
{
    return g_ok;
}

/* ------------------------------------------------------------------ */
/* Surface                                                             */
/* ------------------------------------------------------------------ */

static ui_surface g_s;
static int g_ui_height;

/* 1080p authoring units -> pixels on the live surface. */
static int S(int v)
{
    long r = (long)v * g_ui_height;
    return (int)(r / 1080);
}

void ui_begin(ui_surface *s)
{
    g_s = *s;
    if (g_s.pitch <= 0)
        g_s.pitch = g_s.width;
    g_ui_height = g_s.height;
}

static void blend(int x, int y, uint32_t color, unsigned int a)
{
    uint32_t d, sr, sg, sb, dr, dg, db, ia;
    uint32_t *p;
    if (!g_s.pixels || a == 0)
        return;
    if (x < 0 || y < 0 || x >= g_s.width || y >= g_s.height)
        return;
    p = g_s.pixels + (size_t)y * (size_t)g_s.pitch + (size_t)x;
    if (a >= 255) {
        *p = color;
        return;
    }
    d = *p;
    dr = (d >> 16) & 0xffu;
    dg = (d >> 8) & 0xffu;
    db = d & 0xffu;
    sr = (color >> 16) & 0xffu;
    sg = (color >> 8) & 0xffu;
    sb = color & 0xffu;
    ia = 255u - a;
    dr = (sr * a + dr * ia) / 255u;
    dg = (sg * a + dg * ia) / 255u;
    db = (sb * a + db * ia) / 255u;
    *p = (dr << 16) | (dg << 8) | db;
}

void ui_clear(uint32_t color)
{
    int y;
    if (!g_s.pixels)
        return;
    for (y = 0; y < g_s.height; ++y) {
        uint32_t *p = g_s.pixels + (size_t)y * (size_t)g_s.pitch;
        int x;
        for (x = 0; x < g_s.width; ++x)
            p[x] = color;
    }
}

void ui_fill(int x, int y, int w, int h, uint32_t color)
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h, yy;
    if (!g_s.pixels || w <= 0 || h <= 0)
        return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_s.width) x1 = g_s.width;
    if (y1 > g_s.height) y1 = g_s.height;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (yy = y0; yy < y1; ++yy) {
        uint32_t *p = g_s.pixels + (size_t)yy * (size_t)g_s.pitch + x0;
        int n = x1 - x0;
        while (n-- > 0)
            *p++ = color;
    }
}

void ui_fill_a(int x, int y, int w, int h, uint32_t color, int alpha)
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h, yy, xx;
    if (!g_s.pixels || w <= 0 || h <= 0 || alpha <= 0)
        return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_s.width) x1 = g_s.width;
    if (y1 > g_s.height) y1 = g_s.height;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (yy = y0; yy < y1; ++yy)
        for (xx = x0; xx < x1; ++xx)
            blend(xx, yy, color, (unsigned int)alpha);
}

/* Rounded rectangle. Radius is clamped so a squashed call still lands. */
void ui_round(int x, int y, int w, int h, int r, uint32_t color, int alpha)
{
    int i;
    if (!g_s.pixels || w <= 0 || h <= 0)
        return;
    if (r < 0) r = 0;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    if (r == 0) {
        if (alpha >= 255) ui_fill(x, y, w, h, color);
        else ui_fill_a(x, y, w, h, color, alpha);
        return;
    }

    /* Middle band and the two side slabs are rectangles. */
    if (alpha >= 255) {
        ui_fill(x + r, y, w - 2 * r, h, color);
        ui_fill(x, y + r, r, h - 2 * r, color);
        ui_fill(x + w - r, y + r, r, h - 2 * r, color);
    } else {
        ui_fill_a(x + r, y, w - 2 * r, h, color, alpha);
        ui_fill_a(x, y + r, r, h - 2 * r, color, alpha);
        ui_fill_a(x + w - r, y + r, r, h - 2 * r, color, alpha);
    }

    /* Four quarter discs. */
    for (i = 0; i < r; ++i) {
        float dy = (float)(r - i) - 0.5f;
        float inside = (float)(r * r) - dy * dy;
        int inset;
        if (inside < 0.0f)
            inside = 0.0f;
        inset = r - (int)sqrtf(inside);
        if (inset < 0) inset = 0;
        if (inset > r) inset = r;
        {
            int span = r - inset;
            if (span <= 0)
                continue;
            if (alpha >= 255) {
                ui_fill(x + inset, y + i, span, 1, color);
                ui_fill(x + w - r, y + i, span, 1, color);
                ui_fill(x + w - r, y + h - 1 - i, span, 1, color);
                ui_fill(x + inset, y + h - 1 - i, span, 1, color);
            } else {
                ui_fill_a(x + inset, y + i, span, 1, color, alpha);
                ui_fill_a(x + w - r, y + i, span, 1, color, alpha);
                ui_fill_a(x + w - r, y + h - 1 - i, span, 1, color, alpha);
                ui_fill_a(x + inset, y + h - 1 - i, span, 1, color, alpha);
            }
        }
    }
}

void ui_border(int x, int y, int w, int h, int r, uint32_t color)
{
    (void)r;    /* square corners are fine for a focus ring */
    if (w <= 0 || h <= 0)
        return;
    ui_fill(x, y, w, 2, color);
    ui_fill(x, y + h - 2, w, 2, color);
    ui_fill(x, y, 2, h, color);
    ui_fill(x + w - 2, y, 2, h, color);
}

/* ------------------------------------------------------------------ */
/* Glyph cache                                                         */
/* ------------------------------------------------------------------ */

/* Sized for the two pages, not for general typesetting: at 4K the largest
 * type on screen is 56 authoring px, which rasterises to about 112 device
 * px, so a 128px cell covers every glyph we ask for. */
#define GLYPH_SLOTS 200
#define GLYPH_SIDE  128

typedef struct {
    uint32_t key;
    int w, h, xoff, yoff, adv;
    unsigned char bmp[GLYPH_SIDE * GLYPH_SIDE];
} glyph_slot;

static glyph_slot g_glyph[GLYPH_SLOTS];
static int g_glyph_n;
static int g_glyph_rr;

static int utf8_next(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    int c = *p++;
    if (c == 0) {
        *pp = (const char *)p - 1;
        return 0;
    }
    if (c < 0x80) {
        *pp = (const char *)p;
        return c;
    }
    if ((c & 0xe0) == 0xc0) {
        int c2 = *p;
        if (c2 == 0 || (c2 & 0xc0) != 0x80) {
            *pp = (const char *)p;
            return 0xfffd;
        }
        *pp = (const char *)(p + 1);
        return ((c & 0x1f) << 6) | (c2 & 0x3f);
    }
    if ((c & 0xf0) == 0xe0) {
        int c2 = p[0], c3 = p[1];
        if (c2 == 0 || c3 == 0 || (c2 & 0xc0) != 0x80 || (c3 & 0xc0) != 0x80) {
            *pp = (const char *)p;
            return 0xfffd;
        }
        *pp = (const char *)(p + 2);
        return ((c & 0x0f) << 12) | ((c2 & 0x3f) << 6) | (c3 & 0x3f);
    }
    if ((c & 0xf8) == 0xf0) {
        int c2 = p[0], c3 = p[1], c4 = p[2];
        if (c2 == 0 || c3 == 0 || c4 == 0 ||
            (c2 & 0xc0) != 0x80 || (c3 & 0xc0) != 0x80 || (c4 & 0xc0) != 0x80) {
            *pp = (const char *)p;
            return 0xfffd;
        }
        *pp = (const char *)(p + 3);
        return ((c & 0x07) << 18) | ((c2 & 0x3f) << 12) |
               ((c3 & 0x3f) << 6) | (c4 & 0x3f);
    }
    *pp = (const char *)p;
    return 0xfffd;
}

static glyph_slot *glyph_lookup(int px, int cp)
{
    stbtt_fontinfo *font = &g_font;
    if (!stbtt_FindGlyphIndex(font, cp)) {
        font = g_language == UI_JA ? &g_cjk_ja : &g_cjk_zh;
        if (!stbtt_FindGlyphIndex(font, cp))
            font = font == &g_cjk_ja ? &g_cjk_zh : &g_cjk_ja;
        if (!stbtt_FindGlyphIndex(font, cp)) font = &g_font;
    }
    unsigned face = font == &g_cjk_ja ? 2u : font == &g_cjk_zh ? 1u : 0u;
    uint32_t key = ((uint32_t)px << 23) | (face << 21) | (uint32_t)cp;
    int i, idx;
    glyph_slot *g;
    float scale;
    int adv = 0, lsb = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    if (key == 0)
        key = 1;
    for (i = 0; i < g_glyph_n; ++i)
        if (g_glyph[i].key == key)
            return &g_glyph[i];

    if (g_glyph_n < GLYPH_SLOTS) {
        idx = g_glyph_n++;
    } else {
        idx = g_glyph_rr;
        g_glyph_rr = (g_glyph_rr + 1) % GLYPH_SLOTS;
    }
    g = &g_glyph[idx];
    memset(g, 0, sizeof(*g));
    g->key = key;

    scale = stbtt_ScaleForPixelHeight(font, (float)px);
    stbtt_GetCodepointHMetrics(font, cp, &adv, &lsb);
    (void)lsb;
    g->adv = (int)((float)adv * scale + 0.5f);

    stbtt_GetCodepointBitmapBox(font, cp, scale, scale, &x0, &y0, &x1, &y1);
    {
        int w = x1 - x0;
        int h = y1 - y0;
        if (w > 0 && h > 0 && w <= GLYPH_SIDE && h <= GLYPH_SIDE) {
            stbtt_MakeCodepointBitmap(font, g->bmp, w, h, GLYPH_SIDE,
                                      scale, scale, cp);
            g->w = w;
            g->h = h;
        }
    }
    g->xoff = x0;
    g->yoff = y0;
    return g;
}

static int line_ascent(int px)
{
    int a = 0, d = 0, gap = 0;
    float scale;
    stbtt_GetFontVMetrics(&g_font, &a, &d, &gap);
    (void)d;
    (void)gap;
    scale = stbtt_ScaleForPixelHeight(&g_font, (float)px);
    return (int)((float)a * scale + 0.5f);
}

void ui_text(int x, int y, int px, uint32_t color, const char *text)
{
    const char *p;
    int baseline, pen;
    if (!g_ok || !g_s.pixels || !text || px < 6)
        return;
    baseline = y + line_ascent(px);
    pen = x;
    p = text;
    while (*p) {
        int cp = utf8_next(&p);
        glyph_slot *g;
        int gy;
        if (cp == 0)
            break;
        g = glyph_lookup(px, cp);
        for (gy = 0; gy < g->h; ++gy) {
            int sy = baseline + g->yoff + gy;
            const unsigned char *src = g->bmp + (size_t)gy * GLYPH_SIDE;
            int gx;
            if (sy < 0 || sy >= g_s.height)
                continue;
            for (gx = 0; gx < g->w; ++gx) {
                unsigned char a = src[gx];
                if (a)
                    blend(pen + g->xoff + gx, sy, color, a);
            }
        }
        pen += g->adv;
    }
}

int ui_width(int px, const char *text)
{
    const char *p;
    int w = 0;
    if (!g_ok || !text || px < 6)
        return 0;
    p = text;
    while (*p) {
        int cp = utf8_next(&p);
        if (cp == 0)
            break;
        w += glyph_lookup(px, cp)->adv;
    }
    return w;
}

void ui_text_center(int cx, int y, int px, uint32_t color, const char *text)
{
    ui_text(cx - ui_width(px, text) / 2, y, px, color, text);
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

/* Controller guide uses geometry for the symbols, independent of fonts. */
static void guide_line(int x0,int y0,int x1,int y1,uint32_t color)
{
    int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,err=dx+dy;
    for (;;) {
        ui_fill(S(x0),S(y0),S(2),S(2),color);
        if(x0==x1 && y0==y1) break;
        int e=2*err;if(e>=dy){err+=dy;x0+=sx;}if(e<=dx){err+=dx;y0+=sy;}
    }
}
static void button_hint(int x, int y, char shape, const char *label)
{
    uint32_t color = C_TEXT;
    switch (shape) {
    case 'o':
        {
            const int radius = S(10);
            const float half_stroke = (float)S(3) * 0.5f;
            for (int dy = -radius - S(3); dy <= radius + S(3); ++dy) {
                for (int dx = -radius - S(3); dx <= radius + S(3); ++dx) {
                    const float distance = sqrtf((float)(dx * dx + dy * dy));
                    const float coverage = half_stroke + 0.5f - fabsf(distance - radius);
                    if (coverage > 0.0f)
                        blend(S(x) + dx, S(y) + dy, color,
                              coverage >= 1.0f ? 255u : (unsigned)(coverage * 255.0f));
                }
            }
        }
        break;
    case 's':
        ui_border(S(x-10), S(y-10), S(20), S(20), 0, color);
        break;
    case 't':
        guide_line(x, y-11, x-11, y+9, color);
        guide_line(x, y-11, x+11, y+9, color);
        guide_line(x-11, y+9, x+11, y+9, color);
        break;
    case 'x':
        guide_line(x-8, y-8, x+8, y+8, color);
        guide_line(x+8, y-8, x-8, y+8, color);
        break;
    }
    ui_text(S(x+23), S(y-13), S(22), color, label);
}

static void text_fit(int x, int y, int max_width, int size, uint32_t color, const char *text)
{
    char line[192];
    if (!text) return;
    snprintf(line, sizeof(line), "%s", text);
    if (ui_width(S(size), line) > S(max_width)) {
        size_t n = strlen(line);
        if (n > sizeof(line)-4) n = sizeof(line)-4;
        do {
            if (n) do { --n; } while (n && ((unsigned char)line[n] & 0xc0) == 0x80);
            memcpy(line+n, "...", 4);
        } while (n && ui_width(S(size), line) > S(max_width));
    }
    ui_text(S(x), S(y), S(size), color, line);
}

static void draw_dualsense(void)
{
    const int left = S(1174), top = S(322), size = S(DUALSENSE_SIZE);
    for (int y = 0; y < size; ++y) {
        int sy = y * DUALSENSE_SIZE / size;
        for (int x = 0; x < size; ++x) {
            int sx = x * DUALSENSE_SIZE / size;
            unsigned alpha = dualsense_alpha[sy * DUALSENSE_SIZE + sx];
            if (alpha)
                blend(left + x, top + y, 0x00DCE7FCu, alpha);
        }
    }
}

static void guide_anchor(int x, int y)
{
    ui_round(S(x-4), S(y-4), S(8), S(8), S(4), 0x0099CFFFu, 255);
}

static void guide_callout(int ax, int ay, int bend, int y, int right,
                          const char *label)
{
    const uint32_t line = 0x006584ADu;
    int end = right ? 1650 : 1160;
    guide_line(ax, ay, bend, y, line);
    guide_line(bend, y, end, y, line);
    guide_anchor(ax, ay);
    int label_x = right ? S(end + 12) : S(end - 12) - ui_width(S(18), label);
    ui_text(label_x, S(y - 10), S(18), C_TEXT, label);
}

const char *const ui_language_codes[UI_LANG_COUNT] = { "zh", "en", "ja", "fr", "ru" };
static const char *const language_names[UI_LANG_COUNT] = { "简体中文", "English", "日本語", "Français", "Русский" };
const ui_home_rect ui_home_controls[UI_HOME_COUNT] = {
    [UI_HOME_LANGUAGE]   = { UI_LANG_X, UI_LANG_Y, UI_LANG_W, UI_LANG_H },
    [UI_HOME_RESOLUTION] = { 126, 531, 755, 60 },
    [UI_HOME_SCALE]      = { 126, 650, 755, 60 },
    [UI_HOME_SETTINGS]   = { 126, 731, 755, 54 },
    [UI_HOME_CONNECT]    = { 126, 801, 755, 88 },
};

void ui_set_language(int language)
{
    g_language = (unsigned)language < UI_LANG_COUNT ? language : UI_ZH;
}

static const char *const translations[][UI_LANG_COUNT] = {
    { "远程桌面", "REMOTE DESKTOP", "リモートデスクトップ", "Bureau à distance", "Удалённый рабочий стол" },
    { "Windows 电脑", "YOUR WINDOWS PC", "Windows PC", "Votre PC Windows", "Ваш компьютер Windows" },
    { "用户", "USER", "ユーザー", "Utilisateur", "Пользователь" },
    { "默认用户", "Default user", "既定のユーザー", "Utilisateur par défaut", "Пользователь по умолчанию" },
    { "就绪", "Ready", "準備完了", "Prêt", "Готово" },
    { "正在连接…", "Connecting...", "接続中…", "Connexion en cours…", "Подключение…" },
    { "已连接", "Connected", "接続済み", "Connecté", "Подключено" },
    { "断开连接", "Disconnected", "切断されました", "Déconnecté", "Соединение разорвано" },
    { "连接失败", "Connection failed", "接続できませんでした", "Échec de la connexion", "Не удалось подключиться" },
    { "无法解析电脑名称，请检查名称或 IP 地址", "PC name not found. Check the name or IP.", "PC 名または IP アドレスを確認", "Vérifiez le nom du PC ou son adresse IP", "Проверьте имя или IP-адрес ПК" },
    { "无法连接端口 %d，请检查远程桌面和防火墙", "Port %d unreachable. Check RDP and firewall.", "ポート %d に接続できません。RDP とファイアウォールを確認", "Port %d inaccessible. Vérifiez RDP et le pare-feu.", "Порт %d недоступен. Проверьте RDP и брандмауэр." },
    { "密码错误，请检查后重试", "Incorrect password. Check and retry.", "パスワードが違います。確認して再試行", "Mot de passe incorrect. Vérifiez et réessayez.", "Неверный пароль. Проверьте и повторите." },
    { "Windows 拒绝登录，请检查账号、密码或权限", "Windows denied sign-in. Check account and permissions.", "Windows がログインを拒否しました。資格情報と権限を確認", "Windows refuse la connexion. Vérifiez le compte et les droits.", "Windows отклонила вход. Проверьте данные и права." },
    { "远程桌面启动失败，请重试", "Remote Desktop setup failed. Try again.", "リモートデスクトップの開始に失敗しました。再試行", "Le Bureau à distance n’a pas démarré. Réessayez.", "Не удалось запустить удалённый рабочий стол. Повторите." },
    { "远程会话中断，请重新连接", "Remote session interrupted. Reconnect.", "リモートセッションが中断しました。再接続", "Session distante interrompue. Reconnectez-vous.", "Удалённый сеанс прерван. Подключитесь снова." },
    { "原因未确定，请重试", "Cause unavailable. Try again.", "原因不明です。再試行", "Cause inconnue. Réessayez.", "Причина неизвестна. Повторите попытку." },
    { "连接设置", "Connection settings", "接続設定", "Paramètres de connexion", "Настройки подключения" },
    { "连接", "Connect", "接続", "Connecter", "Подключиться" },
    { "桌面分辨率", "DESKTOP RESOLUTION", "デスクトップ解像度", "Résolution du bureau", "Разрешение рабочего стола" },
    { "Windows 缩放", "WINDOWS SCALE", "Windows の拡大率", "Mise à l’échelle Windows", "Масштаб Windows" },
    { "上下选择　左右调整", "Up/down: select   Left/right: change", "上下で選択　左右で変更", "Haut/bas : choisir · Gauche/droite : modifier", "Вверх/вниз: выбор · Влево/вправо: изменение" },
    { "远程桌面操作", "REMOTE DESKTOP CONTROLS", "リモート操作", "Commandes du bureau à distance", "Управление рабочим столом" },
    { "滑动移动指针／按住滑动拖动", "Swipe to move / press and move to drag", "スワイプで移動／押しながらドラッグ", "Glisser pour déplacer · Appuyer et glisser pour tirer", "Смахивание: движение · Нажатие и движение: перетаскивание" },
    { "滚动", "Scroll", "スクロール", "Défiler", "Прокрутка" },
    { "方向键", "Arrow keys", "方向キー", "Flèches", "Стрелки" },
    { "移动指针", "Move pointer", "ポインター移動", "Déplacer", "Указатель" },
    { "会话菜单", "Session menu", "セッションメニュー", "Menu de session", "Меню сеанса" },
    { "右键", "Right click", "右クリック", "Clic droit", "Правый клик" },
    { "退格", "Backspace", "削除", "Effacer", "Удалить" },
    { "单击／拖动", "Click / drag", "クリック／ドラッグ", "Clic / glisser", "Клик / перетаск." },
    { "上下滚动", "Scroll up/down", "上下スクロール", "Défiler haut/bas", "Прокрутка вверх/вниз" },
    { "宽度：方向键 ±1 或 10，L1/R1 ±100", "Width: D-pad +/-1 or 10, L1/R1 +/-100", "幅：方向キー ±1 または 10、L1/R1 ±100", "Largeur : flèches ±1 ou 10, L1/R1 ±100", "Ширина: стрелки ±1 или 10, L1/R1 ±100" },
    { "高度：方向键 ±1 或 10，L1/R1 ±100", "Height: D-pad +/-1 or 10, L1/R1 +/-100", "高さ：方向キー ±1 または 10、L1/R1 ±100", "Hauteur : flèches ±1 ou 10, L1/R1 ±100", "Высота: стрелки ±1 или 10, L1/R1 ±100" },
    { "确认后编辑自定义尺寸", "Edit custom size", "決定でサイズを編集", "Valider pour modifier la taille", "Подтвердите для изменения размера" },
    { "（自定义）", " (Custom)", "（カスタム）", " (Personnalisée)", " (Своя)" },
    { "鼠标", "Mouse", "マウス", "Souris", "Мышь" },
    { "键盘", "Keyboard", "キーボード", "Clavier", "Клавиатура" },
    { "移动鼠标或按下键盘按键以识别设备。", "Move the mouse or press a key to detect the device.", "マウスを動かすか、キーを押すと認識します。", "Bougez la souris ou appuyez sur une touche pour détecter le périphérique.", "Для обнаружения устройства подвигайте мышью или нажмите клавишу." },
    { "无设备", "No device", "未接続", "Aucun appareil", "Нет устройства" },
    { "主机", "Host", "ホスト", "Hôte", "Хост" },
    { "端口", "Port", "ポート", "Port", "Порт" },
    { "用户名", "Username", "ユーザー名", "Identifiant", "Имя пользователя" },
    { "密码", "Password", "パスワード", "Mot de passe", "Пароль" },
    { "记住密码", "Remember password", "パスワードを保存", "Mémoriser le mot de passe", "Запомнить пароль" },
    { "连接记录", "History", "接続履歴", "Historique", "История" },
    { "保存", "Save", "保存", "Enregistrer", "Сохранить" },
    { "主机名或 IP 地址", "Hostname or IP address", "ホスト名または IP アドレス", "Nom d’hôte ou adresse IP", "Имя хоста или IP-адрес" },
    { "（空）", "(empty)", "（空）", "(vide)", "(пусто)" },
    { "开", "On", "オン", "Oui", "Вкл." },
    { "关", "Off", "オフ", "Non", "Выкл." },
    { "上下选择　左右浏览", "Up/down: select   Left/right: browse", "上下で選択　左右で切り替え", "Haut/bas : choisir · Gauche/droite : parcourir", "Вверх/вниз: выбор · Влево/вправо: просмотр" },
    { "选择", "Select", "選択", "Choisir", "Выбрать" },
    { "返回", "Back", "戻る", "Retour", "Назад" },
    { "L1 / R1    小写", "L1 / R1    Lowercase", "L1 / R1    小文字", "L1 / R1    Minuscules", "L1 / R1    Строчные" },
    { "L1 / R1    大写", "L1 / R1    Uppercase", "L1 / R1    大文字", "L1 / R1    Majuscules", "L1 / R1    Прописные" },
    { "L1 / R1    符号", "L1 / R1    Symbols", "L1 / R1    記号", "L1 / R1    Symboles", "L1 / R1    Символы" },
    { "空格", "Space", "空白", "Espace", "Пробел" },
    { "输入", "Type", "入力", "Saisir", "Ввод" },
    { "应用", "Apply", "適用", "Appliquer", "Применить" },
    { "取消", "Cancel", "キャンセル", "Annuler", "Отмена" },
    { "正在断开连接…", "Closing the remote connection...", "接続を切断中…", "Déconnexion en cours…", "Завершение подключения…" },
    { "远程桌面仍在连接", "Your remote desktop is still connected", "リモートデスクトップに接続中", "Le bureau à distance est toujours connecté", "Удалённый рабочий стол всё ещё подключён" },
    { "断开连接", "Disconnect", "切断", "Déconnecter", "Отключиться" },
    { "请稍候", "Please wait", "お待ちください", "Veuillez patienter", "Подождите" },
    { "／Options：继续", "/ Options: Resume", "／Options：再開", "/ Options : Reprendre", "/ Options: продолжить" },
    { "没有已保存的连接", "No saved connections", "保存済みの接続はありません", "Aucune connexion enregistrée", "Нет сохранённых подключений" },
    { "已载入记录，选择保存即可使用。", "Loaded into editor. Select Save to use this connection.", "読み込みました。使用するには保存してください。", "Connexion chargée. Choisissez Enregistrer pour l’utiliser.", "Подключение загружено. Выберите «Сохранить»." },
    { "请输入主机名或 IP 地址。", "Enter a hostname or IP address.", "ホスト名または IP アドレスを入力してください。", "Saisissez un nom d’hôte ou une adresse IP.", "Введите имя хоста или IP-адрес." },
    { "端口必须在 1 到 65535 之间。", "Port must be between 1 and 65535.", "ポートは 1～65535 を指定してください。", "Le port doit être compris entre 1 et 65535.", "Порт должен быть от 1 до 65535." },
    { "连接设置保存失败。", "Could not save connection settings.", "接続設定を保存できませんでした。", "Impossible d’enregistrer les paramètres de connexion.", "Не удалось сохранить настройки подключения." },
    { "正在等待远程画面", "Waiting for the first frame", "画面を待っています", "En attente de la première image", "Ожидание первого кадра" },
};

const char *ui_localize(const char *english)
{
    if (!english || g_language == UI_EN) return english;
    for (size_t i = 0; i < sizeof(translations) / sizeof(translations[0]); ++i)
        if (!strcmp(english, translations[i][UI_EN])) return translations[i][g_language];
    return english;
}

static void draw_device_status(int x, const char *label, int connected)
{
    ui_round(S(x), S(802), S(379), S(78), S(14), 0x00202D45u, 255);
    text_fit(x + 20, 812, 339, 21, C_WHITE, ui_localize(label));
    ui_round(S(x + 20), S(850), S(9), S(9), S(5), connected ? C_OK : C_DIM, 255);
    text_fit(x + 39, 842, 320, 21, connected ? C_OK : C_DIM,
             ui_localize(connected ? "Connected" : "No device"));
}

static void draw_remote_controls(const ui_home *m)
{
    const uint32_t line = 0x006584ADu;
    const char *touch = ui_localize("Swipe to move / press and move to drag");
    ui_round(S(963), S(175), S(871), S(740), S(28), 0x00121D32u, 255);
    ui_text(S(1004), S(212), S(20), C_DIM, ui_localize("REMOTE DESKTOP CONTROLS"));
    draw_dualsense();
    ui_text_center(S(1398),S(310),S(19),C_TEXT,touch);
    guide_line(1398, 340, 1398, 462, line);
    guide_anchor(1398, 462);
    guide_callout(1272, 423, 1190, 389, 0, ui_localize("Scroll"));
    guide_callout(1518, 423, 1621, 389, 1, ui_localize("Scroll"));
    guide_callout(1274, 494, 1220, 470, 0, ui_localize("Arrow keys"));
    guide_callout(1331, 547, 1227, 554, 0, ui_localize("Move pointer"));
    guide_callout(1491, 449, 1619, 439, 1, ui_localize("Session menu"));
    guide_callout(1521, 462, 1628, 477, 1, "Tab");
    guide_callout(1548, 490, 1627, 506, 1, ui_localize("Right click"));
    guide_callout(1493, 490, 1568, 530, 1, ui_localize("Backspace"));
    guide_callout(1521, 518, 1595, 568, 1, ui_localize("Click / drag"));
    guide_callout(1462, 547, 1599, 619, 1, ui_localize("Scroll up/down"));
    draw_device_status(1004, "Mouse", m->mouse_connected);
    draw_device_status(1414, "Keyboard", m->keyboard_connected);
    if (!m->mouse_connected || !m->keyboard_connected) {
        text_fit(1004, 887, 789, 17, C_DIM,
                 ui_localize("Move the mouse or press a key to detect the device."));
    }
}

static void ui_console_background(void)
{
    for(int y=0;y<g_s.height;++y) {
        unsigned t=(unsigned)y*255u/(unsigned)g_s.height;
        uint32_t c=((5u+t*5u/255u)<<16)|((12u+t*7u/255u)<<8)|(30u+t*20u/255u);
        ui_fill(0,y,g_s.width,1,c);
    }
    ui_fill(S(72),S(137),S(1776),S(2),0x002A3A59u);
}
static void draw_language(const ui_home *m)
{
    const ui_home_rect *r = &ui_home_controls[UI_HOME_LANGUAGE];
    const int x = r->x, y = r->y, w = r->w;
    int focused = m->focus == UI_HOME_LANGUAGE;
    const uint32_t dark = 0x00121D32u;
    uint32_t fg = focused ? dark : C_WHITE;
    ui_round(S(x),S(y),S(w),S(r->h),S(14),focused ? C_WHITE : 0x00202D45u,255);
    int cx = x + 35, cy = y + 30;
    /* The SVG is baked into size-specific alpha masks at build preparation. */
    int size = S(32);
    if (size > 0) {
        const unsigned char *mask = size <= 21 ? psrdp_globe_21 :
                                    size <= 32 ? psrdp_globe_32 : psrdp_globe_64;
        int side = size <= 21 ? 21 : size <= 32 ? 32 : 64;
        int left = S(cx) - size / 2, top = S(cy) - size / 2;
        for (int row = 0; row < size; ++row)
            for (int col = 0; col < size; ++col)
                blend(left + col, top + row, fg,
                      mask[(row * side / size) * side + col * side / size]);
    }
    ui_text(S(x+65),S(y+14),S(25),fg,language_names[g_language]);
    int arrow = x + w - 31;
    int delta = m->language_open ? -5 : 5;
    guide_line(arrow-7,cy-delta,arrow,cy+delta,fg);
    guide_line(arrow,cy+delta,arrow+7,cy-delta,fg);
    if (!m->language_open) return;
    ui_round(S(x),S(UI_LANG_LIST_Y),S(w),S(UI_LANG_ROW_H*UI_LANG_COUNT),S(13),dark,255);
    for (int i = 0; i < UI_LANG_COUNT; ++i) {
        int top = UI_LANG_LIST_Y + i * UI_LANG_ROW_H;
        int selected = i == m->language_focus;
        if (selected) ui_round(S(x+4),S(top+3),S(w-8),S(UI_LANG_ROW_H-6),S(10),C_WHITE,255);
        ui_text(S(x+30),S(top+13),S(24),selected ? dark : C_WHITE,language_names[i]);
        if (i == g_language) ui_round(S(x+w-35),S(top+22),S(10),S(10),S(5),C_OK,255);
    }
}

static uint32_t draw_home_control(const ui_home *m, ui_home_focus control)
{
    const ui_home_rect *r = &ui_home_controls[control];
    int focused = m->focus == control;
    int primary = control == UI_HOME_CONNECT;
    uint32_t fill = focused ? C_WHITE : 0x00202D45u;
    if (primary && m->busy) fill = 0x00202D45u;
    else if (primary && !focused) fill = 0x001D4ED8u;
    ui_round(S(r->x), S(r->y), S(r->w), S(r->h), S(primary ? 19 : 14), fill, 255);
    return primary && m->busy ? C_DIM : (focused ? 0x00121D32u : C_WHITE);
}

void ui_draw_home(const ui_home *m)
{
    static const char *const status_text[] = {
        "Ready", "Connecting...", "Connected", "Disconnected", "Connection failed"
    };
    char line[160];
    ui_console_background();if(!m)return;
    ui_text(S(86),S(48),S(46),C_WHITE,"PSrdp");
    ui_text(S(260),S(69),S(20),C_DIM,ui_localize("REMOTE DESKTOP"));
    ui_text(S(86),S(108),S(20),C_DIM,"v" PSRDP_VERSION);
    ui_round(S(86),S(175),S(835),S(740),S(28),0x00121D32u,255);
    ui_text(S(126),S(212),S(20),C_DIM,ui_localize("YOUR WINDOWS PC"));
    snprintf(line,sizeof(line),"%s:%d",m->host,m->port);
    text_fit(126,260,745,34,C_WHITE,line);
    ui_text(S(126),S(335),S(18),C_DIM,ui_localize("USER"));
    text_fit(126,365,745,28,C_TEXT,(m->user&&m->user[0])?m->user:ui_localize("Default user"));

    ui_fill(S(126),S(430),S(755),S(1),C_EDGE);
    ui_round(S(128),S(465),S(10),S(10),S(5),m->status_bad?C_BAD:C_OK,255);
    ui_text(S(150),S(452),S(26),m->status_bad?C_BAD:C_TEXT,ui_localize(status_text[(unsigned)m->status <= UI_FAILED ? m->status : UI_READY]));
    if (m->status == UI_FAILED) {
        static const char *const reasons[RDP_FAILURE_COUNT] = {
            [RDP_FAILURE_UNKNOWN] = "Cause unavailable. Try again.",
            [RDP_FAILURE_HOST] = "PC name not found. Check the name or IP.",
            [RDP_FAILURE_PORT] = "Port %d unreachable. Check RDP and firewall.",
            [RDP_FAILURE_PASSWORD] = "Incorrect password. Check and retry.",
            [RDP_FAILURE_LOGIN] = "Windows denied sign-in. Check account and permissions.",
            [RDP_FAILURE_SETUP] = "Remote Desktop setup failed. Try again.",
            [RDP_FAILURE_INTERRUPTED] = "Remote session interrupted. Reconnect.",
        };
        unsigned reason = (unsigned)m->failure < RDP_FAILURE_COUNT ? (unsigned)m->failure : 0;
        const char *detail = ui_localize(reasons[reason]);
        if (reason == RDP_FAILURE_PORT) {
            /* The localized template contains one integer placeholder. */
            const char *port = strstr(detail, "%d");
            if (port) {
                snprintf(line, sizeof(line), "%.*s%d%s", (int)(port - detail), detail, m->port, port + 2);
                detail = line;
            }
        }
        text_fit(150, 482, 730, 19, C_BAD, detail);
    }
    const ui_home_rect *res = &ui_home_controls[UI_HOME_RESOLUTION];
    const ui_home_rect *scale = &ui_home_controls[UI_HOME_SCALE];
    const ui_home_rect *settings = &ui_home_controls[UI_HOME_SETTINGS];
    const ui_home_rect *connect = &ui_home_controls[UI_HOME_CONNECT];
    ui_text(S(res->x), S(res->y - 28), S(17), C_DIM, ui_localize("DESKTOP RESOLUTION"));
    uint32_t text = draw_home_control(m, UI_HOME_RESOLUTION);
    snprintf(line, sizeof(line), "%s%s", m->res ? m->res : "1920x1080",
             m->custom ? ui_localize(" (Custom)") : "");
    ui_text(S(res->x + 29), S(res->y + 12), S(28), text, "<");
    ui_text_center(S(res->x + res->w / 2), S(res->y + 12), S(28), text, line);
    ui_text(S(res->x + res->w - 37), S(res->y + 12), S(28), text, ">");
    static const char *const custom_hints[] = { NULL, "Edit custom size",
        "Width: D-pad +/-1 or 10, L1/R1 +/-100", "Height: D-pad +/-1 or 10, L1/R1 +/-100" };
    if (m->custom > 0 && m->custom < 4)
        text_fit(res->x, res->y + res->h + 5, res->w, 18, C_DIM,
                 ui_localize(custom_hints[m->custom]));
    ui_text(S(scale->x), S(scale->y - 27), S(17), C_DIM, ui_localize("WINDOWS SCALE"));
    text = draw_home_control(m, UI_HOME_SCALE);
    snprintf(line, sizeof(line), "%u%%", m->desktop_scale ? m->desktop_scale : 100);
    ui_text(S(scale->x + 29), S(scale->y + 12), S(28), text, "<");
    ui_text_center(S(scale->x + scale->w / 2), S(scale->y + 13), S(26), text, line);
    ui_text(S(scale->x + scale->w - 37), S(scale->y + 12), S(28), text, ">");
    text = draw_home_control(m, UI_HOME_SETTINGS);
    ui_text_center(S(settings->x + settings->w / 2), S(settings->y + 12), S(24), text,
                   ui_localize("Connection settings"));
    text = draw_home_control(m, UI_HOME_CONNECT);
    ui_text_center(S(connect->x + connect->w / 2), S(connect->y + 22), S(36), text,
                   ui_localize(m->busy ? "Connecting..." : "Connect"));
    draw_remote_controls(m);
    ui_fill(S(86),S(930),S(1748),S(1),C_EDGE);
    text_fit(92,966,900,21,C_DIM,ui_localize("Up/down: select   Left/right: change"));
    if (m->output) text_fit(1004,966,830,19,C_DIM,m->output);
    draw_language(m);

    /* Author signature: GitHub mark + repository URL, bottom-right corner. */
    {
        const unsigned char *mask = psrdp_github_21;
        int side = 21, size = S(21);
        int text_h = S(19);
        const char *url = "github.com/yecheng770/psrdp";
        int url_w = ui_width(text_h, url);
        int url_x = S(1834), text_y = S(1002);
        int icon_x = url_x - url_w - size - S(7), icon_y = text_y + (text_h - size) / 2;
        for (int row = 0; row < size; ++row)
            for (int col = 0; col < size; ++col)
                blend(icon_x + col, icon_y + row, C_DIM,
                      mask[(row * side / size) * side + col * side / size]);
        ui_text(url_x, text_y, text_h, C_DIM, url);
    }
}

static const char keyboard_pages[UI_KEY_PAGES][UI_KEY_COLS * UI_KEY_ROWS + 1] = {
    "1234567890.-" "qwertyuiop_@" "asdfghjkl:/+" "zxcvbnm,=!? ",
    "1234567890.-" "QWERTYUIOP_@" "ASDFGHJKL:/+" "ZXCVBNM,=!? ",
    "1234567890.-" "!@#$%^&*()_+" "~`[]{}<>;:'\"" "\\|/?=,     "
};

char ui_keyboard_char(int page, int index)
{
    return keyboard_pages[page][index] ? keyboard_pages[page][index] : ' ';
}

void ui_draw_connection(const ui_connection *m)
{
    const char *const labels[UI_CONN_COUNT] = {
        ui_localize("Host"), ui_localize("Port"), ui_localize("Username"), ui_localize("Password"), ui_localize("Remember password"), ui_localize("History"), ui_localize("Save")
    };
    const char *values[UI_CONN_COUNT] = {
        m->host && m->host[0] ? m->host : ui_localize("Hostname or IP address"), m->port,
        m->user && m->user[0] ? m->user : ui_localize("(empty)"), m->has_password ? "********" : ui_localize("(empty)"),
        m->remember ? ui_localize("On") : ui_localize("Off"),
        m->has_history ? m->history : ui_localize("No saved connections"), ""
    };
    ui_console_background();
    ui_text(S(86),S(55),S(44),C_WHITE,ui_localize("Connection settings"));
    ui_round(S(86),S(165),S(1748),S(750),S(28),0x00121D32u,255);
    for (int row = 0; row < UI_CONN_COUNT; ++row) {
        int y = 208 + row * 86;
        int active = row == m->focus;
        uint32_t color = active ? 0x00121D32u : C_WHITE;
        ui_round(S(125),S(y),S(1670),S(64),S(15),active?C_WHITE:0x00202D45u,255);
        ui_text(S(155),S(y+15),S(27),color,labels[row]);
        if (row == UI_CONN_HISTORY && m->has_history) {
            ui_text(S(505),S(y+15),S(27),color,"<");
            ui_text(S(1745),S(y+15),S(27),color,">");
        }
        text_fit(555,y+15,1150,27,color,values[row]);
    }
    if (m->message) text_fit(130,829,1640,23,C_ACCENT,ui_localize(m->message));
    ui_text(S(130),S(966),S(22),C_DIM,ui_localize("Up/down: select   Left/right: browse"));
    button_hint(630, 980, 'x', ui_localize("Select"));
    button_hint(825, 980, 'o', ui_localize("Back"));
    if (!m->key_open) return;

    const char *entry = m->entry;
    char masked[128];
    if (m->focus == UI_CONN_PASS) {
        size_t count = strlen(entry);
        memset(masked, '*', count);
        masked[count] = 0;
        entry = masked;
    }
    ui_fill_a(0,0,g_s.width,g_s.height,0x00030A18u,205);
    ui_round(S(275),S(178),S(1370),S(723),S(28),0x00121D32u,255);
    ui_text(S(333),S(220),S(35),C_WHITE,labels[m->focus]);
    ui_round(S(330),S(280),S(1260),S(71),S(14),0x00202D45u,255);
    const char *visible = entry;
    while (*visible && ui_width(S(30), visible) > S(1190)) {
        do { ++visible; } while (((unsigned char)*visible & 0xc0) == 0x80);
    }
    ui_text(S(355),S(298),S(30),C_WHITE,*visible ? visible : ui_localize("(empty)"));
    ui_text(S(334),S(379),S(20),C_DIM,
            m->key_page == 0 ? ui_localize("L1 / R1    Lowercase") : m->key_page == 1 ? ui_localize("L1 / R1    Uppercase") : ui_localize("L1 / R1    Symbols"));
    for (int i = 0; i < UI_KEY_COLS * UI_KEY_ROWS; ++i) {
        int x = 333 + (i % UI_KEY_COLS) * 105;
        int y = 427 + (i / UI_KEY_COLS) * 81;
        int selected = i == m->key_index;
        char key[] = {ui_keyboard_char(m->key_page, i), 0};
        int enabled = m->focus != UI_CONN_PORT || (key[0] >= '0' && key[0] <= '9');
        uint32_t color = selected ? 0x00121D32u : enabled ? C_WHITE : C_DIM;
        ui_round(S(x),S(y),S(93),S(65),S(13),selected?C_WHITE:0x00202D45u,255);
        ui_text_center(S(x+46),S(y+13),S(27),color,key[0]==' '?ui_localize("Space"):key);
    }
    button_hint(350, 819, 'x', ui_localize("Type"));
    button_hint(590, 819, 's', ui_localize("Backspace"));
    button_hint(910, 819, 't', ui_localize("Apply"));
    button_hint(1150, 819, 'o', ui_localize("Cancel"));
}

void ui_draw_banner(const char *title, const char *detail, const char *hint)
{
    int cy = S(420);
    ui_fill_a(0, 0, g_s.width, g_s.height, 0x00000000, 200);
    if (title && title[0])
        ui_text_center(S(960), cy, S(56), C_TEXT, title);
    if (detail && detail[0])
        ui_text_center(S(960), cy + S(96), S(28), C_DIM, detail);
    if (hint && hint[0])
        ui_text_center(S(960), cy + S(168), S(22), C_ACCENT, hint);
}

void ui_draw_cursor(int x, int y)
{
    int r = S(12);
    if (r < 4) r = 4;
    ui_round(x - r, y - r, r * 2, r * 2, r, C_WHITE, 230);
    ui_round(x - r + 3, y - r + 3, (r - 3) * 2, (r - 3) * 2, r - 3,
             0x00000000, 130);
}

/* Local overlay: the remote desktop stays connected behind this plate. */
void ui_draw_session_menu(int stopping)
{
    ui_fill_a(0,0,g_s.width,g_s.height,0x00030A18u,185);
    ui_round(S(540),S(258),S(840),S(564),S(30),0x00121D32u,255);
    ui_text(S(604),S(308),S(44),C_WHITE,ui_localize("Session menu"));
    ui_text(S(604),S(372),S(22),C_DIM,
            stopping ? ui_localize("Closing the remote connection...") : ui_localize("Your remote desktop is still connected"));
    ui_round(S(604),S(506),S(712),S(92),S(18),stopping?C_PANEL:C_ACCENT,255);
    if (!stopping) ui_border(S(600),S(502),S(720),S(100),S(18),C_WHITE);
    ui_text_center(S(960),S(531),S(30),stopping?C_DIM:C_WHITE,ui_localize("Disconnect"));
    if (stopping) ui_text_center(S(960),S(738),S(20),C_DIM,ui_localize("Please wait"));
    else {
        button_hint(690, 750, 'x', ui_localize("Disconnect"));
        button_hint(943, 750, 'o', ui_localize("/ Options: Resume"));
    }
}
