#include "ui.h"
#include "app_id.h"
#include "config.h"
#include "input.h"
#include "webos_keys.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <json-c/json.h>
#include <arpa/inet.h>
#include <ctype.h>

#include "config_import.h"
#include "app_log.h"

/* ── Layout ──────────────────────────────────────────────────────────────────
 * Two-panel layout:
 *   Header band (full width, top)
 *   Left  panel — step guide + instructions
 *   Right panel — IP input + action buttons / keypad
 *   Status row  — config state pills
 *   Footer bar  — navigation hints
 */

#define SCREEN_W    1920
#define SCREEN_H    1080

/* Header */
#define HDR_Y       24
#define HDR_H       72

/* Content panels (below header) */
#define CONTENT_Y   (HDR_Y + HDR_H + 20)   /* 116 */
#define CONTENT_H   724

#define LEFT_X      60
#define LEFT_W      680
#define DIVIDER_X   (LEFT_X + LEFT_W + 12)
#define RIGHT_X     (DIVIDER_X + 12)
#define RIGHT_W     (SCREEN_W - RIGHT_X - 60)

/* Footer */
#define FOOTER_Y    (CONTENT_Y + CONTENT_H + 22)   /* 862 */
#define FOOTER_H    62

/* ── Colour palette (R,G,B,A) ─────────────────────────────────────────────── */

/* Backgrounds */
#define COL_BG              0x04, 0x07, 0x12, 0xff   /* deep navy */
#define COL_BG_DEEPER       0x02, 0x04, 0x0c, 0xff   /* header overlay */
#define COL_PANEL           0x09, 0x11, 0x28, 0xff   /* panels / footer */
#define COL_ELEVATED        0x0d, 0x17, 0x32, 0xff   /* right panel */
#define COL_CARD            0x14, 0x20, 0x44, 0xff   /* button / input BG */
#define COL_CODE_BG         0x07, 0x0e, 0x22, 0xff   /* code block BG */

/* Accent */
#define COL_ACCENT          0x00, 0x72, 0xff, 0xff   /* PlayStation blue */
#define COL_ACCENT_MID      0x00, 0x48, 0xc0, 0xff
#define COL_ACCENT_DARK     0x00, 0x2e, 0x82, 0xff

/* Semantic */
#define COL_SUCCESS         0x00, 0xb8, 0x58, 0xff
#define COL_SUCCESS_BG      0x00, 0x28, 0x14, 0xff
#define COL_WARNING         0xf8, 0x9c, 0x1c, 0xff
#define COL_WARNING_BG      0x30, 0x1e, 0x04, 0xff
#define COL_ERROR           0xff, 0x44, 0x50, 0xff
#define COL_ERROR_BG        0x30, 0x08, 0x0c, 0xff

/* Borders */
#define COL_BORDER          0x1c, 0x2c, 0x58, 0xff
#define COL_BORDER_DIM      0x10, 0x18, 0x36, 0xff

/* Text */
#define COL_TEXT            0xec, 0xf2, 0xff, 0xff
#define COL_TEXT_DIM        0x78, 0x8c, 0xb8, 0xff
#define COL_TEXT_FAINT      0x3c, 0x4c, 0x78, 0xff
#define COL_TEXT_CODE       0x48, 0xcc, 0xff, 0xff

/* ── Minimal 5×7 pixel font (printable ASCII 0x20–0x7e) ──────────────────── */

static const uint8_t FONT_DATA[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 0x20 space
    {0x00,0x00,0x5f,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7f,0x14,0x7f,0x14}, // #
    {0x24,0x2a,0x7f,0x2a,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1c,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1c,0x00}, // )
    {0x08,0x2a,0x1c,0x2a,0x08}, // *
    {0x08,0x08,0x3e,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3e,0x51,0x49,0x45,0x3e}, // 0
    {0x00,0x42,0x7f,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4b,0x31}, // 3
    {0x18,0x14,0x12,0x7f,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3c,0x4a,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1e}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x08,0x14,0x22,0x41,0x00}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x00,0x41,0x22,0x14,0x08}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3e}, // @
    {0x7e,0x11,0x11,0x11,0x7e}, // A
    {0x7f,0x49,0x49,0x49,0x36}, // B
    {0x3e,0x41,0x41,0x41,0x22}, // C
    {0x7f,0x41,0x41,0x22,0x1c}, // D
    {0x7f,0x49,0x49,0x49,0x41}, // E
    {0x7f,0x09,0x09,0x09,0x01}, // F
    {0x3e,0x41,0x49,0x49,0x7a}, // G
    {0x7f,0x08,0x08,0x08,0x7f}, // H
    {0x00,0x41,0x7f,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3f,0x01}, // J
    {0x7f,0x08,0x14,0x22,0x41}, // K
    {0x7f,0x40,0x40,0x40,0x40}, // L
    {0x7f,0x02,0x0c,0x02,0x7f}, // M
    {0x7f,0x04,0x08,0x10,0x7f}, // N
    {0x3e,0x41,0x41,0x41,0x3e}, // O
    {0x7f,0x09,0x09,0x09,0x06}, // P
    {0x3e,0x41,0x51,0x21,0x5e}, // Q
    {0x7f,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7f,0x01,0x01}, // T
    {0x3f,0x40,0x40,0x40,0x3f}, // U
    {0x1f,0x20,0x40,0x20,0x1f}, // V
    {0x3f,0x40,0x38,0x40,0x3f}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0x00,0x7f,0x41,0x41,0x00}, // [
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x00,0x41,0x41,0x7f,0x00}, // ]
    {0x04,0x02,0x01,0x02,0x04}, // ^
    {0x40,0x40,0x40,0x40,0x40}, // _
    {0x00,0x01,0x02,0x04,0x00}, // `
    {0x20,0x54,0x54,0x54,0x78}, // a
    {0x7f,0x48,0x44,0x44,0x38}, // b
    {0x38,0x44,0x44,0x44,0x20}, // c
    {0x38,0x44,0x44,0x48,0x7f}, // d
    {0x38,0x54,0x54,0x54,0x18}, // e
    {0x08,0x7e,0x09,0x01,0x02}, // f
    {0x0c,0x52,0x52,0x52,0x3e}, // g
    {0x7f,0x08,0x04,0x04,0x78}, // h
    {0x00,0x44,0x7d,0x40,0x00}, // i
    {0x20,0x40,0x44,0x3d,0x00}, // j
    {0x7f,0x10,0x28,0x44,0x00}, // k
    {0x00,0x41,0x7f,0x40,0x00}, // l
    {0x7c,0x04,0x18,0x04,0x78}, // m
    {0x7c,0x08,0x04,0x04,0x78}, // n
    {0x38,0x44,0x44,0x44,0x38}, // o
    {0x7c,0x14,0x14,0x14,0x08}, // p
    {0x08,0x14,0x14,0x18,0x7c}, // q
    {0x7c,0x08,0x04,0x04,0x08}, // r
    {0x48,0x54,0x54,0x54,0x20}, // s
    {0x04,0x3f,0x44,0x40,0x20}, // t
    {0x3c,0x40,0x40,0x20,0x7c}, // u
    {0x1c,0x20,0x40,0x20,0x1c}, // v
    {0x3c,0x40,0x30,0x40,0x3c}, // w
    {0x44,0x28,0x10,0x28,0x44}, // x
    {0x0c,0x50,0x50,0x50,0x3c}, // y
    {0x44,0x64,0x54,0x4c,0x44}, // z
    {0x00,0x08,0x36,0x41,0x00}, // {
    {0x00,0x00,0x7f,0x00,0x00}, // |
    {0x00,0x41,0x36,0x08,0x00}, // }
    {0x10,0x08,0x08,0x10,0x08}, // ~
};

/* ── Primitive drawing helpers ───────────────────────────────────────────── */

/* Output-pixel scale factor: the logical canvas is SCREEN_W x SCREEN_H; when
 * the renderer output is larger (e.g. a native 4K surface), every rect emitted
 * by these primitives is multiplied by g_ui_scale so the layout constants and
 * hit-testing math can stay in canvas coordinates. */
static int g_ui_scale = 1;

static void set_color(SDL_Renderer *r, int R, int G, int B, int A)
{
    SDL_SetRenderDrawColor(r, R, G, B, A);
}

static void fill_rect(SDL_Renderer *r, int x, int y, int w, int h)
{
    SDL_Rect rect = {x * g_ui_scale, y * g_ui_scale,
                     w * g_ui_scale, h * g_ui_scale};
    SDL_RenderFillRect(r, &rect);
}

static void draw_rect_outline(SDL_Renderer *r, int x, int y, int w, int h)
{
    SDL_Rect rect = {x * g_ui_scale, y * g_ui_scale,
                     w * g_ui_scale, h * g_ui_scale};
    SDL_RenderDrawRect(r, &rect);
}

static void draw_glyph(SDL_Renderer *r, int x, int y, uint8_t ch, int scale)
{
    if (ch < 0x20 || ch > 0x7e) return;
    const uint8_t *col = FONT_DATA[ch - 0x20];
    for (int cx = 0; cx < 5; cx++)
        for (int cy = 0; cy < 7; cy++)
            if (col[cx] & (1 << cy))
                fill_rect(r, x + cx * scale, y + cy * scale, scale, scale);
}

