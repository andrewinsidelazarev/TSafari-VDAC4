/* Формы (что это и как устроено — в form.h).
 *
 * ГДЕ ЧТО ХРАНИТСЯ.
 *   ctl[]   — элементы страницы по номерам: вид, где стоят, состояние (флажок, выбранный пункт);
 *   forms[] — формы: куда и каким способом отправлять;
 *   opts[]  — пункты всех списков подряд (у списка — номер первого и их число);
 *   strs    — строки (имена, значения, подписи) подряд через нулевой байт; у элемента —
 *             только смещение строки (0 — пустая строка);
 *   набранный текст поля — массив кодов Unicode (по букве на число), свой у каждого поля: так
 *   каретку двигать на букву просто (в UTF-8 буквы разной длины).
 *
 * ВИД (дизайн Codex, design/render_previews.py, кадр 08c):
 *   поле — белая плашка 44 точки высотой со скруглением 6 и рамкой 1 точка цвета line; в фокусе
 *   рамка 2 точки цвета link; текст 18 px с отступом 14;
 *   кнопка — тёмная (navy) плашка 44 точки со скруглением 8, подпись светлая по центру;
 *   флажок — квадрат 20×20 со скруглением 3: включён — заливка link и белая галочка;
 *   переключатель — кольцо muted, внутри белый круг, включён — точка link;
 *   список — как поле, справа стрелка (chevron), раскрытый — поверх страницы, строки 46 точек
 *   (как подсказки строки адреса). */
#include <stdio.h>
#include <string.h>
#include "form.h"
#include "page.h"
#include "font.h"
#include "charset.h"
#include "ui.h"
#include "ui_data.h"
#include "ui_design.h"
#include "ts.h"

/* Вид — из дизайна Codex. */
#define FIELD_H   TSA_UI_FORM_FIELD_HEIGHT      /* 44 */
#define FIELD_R   TSA_UI_FORM_FIELD_RADIUS      /* 6 */
#define BUTTON_R  TSA_UI_FORM_BUTTON_RADIUS     /* 8 */
#define PAD_X     14                            /* текст от левого края поля */
#define TEXT_Y    7                             /* верх клетки текста от верха поля */
#define FACE      FACE_SMALL                    /* 18 px: клетка 11×31, базовая линия 23 */
#define CW        11                            /* шаг буквы этого начертания */
#define LINE_H    31                            /* строка textarea */
#define SMALL_H   28                            /* «маленькое» поле (CSS height до 24): 14 px, клетка 8×24 */
#define BOX       24                            /* флажок и переключатель: клетка 24×24 */
#define MARGIN    2                             /* поля блока сверху и снизу */
#define GAP       6                             /* промежуток справа от элемента */
#define COL_FIELD    TSA_UI_COLOR_WHITE_RGB888
/* Рамка поля в покое: у Codex — line (0xCED7D6), но на бумаге 0xF7F3E7 это контраст ~1,2:1, и на
 * телевизоре поле почти не видно (пользователь, 04.11, главная Google). Середина между line и
 * muted — контраст ~2,4:1. */
#define COL_BORDER   0x90A0A4u
#define COL_FOCUS    TSA_UI_COLOR_LINK_RGB888
#define COL_INK      TSA_UI_COLOR_INK_RGB888
#define COL_MUTED    TSA_UI_COLOR_MUTED_RGB888
#define COL_SOFT     TSA_UI_COLOR_SOFT_RGB888
#define COL_BUTTON   TSA_UI_BUTTON_BG_RGB888
#define COL_LABEL    TSA_UI_COLOR_UI_TEXT_RGB888
#define COL_OFF      TSA_UI_COLOR_DISABLED_RGB888
#define COL_LINK     TSA_UI_COLOR_LINK_RGB888
#define COL_HILITE   TSA_UI_COLOR_LINK_BG_RGB888

#define TEXT_MAX  4000u                         /* букв в поле — не больше */

typedef struct {
    uint8_t type, checked, def_checked, disabled, readonly, placed;
    int16_t form;                       /* номер формы; −1 — элемент без формы */
    int x, y, w, h;                     /* где стоит на странице (placed = 1) */
    uint32_t name, value, ph, label;    /* строки в strs: имя, значение, подсказка, подпись кнопки */
    uint32_t *text;                     /* набранное: коды Unicode */
    unsigned len, cap, caret, view;     /* сколько букв, место, каретка, первая видимая буква (строка) */
    unsigned maxlen;
    int opt0, opt_n, sel, def_sel;      /* список: первый пункт в opts, их число, выбранный */
    uint16_t rows, cols;
    uint8_t face, pad, ty, cw;          /* начертание текста, отступ слева, верх текста, шаг буквы */
    uint8_t no_border;
    uint32_t fill, ink;                 /* фон и текст с сайта (CSS); 0 — по дизайну */
} ctl_t;
typedef struct { uint32_t action, target; uint8_t post, cs; int16_t part; } form_t;
typedef struct { uint32_t label, value; uint8_t disabled; } opt_t;

static ctl_t *ctl;
static unsigned ctl_n, ctl_cap;
static form_t *forms;
static unsigned form_n, form_cap;
static int form_now = -1;               /* форма, в которой сейчас разбор (−1 — вне формы) */
static opt_t *opts;
static unsigned opt_n, opt_cap;
static char *strs;
static size_t str_len, str_cap;
static int focus = -1;                  /* поле, в которое идёт набор; −1 — нет */
static int changed;                     /* каретка или список поменялись */
static void dd_close(void);

/* Массив растёт удвоением (как в page.c). 0 — нет памяти. */
static int grow(void **p, unsigned *cap, unsigned need, size_t item) {
    if (need <= *cap) return 1;
    unsigned c = *cap ? *cap * 2 : 32;
    while (c < need) c *= 2;
    void *q = ts_core->realloc(*p, c * item);
    if (!q) return 0;
    *p = q; *cap = c;
    return 1;
}

/* Строку — в strs; её смещение (пустая и NULL — 0: там всегда нулевой байт). */
static uint32_t str_add(const char *s) {
    if (!s || !s[0]) return 0;
    size_t n = strlen(s) + 1;
    if (str_len + n > str_cap) {
        size_t c = str_cap ? str_cap * 2 : 4096;
        while (c < str_len + n) c *= 2;
        char *q = ts_core->realloc(strs, c);
        if (!q) return 0;
        strs = q; str_cap = c;
    }
    memcpy(strs + str_len, s, n);
    str_len += n;
    return (uint32_t)(str_len - n);
}
static const char *str(uint32_t at) { return strs ? strs + at : ""; }

/* Текст поля из строки UTF-8 (значение по умолчанию, «вернуть как было»); буквы читает
 * utf8_next шрифта (font.h). */
