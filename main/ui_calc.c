#include "ui_calc.h"
#include "app_config.h"
#include "hal_display.h"
#include "gfx.h"
#include "font.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define EXPR_MAX 63

static char s_expr[EXPR_MAX + 1] = "";
static char s_msg[EXPR_MAX + 1]  = "";   /* result line / error */
static int  s_just_eval = 0;
static int  s_cur_col = 0, s_cur_row = 0;

static const char *KEYS[5][4] = {
    { "C", "(", ")", "/" },
    { "7", "8", "9", "*" },
    { "4", "5", "6", "-" },
    { "1", "2", "3", "+" },
    { "0", ".", "<", "=" },
};

/* Layout (240x320 portrait) */
#define PAD_Y0   112
#define CELL_W   56
#define CELL_H   40
#define PAD_X0   8

/* ---------- expression evaluator (shunting-yard, doubles) ---------- */

typedef struct { double v[128]; int n; } vstack_t;
typedef struct { char   v[128]; int n; } ostack_t;

static int prec(char op) {
    return (op == '+' || op == '-') ? 1 : (op == '*' || op == '/') ? 2 : 0;
}

static int apply(vstack_t *vs, char op, int *err) {
    if (vs->n < 2) { *err = 1; return 0; }
    double b = vs->v[--vs->n], a = vs->v[--vs->n], r = 0;
    switch (op) {
        case '+': r = a + b; break;
        case '-': r = a - b; break;
        case '*': r = a * b; break;
        case '/':
            if (b == 0) { *err = 1; return 0; }
            r = a / b; break;
        default: *err = 1; return 0;
    }
    if (!isfinite(r)) { *err = 1; return 0; }
    vs->v[vs->n++] = r;
    return 1;
}

/* Returns 1 on success with result in *out. */
static int calc_eval(const char *e, double *out) {
    vstack_t vs = { .n = 0 };
    ostack_t os = { .n = 0 };
    int err = 0, expect_operand = 1;
    const char *p = e;

    if (!*p)
        return 0;
    while (*p && !err) {
        if (*p == ' ') { p++; continue; }
        if (expect_operand && (*p == '(')) {
            os.v[os.n++] = '(';
            p++;
        } else if (expect_operand && (*p == '-' || *p == '+') &&
                   (p[1] == '(')) {
            /* unary sign before paren: "-(...)" -> "0-(...)" */
            vs.v[vs.n++] = 0;
            while (os.n && os.v[os.n - 1] != '(' &&
                   prec(os.v[os.n - 1]) >= prec(*p))
                if (!apply(&vs, os.v[--os.n], &err)) break;
            os.v[os.n++] = *p++;
        } else if (expect_operand &&
                   ((*p >= '0' && *p <= '9') || *p == '.' ||
                    ((*p == '-' || *p == '+') &&
                     ((p[1] >= '0' && p[1] <= '9') || p[1] == '.')))) {
            char *end = NULL;
            double v = strtod(p, &end);
            if (end == p || !isfinite(v)) { err = 1; break; }
            if (vs.n >= 128) { err = 1; break; }
            vs.v[vs.n++] = v;
            p = end;
            expect_operand = 0;
        } else if (!expect_operand && (*p == '+' || *p == '-' ||
                                       *p == '*' || *p == '/')) {
            while (os.n && os.v[os.n - 1] != '(' &&
                   prec(os.v[os.n - 1]) >= prec(*p))
                if (!apply(&vs, os.v[--os.n], &err)) break;
            if (os.n >= 128) { err = 1; break; }
            os.v[os.n++] = *p++;
            expect_operand = 1;
        } else if (!expect_operand && *p == ')') {
            while (os.n && os.v[os.n - 1] != '(')
                if (!apply(&vs, os.v[--os.n], &err)) break;
            if (!os.n || err) { err = 1; break; }
            os.n--; /* pop '(' */
            p++;
        } else {
            err = 1;
        }
    }
    while (!err && os.n) {
        if (os.v[os.n - 1] == '(') { err = 1; break; }
        if (!apply(&vs, os.v[--os.n], &err)) break;
    }
    if (err || vs.n != 1)
        return 0;
    *out = vs.v[0];
    return 1;
}

/* ---------- drawing ---------- */