static void draw_text(SDL_Renderer *r, int x, int y, const char *text, int scale)
{
    int ox = x;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') { x = ox; y += 9 * scale; continue; }
        draw_glyph(r, x, y, (uint8_t)*p, scale);
        x += 6 * scale;
    }
}

static int text_width(const char *text, int scale)
{
    int w = 0;
    for (const char *p = text; *p && *p != '\n'; p++)
        w += 6 * scale;
    return w;
}

static bool point_in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

/* Mouse coordinates are window pixels; all UI hitboxes use the 1920x1080 canvas. */
static void pointer_to_ui(SDL_Renderer *renderer, int x, int y,
                          int *ui_x, int *ui_y)
{
    int out_w = SCREEN_W, out_h = SCREEN_H;
    if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) != 0 ||
        out_w <= 0 || out_h <= 0) {
        out_w = SCREEN_W;
        out_h = SCREEN_H;
    }
    *ui_x = x * SCREEN_W / out_w;
    *ui_y = y * SCREEN_H / out_h;
}

static void push_ui_key(SDL_Keycode key)
{
    SDL_Event fake;
    memset(&fake, 0, sizeof(fake));
    fake.type = SDL_KEYDOWN;
    fake.key.state = SDL_PRESSED;
    fake.key.keysym.sym = key;
    SDL_PushEvent(&fake);
}

static SDL_Keycode stick_nav_key(const SDL_ControllerAxisEvent *axis,
                                 int *x_state, int *y_state)
{
    const int deadzone = 16000;
    int direction = axis->value <= -deadzone ? -1 :
                    axis->value >=  deadzone ?  1 : 0;
    int *state = NULL;
    SDL_Keycode negative = 0, positive = 0;
    if (axis->axis == SDL_CONTROLLER_AXIS_LEFTX) {
        state = x_state; negative = SDLK_LEFT; positive = SDLK_RIGHT;
    } else if (axis->axis == SDL_CONTROLLER_AXIS_LEFTY) {
        state = y_state; negative = SDLK_UP; positive = SDLK_DOWN;
    } else {
        return 0;
    }
    if (*state == direction) return 0;
    *state = direction;
    return direction < 0 ? negative : direction > 0 ? positive : 0;
}

static void draw_text_centered(SDL_Renderer *r, int cx, int y,
                                const char *text, int scale)
{
    draw_text(r, cx - text_width(text, scale) / 2, y, text, scale);
}

/* Rounded rect — two overlapping bars forming a cross with clipped corners */
static void fill_rounded_rect(SDL_Renderer *r, int x, int y, int w, int h, int radius)
{
    if (radius * 2 >= w) radius = w / 2 - 1;
    if (radius * 2 >= h) radius = h / 2 - 1;
    fill_rect(r, x + radius, y,          w - radius * 2, h);
    fill_rect(r, x,          y + radius, w,              h - radius * 2);
}

/* Filled horizontal line (1px tall) */
static void hline(SDL_Renderer *r, int x, int y, int w)
{
    fill_rect(r, x, y, w, 1);
}

void ui_set_output_scale(SDL_Renderer *r)
{
    int w = 0, h = 0;
    g_ui_scale = 1;
    if (SDL_GetRendererOutputSize(r, &w, &h) == 0 && h > SCREEN_H)
        g_ui_scale = h / SCREEN_H;
    app_log_always("[APP] UI output scale: %d (renderer output %dx%d)\n",
                   g_ui_scale, w, h);
}

/* ── Code block ─────────────────────────────────────────────────────────── */

static void draw_code_block(SDL_Renderer *r, int x, int y, int w,
                             const char *text, int scale)
{
    int lines = 1;
    for (const char *p = text; *p; p++) if (*p == '\n') lines++;
    int h = lines * 9 * scale + 20;

    set_color(r, COL_CODE_BG);
    fill_rounded_rect(r, x, y, w, h, 8);
    set_color(r, COL_ACCENT_DARK);
    fill_rect(r, x, y, 4, h);
    set_color(r, COL_TEXT_CODE);
    draw_text(r, x + 16, y + 10, text, scale);
}

/* ── Step progress badge ────────────────────────────────────────────────── */

/* Draw a single numbered step badge (44×44) at top-left corner (bx, by).
 * done   = step completed  → green fill, "OK" label
 * active = current step    → blue fill, white number
 * else   = future step     → dim border, dim number
 */
static void draw_step_badge(SDL_Renderer *r, int bx, int by, int size,
                             int num, bool done, bool active)
{
    int radius = size / 5;

    /* Badge fill */
    if (done) {
        set_color(r, COL_SUCCESS);
    } else if (active) {
        set_color(r, COL_ACCENT);
    } else {
        set_color(r, COL_BORDER_DIM);
    }
    fill_rounded_rect(r, bx, by, size, size, radius);

    /* Inner border highlight for active */
    if (active) {
        set_color(r, 0x40, 0x90, 0xff, 0xff);
        fill_rounded_rect(r, bx + 2, by + 2, size - 4, size - 4, radius - 1);
        set_color(r, COL_ACCENT);
        fill_rounded_rect(r, bx + 3, by + 3, size - 6, size - 6, radius - 2);
    }

    /* Content text — vertically centred */
    int text_y = by + (size - 14) / 2;  /* 14 = 7 * scale2 */
    if (done) {
        set_color(r, 0x00, 0x20, 0x0c, 0xff);
        draw_text_centered(r, bx + size / 2, text_y, "OK", 2);
    } else {
        set_color(r, active ? COL_TEXT : COL_TEXT_FAINT);
        char s[2] = { '0' + (char)num, '\0' };
        draw_text_centered(r, bx + size / 2, text_y, s, 2);
    }
}

/* ── Status pill ────────────────────────────────────────────────────────── */

static int draw_status_pill(SDL_Renderer *r, int x, int y,
                             const char *label, bool ok)
{
    int tw = text_width(label, 2);
    int pw = tw + 28;
    int ph = 34;

    if (ok) {
        set_color(r, COL_SUCCESS_BG);
        fill_rounded_rect(r, x, y, pw, ph, ph / 2);
        set_color(r, COL_SUCCESS);
        fill_rect(r, x + 6, y + ph/2 - 1, 8, 2);  /* mini tick left */
        draw_text(r, x + 18, y + (ph - 14) / 2, label, 2);
    } else {
        set_color(r, COL_ERROR_BG);
        fill_rounded_rect(r, x, y, pw, ph, ph / 2);
        set_color(r, COL_ERROR);
        draw_text(r, x + 14, y + (ph - 14) / 2, label, 2);
    }
    return pw + 12;
}

/* ── Button ─────────────────────────────────────────────────────────────── */

static void draw_button(SDL_Renderer *r, int x, int y, int w, int h,
                        const char *label, bool focused, bool enabled)
{
    int radius = 10;
    int text_y = y + (h - 14) / 2;

    if (!enabled) {
        /* Disabled — very faint */
        set_color(r, COL_BORDER_DIM);
        fill_rounded_rect(r, x, y, w, h, radius);
        set_color(r, COL_TEXT_FAINT);
        draw_text_centered(r, x + w / 2, text_y, label, 2);
        return;
    }

    if (focused) {
        /* Focused — solid accent fill with subtle top highlight */
        set_color(r, COL_ACCENT);
        fill_rounded_rect(r, x, y, w, h, radius);
        /* Subtle top sheen */
        set_color(r, 0x40, 0x90, 0xff, 0xff);
        fill_rounded_rect(r, x + 2, y + 2, w - 4, h / 3, radius - 1);
        set_color(r, COL_ACCENT);
        fill_rounded_rect(r, x + 2, y + h / 4, w - 4, h / 2, radius - 1);
        /* White text */
        set_color(r, COL_TEXT);
        draw_text_centered(r, x + w / 2, text_y, label, 2);
        /* Bottom accent line */
        set_color(r, 0x60, 0xa8, 0xff, 0xff);
        hline(r, x + radius, y + h - 3, w - radius * 2);
    } else {
        /* Normal enabled */
        set_color(r, COL_CARD);
        fill_rounded_rect(r, x, y, w, h, radius);
        set_color(r, COL_BORDER);
        draw_rect_outline(r, x, y, w, h);
        set_color(r, COL_TEXT);
        draw_text_centered(r, x + w / 2, text_y, label, 2);
    }
}

/* ── Input box ──────────────────────────────────────────────────────────── */

static void draw_input_box(SDL_Renderer *r, int x, int y, int w, int h,
                           const char *text, int cursor, bool focused)
{
    int radius = 10;

    /* Background */
    set_color(r, COL_CODE_BG);
    fill_rounded_rect(r, x, y, w, h, radius);

    /* Border — thicker + brighter when focused */
    if (focused) {
        set_color(r, COL_ACCENT);
        draw_rect_outline(r, x,     y,     w,     h);
        draw_rect_outline(r, x + 1, y + 1, w - 2, h - 2);
    } else {
        set_color(r, COL_BORDER);
        draw_rect_outline(r, x, y, w, h);
    }

    int scale = 3;
    int ty = y + (h - 7 * scale) / 2;
    int tx = x + 20;

    if (text && text[0]) {
        set_color(r, COL_TEXT);
        draw_text(r, tx, ty, text, scale);
    } else {
        set_color(r, COL_TEXT_FAINT);
        draw_text(r, tx, ty, "192.168.x.x", scale);
    }

    /* Blinking cursor */
    if (focused && ((SDL_GetTicks() / 500) % 2) == 0) {
        int cx = tx + cursor * (6 * scale);
        set_color(r, COL_TEXT);
        fill_rect(r, cx, ty, 2, 7 * scale);
    }
}