static void set_text(ctl_t *c, const char *s) {
    const char *end = s + strlen(s);
    c->len = 0;
    while (s < end) {
        uint32_t ch = utf8_next(&s, end);
        if (ch == '\r') continue;
        if (c->type != FC_TEXTAREA && ch == '\n') ch = ' ';
        if (c->len >= TEXT_MAX || !grow((void **)&c->text, &c->cap, c->len + 1, sizeof(uint32_t))) break;
        c->text[c->len++] = ch;
    }
    c->caret = c->len;
    c->view = 0;
}

void form_clear(void) {
    for (unsigned i = 0; i < ctl_n; i++) if (ctl[i].text) ts_core->free(ctl[i].text);
    ctl_n = form_n = opt_n = 0;
    if (!strs && (strs = ts_core->realloc(NULL, 4096)) != NULL) str_cap = 4096;
    str_len = 0;
    if (strs) { strs[0] = 0; str_len = 1; }                 /* смещение 0 — пустая строка */
    form_now = -1;
    focus = -1;
    dd_close();
    changed = 1;
}

void form_open(const char *action, int post, const char *target, int part, int cs) {
    if (!grow((void **)&forms, &form_cap, form_n + 1, sizeof(form_t))) { form_now = -1; return; }
    form_t *f = &forms[form_n];
    f->action = str_add(action);
    f->target = str_add(target);
    f->post = (uint8_t)post;
    f->cs = (uint8_t)cs;
    f->part = (int16_t)part;
    form_now = (int)form_n++;
}

void form_close(void) { form_now = -1; }

/* --- размер элементов --- */

static int is_textlike(int t) { return t == FC_TEXT || t == FC_PASSWORD || t == FC_TEXTAREA; }

/* Сколько букв в строке UTF-8. */
static unsigned u8len(const char *s) {
    unsigned n = 0;
    if (s) for (; *s; s++) if (((uint8_t)*s & 0xC0) != 0x80) n++;
    return n;
}

/* Подпись кнопки: своя подпись, иначе значение, иначе — по виду. */
static const char *button_label(int type, const char *label, const char *value) {
    if (label && label[0]) return label;
    if (value && value[0]) return value;
    return type == FC_RESET ? "Reset" : type == FC_FILE ? "Choose file" : type == FC_BUTTON ? "Button" : "Submit";
}

/* Как в элементе стоит текст: обычно — 18 px с отступом 14 (дизайн Codex); сайт задал высоту
 * до 24 точек — 14 px и отступ 6 (поле в строке меню, как у vtrd.in); узкое поле (CSS width
 * меньше 120) — отступ 6, иначе в нём не видно букв. */
typedef struct { int face, pad, ty, cw, h; } metrics_t;
static metrics_t metrics(const form_new_t *n) {
    metrics_t m = {FACE, PAD_X, TEXT_Y, CW, FIELD_H};
    if (n->height > 0 && n->height <= 24 && n->type != FC_TEXTAREA && n->type != FC_CHECKBOX && n->type != FC_RADIO) {
        m.face = FACE_CAPTION;
        m.cw = font_faces[FACE_CAPTION].w;
        m.h = SMALL_H;
        m.ty = (SMALL_H - font_faces[FACE_CAPTION].h) / 2;
        m.pad = 6;
    }
    if (n->width > 0 && n->width < 120) m.pad = 6;
    return m;
}

void form_size(const form_new_t *n, int max_w, int *w, int *h, int *desc) {
    int bw, bh, bd;                                         /* сам элемент и сколько его ниже базовой линии */
    metrics_t m = metrics(n);
    int base = m.ty + font_faces[m.face].baseline;          /* базовая линия текста в элементе */
    if (max_w < 60) max_w = 60;
    switch (n->type) {
    case FC_CHECKBOX: case FC_RADIO:
        bw = BOX; bh = BOX; bd = 6;                         /* середина квадрата — на середине строчных букв */
        break;
    case FC_TEXTAREA: {
        int cols = n->cols > 0 ? n->cols : 20, rows = n->rows > 0 ? n->rows : 2;
        if (rows > 20) rows = 20;
        bw = n->width > 0 ? n->width : cols * CW + 2 * PAD_X;
        bh = rows * LINE_H + 2 * TEXT_Y;
        bd = bh - (TEXT_Y + font_faces[FACE].baseline);     /* первая строка — на базовой линии */
        break;
    }
    case FC_SELECT:
        bw = n->width > 0 ? n->width : (n->cols > 0 ? n->cols : 4) * m.cw + 2 * m.pad + 34;
        if (bw < 80) bw = 80;
        bh = m.h; bd = m.h - base;
        break;
    case FC_SUBMIT: case FC_RESET: case FC_BUTTON: case FC_IMAGE: case FC_FILE:
        bw = n->width > 0 ? n->width : (int)u8len(button_label(n->type, n->label, n->value)) * m.cw + 2 * m.pad + 4;   /* как у Codex: +32 */
        if (bw < 44) bw = 44;
        bh = m.h; bd = m.h - base;
        break;
    default:
        bw = n->width > 0 ? n->width : (n->size > 0 ? n->size : 20) * m.cw + 2 * m.pad;
        if (bw < 40) bw = 40;
        bh = m.h; bd = m.h - base;
        break;
    }
    if (bw > max_w - GAP) bw = max_w - GAP;
    *w = bw + GAP;
    *h = bh + 2 * MARGIN;
    *desc = bd + MARGIN;
}

int form_add(const form_new_t *n, int max_w) {
    if (!grow((void **)&ctl, &ctl_cap, ctl_n + 1, sizeof(ctl_t))) return -1;
    if (ctl_n >= FORM_ID_LIMIT) return -1;
    ctl_t *c = &ctl[ctl_n];
    memset(c, 0, sizeof(*c));
    c->type = (uint8_t)n->type;
    c->form = (int16_t)form_now;
    c->checked = c->def_checked = (uint8_t)(n->checked != 0);
    c->disabled = (uint8_t)(n->disabled != 0 || n->type == FC_FILE);
    c->readonly = (uint8_t)(n->readonly != 0);
    c->name = str_add(n->name);
    c->value = str_add(n->value);
    c->ph = str_add(n->placeholder);
    c->label = str_add(n->label);
    c->maxlen = n->maxlength > 0 ? (unsigned)n->maxlength : TEXT_MAX;
    c->rows = (uint16_t)(n->rows > 0 ? (n->rows > 20 ? 20 : n->rows) : 2);
    c->opt0 = (int)opt_n;
    c->sel = c->def_sel = -1;
    int w, h, d;
    form_size(n, max_w, &w, &h, &d);
    c->w = w - GAP;
    c->h = h - 2 * MARGIN;
    metrics_t m = metrics(n);
    c->face = (uint8_t)m.face; c->pad = (uint8_t)m.pad; c->ty = (uint8_t)m.ty; c->cw = (uint8_t)m.cw;
    c->fill = n->fill;
    c->ink = n->ink;
    c->no_border = (uint8_t)(n->no_border != 0);
    if (is_textlike(n->type)) set_text(c, n->value ? n->value : "");
    return (int)ctl_n++;
}