static void draw_all(void) {
    hacku_fb_t *fb = hacku_display_fb();
    gfx_fill_rect(fb, 0, 0, HACKU_DISP_W, HACKU_DISP_H, CALC_BG);

    /* Expression + result lines, right aligned. */
    gfx_fill_rect(fb, 0, 0, HACKU_DISP_W, 104, CALC_DISP);
    const char *expr = s_expr[0] ? s_expr : "0";
    int ew = gfx_text_w(&hacku_font, expr, 1);
    gfx_text(fb, &hacku_font, HACKU_DISP_W - 8 - ew, 10,
             expr, CALC_DIM, -1, 1);
    int mw = gfx_text_w(&hacku_font, s_msg, 2);
    gfx_text(fb, &hacku_font, HACKU_DISP_W - 8 - mw, 44,
             s_msg, CALC_TEXT, -1, 2);
    gfx_hline(fb, 0, 103, HACKU_DISP_W, C_GRAY25);

    /* Keypad. */
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 4; c++) {
            int x = PAD_X0 + c * CELL_W, y = PAD_Y0 + r * CELL_H;
            int cur = (c == s_cur_col && r == s_cur_row);
            uint16_t bg = CALC_KEY;
            uint16_t fg = CALC_TEXT;
            const char *lb = KEYS[r][c];
            if (lb[0] == 'C') { bg = C_RED; }
            else if (lb[0] == '=') { bg = CALC_KEY_FN; }
            else if (lb[0] < '0' || lb[0] > '9') { fg = CALC_KEY_FN; }
            gfx_fill_rect(fb, x + 2, y + 2, CELL_W - 4, CELL_H - 4, bg);
            if (cur)
                gfx_rect(fb, x + 2, y + 2, CELL_W - 4, CELL_H - 4, CALC_CURSOR);
            int tw = gfx_text_w(&hacku_font, lb, 1);
            gfx_text(fb, &hacku_font,
                     x + (CELL_W - tw) / 2, y + (CELL_H - hacku_font.h) / 2,
                     lb, fg, -1, 1);
        }
    }
    hacku_display_flush_all();
}

void ui_calc_enter(void) {
    draw_all();
}

static void fmt_result(double v, char *out, size_t n) {
    snprintf(out, n, "%.10g", v);
}

/* Current number segment has a dot already? */
static int seg_has_dot(void) {
    int i = strlen(s_expr);
    while (i > 0) {
        char c = s_expr[--i];
        if (c == '.') return 1;
        if (c < '0' || c > '9') return 0;
    }
    return 0;
}

static void press_label(const char *lb, int *action) {
    *action = ACT_NONE;
    if (!strcmp(lb, "C")) {
        s_expr[0] = 0; s_msg[0] = 0; s_just_eval = 0;
    } else if (!strcmp(lb, "<")) {
        if (s_just_eval) { s_expr[0] = 0; s_msg[0] = 0; s_just_eval = 0; }
        else {
            size_t n = strlen(s_expr);
            if (n) s_expr[n - 1] = 0;
        }
    } else if (!strcmp(lb, "=")) {
        if (!strcmp(s_expr, "4+6")) {
            *action = ACT_UNLOCK; /* the disguise drops here */
            return;
        }
        double v = 0;
        if (calc_eval(s_expr, &v)) {
            fmt_result(v, s_msg, sizeof(s_msg));
            snprintf(s_expr, sizeof(s_expr), "%s", s_msg);
        } else {
            snprintf(s_msg, sizeof(s_msg), "Error");
        }
        s_just_eval = 1;
    } else {
        if (s_just_eval) {
            if ((lb[0] >= '0' && lb[0] <= '9') || lb[0] == '.' || lb[0] == '(') {
                s_expr[0] = 0; s_msg[0] = 0; /* fresh expression */
            } else {
                snprintf(s_msg, sizeof(s_msg), "%s", s_expr); /* chain on result */
            }
            s_just_eval = 0;
        }
        if (lb[0] == '.' && seg_has_dot())
            return;
        size_t n = strlen(s_expr);
        if (n < EXPR_MAX) {
            s_expr[n] = lb[0];
            s_expr[n + 1] = 0;
        }
    }
}

int ui_calc_key(hacku_key_t k) {
    int action = ACT_NONE;
    switch (k) {
        case KEY_UP:    s_cur_row = (s_cur_row + 4) % 5; break;
        case KEY_DOWN:  s_cur_row = (s_cur_row + 1) % 5; break;
        case KEY_LEFT:  s_cur_col = (s_cur_col + 3) % 4; break;
        case KEY_RIGHT: s_cur_col = (s_cur_col + 1) % 4; break;
        case KEY_OK:    press_label(KEYS[s_cur_row][s_cur_col], &action); break;
        case KEY_BACK:  press_label("<", &action); break;
        default: return ACT_NONE;
    }
    if (action != ACT_UNLOCK)
        draw_all();
    return action;
}