/* ── Setup screen ───────────────────────────────────────────────────────── */

static void render_setup_screen(SDL_Renderer *r, const char *status,
                                const char *ip, int focus,
                                const char *msg,
                                bool has_keys,
                                bool has_valid_ip,
                                bool can_connect,
                                bool show_keypad)
{
    /* Determine current step (1-3) for progress display */
    int step;
    if (can_connect)        step = 3;   /* everything ready */
    else if (has_keys)      step = 2;   /* have keys, need valid IP */
    else                    step = 1;   /* need keys first */

    /* ── Background ──────────────────────────────────────────────────────── */
    set_color(r, COL_BG);
    fill_rect(r, 0, 0, SCREEN_W, SCREEN_H);

    /* Very top vignette */
    set_color(r, COL_BG_DEEPER);
    fill_rect(r, 0, 0, SCREEN_W, 60);

    /* ── Header band ─────────────────────────────────────────────────────── */
    set_color(r, COL_PANEL);
    fill_rect(r, 0, HDR_Y, SCREEN_W, HDR_H);

    /* Bottom border of header */
    set_color(r, COL_BORDER_DIM);
    hline(r, 0, HDR_Y + HDR_H - 1, SCREEN_W);

    /* Left PS blue accent bar */
    set_color(r, COL_ACCENT);
    fill_rect(r, 0, HDR_Y, 8, HDR_H);

    /* App name */
    set_color(r, COL_TEXT);
    draw_text(r, 36, HDR_Y + 10, "CHIAKI", 4);

    /* Subtitle */
    set_color(r, COL_TEXT_DIM);
    draw_text(r, 36, HDR_Y + 50, "PS5 Remote Play for webOS", 2);

    /* Right side: connection readiness */
    if (can_connect) {
        char ps5str[80];
        snprintf(ps5str, sizeof(ps5str), "PS5:  %s", ip ? ip : "");
        int sw = text_width(ps5str, 2);
        set_color(r, COL_TEXT_DIM);
        draw_text(r, SCREEN_W - sw - 48, HDR_Y + 14, ps5str, 2);
        set_color(r, COL_SUCCESS);
        draw_text(r, SCREEN_W - text_width("Ready to connect", 2) - 48,
                  HDR_Y + 42, "Ready to connect", 2);
    } else {
        set_color(r, COL_TEXT_FAINT);
        draw_text(r, SCREEN_W - text_width("Setup required", 2) - 48,
                  HDR_Y + 28, "Setup required", 2);
    }

    /* ── Left panel ──────────────────────────────────────────────────────── */
    set_color(r, COL_PANEL);
    fill_rounded_rect(r, LEFT_X, CONTENT_Y, LEFT_W, CONTENT_H, 14);

    /* Left accent strip */
    set_color(r, COL_ACCENT_DARK);
    fill_rect(r, LEFT_X, CONTENT_Y + 14, 4, CONTENT_H - 28);
    set_color(r, COL_ACCENT);
    /* Highlight the strip portion matching current step */
    int strip_seg = (CONTENT_H - 28) / 3;
    fill_rect(r, LEFT_X, CONTENT_Y + 14 + (step - 1) * strip_seg, 4, strip_seg);

    /* ── Step progress row (inside left panel) ───────────────────────────── */
    const int BADGE_SZ = 44;
    const int STEP_Y   = CONTENT_Y + 32;

    /* Three badge centre-x positions within the left panel */
    static const int STEP_OFFSETS[3] = { 110, 340, 570 };

    /* Connecting lines between badges */
    for (int i = 0; i < 2; i++) {
        int lx1 = LEFT_X + STEP_OFFSETS[i] + BADGE_SZ / 2;
        int lx2 = LEFT_X + STEP_OFFSETS[i + 1] - BADGE_SZ / 2;
        int ly  = STEP_Y + BADGE_SZ / 2;
        /* Base line */
        set_color(r, COL_BORDER_DIM);
        fill_rect(r, lx1, ly - 1, lx2 - lx1, 3);
        /* Progress overlay */
        if (i + 2 <= step) {
            set_color(r, (step == 3) ? COL_SUCCESS : COL_ACCENT_MID);
            fill_rect(r, lx1, ly - 1, lx2 - lx1, 3);
        }
    }

    /* Step badges */
    static const char *STEP_LABELS[3] = { "GET KEYS", "PS5 IP", "CONNECT" };
    for (int i = 0; i < 3; i++) {
        bool done   = (i + 1 < step);
        bool active = (i + 1 == step);
        int bx = LEFT_X + STEP_OFFSETS[i] - BADGE_SZ / 2;
        draw_step_badge(r, bx, STEP_Y, BADGE_SZ, i + 1, done, active);
        /* Label below */
        set_color(r,
            done   ? COL_SUCCESS :
            active ? COL_TEXT    : COL_TEXT_FAINT);
        draw_text_centered(r, LEFT_X + STEP_OFFSETS[i],
                           STEP_Y + BADGE_SZ + 8, STEP_LABELS[i], 1);
    }

    /* Divider below step row */
    set_color(r, COL_BORDER_DIM);
    hline(r, LEFT_X + 20, STEP_Y + BADGE_SZ + 28, LEFT_W - 40);

    /* ── Step-specific instructions ──────────────────────────────────────── */
    int ix = LEFT_X + 22;
    int iy = STEP_Y + BADGE_SZ + 46;
    int iw = LEFT_W - 44;

    if (step == 1) {
        set_color(r, COL_ACCENT);
        draw_text(r, ix, iy, "Import chiaki-ng settings", 2);
        iy += 32;

        set_color(r, COL_TEXT_DIM);
        draw_text(r, ix, iy, "On your PC:", 2);
        iy += 22;

        set_color(r, COL_TEXT);
        draw_text(r, ix + 14, iy, "1. Open chiaki-ng on computer", 2); iy += 20;
        draw_text(r, ix + 14, iy, "2. Settings -> Config -> Export Settings To File", 2); iy += 20;
        draw_text(r, ix + 14, iy, "3. Save chiaki-ng-Default.ini", 2); iy += 32;

        set_color(r, COL_TEXT_DIM);
        draw_text(r, ix, iy, "Copy to the TV via ADB:", 2); iy += 22;

        draw_code_block(r, ix, iy, iw,
            "adb push chiaki-ng-Default.ini \\\n"
            " /media/developer/apps/usr/palm/\n"
            " applications/" CHIAKI_APP_ID "/", 2);
        iy += 114;

		set_color(r, COL_TEXT_DIM);
        draw_text(r, ix, iy, "Or use WebOS Dev Manager for copying file to:", 2); iy += 22;
		
		draw_code_block(r, ix, iy, iw,
            " /media/developer/apps/usr/palm/\n"
            " applications/" CHIAKI_APP_ID "/", 2);
        iy += 114;

        set_color(r, COL_TEXT_DIM);
        draw_text(r, ix, iy, "Then on this screen:", 2); iy += 22;
        set_color(r, COL_TEXT);
        draw_text(r, ix + 14, iy, "Press  Import Config", 2); iy += 20;
        draw_text(r, ix + 14, iy, "The matching PS5 IP is imported too", 2);

    } else if (step == 2) {
        set_color(r, COL_ACCENT);
        draw_text(r, ix, iy, "Enter your PS5 IP address", 2);
        iy += 32;

        set_color(r, COL_TEXT_DIM);
        draw_text(r, ix, iy, "Find it on your PS5:", 2);
        iy += 22;

        set_color(r, COL_TEXT);
        draw_text(r, ix + 14, iy, "Settings", 2); iy += 18;
        draw_text(r, ix + 22, iy, "-> Network", 2); iy += 18;
        draw_text(r, ix + 30, iy, "-> Connection Status", 2); iy += 18;
        draw_text(r, ix + 38, iy, "-> View Connection Status", 2); iy += 30;

        set_color(r, COL_TEXT_DIM);
        draw_text(r, ix, iy, "Look for  IP Address:", 2); iy += 22;
        draw_code_block(r, ix, iy, iw, "e.g.  192.168.1.50", 2);
        iy += 52;

        /* Keys status — already done at this point */
        set_color(r, COL_SUCCESS);
        draw_text(r, ix, iy, "Registration keys: OK", 2); iy += 22;
        set_color(r, COL_TEXT_DIM);
        draw_text(r, ix, iy, "Just need a valid IP to connect.", 2);

    } else {
        /* Step 3 — ready */
        set_color(r, COL_ACCENT);
        draw_text(r, ix, iy, "Ready to stream", 2);
        iy += 32;

        set_color(r, COL_SUCCESS);
        draw_text(r, ix, iy, "Registration keys:  OK", 2); iy += 24;

        char ip_line[80];
        snprintf(ip_line, sizeof(ip_line), "PS5 IP address:     %s", ip ? ip : "?");
        draw_text(r, ix, iy, ip_line, 2); iy += 40;

        set_color(r, COL_TEXT_DIM);
        draw_text(r, ix, iy, "Press  Connect  to begin streaming.", 2); iy += 22;
        draw_text(r, ix, iy, "If wakeup is configured the PS5 will", 2); iy += 20;
        draw_text(r, ix, iy, "wake automatically from rest mode.", 2);
    }

    /* ── Right panel ─────────────────────────────────────────────────────── */
    set_color(r, COL_ELEVATED);
    fill_rounded_rect(r, RIGHT_X, CONTENT_Y, RIGHT_W, CONTENT_H, 14);

    /* Right panel right accent strip (subtle) */
    set_color(r, COL_BORDER_DIM);
    fill_rect(r, RIGHT_X + RIGHT_W - 4, CONTENT_Y + 14, 4, CONTENT_H - 28);

    int rx = RIGHT_X + 52;
    int ry = CONTENT_Y + 52;
    int rw = RIGHT_W - 104;

    /* ── IP address field ───────────────────────────────────────────────── */
    set_color(r, COL_TEXT_DIM);
    draw_text(r, rx, ry, "PS5 IP ADDRESS", 2);
    ry += 26;

    bool ip_box_focused = (focus == 0) || show_keypad;
    int ip_cursor = ip ? (int)strlen(ip) : 0;
    draw_input_box(r, rx, ry, rw, 72, ip, ip_cursor, ip_box_focused);
    ry += 72 + 20;

    if (show_keypad) {
        /* ── On-screen numeric keypad ───────────────────────────────────── */
        set_color(r, COL_TEXT_DIM);
        draw_text(r, rx, ry, "ENTER IP  (arrows + OK)", 2);
        ry += 26;

        static const char *KEYS[12] = {
            "1","2","3","4","5","6","7","8","9",".","0","DEL"
        };
        const int KW = 128, KH = 64, KGAP = 12, COLS = 3, ROWS = 4;

        for (int i = 0; i < 12; i++) {
            int row = i / COLS, col = i % COLS;
            int kx = rx + col * (KW + KGAP);
            int ky = ry + row * (KH + KGAP);
            draw_button(r, kx, ky, KW, KH, KEYS[i], (focus == i + 1), true);
        }

        int kpad_h = ROWS * KH + (ROWS - 1) * KGAP;
        int done_y = ry + kpad_h + 16;
        int done_w = COLS * KW + (COLS - 1) * KGAP;
        draw_button(r, rx, done_y, done_w, KH, "Done", (focus == 15), true);

    } else {
        /* ── Main action buttons ────────────────────────────────────────── */
        int btn_h = 80;
        int btn_w = (rw - 20) / 2;

        draw_button(r, rx,              ry, btn_w, btn_h, "Import Config", focus == 13, true);
        draw_button(r, rx + btn_w + 20, ry, btn_w, btn_h, "Connect",       focus == 14, can_connect);
        ry += btn_h + 16;

        /* Settings — full width */
        draw_button(r, rx, ry, rw, 60, "Settings", focus == 16, true);
        ry += 60 + 36;

        /* Contextual guidance */
        if (step == 1) {
            set_color(r, COL_TEXT_DIM);
            draw_text(r, rx, ry, "If you have copied", 2); ry += 20;
            draw_text(r, rx, ry, "chiaki-ng-Default.ini to the TV,", 2); ry += 20;
            draw_text(r, rx, ry, "press  Import Config  to continue.", 2);
        } else if (step == 2) {
            set_color(r, COL_TEXT_DIM);
            draw_text(r, rx, ry, "Keys imported. Enter the PS5 IP", 2); ry += 20;
            draw_text(r, rx, ry, "address above, then press Connect.", 2);
        }
    }

    /* ── Status message (if any) ────────────────────────────────────────── */
    if (msg && msg[0]) {
        int msg_y = CONTENT_Y + CONTENT_H - 56;
        /* Pick colour based on content */
        if (strstr(msg, "OK") || strstr(msg, "ok") || strstr(msg, "Import OK"))
            set_color(r, COL_SUCCESS);
        else if (strstr(msg, "failed") || strstr(msg, "MISSING") ||
                 strstr(msg, "not found") || strstr(msg, "Missing"))
            set_color(r, COL_ERROR);
        else
            set_color(r, COL_WARNING);

        /* Truncate long messages to fit */
        char trunc[72];
        strncpy(trunc, msg, sizeof(trunc) - 1);
        trunc[sizeof(trunc) - 1] = '\0';
        draw_text(r, RIGHT_X + 52, msg_y, trunc, 2);
    }

    /* ── Status pills row ───────────────────────────────────────────────── */
    int pill_y  = FOOTER_Y - 48;
    int pill_x  = LEFT_X;

    pill_x += draw_status_pill(r, pill_x, pill_y, "Keys: Ready", has_keys);
    pill_x += draw_status_pill(r, pill_x, pill_y, ip && ip[0] ? ip : "IP: Not Set",
                                has_valid_ip);
    /* Note: can_connect also requires keys, so it's only true if both are set */
    if (can_connect)
        draw_status_pill(r, pill_x, pill_y, "Ready", true);

    /* ── Footer hint bar ────────────────────────────────────────────────── */
    set_color(r, COL_PANEL);
    fill_rect(r, 0, FOOTER_Y, SCREEN_W, FOOTER_H);
    set_color(r, COL_BORDER_DIM);
    hline(r, 0, FOOTER_Y, SCREEN_W);

    set_color(r, COL_TEXT_FAINT);
    if (show_keypad) {
        draw_text_centered(r, SCREEN_W / 2, FOOTER_Y + (FOOTER_H - 14) / 2,
            "ARROWS: Move keypad   OK: Type digit   RED/BACK: Close keypad", 2);
    } else {
        draw_text_centered(r, SCREEN_W / 2, FOOTER_Y + (FOOTER_H - 14) / 2,
            "ARROWS: Navigate   OK: Select / Edit IP   RED: Return / Exit", 2);
    }

    /* Debug key-status — very small, bottom-left corner */
    set_color(r, COL_TEXT_FAINT);
    draw_text(r, LEFT_X, FOOTER_Y + FOOTER_H - 12, status, 1);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Settings screen
 * ══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    SET_RESOLUTION = 0,   /* VIDEO group */
    SET_FPS,
    SET_BITRATE,
    SET_CODEC,
    SET_PS5,              /* SESSION group — must match render order below */
    SET_WAKE_ON_START,
    SET_SLEEP_ON_EXIT,
    SET_HW_DECODER,       /* DEVELOPER group */
    SET_LOG_LEVEL,
    SET_RETURN,
    SET_COUNT
} SettingsFocus;