void form_option(int id, const char *label, const char *value, int selected, int disabled) {
    if (id < 0 || (unsigned)id >= ctl_n || ctl[id].type != FC_SELECT) return;
    if (!grow((void **)&opts, &opt_cap, opt_n + 1, sizeof(opt_t))) return;
    ctl_t *c = &ctl[id];
    opt_t *o = &opts[opt_n++];
    o->label = str_add(label);
    o->value = value ? (value[0] ? str_add(value) : 0) : o->label;
    o->disabled = (uint8_t)(disabled != 0);
    if (selected || (c->sel < 0 && !disabled)) c->sel = c->def_sel = c->opt_n;   /* без selected — первый */
    c->opt_n++;
}

void form_place(int id, int x, int y, uint32_t bg) {
    if (id < 0 || (unsigned)id >= ctl_n || ctl[id].type == FC_HIDDEN) return;
    ctl_t *c = &ctl[id];
    c->x = x;
    c->y = y + MARGIN;
    c->placed = 1;
    page_widget(c->x, c->y, c->w, c->h, id, bg, -1);
}

/* --- рисование в страницу --- */

/* Буквы (коды Unicode) → номера глифов шрифта; пароль — точки. */
static unsigned to_glyphs(const ctl_t *c, unsigned from, unsigned n, uint16_t *g) {
    for (unsigned i = 0; i < n; i++) {
        uint32_t ch = c->type == FC_PASSWORD ? 0x2022 : c->text[from + i];
        if (ch == '\n') ch = ' ';
        g[i] = (uint16_t)font_lookup(ch);
    }
    return n;
}

/* Строка UTF-8 → глифы (подписи кнопок и пунктов). */
static unsigned label_glyphs(const char *s, uint16_t *g, unsigned max) {
    return font_shape(s, (unsigned)strlen(s), g, max);
}

/* Плашка с рамкой: внешний скруглённый прямоугольник цвета рамки, внутри — заливка. */
static void framed(int x, int y, int w, int h, int r, int bw, uint32_t border, uint32_t fill, uint32_t bg) {
    page_paint_round(x, y, w, h, r, border, bg);
    page_paint_round(x + bw, y + bw, w - 2 * bw, h - 2 * bw, r - bw, fill, border);
}

/* Видимых букв в однострочном поле. */
static unsigned visible_cols(const ctl_t *c) {
    int v = (c->w - 2 * c->pad) / c->cw;
    return v > 1 ? (unsigned)v : 1u;
}

/* Визуальные строки textarea: длинная строка переносится по ширине поля (по буквам). Начала
 * строк — в starts (не больше max), возвращает число строк. */
static unsigned ta_split(const ctl_t *c, unsigned *starts, unsigned max) {
    unsigned per = visible_cols(c), n = 0, s = 0;
    for (;;) {
        if (n < max) starts[n] = s;
        n++;
        unsigned k = s;
        while (k < c->len && c->text[k] != '\n' && k - s < per) k++;
        if (k < c->len && c->text[k] == '\n') { s = k + 1; continue; }
        if (k >= c->len) break;
        s = k;
    }
    return n;
}
#define TA_LINES 1024
static unsigned ta_starts[TA_LINES];
/* Конец видимых букв строки i (без перевода строки). */
static unsigned ta_end(const ctl_t *c, unsigned i, unsigned n) {
    unsigned e = i + 1 < n ? ta_starts[i + 1] : c->len;
    if (e > ta_starts[i] && e <= c->len && c->text[e - 1] == '\n') e--;
    return e;
}
/* Строка, где стоит каретка. */
static unsigned ta_line_of(unsigned caret, unsigned n) {
    unsigned li = 0;
    for (unsigned i = 0; i < n && i < TA_LINES; i++) if (ta_starts[i] <= caret) li = i;
    return li;
}