static int settings_focus_at(int x, int y)
{
    const int row_x = 236;
    const int row_w = 1448;
    static const int row_y[SET_COUNT] = {
        206, 256, 306, 356, 430, 480, 530, 604, 654, 712
    };
    for (int i = 0; i < SET_COUNT; ++i) {
        int height = i == SET_RETURN ? 50 : 44;
        if (point_in_rect(x, y, row_x, row_y[i], row_w, height))
            return i;
    }
    return -1;
}

typedef struct { int w, h; const char *label; } ResOption;

static const ResOption RES_OPTIONS[] = {
    {1280, 720,  "720p"},
    {1920, 1080, "1080p"},
};
static const int FPS_OPTIONS[]    = {30, 60};
static const int BITRATE_OPTIONS[] = {5000, 10000, 15000, 25000, 35000, 45000};
static const char *CODEC_OPTIONS[] = {"h264", "h265", "h265_hdr"};
static const char *CODEC_LABELS[]  = {"H.264 (AVC)", "H.265 (HEVC)", "H.265 HDR"};
static const char *LOG_LEVEL_STRS[]   = {"off","error","warning","info","verbose","debug"};
static const char *LOG_LEVEL_LABELS[] = {"Off","Error","Warning","Info","Verbose","Debug"};

static const char *ui_log_level_to_string(int mask)
{
    if (mask == 0) return "off";
    if (mask == 16) return "error";
    if ((mask & (16|8|4|2|1)) == (16|8|4|2|1)) return "debug";
    if ((mask & (16|8|4|2))   == (16|8|4|2))   return "verbose";
    if ((mask & (16|8|4))     == (16|8|4))     return "info";
    if ((mask & (16|8))       == (16|8))       return "warning";
    return "warning";
}

static int ui_log_level_from_string(const char *s)
{
    if (!s || !s[0] || !strcmp(s,"off"))     return 0;
    if (!strcmp(s,"error"))                  return 16;
    if (!strcmp(s,"warning"))                return 16|8;
    if (!strcmp(s,"info"))                   return 16|8|4;
    if (!strcmp(s,"verbose"))                return 16|8|4|2;
    if (!strcmp(s,"debug"))                  return 16|8|4|2|1;
    return 16|8;
}