void form_paint(int id, int x, int y, int w, int h, uint32_t bg) {
    if (id < 0 || (unsigned)id >= ctl_n) return;
    ctl_t *c = &ctl[id];
    uint16_t g[128];
    int ty = y + c->ty, tx = x + c->pad, r = h < FIELD_H ? 4 : FIELD_R;
    switch (c->type) {
    case FC_TEXT: case FC_PASSWORD: case FC_TEXTAREA: {
        int f = id == focus;
        uint32_t fill = c->fill ? c->fill : c->disabled ? COL_SOFT : COL_FIELD;
        if (f || !c->no_border) framed(x, y, w, h, r, f ? 2 : 1, f ? COL_FOCUS : COL_BORDER, fill, bg);
        else page_paint_round(x, y, w, h, 2, fill, bg);     /* сайт убрал рамку */
        uint32_t ink = c->ink ? c->ink : c->disabled ? COL_MUTED : COL_INK;
        if (!c->len && c->ph && !f) {                             /* подсказка в пустом поле */
            unsigned k = label_glyphs(str(c->ph), g, 128);
            /* у поля свой цвет текста (CSS) — подсказка его же цвета, приглушённая к фону (как у
             * браузеров: ::placeholder по умолчанию — цвет текста с прозрачностью 0,54); иначе — по
             * дизайну */
            uint32_t ph = COL_MUTED;
            if (c->ink) {
                ph = 0;
                for (int s = 0; s < 24; s += 8)
                    ph |= (((c->ink >> s & 255u) * 138u + (fill >> s & 255u) * 117u) / 255u) << s;
                if (!ph) ph = 1;
            }
            page_paint_text(tx, ty, c->face, g, k, ph, fill, 0, x + 3, x + w - 3);
            break;
        }
        if (c->type != FC_TEXTAREA) {
            unsigned k = c->len - c->view, vis = visible_cols(c) + 1;
            if (k > vis) k = vis;
            if (k > 128) k = 128;
            page_paint_text(tx, ty, c->face, g, to_glyphs(c, c->view, k, g), ink, fill, 0, x + 3, x + w - 3);
            break;
        }
        unsigned n = ta_split(c, ta_starts, TA_LINES);
        for (unsigned row = 0; row < c->rows && c->view + row < n && c->view + row < TA_LINES; row++) {
            unsigned li = c->view + row, s = ta_starts[li], e = ta_end(c, li, n), k = e - s;
            if (k > 128) k = 128;
            page_paint_text(tx, ty + (int)row * LINE_H, c->face, g, to_glyphs(c, s, k, g), ink, fill, 0, x + 3, x + w - 3);
        }
        break;
    }
    case FC_CHECKBOX: {
        uint32_t on = c->disabled ? COL_OFF : COL_LINK;
        int by = y + (h - BOX) / 2;
        if (c->checked) {
            page_paint_round(x + 2, by + 2, 20, 20, 3, on, bg);
            page_paint_mask(x, by, TSA_UI_ASSET_CHECK_WIDTH, TSA_UI_ASSET_CHECK_HEIGHT,
                            ui_assets + TSA_UI_ASSET_CHECK_OFFSET, TSA_UI_ASSET_CHECK_STRIDE, COL_FIELD, on);
        } else {
            framed(x + 2, by + 2, 20, 20, 3, 1, c->disabled ? COL_OFF : COL_MUTED, COL_FIELD, bg);
        }
        break;
    }
    case FC_RADIO: {
        int by = y + (h - BOX) / 2;
        uint32_t ring = c->disabled ? COL_OFF : COL_MUTED;
        framed(x + 2, by + 2, 20, 20, 10, 2, ring, COL_FIELD, bg);
        if (c->checked) page_paint_round(x + 7, by + 7, 10, 10, 5, c->disabled ? COL_OFF : COL_LINK, COL_FIELD);
        break;
    }
    case FC_SELECT: {
        uint32_t fill = c->fill ? c->fill : c->disabled ? COL_SOFT : COL_FIELD;
        if (c->no_border) page_paint_round(x, y, w, h, 2, fill, bg);
        else framed(x, y, w, h, r, 1, COL_BORDER, fill, bg);
        if (c->sel >= 0) {
            unsigned k = label_glyphs(str(opts[c->opt0 + c->sel].label), g, 128);
            page_paint_text(tx, ty, c->face, g, k, c->ink ? c->ink : c->disabled ? COL_MUTED : COL_INK, fill, 0, x + 3, x + w - 40);
        }
        page_paint_mask(x + w - 34, y + (h - TSA_UI_ASSET_CHEVRON_HEIGHT) / 2, TSA_UI_ASSET_CHEVRON_WIDTH,
                        TSA_UI_ASSET_CHEVRON_HEIGHT, ui_assets + TSA_UI_ASSET_CHEVRON_OFFSET, TSA_UI_ASSET_CHEVRON_STRIDE,
                        c->ink ? c->ink : COL_INK, fill);
        break;
    }
    case FC_SUBMIT: case FC_RESET: case FC_BUTTON: case FC_IMAGE: case FC_FILE: {
        uint32_t fill = c->fill ? c->fill : c->disabled ? COL_OFF : COL_BUTTON;
        page_paint_round(x, y, w, h, h < FIELD_H ? 4 : BUTTON_R, fill, bg);
        unsigned k = label_glyphs(button_label(c->type, str(c->label), str(c->value)), g, 128);
        int bx = x + (w - (int)k * c->cw) / 2;
        if (bx < x + 4) bx = x + 4;
        page_paint_text(bx, ty, c->face, g, k, c->ink ? c->ink : COL_LABEL, fill, 0, x + 3, x + w - 3);
        break;
    }
    default:
        break;
    }
}

/* Перерисовать элемент на странице (его строки в кольце видеочипа). */
static void redraw(int id) {
    if (id >= 0 && (unsigned)id < ctl_n && ctl[id].placed) page_redraw(ctl[id].y, ctl[id].h);
}

/* --- фокус и набор текста --- */

static void set_focus(int id) {
    if (id == focus) return;
    int old = focus;
    focus = id;
    redraw(old);
    redraw(id);
    changed = 1;
}

/* Каретку держать в видимой части поля: сдвинуть окно (view), если она вышла за край. */
static void keep_visible(ctl_t *c) {
    if (c->type == FC_TEXTAREA) {
        unsigned n = ta_split(c, ta_starts, TA_LINES), li = ta_line_of(c->caret, n);
        if (li < c->view) c->view = li;
        if (li >= c->view + c->rows) c->view = li - c->rows + 1;
        return;
    }
    unsigned vis = visible_cols(c);
    if (c->caret < c->view) c->view = c->caret;
    if (c->caret > c->view + vis) c->view = c->caret - vis;
}

/* Вставить букву перед кареткой. */
static void insert(ctl_t *c, uint32_t ch) {
    if (c->len >= c->maxlen || c->len >= TEXT_MAX) return;
    if (!grow((void **)&c->text, &c->cap, c->len + 1, sizeof(uint32_t))) return;
    memmove(c->text + c->caret + 1, c->text + c->caret, (c->len - c->caret) * sizeof(uint32_t));
    c->text[c->caret++] = ch;
    c->len++;
}
/* Убрать букву номер at. */
static void erase(ctl_t *c, unsigned at) {
    if (at >= c->len) return;
    memmove(c->text + at, c->text + at + 1, (c->len - at - 1) * sizeof(uint32_t));
    c->len--;
}

/* Каретка — в точку щелчка (экран x, y; верх поля на экране — sy). */
static void caret_at(ctl_t *c, int x, int y, int sy) {
    int col = (x - (c->x + c->pad) + c->cw / 2) / c->cw;
    if (col < 0) col = 0;
    if (c->type != FC_TEXTAREA) {
        unsigned p = c->view + (unsigned)col;
        c->caret = p > c->len ? c->len : p;
        return;
    }
    unsigned n = ta_split(c, ta_starts, TA_LINES);
    int row = (y - (sy + c->ty)) / LINE_H;
    if (row < 0) row = 0;
    unsigned li = c->view + (unsigned)row;
    if (li >= n) li = n - 1;
    if (li >= TA_LINES) li = TA_LINES - 1;
    unsigned s = ta_starts[li], e = ta_end(c, li, n);
    c->caret = s + (unsigned)col > e ? e : s + (unsigned)col;
}

/* --- отправка --- */

static char *data;                       /* «имя=значение&…» собирается здесь */
static size_t data_len, data_cap;

static int put(const char *s, size_t n) {
    if (data_len + n + 1 > data_cap) {
        size_t c = data_cap ? data_cap * 2 : 1024;
        while (c < data_len + n + 1) c *= 2;
        char *q = ts_core->realloc(data, c);
        if (!q) return 0;
        data = q; data_cap = c;
    }
    memcpy(data + data_len, s, n);
    data_len += n;
    data[data_len] = 0;
    return 1;
}

/* Байт — в «процентной» записи адресов (application/x-www-form-urlencoded): буквы, цифры и
 * «*-._» как есть, пробел — «+», остальное — «%» и две шестнадцатеричные цифры. */