static int ui_find_resolution_idx(const AppConfig *cfg)
{
    int w = cfg ? cfg->video_width : 1920, h = cfg ? cfg->video_height : 1080;
    for (int i = 0; i < (int)(sizeof(RES_OPTIONS)/sizeof(RES_OPTIONS[0])); i++)
        if (RES_OPTIONS[i].w == w && RES_OPTIONS[i].h == h) return i;
    return 1;
}
static int ui_find_fps_idx(const AppConfig *cfg)
{
    int fps = cfg ? cfg->video_fps : 60;
    for (int i = 0; i < (int)(sizeof(FPS_OPTIONS)/sizeof(FPS_OPTIONS[0])); i++)
        if (FPS_OPTIONS[i] == fps) return i;
    return 1;
}
static int ui_find_bitrate_idx(const AppConfig *cfg)
{
    int br = cfg ? cfg->video_bitrate : 15000;
    for (int i = 0; i < (int)(sizeof(BITRATE_OPTIONS)/sizeof(BITRATE_OPTIONS[0])); i++)
        if (BITRATE_OPTIONS[i] == br) return i;
    int best = 2, best_diff = 1000000;
    for (int i = 0; i < (int)(sizeof(BITRATE_OPTIONS)/sizeof(BITRATE_OPTIONS[0])); i++) {
        int d = BITRATE_OPTIONS[i] - br; if (d < 0) d = -d;
        if (d < best_diff) { best_diff = d; best = i; }
    }
    return best;
}
static int ui_find_codec_idx(const AppConfig *cfg)
{
    const char *c = (cfg && cfg->video_codec) ? cfg->video_codec : "h265";
    for (int i = 0; i < 3; i++) if (!strcmp(CODEC_OPTIONS[i], c)) return i;
    return 1;
}
static int ui_find_log_level_idx(const AppConfig *cfg)
{
    const char *s = ui_log_level_to_string(cfg ? cfg->log_level : (16|8));
    for (int i = 0; i < 6; i++) if (!strcmp(LOG_LEVEL_STRS[i], s)) return i;
    return 2;
}

/* Draw a settings option row with label on left, value chip on right */
static void ui_draw_option_row(SDL_Renderer *r, int x, int y, int w, int h,
                               const char *label, const char *value, bool focused)
{
    int radius = 8;

    if (focused) {
        /* Focused row: accent fill */
        set_color(r, COL_ACCENT_DARK);
        fill_rounded_rect(r, x, y, w, h, radius);
        /* Left focus indicator */
        set_color(r, COL_ACCENT);
        fill_rect(r, x, y + 2, 4, h - 4);
    } else {
        set_color(r, COL_CODE_BG);
        fill_rounded_rect(r, x, y, w, h, radius);
        set_color(r, COL_BORDER_DIM);
        draw_rect_outline(r, x, y, w, h);
    }

    int scale = 2;
    int ty = y + (h - 7 * scale) / 2;

    set_color(r, focused ? COL_TEXT : COL_TEXT_DIM);
    draw_text(r, x + 20, ty, label, scale);

    if (value) {
        int vw = text_width(value, scale);
        /* Value chip */
        if (focused) {
            set_color(r, COL_ACCENT);
            fill_rounded_rect(r, x + w - vw - 32, ty - 4, vw + 24, 7 * scale + 8, 6);
            set_color(r, COL_TEXT);
        } else {
            set_color(r, COL_CARD);
            fill_rounded_rect(r, x + w - vw - 32, ty - 4, vw + 24, 7 * scale + 8, 6);
            set_color(r, COL_TEXT_DIM);
        }
        draw_text(r, x + w - vw - 20, ty, value, scale);
    }
}