static void put_byte(uint8_t b) {
    static const char hex[] = "0123456789ABCDEF";
    if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '*' || b == '-' ||
        b == '.' || b == '_') {
        char ch = (char)b;
        put(&ch, 1);
    } else if (b == ' ') {
        put("+", 1);
    } else {
        char e[3] = {'%', hex[b >> 4], hex[b & 15]};
        put(e, 3);
    }
}

/* Букву — в байты кодировки страницы: UTF-8 — до 4 байт; однобайтовая — её байт, а буквы,
 * которой в ней нет, — «&#код;», как делают браузеры. Перевод строки — CR LF. */
static void put_char(uint32_t ch, int cs) {
    if (ch == '\n') { put_byte('\r'); put_byte('\n'); return; }
    if (cs == CS_UTF8) {
        uint8_t u[4];
        int n;
        if (ch < 0x80) { u[0] = (uint8_t)ch; n = 1; }
        else if (ch < 0x800) { u[0] = (uint8_t)(0xC0 | ch >> 6); u[1] = (uint8_t)(0x80 | (ch & 63)); n = 2; }
        else if (ch < 0x10000) { u[0] = (uint8_t)(0xE0 | ch >> 12); u[1] = (uint8_t)(0x80 | ((ch >> 6) & 63)); u[2] = (uint8_t)(0x80 | (ch & 63)); n = 3; }
        else { u[0] = (uint8_t)(0xF0 | ch >> 18); u[1] = (uint8_t)(0x80 | ((ch >> 12) & 63)); u[2] = (uint8_t)(0x80 | ((ch >> 6) & 63)); u[3] = (uint8_t)(0x80 | (ch & 63)); n = 4; }
        for (int i = 0; i < n; i++) put_byte(u[i]);
        return;
    }
    int b = charset_from_unicode(cs, ch);
    if (b >= 0) { put_byte((uint8_t)b); return; }
    char e[16];
    int n = snprintf(e, sizeof(e), "&#%u;", (unsigned)ch);
    for (int i = 0; i < n; i++) put_byte((uint8_t)e[i]);
}
static void put_utf8(const char *s, int cs) {
    const char *end = s + strlen(s);
    while (s < end) put_char(utf8_next(&s, end), cs);
}

/* Пара «имя=значение» (значение — строкой UTF-8 или буквами поля). */
static void pair(const char *name, const char *value, const ctl_t *field, int cs) {
    if (data_len) put("&", 1);
    put_utf8(name, cs);
    put("=", 1);
    if (field) for (unsigned i = 0; i < field->len; i++) put_char(field->text[i], cs);
    else put_utf8(value, cs);
}

/* Собрать отправку формы f; submitter — нажатая кнопка (её имя и значение тоже уходят; −1 —
 * без кнопки). 0 — нечего отправлять. */
static int build(int f, int submitter, form_submit_t *out) {
    if (f < 0 || (unsigned)f >= form_n) return 0;
    const form_t *F = &forms[f];
    data_len = 0;
    if (data) data[0] = 0;
    for (unsigned i = 0; i < ctl_n; i++) {
        const ctl_t *c = &ctl[i];
        if (c->form != f || c->disabled || !c->name) continue;
        const char *nm = str(c->name);
        switch (c->type) {
        case FC_TEXT: case FC_PASSWORD: case FC_TEXTAREA: pair(nm, NULL, c, F->cs); break;
        case FC_HIDDEN: pair(nm, str(c->value), NULL, F->cs); break;
        case FC_CHECKBOX: case FC_RADIO: if (c->checked) pair(nm, c->value ? str(c->value) : "on", NULL, F->cs); break;
        case FC_SELECT:
            if (c->sel >= 0) pair(nm, str(opts[c->opt0 + c->sel].value), NULL, F->cs);
            break;
        case FC_SUBMIT: if ((int)i == submitter) pair(nm, str(c->value), NULL, F->cs); break;
        case FC_IMAGE:
            if ((int)i == submitter) {
                char k[96];
                snprintf(k, sizeof(k), "%.80s.x", nm); pair(k, "0", NULL, F->cs);
                snprintf(k, sizeof(k), "%.80s.y", nm); pair(k, "0", NULL, F->cs);
            }
            break;
        default: break;
        }
    }
    snprintf(out->target, sizeof(out->target), "%s", str(F->target));
    out->part = F->part;
    const char *action = str(F->action);
    if (F->post) {
        snprintf(out->url, sizeof(out->url), "%s", action);
        out->body = ts_alloc(data_len + 1);
        if (!out->body) return 0;
        memcpy(out->body, data ? data : "", data_len + 1);
        out->body_len = data_len;
        return 1;
    }
    /* GET: прежний запрос и фрагмент адреса action отбрасываются, пары идут после «?». Адрес
     * длиннее URL_MAX прошивка не возьмёт — тогда пары, что не влезли, отбрасываются целиком. */
    size_t base = strcspn(action, "?#");
    if (base > sizeof(out->url) - 2) base = sizeof(out->url) - 2;
    memcpy(out->url, action, base);
    out->url[base] = '?';
    size_t room = sizeof(out->url) - base - 2, k = data_len;
    if (k > room) {
        k = room;
        while (k && data[k] != '&') k--;
    }
    memcpy(out->url + base + 1, data ? data : "", k);
    out->url[base + 1 + k] = 0;
    out->body = NULL;
    out->body_len = 0;
    return 1;
}

/* «Вернуть как было»: значения по умолчанию у всех элементов формы f. */
static void reset_form(int f) {
    for (unsigned i = 0; i < ctl_n; i++) {
        ctl_t *c = &ctl[i];
        if (c->form != f) continue;
        if (is_textlike(c->type)) set_text(c, str(c->value));
        c->checked = c->def_checked;
        c->sel = c->def_sel;
        redraw((int)i);
    }
}

/* Первая кнопка «отправить» формы f (её значение уходит при Enter в поле) или −1. */
static int default_button(int f) {
    for (unsigned i = 0; i < ctl_n; i++)
        if (ctl[i].form == f && (ctl[i].type == FC_SUBMIT || ctl[i].type == FC_IMAGE) && !ctl[i].disabled) return (int)i;
    return -1;
}

/* --- раскрытый список (поверх страницы, рисует видеочип) ---
 * Подписи пунктов рисуются в маски L4 (как текст строки адреса в ui.c) в RAM_G с адреса
 * DD_RAMG: по маске 34 строки на пункт; видно до DD_ROWS пунктов, остальное — колесом. */