static void ui_render_settings_screen(SDL_Renderer *r,
                                      const AppConfig *cfg,
                                      int focus,
                                      const char *msg,
                                      int res_idx, int fps_idx,
                                      int br_idx,  int codec_idx,
                                      int log_idx)
{
    /* Background */
    set_color(r, COL_BG);
    fill_rect(r, 0, 0, SCREEN_W, SCREEN_H);
    set_color(r, COL_BG_DEEPER);
    fill_rect(r, 0, 0, SCREEN_W, 60);

    /* Header */
    set_color(r, COL_PANEL);
    fill_rect(r, 0, HDR_Y, SCREEN_W, HDR_H);
    set_color(r, COL_BORDER_DIM);
    hline(r, 0, HDR_Y + HDR_H - 1, SCREEN_W);
    set_color(r, COL_ACCENT);
    fill_rect(r, 0, HDR_Y, 8, HDR_H);
    set_color(r, COL_TEXT);
    draw_text(r, 36, HDR_Y + 10, "CHIAKI", 4);
    set_color(r, COL_TEXT_DIM);
    draw_text(r, 36, HDR_Y + 50, "Settings", 2);

    /* Content area (single card, centred) */
    const int CX    = 200;
    const int CY    = CONTENT_Y;
    const int CW    = SCREEN_W - 400;
    const int CH    = CONTENT_H;

    set_color(r, COL_PANEL);
    fill_rounded_rect(r, CX, CY, CW, CH, 14);
    set_color(r, COL_ACCENT);
    fill_rect(r, CX, CY, 6, CH);

    /* Section header */
    int rx = CX + 36, ry = CY + 22, rw = CW - 72;
    set_color(r, COL_TEXT);
    draw_text(r, rx, ry, "Streaming Options", 3);
    set_color(r, COL_TEXT_FAINT);
    draw_text(r, rx + CW - 72 - text_width("Saved to config.json", 2),
              ry + 4, "Saved to config.json", 2);
    ry += 38;
    set_color(r, COL_BORDER_DIM);
    hline(r, rx, ry, rw);
    ry += 10;

    const int ROW_H   = 44;
    const int ROW_GAP = 6;
    char vbuf[64];

    /* Group: Video */
    set_color(r, COL_TEXT_FAINT);
    draw_text(r, rx, ry, "VIDEO", 2); ry += 20;

    snprintf(vbuf, sizeof(vbuf), "%s  (%dx%d)",
             RES_OPTIONS[res_idx].label, RES_OPTIONS[res_idx].w, RES_OPTIONS[res_idx].h);
    ui_draw_option_row(r, rx, ry, rw, ROW_H, "Resolution", vbuf, focus == SET_RESOLUTION);
    ry += ROW_H + ROW_GAP;

    snprintf(vbuf, sizeof(vbuf), "%d fps", FPS_OPTIONS[fps_idx]);
    ui_draw_option_row(r, rx, ry, rw, ROW_H, "Frame Rate", vbuf, focus == SET_FPS);
    ry += ROW_H + ROW_GAP;

    snprintf(vbuf, sizeof(vbuf), "%d kbps", BITRATE_OPTIONS[br_idx]);
    ui_draw_option_row(r, rx, ry, rw, ROW_H, "Bitrate", vbuf, focus == SET_BITRATE);
    ry += ROW_H + ROW_GAP;

    ui_draw_option_row(r, rx, ry, rw, ROW_H, "Codec", CODEC_LABELS[codec_idx], focus == SET_CODEC);
    ry += ROW_H + ROW_GAP + 4;

    /* Group: Session */
    set_color(r, COL_TEXT_FAINT);
    draw_text(r, rx, ry, "SESSION", 2); ry += 20;

    ui_draw_option_row(r, rx, ry, rw, ROW_H, "PS5 Mode",
                       (cfg && cfg->ps5) ? "On" : "Off", focus == SET_PS5);
    ry += ROW_H + ROW_GAP;

    ui_draw_option_row(r, rx, ry, rw, ROW_H, "Wake on Start",
                       (cfg && cfg->wakeup) ? "On" : "Off", focus == SET_WAKE_ON_START);
    ry += ROW_H + ROW_GAP;

    ui_draw_option_row(r, rx, ry, rw, ROW_H, "Sleep on Exit",
                       (cfg && cfg->sleep_on_exit) ? "On" : "Off", focus == SET_SLEEP_ON_EXIT);
    ry += ROW_H + ROW_GAP + 4;

    /* Group: Developer */
    set_color(r, COL_TEXT_FAINT);
    draw_text(r, rx, ry, "DEVELOPER", 2); ry += 20;

    ui_draw_option_row(r, rx, ry, rw, ROW_H, "HW Decoder (unused)",
                       (cfg && cfg->hw_decode) ? "On" : "Off", focus == SET_HW_DECODER);
    ry += ROW_H + ROW_GAP;

    ui_draw_option_row(r, rx, ry, rw, ROW_H, "Log Level",
                       LOG_LEVEL_LABELS[log_idx], focus == SET_LOG_LEVEL);
    ry += ROW_H + ROW_GAP + 8;

    /* Return button */
    draw_button(r, rx, ry, rw, 50, "Return", focus == SET_RETURN, true);

    /* Message (below card, above footer) */
    if (msg && msg[0]) {
        set_color(r, COL_TEXT_DIM);
        draw_text_centered(r, SCREEN_W / 2, CY + CH + 10, msg, 2);
    }

    /* Footer */
    set_color(r, COL_PANEL);
    fill_rect(r, 0, FOOTER_Y, SCREEN_W, FOOTER_H);
    set_color(r, COL_BORDER_DIM);
    hline(r, 0, FOOTER_Y, SCREEN_W);
    set_color(r, COL_TEXT_FAINT);
    draw_text_centered(r, SCREEN_W / 2, FOOTER_Y + (FOOTER_H - 14) / 2,
        "UP/DOWN: Navigate   LEFT/RIGHT: Change value   RED: Return", 2);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Loading screen
 * ══════════════════════════════════════════════════════════════════════════ */

void ui_render_loading(SDL_Renderer *r, const char *base_text)
{
    int sw = SCREEN_W, sh = SCREEN_H;
    (void)SDL_GetRendererOutputSize(r, &sw, &sh);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    set_color(r, COL_BG);
    fill_rect(r, 0, 0, sw, sh);

    /* PS blue top accent bar */
    set_color(r, COL_ACCENT);
    fill_rect(r, 0, 0, sw, 6);

    /* App branding in top-left */
    set_color(r, COL_TEXT_FAINT);
    draw_text(r, 40, 30, "CHIAKI", 3);

    /* Centre content */
    if (!base_text) base_text = "Loading";

    /* Animated spinner (8-frame dot ring around centre text) */
    int cx = sw / 2, cy = sh / 2;

    /* Spinner dots: 8 positions around a circle radius 42 */
    /* Approximate circle positions at radius 42 */
    static const int DOT_DX[8] = {  0, 30, 42, 30,  0, -30, -42, -30 };
    static const int DOT_DY[8] = {-42,-30,  0, 30, 42,  30,   0, -30 };
    int spin_frame = (int)((SDL_GetTicks() / 80) % 8);

    for (int i = 0; i < 8; i++) {
        int age = (8 + spin_frame - i) % 8;  /* 0 = newest (brightest) */
        int bright = 200 - age * 22;
        if (bright < 40) bright = 40;
        set_color(r, 0x00, (int)(bright * 0.6f), bright, 0xff);
        int dsz = (age == 0) ? 10 : 6;
        fill_rounded_rect(r,
            cx + DOT_DX[i] - dsz / 2,
            cy + DOT_DY[i] - dsz / 2,
            dsz, dsz, 3);
    }

    /* Animated message with dots */
    int dot_count = (int)((SDL_GetTicks() / 400) % 4);
    char msg[256];
    snprintf(msg, sizeof(msg), "%s%.*s", base_text, dot_count, "...");

    int msg_scale = 4;
    int mw = text_width(msg, msg_scale);
    set_color(r, COL_TEXT);
    draw_text(r, cx - mw / 2, cy + 70, msg, msg_scale);

    /* Subtitle */
    set_color(r, COL_TEXT_DIM);
    draw_text_centered(r, cx, cy + 116, "Please wait", 2);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Stats overlay
 * ══════════════════════════════════════════════════════════════════════════ */

void ui_render_stats_overlay(SDL_Renderer *r, const char *text)
{
    if (!r || !text || !text[0]) return;

    int sw = SCREEN_W, sh = SCREEN_H;
    (void)SDL_GetRendererOutputSize(r, &sw, &sh);

    const int scale  = 2;
    const int pad    = 20;
    const int line_h = 9 * scale;

    /* Measure */
    int lines = 1, maxw = 0, curw = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') { if (curw > maxw) maxw = curw; curw = 0; lines++; continue; }
        curw += 6 * scale;
    }
    if (curw > maxw) maxw = curw;

    int box_w = maxw + pad * 2 + 8;  /* +8 for left accent bar */
    int box_h = lines * line_h + pad * 2;
    if (box_w > sw - 40) box_w = sw - 40;
    if (box_h > sh - 40) box_h = sh - 40;

    int x = 24, y = 24;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    /* Shadow */
    set_color(r, 0x00, 0x00, 0x00, 100);
    fill_rounded_rect(r, x + 4, y + 4, box_w, box_h, 12);

    /* Background */
    set_color(r, 0x06, 0x0e, 0x24, 210);
    fill_rounded_rect(r, x, y, box_w, box_h, 12);

    /* Border */
    set_color(r, COL_BORDER);
    draw_rect_outline(r, x, y, box_w, box_h);

    /* Left accent bar */
    set_color(r, COL_ACCENT);
    fill_rect(r, x, y + 3, 4, box_h - 6);

    /* Text */
    set_color(r, COL_TEXT);
    draw_text(r, x + pad + 8, y + pad, text, scale);

    /* Top label */
    set_color(r, COL_ACCENT_MID);
    hline(r, x + 4, y + pad + lines * line_h + 6, box_w - 8);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Startup hint overlay
 * ══════════════════════════════════════════════════════════════════════════ */

void ui_render_hint(SDL_Renderer *r, const char *text, float opacity)
{
    if (!r || !text || !text[0] || opacity <= 0.0f) return;

    int sw = SCREEN_W, sh = SCREEN_H;
    (void)SDL_GetRendererOutputSize(r, &sw, &sh);
    (void)sh;

    const int scale = 2;
    const int pad   = 14;
    int tw = text_width(text, scale);

    int box_w = tw + pad * 2 + 8;
    int box_h = 7 * scale + pad * 2;
    int x = 24, y = 24;

    if (opacity > 1.0f) opacity = 1.0f;
    uint8_t alpha_bg   = (uint8_t)(180.0f * opacity);
    uint8_t alpha_text = (uint8_t)(255.0f * opacity);
    uint8_t alpha_acc  = (uint8_t)(220.0f * opacity);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    set_color(r, 0x06, 0x0e, 0x24, alpha_bg);
    fill_rounded_rect(r, x, y, box_w, box_h, box_h / 2);

    set_color(r, 0x00, 0x72, 0xff, alpha_acc);
    fill_rect(r, x, y + 3, 4, box_h - 6);

    set_color(r, 0xec, 0xf2, 0xff, alpha_text);
    draw_text(r, x + pad + 8, y + pad, text, scale);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Shared helpers (config / validation)
 * ══════════════════════════════════════════════════════════════════════════ */

static bool ui_has_keys(const AppConfig *cfg)
{
    return cfg
        && cfg->psn_account_id_b64 && cfg->psn_account_id_b64[0]
        && cfg->registered_key_b64 && cfg->registered_key_b64[0]
        && cfg->rp_key_b64 && cfg->rp_key_b64[0]
        && strlen(cfg->registered_key_b64) >= 8;
}

static bool ui_valid_ipv4(const char *s)
{
    if (!s || !s[0]) return false;
    struct in_addr a;
    return inet_pton(AF_INET, s, &a) == 1;
}

static bool ui_write_host(const char *config_path, const char *host)
{
    if (!config_path || !host || !host[0]) return false;
    struct json_object *root = json_object_from_file(config_path);
    if (!root) root = json_object_new_object();
    json_object_object_del(root, "host");
    json_object_object_add(root, "host", json_object_new_string(host));
    int rc = json_object_to_file_ext(config_path, root, JSON_C_TO_STRING_PRETTY);
    json_object_put(root);
    return rc == 0;
}

static bool ui_write_settings_json(const char *config_path, const AppConfig *cfg)
{
    if (!config_path || !cfg) return false;
    struct json_object *root = json_object_from_file(config_path);
    if (!root) root = json_object_new_object();

#define SET_INT(k, v)  json_object_object_del(root, k); \
                       json_object_object_add(root, k, json_object_new_int(v))
#define SET_BOOL(k, v) json_object_object_del(root, k); \
                       json_object_object_add(root, k, json_object_new_boolean(v))
#define SET_DBL(k, v)  json_object_object_del(root, k); \
                       json_object_object_add(root, k, json_object_new_double(v))
#define SET_STR(k, v)  json_object_object_del(root, k); \
                       json_object_object_add(root, k, json_object_new_string(v))

    SET_INT ("video_width",   cfg->video_width);
    SET_INT ("video_height",  cfg->video_height);
    SET_INT ("video_fps",     cfg->video_fps);
    SET_INT ("video_bitrate", cfg->video_bitrate);
    SET_BOOL("ps5",           cfg->ps5);
    SET_BOOL("hw_decode",     cfg->hw_decode);
    SET_BOOL("wakeup",        cfg->wakeup);
    SET_BOOL("sleep_on_exit", cfg->sleep_on_exit);
    SET_DBL ("packet_loss_max", cfg->packet_loss_max);
    SET_BOOL("idr_on_fec_failure", cfg->idr_on_fec_failure);
    SET_STR ("video_codec",   (cfg->video_codec && cfg->video_codec[0]) ? cfg->video_codec : "h265");
    SET_STR ("log_level",     ui_log_level_to_string(cfg->log_level));

#undef SET_INT
#undef SET_BOOL
#undef SET_DBL
#undef SET_STR

    int rc = json_object_to_file_ext(config_path, root, JSON_C_TO_STRING_PRETTY);
    json_object_put(root);
    return rc == 0;
}

static void ui_reload_cfg(AppConfig *cfg, const char *config_path)
{
    if (!cfg) return;
    config_free(cfg);
    (void)config_load(cfg, config_path);
}

static void ui_build_ini_path(const char *config_path, char *out, size_t outlen)
{
    if (!out || outlen == 0) return;
    out[0] = '\0';
    if (!config_path) { snprintf(out, outlen, "chiaki-ng-Default.ini"); return; }
    const char *slash = strrchr(config_path, '/');
    if (!slash) { snprintf(out, outlen, "chiaki-ng-Default.ini"); return; }
    size_t dirlen = (size_t)(slash - config_path);
    if (dirlen + 1 >= outlen) dirlen = outlen - 2;
    memcpy(out, config_path, dirlen); out[dirlen] = '\0';
    snprintf(out + dirlen, outlen - dirlen, "/chiaki-ng-Default.ini");
}

static void ui_set_codec(AppConfig *cfg, const char *codec)
{
    if (!cfg) return;
    if (!codec) codec = "h265";
    if (cfg->video_codec && !strcmp(cfg->video_codec, codec)) return;
    free(cfg->video_codec);
    cfg->video_codec = strdup(codec);
}

static void ui_apply_settings_indices(AppConfig *cfg,
                                      int res_idx, int fps_idx,
                                      int br_idx,  int codec_idx, int log_idx)
{
    if (!cfg) return;
    if (res_idx   < 0 || res_idx   >= (int)(sizeof(RES_OPTIONS)/sizeof(RES_OPTIONS[0]))) res_idx   = 1;
    if (fps_idx   < 0 || fps_idx   >= (int)(sizeof(FPS_OPTIONS)/sizeof(FPS_OPTIONS[0]))) fps_idx   = 1;
    if (br_idx    < 0 || br_idx    >= (int)(sizeof(BITRATE_OPTIONS)/sizeof(BITRATE_OPTIONS[0]))) br_idx = 2;
    if (codec_idx < 0 || codec_idx >= 3) codec_idx = 1;
    if (log_idx   < 0 || log_idx   >= 6) log_idx   = 2;

    cfg->video_width   = RES_OPTIONS[res_idx].w;
    cfg->video_height  = RES_OPTIONS[res_idx].h;
    cfg->video_fps     = FPS_OPTIONS[fps_idx];
    cfg->video_bitrate = BITRATE_OPTIONS[br_idx];
    ui_set_codec(cfg, CODEC_OPTIONS[codec_idx]);
    cfg->log_level = ui_log_level_from_string(LOG_LEVEL_STRS[log_idx]);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Settings event loop
 * ══════════════════════════════════════════════════════════════════════════ */

static void ui_run_settings(SDL_Renderer *renderer, AppConfig *cfg,
                             const char *config_path, InputContext *input_ctx)
{
    if (!renderer || !cfg) return;

    int res_idx   = ui_find_resolution_idx(cfg);
    int fps_idx   = ui_find_fps_idx(cfg);
    int br_idx    = ui_find_bitrate_idx(cfg);
    int codec_idx = ui_find_codec_idx(cfg);
    int log_idx   = ui_find_log_level_idx(cfg);
    int focus     = SET_RESOLUTION;
    char msg[256] = {0};

    bool wants_exit = false;
    int stick_x = 0, stick_y = 0;

    while (1) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            input_handle_event(input_ctx, &ev);
            if (ev.type == SDL_QUIT) { wants_exit = true; continue; }
            if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode sym = ev.key.keysym.sym;
                if (webos_event_is_back_or_red(&ev.key)) { wants_exit = true; continue; }

                if      (sym == SDLK_UP   && focus > 0)          focus--;
                else if (sym == SDLK_DOWN && focus < SET_RETURN) focus++;
                else {
                    bool fwd = (sym == SDLK_RIGHT || sym == SDLK_RETURN ||
                                sym == SDLK_KP_ENTER || sym == SDLK_SPACE);
                    bool changed = false;
                    int n;

                    if      (focus == SET_RESOLUTION)  { n = (int)(sizeof(RES_OPTIONS)/sizeof(RES_OPTIONS[0])); res_idx   = (res_idx   + (fwd?1:-1) + n) % n; changed = true; }
                    else if (focus == SET_FPS)          { n = (int)(sizeof(FPS_OPTIONS)/sizeof(FPS_OPTIONS[0])); fps_idx   = (fps_idx   + (fwd?1:-1) + n) % n; changed = true; }
                    else if (focus == SET_BITRATE)      { n = (int)(sizeof(BITRATE_OPTIONS)/sizeof(BITRATE_OPTIONS[0])); br_idx = (br_idx + (fwd?1:-1) + n) % n; changed = true; }
                    else if (focus == SET_CODEC)        { codec_idx = (codec_idx + (fwd?1:-1) + 3) % 3; changed = true; }
                    else if (focus == SET_LOG_LEVEL)    { log_idx   = (log_idx   + (fwd?1:-1) + 6) % 6; changed = true; }
                    else if (focus == SET_PS5)          { cfg->ps5         = !cfg->ps5;         changed = true; }
                    else if (focus == SET_HW_DECODER)   { cfg->hw_decode   = !cfg->hw_decode;   changed = true; }
                    else if (focus == SET_WAKE_ON_START){ cfg->wakeup      = !cfg->wakeup;      changed = true; }
                    else if (focus == SET_SLEEP_ON_EXIT){ cfg->sleep_on_exit = !cfg->sleep_on_exit; changed = true; }
                    else if (focus == SET_RETURN)       { wants_exit = true; continue; }

                    if (changed) {
                        ui_apply_settings_indices(cfg, res_idx, fps_idx, br_idx, codec_idx, log_idx);
                        if (!ui_write_settings_json(config_path, cfg))
                            snprintf(msg, sizeof(msg), "Failed to write config.json");
                        else
                            snprintf(msg, sizeof(msg), "Saved.");
                    }
                }
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B ||
                    ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) { wants_exit = true; continue; }

                SDL_Keycode fake_sym = 0;
                if      (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)    fake_sym = SDLK_UP;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  fake_sym = SDLK_DOWN;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  fake_sym = SDLK_LEFT;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) fake_sym = SDLK_RIGHT;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A)          fake_sym = SDLK_RETURN;

                if (fake_sym) {
                    push_ui_key(fake_sym);
                }
            } else if (ev.type == SDL_CONTROLLERAXISMOTION) {
                SDL_Keycode key = stick_nav_key(&ev.caxis, &stick_x, &stick_y);
                if (key) push_ui_key(key);
            } else if (ev.type == SDL_MOUSEMOTION) {
                int x, y;
                pointer_to_ui(renderer, ev.motion.x, ev.motion.y, &x, &y);
                int hovered = settings_focus_at(x, y);
                if (hovered >= 0) focus = hovered;
            } else if (ev.type == SDL_MOUSEBUTTONDOWN &&
                       ev.button.button == SDL_BUTTON_LEFT) {
                int x, y;
                pointer_to_ui(renderer, ev.button.x, ev.button.y, &x, &y);
                int clicked = settings_focus_at(x, y);
                if (clicked >= 0) {
                    focus = clicked;
                    push_ui_key(SDLK_RETURN);
                }
            }
        }

        /* If Back/B/Return was pressed, finish draining the current event
         * batch, wait for any companion events, drain again, then return
         * cleanly to the launcher. */
        if (wants_exit) {
            SDL_Delay(150);
            SDL_PumpEvents();
            { SDL_Event _d; while (SDL_PollEvent(&_d)) {} }
            return;
        }

        ui_render_settings_screen(renderer, cfg, focus, msg,
                                   res_idx, fps_idx, br_idx, codec_idx, log_idx);
        SDL_RenderPresent(renderer);
        input_pump(input_ctx);
        SDL_Delay(16);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main registration / launcher UI
 * ══════════════════════════════════════════════════════════════════════════ */