#define DD_RAMG   0x100000u
#define DD_ROWS   8
#define DD_STEP   46                     /* шаг строк списка (как у подсказок адреса) */
#define DD_COLS   48                     /* букв в подписи пункта — не больше */
static int dd = -1;                      /* номер раскрытого списка; −1 — закрыт */
static int dd_x, dd_y, dd_w, dd_h, dd_rows, dd_top, dd_hover, dd_cols, dd_drawn = -1;
static uint8_t *dd_buf;

static void dd_close(void) { if (dd >= 0) changed = 1; dd = -1; }

/* Маски подписей видимых пунктов [dd_top, dd_top + dd_rows) — в RAM_G. */
static void dd_render(void) {
    const ctl_t *c = &ctl[dd];
    size_t stride = (size_t)dd_cols * (FONT_CELL_W / 2), bytes = stride * FONT_CELL_H * (size_t)dd_rows;
    if (!dd_buf && !(dd_buf = ts_alloc(DD_ROWS * (size_t)DD_COLS * (FONT_CELL_W / 2) * FONT_CELL_H))) return;
    memset(dd_buf, 0, bytes);
    uint16_t g[DD_COLS];
    for (int r = 0; r < dd_rows; r++) {
        const char *s = str(opts[c->opt0 + dd_top + r].label);
        unsigned n = font_shape(s, (unsigned)strlen(s), g, (unsigned)dd_cols);
        uint8_t *dst = dd_buf + (size_t)r * FONT_CELL_H * stride;
        for (unsigned k = 0; k < n; k++) {
            const uint8_t *cell = font_glyph(g[k]);
            for (int row = 0; row < FONT_CELL_H; row++)
                memcpy(dst + row * stride + k * (FONT_CELL_W / 2), cell + row * (FONT_CELL_W / 2), FONT_CELL_W / 2);
        }
    }
    ft_put(DD_RAMG, dd_buf, bytes);
    dd_drawn = dd_top;
}

/* Раскрыть список id (верх поля на экране — sy): под полем, а если не влезает — над ним;
 * выбранный пункт — посередине видимых. */
static void dd_open(int id, int sy) {
    ctl_t *c = &ctl[id];
    if (!c->opt_n) return;
    int longest = 0;
    for (int i = 0; i < c->opt_n; i++) {
        int k = (int)u8len(str(opts[c->opt0 + i].label));
        if (k > longest) longest = k;
    }
    dd_cols = longest > DD_COLS ? DD_COLS : longest < 2 ? 2 : longest;
    dd_cols = (dd_cols + 1) & ~1;                          /* чётное: строка маски — целые слова */
    dd_rows = c->opt_n < DD_ROWS ? c->opt_n : DD_ROWS;
    dd_w = dd_cols * FONT_CELL_W + 2 * PAD_X + 34;
    if (dd_w < c->w) dd_w = c->w;
    if (dd_w > TS_W - 16) dd_w = TS_W - 16;
    dd_h = dd_rows * DD_STEP + 6;
    dd_x = c->x;
    if (dd_x + dd_w > TS_W - 8) dd_x = TS_W - 8 - dd_w;
    if (dd_x < 8) dd_x = 8;
    dd_y = sy + c->h + 4;
    if (dd_y + dd_h > TS_H - 8) dd_y = sy - 4 - dd_h;
    if (dd_y < 8) dd_y = 8;
    dd_top = c->sel - dd_rows / 2;
    if (dd_top > c->opt_n - dd_rows) dd_top = c->opt_n - dd_rows;
    if (dd_top < 0) dd_top = 0;
    dd_hover = c->sel;
    dd = id;
    dd_render();
    changed = 1;
}

/* Листать список: пункт k должен быть виден. */
static void dd_show(int k) {
    const ctl_t *c = &ctl[dd];
    if (k < dd_top) dd_top = k;
    if (k >= dd_top + dd_rows) dd_top = k - dd_rows + 1;
    if (dd_top > c->opt_n - dd_rows) dd_top = c->opt_n - dd_rows;
    if (dd_top < 0) dd_top = 0;
    if (dd_top != dd_drawn) dd_render();
    changed = 1;
}

/* Выбрать пункт k и закрыть список. */
static void dd_choose(int k) {
    ctl_t *c = &ctl[dd];
    int id = dd;
    dd = -1;
    changed = 1;
    if (k >= 0 && k < c->opt_n && !opts[c->opt0 + k].disabled && k != c->sel) {
        c->sel = k;
        redraw(id);
    }
}

int form_mouse(int x, int y, int pressed, int wheel) {
    if (dd < 0) return 0;
    int in = x >= dd_x && x < dd_x + dd_w && y >= dd_y && y < dd_y + dd_h;
    int row = in ? (y - dd_y - 4) / DD_STEP : -1;
    int k = row >= 0 && row < dd_rows ? dd_top + row : -1;
    if (k != dd_hover && k >= 0) { dd_hover = k; changed = 1; }
    if (wheel) {
        int top = dd_top - wheel;
        const ctl_t *c = &ctl[dd];
        if (top > c->opt_n - dd_rows) top = c->opt_n - dd_rows;
        if (top < 0) top = 0;
        if (top != dd_top) { dd_top = top; dd_render(); changed = 1; }
    }
    if (pressed) {
        if (k >= 0) dd_choose(k);
        else dd_close();                                    /* щелчок мимо — закрыть, не выбирая */
    }
    return 1;
}

/* --- щелчки и клавиши --- */

static int control_at(int x, int y) {
    int id = page_widget_at(x, y);
    return id >= 0 && id < FORM_ID_LIMIT && (unsigned)id < ctl_n ? id : -1;
}

int form_over(int x, int y) {
    int id = control_at(x, y);
    return id >= 0 && !ctl[id].disabled;
}

int form_click(int x, int y, form_submit_t *out) {
    int id = control_at(x, y);
    if (id < 0 || ctl[id].disabled) { set_focus(-1); return id < 0 ? FORM_NONE : FORM_USED; }
    ctl_t *c = &ctl[id];
    int sy = c->y - page_top();
    switch (c->type) {
    case FC_TEXT: case FC_PASSWORD: case FC_TEXTAREA:
        caret_at(c, x, y, sy);
        set_focus(id);
        changed = 1;
        return FORM_USED;
    case FC_CHECKBOX:
        set_focus(-1);
        c->checked ^= 1;
        redraw(id);
        return FORM_USED;
    case FC_RADIO:
        set_focus(-1);
        if (!c->checked) {                                  /* в группе (та же форма и имя) — один */
            for (unsigned i = 0; i < ctl_n; i++)
                if (ctl[i].type == FC_RADIO && ctl[i].checked && ctl[i].form == c->form && !strcmp(str(ctl[i].name), str(c->name))) {
                    ctl[i].checked = 0;
                    redraw((int)i);
                }
            c->checked = 1;
            redraw(id);
        }
        return FORM_USED;
    case FC_SELECT:
        set_focus(-1);
        dd_open(id, sy);
        return FORM_USED;
    case FC_SUBMIT: case FC_IMAGE:
        set_focus(-1);
        return build(c->form, id, out) ? FORM_SUBMIT : FORM_USED;
    case FC_RESET:
        set_focus(-1);
        reset_form(c->form);
        return FORM_USED;
    default:
        set_focus(-1);
        return FORM_USED;
    }
}

int form_key(const key_event_t *ev, form_submit_t *out) {
    if (dd >= 0) {                                          /* раскрытый список */
        const ctl_t *c = &ctl[dd];
        int k = dd_hover < 0 ? c->sel : dd_hover;
        switch (ev->key) {
        case KEY_UP: if (k > 0) { dd_hover = k - 1; dd_show(dd_hover); } return FORM_USED;
        case KEY_DOWN: if (k + 1 < c->opt_n) { dd_hover = k + 1; dd_show(dd_hover); } return FORM_USED;
        case KEY_PGUP: dd_hover = k - dd_rows < 0 ? 0 : k - dd_rows; dd_show(dd_hover); return FORM_USED;
        case KEY_PGDN: dd_hover = k + dd_rows >= c->opt_n ? c->opt_n - 1 : k + dd_rows; dd_show(dd_hover); return FORM_USED;
        case KEY_HOME: dd_hover = 0; dd_show(0); return FORM_USED;
        case KEY_END: dd_hover = c->opt_n - 1; dd_show(dd_hover); return FORM_USED;
        case KEY_ENTER: case KEY_TAB: dd_choose(k); return FORM_USED;
        case KEY_ESC: dd_close(); return FORM_USED;
        case KEY_F5: case KEY_F12: return FORM_NONE;
        default: return FORM_USED;
        }
    }
    if (focus < 0) return FORM_NONE;
    ctl_t *c = &ctl[focus];
    int edit = 0, ta = c->type == FC_TEXTAREA;          /* edit: текст поменялся (иначе — только каретка) */
    unsigned old_view = c->view;
    switch (ev->key) {
    case KEY_CHAR:
        if (ev->ch < 32 || c->readonly) return FORM_USED;
        insert(c, ev->ch);
        edit = 1;
        break;
    case KEY_ENTER:
        if (ta) { if (!c->readonly) { insert(c, '\n'); edit = 1; } break; }
        if (c->form >= 0 && build(c->form, default_button(c->form), out)) { set_focus(-1); return FORM_SUBMIT; }
        return FORM_USED;
    case KEY_BACKSPACE:
        if (c->caret && !c->readonly) { erase(c, c->caret - 1); c->caret--; edit = 1; }
        break;
    case KEY_DELETE:
        if (c->caret < c->len && !c->readonly) { erase(c, c->caret); edit = 1; }
        break;
    case KEY_LEFT: if (c->caret) c->caret--; break;
    case KEY_RIGHT: if (c->caret < c->len) c->caret++; break;
    case KEY_HOME: case KEY_END:
        if (ta) {                                           /* начало и конец строки */
            unsigned n = ta_split(c, ta_starts, TA_LINES), li = ta_line_of(c->caret, n);
            c->caret = ev->key == KEY_HOME ? ta_starts[li] : ta_end(c, li, n);
        } else {
            c->caret = ev->key == KEY_HOME ? 0 : c->len;
        }
        break;
    case KEY_UP: case KEY_DOWN: {
        if (!ta) return FORM_USED;
        unsigned n = ta_split(c, ta_starts, TA_LINES), li = ta_line_of(c->caret, n), col = c->caret - ta_starts[li];
        if (ev->key == KEY_UP && li == 0) return FORM_USED;
        if (ev->key == KEY_DOWN && li + 1 >= n) return FORM_USED;
        li = ev->key == KEY_UP ? li - 1 : li + 1;
        unsigned e = ta_end(c, li, n);
        c->caret = ta_starts[li] + col > e ? e : ta_starts[li] + col;
        break;
    }
    case KEY_TAB: {                                         /* следующее (Shift — прежнее) поле */
        int step = ev->mods & MOD_SHIFT ? -1 : 1;
        for (int i = focus + step; i >= 0 && (unsigned)i < ctl_n; i += step)
            if (is_textlike(ctl[i].type) && ctl[i].placed && !ctl[i].disabled) { set_focus(i); return FORM_USED; }
        return FORM_USED;
    }
    case KEY_ESC:
        set_focus(-1);
        return FORM_USED;
    case KEY_PGUP: case KEY_PGDN: case KEY_F5: case KEY_F12: case KEY_WIN:
        return FORM_NONE;
    default:
        return FORM_USED;
    }
    keep_visible(c);                                        /* текст или окно поля сдвинулись — перерисовать */
    if (edit || c->view != old_view) redraw(focus);
    changed = 1;                                            /* каретка — в дисплей-листе */
    return FORM_USED;
}

/* --- поверх страницы: каретка и раскрытый список --- */

/* Скруглённый прямоугольник видеочипа (как rrect в ui.c): RECTS с толщиной линии r рисует
 * прямоугольник, «раздутый» на r, поэтому углы задаются на r внутрь. */
static void rrect(ft_dl_t *d, int x, int y, int w, int h, int r, uint32_t rgb) {
    dl_put(d, dl_color_rgb(rgb));
    dl_put(d, dl_line_width((unsigned)r * 16));
    dl_put(d, dl_vertex((x + r) * 16, (y + r) * 16));
    dl_put(d, dl_vertex((x + w - r) * 16, (y + h - r) * 16));
}