static int setup_focus_at(int x, int y, bool editing)
{
    const int rx = RIGHT_X + 52;
    const int rw = RIGHT_W - 104;
    const int input_y = CONTENT_Y + 52 + 26;
    if (point_in_rect(x, y, rx, input_y, rw, 72))
        return 0;

    int action_y = input_y + 72 + 20;
    if (editing) {
        const int key_y = action_y + 26;
        const int key_w = 128, key_h = 64, gap = 12;
        for (int i = 0; i < 12; ++i) {
            int row = i / 3, col = i % 3;
            if (point_in_rect(x, y,
                              rx + col * (key_w + gap),
                              key_y + row * (key_h + gap), key_w, key_h))
                return i + 1;
        }
        int done_y = key_y + 4 * key_h + 3 * gap + 16;
        if (point_in_rect(x, y, rx, done_y, 3 * key_w + 2 * gap, key_h))
            return 15;
    } else {
        int button_w = (rw - 20) / 2;
        if (point_in_rect(x, y, rx, action_y, button_w, 80))
            return 13;
        if (point_in_rect(x, y, rx + button_w + 20, action_y, button_w, 80))
            return 14;
        if (point_in_rect(x, y, rx, action_y + 96, rw, 60))
            return 16;
    }
    return -1;
}

UIResult ui_run_registration(SDL_Renderer *renderer, AppConfig *cfg,
                             const char *config_path, ChiakiLog *log,
                             const char *initial_message,
                             InputContext *input_ctx)
{
    (void)log;

    /* Pre-fill IP from config */
    char ip[64] = {0};
    if (cfg && cfg->host && cfg->host[0])
        snprintf(ip, sizeof(ip), "%s", cfg->host);

    /* Focus mapping:
     *   Main page:  0=IP field, 13=Import, 14=Connect, 16=Settings
     *   Keypad:     1..12=digit keys, 15=Done
     */
    int  focus   = 0;
    bool editing = false;
    char msg[256] = {0};
    if (initial_message && initial_message[0])
        snprintf(msg, sizeof(msg), "%s", initial_message);

    /* Start with Connect focused if already ready */
    if (ui_valid_ipv4(ip) && ui_has_keys(cfg))
        focus = 14;

    SDL_StartTextInput();
    int stick_x = 0, stick_y = 0;

    while (1) {
        /* Build lightweight debug status string */
        char status[256];
        snprintf(status, sizeof(status),
            "host:%s  acct:%s  rk:%s  rpk:%s  rt:%s",
            (cfg && cfg->host) ? cfg->host : "(null)",
            (cfg && cfg->psn_account_id_b64) ? "OK" : "NO",
            (cfg && cfg->registered_key_b64 && strlen(cfg->registered_key_b64) >= 8) ? "OK" : "NO",
            (cfg && cfg->rp_key_b64) ? "OK" : "NO",
            (cfg && cfg->psn_refresh_token) ? "OK" : "none");

        bool has_keys    = ui_has_keys(cfg);
        bool has_valid_ip = ui_valid_ipv4(ip);
        bool can_connect = has_valid_ip && has_keys;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            input_handle_event(input_ctx, &ev);
            if (ev.type == SDL_QUIT) return UI_RESULT_QUIT;

            if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode sym = ev.key.keysym.sym;

                /* Back key: close keypad if open, otherwise exit launcher */
                if (sym == SDLK_AC_HOME) return UI_RESULT_QUIT;
                if (webos_event_is_back_or_red(&ev.key)) {
                    if (editing) { editing = false; focus = 0; continue; }
                    return UI_RESULT_QUIT;
                }

                /* Magic Remote number keys edit the IP regardless of focus. */
                bool direct_ip_key =
                    (sym >= SDLK_0 && sym <= SDLK_9) ||
                    (sym >= SDLK_KP_0 && sym <= SDLK_KP_9) ||
                    sym == SDLK_PERIOD || sym == SDLK_KP_PERIOD ||
                    sym == SDLK_BACKSPACE;
                if (direct_ip_key && !editing)
                    focus = 0;

                /* Navigation */
                if (sym == SDLK_UP) {
                    if (editing) {
                        if (focus >= 1 && focus <= 12) {
                            int row = (focus - 1) / 3;
                            if (row > 0) focus -= 3;
                        } else if (focus == 15) {
                            focus = 10; /* "0" key */
                        }
                    } else {
                        if (focus == 13 || focus == 14) focus = 0;
                        else if (focus == 16)            focus = 13;
                    }
                } else if (sym == SDLK_DOWN) {
                    if (editing) {
                        if (focus >= 1 && focus <= 12) {
                            int row = (focus - 1) / 3;
                            if (row == 3) focus = 15;
                            else          focus += 3;
                        }
                    } else {
                        if (focus == 0)               focus = 13;
                        else if (focus == 13 || focus == 14) focus = 16;
                    }
                } else if (sym == SDLK_LEFT) {
                    if (editing) {
                        if (focus >= 1 && focus <= 12) {
                            int col = (focus - 1) % 3;
                            if (col > 0) focus--;
                        }
                    } else {
                        if (focus == 14) focus = 13;
                    }
                } else if (sym == SDLK_RIGHT) {
                    if (editing) {
                        if (focus >= 1 && focus <= 12) {
                            int col = (focus - 1) % 3;
                            if (col < 2) focus++;
                        }
                    } else {
                        if (focus == 13) focus = 14;
                    }
                }

                /* Physical keyboard IP entry */
                if (focus == 0 || editing) {
                    size_t len = strlen(ip);
                    if (sym == SDLK_BACKSPACE) {
                        if (len > 0) ip[len - 1] = '\0';
                    } else if ((sym >= SDLK_0 && sym <= SDLK_9) ||
                               (sym >= SDLK_KP_0 && sym <= SDLK_KP_9) ||
                               sym == SDLK_PERIOD || sym == SDLK_KP_PERIOD) {
                        if (len + 1 < sizeof(ip)) {
                            char c;
                            if      (sym == SDLK_PERIOD || sym == SDLK_KP_PERIOD) c = '.';
                            else if (sym >= SDLK_0 && sym <= SDLK_9)              c = '0' + (char)(sym - SDLK_0);
                            else                                                   c = '0' + (char)(sym - SDLK_KP_0);
                            ip[len] = c; ip[len + 1] = '\0';
                        }
                    }
                }

                /* Select / Activate */
                if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER || sym == SDLK_SPACE) {
                    if (editing && focus >= 1 && focus <= 12) {
                        /* Keypad digit press */
                        static const char *K[12] = {
                            "1","2","3","4","5","6","7","8","9",".","0","DEL"
                        };
                        const char *k = K[focus - 1];
                        size_t len = strlen(ip);
                        if (!strcmp(k, "DEL")) {
                            if (len > 0) ip[len - 1] = '\0';
                        } else if (len + 1 < sizeof(ip)) {
                            char c = k[0];
                            if (c == '.' || (c >= '0' && c <= '9'))
                                { ip[len] = c; ip[len + 1] = '\0'; }
                        }
                    } else if (editing && focus == 15) {
                        editing = false; focus = 0;
                    } else if (!editing && focus == 0) {
                        editing = true; focus = 1;
                    } else if (!editing && focus == 13) {
                        /* Import Config */
                        if (ip[0] && !ui_valid_ipv4(ip)) {
                            snprintf(msg, sizeof(msg), "Clear the IP field or enter a valid address.");
                            focus = 0; continue;
                        }
                        if (ip[0] && !ui_write_host(config_path, ip)) {
                            snprintf(msg, sizeof(msg), "Failed to write host to config.json");
                            continue;
                        }
                        char ini_path[512];
                        ui_build_ini_path(config_path, ini_path, sizeof(ini_path));
                        ChiakiImportResult res = config_try_import_chiaki_ini(ini_path, config_path);
                        switch (res) {
                        case CI_FILE_NOT_FOUND:
                            snprintf(msg, sizeof(msg), "chiaki-ng-Default.ini not found. Copy it to the TV first.");
                            break;
                        case CI_PARSE_ERROR:
                            snprintf(msg, sizeof(msg), "Import failed: could not parse chiaki-ng INI.");
                            break;
                        case CI_WRITE_ERROR:
                            snprintf(msg, sizeof(msg), "Import failed: could not write config.json.");
                            break;
                        case CI_SUCCESS:
                            snprintf(msg, sizeof(msg), "Import OK. PS5 IP loaded. Press Connect.");
                            ui_reload_cfg(cfg, config_path);
                            if (cfg && cfg->host && cfg->host[0])
                                snprintf(ip, sizeof(ip), "%s", cfg->host);
                            if (ui_has_keys(cfg) && ui_valid_ipv4(ip))
                                focus = 14;
                            break;
                        case CI_SUCCESS_NEEDS_HOST:
                            snprintf(msg, sizeof(msg), "Keys imported. Enter the PS5 IP address.");
                            ui_reload_cfg(cfg, config_path);
                            if (cfg && cfg->host && cfg->host[0])
                                snprintf(ip, sizeof(ip), "%s", cfg->host);
                            focus = 0;
                            break;
                        default:
                            snprintf(msg, sizeof(msg), "Import complete.");
                            ui_reload_cfg(cfg, config_path);
                            break;
                        }
                    } else if (!editing && focus == 16) {
                        /* Settings — drains Back events internally before returning */
                        ui_run_settings(renderer, cfg, config_path, input_ctx);
                        ui_reload_cfg(cfg, config_path);
                        if (cfg && cfg->host && cfg->host[0])
                            snprintf(ip, sizeof(ip), "%s", cfg->host);
                        snprintf(msg, sizeof(msg), "Settings updated.");
                        focus = (ui_has_keys(cfg) && ui_valid_ipv4(ip)) ? 14 : 0;
                    } else if (!editing && focus == 14) {
                        /* Connect */
                        if (!ui_valid_ipv4(ip)) {
                            snprintf(msg, sizeof(msg), "Enter a valid PS5 IP address.");
                            focus = 0; continue;
                        }
                        if (!ui_write_host(config_path, ip)) {
                            snprintf(msg, sizeof(msg), "Failed to save host to config.json");
                            continue;
                        }
                        ui_reload_cfg(cfg, config_path);
                        if (!ui_has_keys(cfg)) {
                            snprintf(msg, sizeof(msg), "Missing registration keys. Import chiaki-ng-Default.ini first.");
                            focus = 13; continue;
                        }
                        return UI_RESULT_CONNECT;
                    }
                }

            } else if (ev.type == SDL_TEXTINPUT) {
                if (focus == 0 || editing) {
                    for (const char *p = ev.text.text; *p; p++) {
                        char c = *p;
                        if (!(isdigit((unsigned char)c) || c == '.')) continue;
                        size_t len = strlen(ip);
                        if (len + 1 < sizeof(ip)) { ip[len] = c; ip[len + 1] = '\0'; }
                    }
                }
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B ||
                    ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                    if (editing) { editing = false; focus = 0; continue; }
                    return UI_RESULT_QUIT;
                }
                SDL_Keycode fake_sym = 0;
                if      (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)    fake_sym = SDLK_UP;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  fake_sym = SDLK_DOWN;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  fake_sym = SDLK_LEFT;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) fake_sym = SDLK_RIGHT;
                else if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A)          fake_sym = SDLK_RETURN;
                if (fake_sym) {
                    push_ui_key(fake_sym);
                }
            } else if (ev.type == SDL_CONTROLLERAXISMOTION) {
                SDL_Keycode key = stick_nav_key(&ev.caxis, &stick_x, &stick_y);
                if (key) push_ui_key(key);
            } else if (ev.type == SDL_MOUSEMOTION) {
                int x, y;
                pointer_to_ui(renderer, ev.motion.x, ev.motion.y, &x, &y);
                int hovered = setup_focus_at(x, y, editing);
                if (hovered >= 0) focus = hovered;
            } else if (ev.type == SDL_MOUSEBUTTONDOWN &&
                       ev.button.button == SDL_BUTTON_LEFT) {
                int x, y;
                pointer_to_ui(renderer, ev.button.x, ev.button.y, &x, &y);
                int clicked = setup_focus_at(x, y, editing);
                if (clicked >= 0) {
                    focus = clicked;
                    push_ui_key(SDLK_RETURN);
                }
            }
        }

        render_setup_screen(renderer, status, ip, focus, msg,
                             has_keys, has_valid_ip, can_connect, editing);
        SDL_RenderPresent(renderer);
        input_pump(input_ctx);
        SDL_Delay(16);
    }
}