void form_dl(ft_dl_t *d, int top) {
    if (focus >= 0 && ctl[focus].placed) {                  /* каретка: черта 2×24 цвета текста */
        const ctl_t *c = &ctl[focus];
        int cx, cy;
        if (c->type == FC_TEXTAREA) {
            unsigned n = ta_split(c, ta_starts, TA_LINES), li = ta_line_of(c->caret, n);
            cx = c->x + c->pad + (int)(c->caret - ta_starts[li]) * c->cw;
            cy = li < c->view || li >= c->view + c->rows ? -100 : c->y - top + c->ty + (int)(li - c->view) * LINE_H + 4;
        } else {
            cx = c->x + c->pad + (int)(c->caret - c->view) * c->cw;
            cy = c->y - top + c->ty + 4;
        }
        int ch = font_faces[c->face].h - 7;                 /* высота каретки: 24 у 18 px, 17 у 14 px */
        if (cy > -ch && cy < TS_H) {
            dl_put(d, dl_begin(FT_RECTS));
            dl_put(d, dl_color_a(255));
            rrect(d, cx - 1, cy, 2, ch, 1, c->ink ? c->ink : COL_INK);
            dl_put(d, dl_end());
            /* Метка языка ввода (EN или RU) справа от каретки — как в строке адреса (дизайн Codex,
             * п. 15): видна, когда каретка в конце текста, и 1 секунду после смены раскладки;
             * не влезает в поле — не видна. Цвет — как у рамки поля в фокусе. */
            int fx = cx + TSA_UI_INPUT_LANG_OFFSET_FROM_CARET_X;
            int fy = c->h < FIELD_H ? c->y - top + (c->h - TSA_UI_INPUT_LANG_HEIGHT) / 2 : cy + (ch - TSA_UI_INPUT_LANG_HEIGHT) / 2;
            if ((c->caret == c->len || ui_lang_flash()) && fx + TSA_UI_INPUT_LANG_WIDTH <= c->x + c->w - 3) {
                dl_put(d, dl_begin(FT_BITMAPS));
                dl_put(d, dl_color_rgb(COL_FOCUS));
                dl_bitmap(d, input_russian() ? TSA_UI_ASSET_INPUT_RU_RAM_G : TSA_UI_ASSET_INPUT_EN_RAM_G,
                          FT_L4, TSA_UI_ASSET_INPUT_EN_STRIDE, TSA_UI_INPUT_LANG_WIDTH, TSA_UI_INPUT_LANG_HEIGHT);
                dl_put(d, dl_vertex(fx * 16, fy * 16));
                dl_put(d, dl_end());
            }
        }
    }
    if (dd < 0) return;
    const ctl_t *c = &ctl[dd];
    dl_put(d, dl_begin(FT_RECTS));
    dl_put(d, dl_color_a(255));
    rrect(d, dd_x, dd_y, dd_w, dd_h, 8, COL_BORDER);
    rrect(d, dd_x + 1, dd_y + 1, dd_w - 2, dd_h - 2, 7, COL_FIELD);
    if (dd_hover >= dd_top && dd_hover < dd_top + dd_rows)
        rrect(d, dd_x + 4, dd_y + 4 + (dd_hover - dd_top) * DD_STEP, dd_w - 8, 44, 6, COL_HILITE);
    if (c->opt_n > dd_rows) {                               /* полоска: какая часть списка видна */
        int track = dd_h - 16, len = track * dd_rows / c->opt_n, at = track * dd_top / c->opt_n;
        if (len < 12) len = 12;
        rrect(d, dd_x + dd_w - 6, dd_y + 8 + at, 3, len, 1, COL_BORDER);
    }
    dl_put(d, dl_end());
    dl_put(d, dl_begin(FT_BITMAPS));
    size_t stride = (size_t)dd_cols * (FONT_CELL_W / 2);
    for (int r = 0; r < dd_rows; r++) {
        int k = dd_top + r, ry = dd_y + 4 + r * DD_STEP;
        dl_put(d, dl_color_rgb(opts[c->opt0 + k].disabled ? COL_MUTED : COL_INK));
        dl_bitmap(d, DD_RAMG + (uint32_t)(r * FONT_CELL_H * stride), FT_L4, (unsigned)stride,
                  (unsigned)dd_cols * FONT_CELL_W, FONT_CELL_H);
        dl_put(d, dl_vertex((dd_x + PAD_X) * 16, (ry + 5) * 16));
        if (k == c->sel) {                                  /* выбранный — галочка справа */
            dl_put(d, dl_color_rgb(COL_LINK));
            dl_bitmap(d, TSA_UI_ASSET_CHECK_RAM_G, FT_L4, TSA_UI_ASSET_CHECK_STRIDE, TSA_UI_ASSET_CHECK_WIDTH,
                      TSA_UI_ASSET_CHECK_HEIGHT);
            dl_put(d, dl_vertex((dd_x + dd_w - 34) * 16, (ry + 10) * 16));
        }
    }
    dl_put(d, dl_end());
}

/* --- через переразметку: элементы той же страницы идут в том же порядке, номер — тот же --- */
typedef struct { uint8_t type, checked; int sel; uint32_t *text; unsigned len, caret, view; } kept_t;
static kept_t *kept;
static unsigned kept_n, kept_cap;
static int kept_focus = -1;

void form_save(void) {
    for (unsigned i = 0; i < kept_n; i++) if (kept[i].text) ts_core->free(kept[i].text);
    kept_n = 0;
    if (!grow((void **)&kept, &kept_cap, ctl_n ? ctl_n : 1, sizeof(kept_t))) return;
    for (unsigned i = 0; i < ctl_n; i++) {
        const ctl_t *c = &ctl[i];
        kept_t *k = &kept[kept_n++];
        k->type = c->type; k->checked = c->checked; k->sel = c->sel;
        k->len = c->len; k->caret = c->caret; k->view = c->view;
        k->text = NULL;
        if (c->len && (k->text = ts_alloc(c->len * sizeof(uint32_t)))) memcpy(k->text, c->text, c->len * sizeof(uint32_t));
        else k->len = 0;
    }
    kept_focus = focus;
}

void form_restore(void) {
    int same = kept_n == ctl_n;
    for (unsigned i = 0; same && i < ctl_n; i++) same = kept[i].type == ctl[i].type;
    for (unsigned i = 0; i < kept_n; i++) {
        kept_t *k = &kept[i];
        if (same) {
            ctl_t *c = &ctl[i];
            c->checked = k->checked;
            c->sel = k->sel;
            if (c->text) ts_core->free(c->text);
            c->text = k->text; c->cap = c->len = k->len;   /* набранное — элементу */
            c->caret = k->caret > c->len ? c->len : k->caret;
            c->view = k->view;
            k->text = NULL;
        } else if (k->text) {
            ts_core->free(k->text);
        }
    }
    if (same && kept_focus >= 0 && (unsigned)kept_focus < ctl_n) focus = kept_focus;
    kept_n = 0;
    changed = 1;
}

#ifdef TSAFARI_HOST
/* Стенд на ПК (tests/host.c): где элемент на странице — чтобы по нему щёлкнуть. */
int form_host_rect(int id, int *x, int *y, int *w, int *h) {
    if (id < 0 || (unsigned)id >= ctl_n || !ctl[id].placed) return 0;
    *x = ctl[id].x; *y = ctl[id].y; *w = ctl[id].w; *h = ctl[id].h;
    return 1;
}
#endif

int form_changed(void) {
    /* раскладка сменилась или кончилась секунда показа метки — у поля в фокусе метка другая */
    static int last_ru = -1, last_flash;
    if (focus >= 0 && (input_russian() != last_ru || ui_lang_flash() != last_flash)) changed = 1;
    last_ru = input_russian();
    last_flash = ui_lang_flash();
    int c = changed;
    changed = 0;
    return c;
}
