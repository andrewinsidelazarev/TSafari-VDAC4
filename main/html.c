/* Разбор HTML (что это — в html.h).
 *
 * ПОТОКОМ, БЕЗ ДЕРЕВА. Большие браузеры сначала строят из страницы «дерево» — все теги со
 * вложенностью, — а потом рисуют его. Это требует много памяти. Мы проще: читаем страницу
 * один раз слева направо и сразу отдаём текст в раскладку (doc.c). Что сейчас открыто —
 * ссылка, заголовок, список, — помним в нескольких переменных (link_id, heading, depth…), и
 * от них зависит стиль текста.
 *
 * ГЛАВНЫЙ ЦИКЛ (html_render): идём по байтам страницы.
 *   - текст до следующего «<» → text(): перевести в UTF-8, раскрыть «&amp;»-сущности,
 *     схлопнуть пробелы (в HTML любые пробелы и переводы строк подряд — один пробел; только
 *     внутри <pre> текст выводится как есть);
 *   - «<!-- … -->» — комментарий, пропустить; «<!DOCTYPE …>», «<?xml …>» — тоже;
 *   - «<имя атрибуты>» или «</имя>» → tag(): что делать с этим тегом.
 *
 * Блоки (абзац, заголовок, список…) начинаются с новой строки и дают промежуток. Ссылки
 * получают номера: адрес кладётся в таблицу, номер — в стиль текста; при щелчке page_link_at
 * вернёт номер, а html_link — адрес.
 * Содержимое <script>, <style>, <svg> и подобных пропускается целиком: это не текст для
 * человека. <noscript> показывается — у нас JS не выполняется; <noframes> — нет: фреймы мы
 * понимаем. Поля форм (<input>, <select>, <textarea>, <button>) — настоящие элементы (form.c):
 * в них печатают, флажки щёлкают, форма отправляется GET или POST.
 *
 * ФРЕЙМЫ (что это — в html.h). html_frames быстро просматривает страницу и, если это набор
 * фреймов, выписывает их имена и адреса; страницы фреймов потом разбираются по очереди
 * (html_part) в одну составную страницу. У каждой ссылки запоминается её target и номер
 * фрейма, в котором она стоит, — по ним app.c решает, куда её открыть. */
#include <stdio.h>
#include <string.h>
#include "html.h"
#include "doc.h"
#include "page.h"
#include "font.h"
#include "charset.h"
#include "url.h"
#include "form.h"
#include "image.h"
#include "ui_design.h"
#include "ts.h"
#include "css.h"
#ifdef TSAFARI_HOST
#include <stdlib.h>                      /* стенд ПК: getenv (TSAFARI_HIDELOG — что скрыл CSS) */
#endif

/* Цвета страницы — из дизайна Codex. */
#define COL_TEXT TSA_UI_COLOR_INK_RGB888
#define COL_DIM  TSA_UI_COLOR_MUTED_RGB888
#define COL_LINK TSA_UI_COLOR_LINK_RGB888
#define COL_RULE TSA_UI_COLOR_LINE_RGB888
#define COL_BG   TSA_UI_COLOR_PAPER_RGB888
#define COL_SOFT TSA_UI_COLOR_SOFT_RGB888          /* заливка цитаты, заголовка таблицы */
#define COL_CODE TSA_UI_COLOR_CODE_RGB888          /* заливка блока кода */
#define COL_LINE TSA_UI_COLOR_LINE_RGB888          /* линии таблицы */
#define COL_VISITED TSA_UI_COLOR_VISITED_RGB888    /* посещённая ссылка */

/* --- ссылки страницы ---
 * Адреса лежат подряд в одном большом массиве pool: адрес, нулевой байт, значение target,
 * нулевой байт. links[номер] — где в pool начинается адрес ссылки и в каком фрейме она стоит.
 * Оба массива растут удвоением. */
typedef struct {
    uint32_t at;                         /* начало адреса в pool */
    int16_t part;                        /* номер фрейма; −1 — обычная страница */
} link_t;
static char *pool;
static size_t pool_len, pool_cap;
static link_t *links;
static unsigned link_n, link_cap;
static int part_now = -1;                /* номер фрейма, который сейчас разбирается */

/* Запомнить адрес и target; вернуть номер ссылки (−1 — нет памяти или ссылок больше 32767:
 * номер хранится в 16-битном поле элемента страницы). */
static int add_link(const char *url, const char *target) {
    size_t n = strlen(url) + 1, t = strlen(target) + 1;
    if (link_n >= 32767) return -1;
    if (link_n == link_cap) {
        unsigned cap = link_cap ? link_cap * 2 : 256;
        link_t *p = ts_core->realloc(links, cap * sizeof(link_t));
        if (!p) return -1;
        links = p; link_cap = cap;
    }
    if (pool_len + n + t > pool_cap) {
        size_t cap = pool_cap ? pool_cap : 16384;
        while (cap < pool_len + n + t) cap *= 2;
        char *p = ts_core->realloc(pool, cap);
        if (!p) return -1;
        pool = p; pool_cap = cap;
    }
    memcpy(pool + pool_len, url, n);
    memcpy(pool + pool_len + n, target, t);
    links[link_n].at = (uint32_t)pool_len;
    links[link_n].part = (int16_t)part_now;
    pool_len += n + t;
    return (int)link_n++;
}

const char *html_link(int id) { return id >= 0 && (unsigned)id < link_n ? pool + links[id].at : NULL; }
/* target лежит сразу за нулевым байтом адреса. */
const char *html_link_target(int id) { const char *u = html_link(id); return u ? u + strlen(u) + 1 : ""; }
int html_link_part(int id) { return id >= 0 && (unsigned)id < link_n ? links[id].part : -1; }

/* --- состояние разбора --- */
static char base[URL_MAX];               /* от какого адреса считать ссылки (адрес страницы или <base>) */
static char base_target[32];             /* target для ссылок без своего (<base target=…>) */
static char title[160];                  /* текст <title> */
static int title_lock;                   /* заголовок уже есть (от первого фрейма) — не менять */
static int cs;                           /* кодировка страницы (charset.h) */
static int pre;                          /* внутри <pre>: пробелы и переводы строк как есть */
static int in_title;                     /* внутри <title>: текст идёт в заголовок, не на страницу */
static int space;                        /* был пробел, ещё не выведенный (схлопывание пробелов) */
static int pre_open;                     /* <pre> только что открыт: перевод строки сразу за ним — мимо */
static int title_len;
/* Вложенные списки: для каждого уровня — нумерованный ли (<ol>) и номер текущего пункта. */
static int list_kind[16], list_num[16], depth;

/* --- стиль: стек открытых тегов ---
 * Стиль текста зависит от всех открытых тегов: <b> внутри <a> внутри <font color> — жирная
 * ссылка нужного цвета. Поэтому на каждый открывающий тег кладётся запись в стек stk: копия
 * стиля сверху с поправками этого тега (жирный, цвет, размер…). Закрывающий тег снимает со стека
 * свою запись и всё, что лежит выше неё (незакрытые внутри теги). Текст пишется стилем верхней
 * записи. Блочные теги (абзац, список, цитата) помнят, какими были выравнивание, отступ и <pre>
 * до них, и при закрытии всё это возвращают. floor_n — дно для ячейки таблицы: её разбор не
 * может закрыть теги, открытые снаружи таблицы. */
#define ST_MAX 48
typedef struct {
    char tag[12];
    css_el_t el;                         /* отпечатки тега, классов и id — для селекторов CSS (css.h) */
    uint32_t fg, bg;                     /* цвет букв; фон под буквами (0 — без своего фона) */
    uint8_t face, flags;                 /* начертание (font.h); PAGE_BOLD, PAGE_UNDERLINE, PAGE_STRIKE */
    int16_t link;                        /* номер ссылки или −1 */
    uint8_t block, box, pre_was, list;   /* блочный; открыл блок с фоном; <pre> до него; открыл список */
    uint8_t css;                         /* открыл коробку CSS (doc_cssbox_begin) */
    uint8_t tt, ls;                      /* text-transform (1 — ВЕРХНИЙ, 2 — нижний, 3 — С Заглавной); маркер (LS_…) */
    int8_t align;                        /* выравнивание до него */
    int16_t indent;                      /* отступ до него */
    int16_t mr;                          /* строчный тег: поле справа (CSS margin), точки */
    int16_t cw, ch;                      /* своя ширина и высота содержимого из CSS (0 — нет): для процентов вложенных */
    int16_t mb_css;                      /* margin-bottom из CSS (−1 — не сказан): промежуток после блока */
    uint8_t anon, ovh;                   /* обёртка колонки flex (селекторы её не видят); overflow: hidden */
    uint8_t fclear;                      /* в нём были плавающие блоки — при закрытии опуститься ниже них */
    uint8_t nowrap;                      /* white-space: nowrap — строки не переносить (наследуется) */
    int16_t fmark;                       /* сколько плавающих было, когда он открылся (doc_float_mark) */
    uint8_t pos;                         /* position задан (relative, absolute…): блок отсчёта для absolute */
    int32_t cbx, cby, cbw, cbh;          /* его «коробка с полями» (padding box): где и какого размера;
                                          * cbh 0 — высота не известна (по содержимому), < 0 — пока
                                          * известны только поля (cb_settle) */
} st_t;
static st_t stk[ST_MAX];
static int sn, floor_n;                  /* записей в стеке; ниже floor_n не закрывать */
static uint32_t link_rgb, vlink_rgb;     /* цвета ссылок страницы (<body link= vlink=>) */
static int body_rect = -1, body_top;     /* фон <body> фрейма: прямоугольник и его верх */
static uint32_t body_bg;                 /* фон <body> (0 — фон страницы по умолчанию) */
static int table_depth;                  /* вложенность таблиц при разборе */
static int flex_depth;                   /* вложенность колонок flex и grid (свой предел — NEST_MAX) */
static int rr_flex;                      /* следующий render_range — ребёнок flex (считать flex_depth) */
static int (*visited_cb)(const char *url);
void html_set_visited(int (*cb)(const char *url)) { visited_cb = cb; }
static st_t *top(void) { return &stk[sn - 1]; }
static char *tmp;                        /* рабочий буфер text() */
static size_t tmp_cap;

const char *html_title(void) { return title; }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* Рабочий буфер не меньше n байт; растёт по мере надобности и не уменьшается. */
static char *scratch(size_t n) {
    if (n > tmp_cap) {
        char *p = ts_core->realloc(tmp, n);
        if (!p) return NULL;
        tmp = p; tmp_cap = n;
    }
    return tmp;
}

/* Стиль текста — верхней записи стека. */
static doc_style_t style_now(void) {
    const st_t *t = top();
    doc_style_t s = {t->fg, t->bg, t->flags | (t->nowrap ? DOC_NOWRAP : 0u), t->link, t->face};
    return s;
}
/* Тот же стиль без ссылки, подчёркивания и зачёркивания (пробел перед ссылкой, пометки). */
static doc_style_t style_plain(void) {
    doc_style_t s = style_now();
    s.flags &= ~(unsigned)(PAGE_UNDERLINE | PAGE_STRIKE);
    s.link = -1;
    return s;
}

/* Текст страницы — в раскладку текущим стилем. */
/* Сколько видимого вывела страница (буквы, пометки, элементы форм) и сколько у неё <script>:
 * ничего не вывела, а программы есть — страницу строит JavaScript (html_render). */
static unsigned shown_n, script_n;

static void out_utf8(const char *s, size_t n) {
    doc_style_t st = style_now();
    for (size_t i = 0; i < n && !shown_n; i++) if (s[i] != ' ') shown_n = 1;
    doc_text(s, (unsigned)n, &st);
}
/* Служебная пометка бледным цветом («[video]», «[Найти]»). */
static void out_dim(const char *s) {
    shown_n = 1;
    doc_style_t st = style_plain();
    st.fg = COL_DIM;
    if (space && !doc_empty()) doc_text(" ", 1, &st);
    space = 0;
    doc_text(s, (unsigned)strlen(s), &st);
}

/* Служебная пометка-ссылка («[frame]»); id < 0 — просто бледная пометка. Пробел перед ней —
 * без подчёркивания. */
static void out_link(const char *s, int id) {
    if (id < 0) { out_dim(s); return; }
    shown_n = 1;
    doc_style_t plain = style_plain(), st = style_plain();
    st.fg = link_rgb;
    st.flags |= PAGE_UNDERLINE;
    st.link = id;
    if (space && !doc_empty()) doc_text(" ", 1, &plain);
    space = 0;
    doc_text(s, (unsigned)strlen(s), &st);
}

/* Граница блока: закончить абзац, дальше промежуток gap точек. */
static void block(int gap) { space = 0; doc_flush(gap); }

/* Код Unicode в UTF-8; длина. */
static int put_utf8(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63));
    o[3] = (char)(0x80 | (c & 63)); return 4;
}

/* Сущности — способ записать знак, которого нет на клавиатуре или который значит что-то в
 * HTML: «&lt;» — это «<», «&amp;» — «&», «&nbsp;» — неразрывный пробел, «&#1046;» или
 * «&#x416;» — знак с кодом Unicode 1046 (десятичным) или 0x416 (шестнадцатеричным), это «Ж».
 * Здесь — самые частые именованные; их в HTML больше двух тысяч. */
static const struct { const char *name; uint16_t code; } entities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"copy", 0xA9},
    {"reg", 0xAE}, {"trade", 0x2122}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"mdash", 0x2014}, {"ndash", 0x2013},
    {"hellip", 0x2026}, {"bull", 0x2022}, {"middot", 0xB7}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
    {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bdquo", 0x201E}, {"times", 0xD7}, {"deg", 0xB0}, {"euro", 0x20AC},
    {"pound", 0xA3}, {"sect", 0xA7}, {"para", 0xB6}, {"plusmn", 0xB1}, {"frac12", 0xBD}, {"shy", 0xAD},
    {"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193}, {"numero", 0x2116}, {"thinsp", 0x2009},
    {"ensp", 0x2002}, {"emsp", 0x2003}, {"zwnj", 0x200C}, {"zwj", 0x200D}, {"minus", 0x2212}, {"divide", 0xF7},
};

/* Заменить сущности «&…;» знаками UTF-8 прямо в той же строке: знак в UTF-8 (до 4 байт)
 * никогда не длиннее своей записи «&…;», поэтому результат помещается на место исходного.
 * Мягкий перенос (&shy;) и невидимые соединители просто выбрасываются. */
static size_t entities_decode(char *s, size_t n) {
    size_t i = 0, o = 0;
    while (i < n) {
        if (s[i] != '&') { s[o++] = s[i++]; continue; }
        size_t j = i + 1;
        while (j < n && j - i < 12 && s[j] != ';' && s[j] != '&' && s[j] != ' ' && s[j] != '<') j++;
        uint32_t c = 0;
        if (j < n && s[j] == ';' && j > i + 1) {
            if (s[i + 1] == '#') {
                int hex = s[i + 2] == 'x' || s[i + 2] == 'X';
                for (size_t k = i + 2 + hex; k < j; k++) {
                    int d = s[k];
                    if (d >= '0' && d <= '9') d -= '0';
                    else if (hex && lower(d) >= 'a' && lower(d) <= 'f') d = lower(d) - 'a' + 10;
                    else { c = 0; break; }
                    c = c * (hex ? 16 : 10) + (uint32_t)d;
                    if (c > 0x10FFFF) { c = 0xFFFD; }
                }
                if (c == 0 && j > i + 2) c = 0xFFFD;
            } else {
                for (unsigned e = 0; e < sizeof(entities) / sizeof(entities[0]); e++)
                    if (strlen(entities[e].name) == j - i - 1 && !memcmp(entities[e].name, s + i + 1, j - i - 1)) {
                        c = entities[e].code; break;
                    }
            }
        }
        if (!c) { s[o++] = s[i++]; continue; }                  /* не сущность — «&» как есть */
        if (c == 0xAD || c == 0x200C || c == 0x200D) { i = j + 1; continue; }   /* невидимые */
        char u[4];
        int l = put_utf8(c, u);
        if ((size_t)l <= j + 1 - i) { memcpy(s + o, u, (size_t)l); o += (size_t)l; }
        i = j + 1;
    }
    return o;
}

/* Регистр букв по text-transform (tt: 1 — ВЕРХНИЙ, 2 — нижний, 3 — С Заглавной): латиница и
 * кириллица (а–я, ё и прочие 0x400…0x45F) — длина в байтах UTF-8 не меняется. */
static void tt_apply(char *w, size_t o, int tt) {
    int start = 1;
    for (size_t i = 0; tt && i < o; i++) {
        unsigned char c = (unsigned char)w[i];
        int up = tt == 1 || (tt == 3 && start), dn = tt == 2;
        if (c == ' ') { start = 1; continue; }
        if (c < 0x80) {
            if (up && c >= 'a' && c <= 'z') w[i] = (char)(c - 32);
            else if (dn && c >= 'A' && c <= 'Z') w[i] = (char)(c + 32);
            start = 0;
            continue;
        }
        if ((c == 0xD0 || c == 0xD1) && i + 1 < o) {
            uint32_t cp = (uint32_t)(c & 0x1F) << 6 | ((unsigned char)w[i + 1] & 0x3F);
            if (up && cp >= 0x430 && cp <= 0x44F) cp -= 0x20;
            else if (up && cp >= 0x450 && cp <= 0x45F) cp -= 0x50;
            else if (dn && cp >= 0x410 && cp <= 0x42F) cp += 0x20;
            else if (dn && cp >= 0x400 && cp <= 0x40F) cp += 0x50;
            w[i] = (char)(0xC0 | cp >> 6);
            w[i + 1] = (char)(0x80 | (cp & 0x3F));
            i++;
        }
        start = 0;
    }
}

static void out_text(char *w, size_t o) {
    tt_apply(w, o, top()->tt);
    out_utf8(w, o);
}

/* Текст из байтов страницы: кодировка → UTF-8 → сущности → пробелы.
 * Рабочий буфер — две половины: в первой текст в UTF-8 (u), во вторую пишется результат
 * схлопывания пробелов (w). Раньше результат писался поверх входа, и при некоторых
 * сочетаниях вывод обгонял чтение. */
static void text(const uint8_t *p, size_t n) {
    char *u = scratch(2 * (n * 3 + 8));                        /* вход UTF-8 и вывод — раздельно */
    if (!u) return;
    size_t len = entities_decode(u, charset_to_utf8(cs, p, n, u));
    if (in_title) {
        if (title_lock) return;
        for (size_t i = 0; i < len && title_len < (int)sizeof(title) - 1; i++) {
            char c = u[i] == '\n' || u[i] == '\t' || u[i] == '\r' ? ' ' : u[i];
            if (c == ' ' && (title_len == 0 || title[title_len - 1] == ' ')) continue;
            title[title_len++] = c;
        }
        title[title_len] = 0;
        return;
    }
    size_t o = 0, i = 0;
    char *w = u + n * 3 + 8;
    while (i < len) {
        unsigned char c = (unsigned char)u[i];
        if (pre && pre_open) {                              /* как в HTML: перевод строки сразу за <pre> — мимо */
            pre_open = 0;
            if (c == '\r' && i + 1 < len && u[i + 1] == '\n') { i += 2; continue; }
            if (c == '\n') { i++; continue; }
        }
        if (pre) {
            if (c == '\r') { i++; continue; }
            if (c == '\n') {                                   /* строка pre; пустая — промежуток строки */
                if (o) out_text(w, o);
                o = 0; space = 0;
                doc_flush(doc_empty() ? PAGE_LINE_H : 0);
                i++;
                continue;
            }
            if (c == '\t') { w[o++] = ' '; w[o++] = ' '; i++; continue; }
            w[o++] = u[i++];
            continue;
        }
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f') { space = 1; i++; continue; }
        if (space && (o || !doc_empty())) {
            /* пробел перед ссылкой, подчёркнутым, зачёркнутым или текстом с фоном — без них */
            if (!o && (top()->link >= 0 || top()->bg || (top()->flags & (PAGE_UNDERLINE | PAGE_STRIKE)))) {
                doc_style_t plain = style_plain();
                doc_text(" ", 1, &plain);
            } else {
                w[o++] = ' ';
            }
        }
        space = 0;
        if (c == 0xC2 && i + 1 < len && (unsigned char)u[i + 1] == 0xA0) { w[o++] = ' '; i += 2; continue; }   /* NBSP */
        w[o++] = u[i++];
    }
    if (o) out_text(w, o);
}

/* Найти атрибут name в теге (a — текст после имени тега, n байт) и записать его значение в
 * UTF-8 в out (не больше cap байт с нулём); 0 — такого атрибута нет.
 * Атрибуты пишут так: href="адрес", href='адрес', href=адрес или просто checked (без
 * значения). Имя атрибута сравнивается без учёта регистра. */
static int attr(const char *a, size_t n, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name), i = 0;
    while (i < n) {
        while (i < n && (a[i] == ' ' || a[i] == '\n' || a[i] == '\t' || a[i] == '\r' || a[i] == '/')) i++;
        size_t k = i;
        while (k < n && a[k] != '=' && a[k] != ' ' && a[k] != '>' && a[k] != '\n' && a[k] != '\t') k++;
        int match = k - i == nl;
        for (size_t t = 0; match && t < nl; t++) match = lower((unsigned char)a[i + t]) == name[t];
        size_t v0 = k, v1 = k;
        while (v0 < n && (a[v0] == ' ' || a[v0] == '\n')) v0++;
        if (v0 < n && a[v0] == '=') {
            v0++;
            while (v0 < n && (a[v0] == ' ' || a[v0] == '\n')) v0++;
            if (v0 < n && (a[v0] == '"' || a[v0] == '\'')) {
                char q = a[v0++];
                v1 = v0;
                while (v1 < n && a[v1] != q) v1++;
                k = v1 + 1;
            } else {
                v1 = v0;
                while (v1 < n && a[v1] != ' ' && a[v1] != '>' && a[v1] != '\n' && a[v1] != '\t') v1++;
                k = v1;
            }
        } else { v0 = v1 = k; }
        if (match) {
            /* в UTF-8 байт остаётся байтом; однобайтовая кодировка даёт до 3 байтов на знак */
            size_t len = v1 - v0, room = cs == CS_UTF8 ? cap - 1 : (cap - 1) / 3;
            if (len > room) len = room;
            size_t got = charset_to_utf8(cs, (const uint8_t *)a + v0, len, out);
            out[entities_decode(out, got)] = 0;
            return 1;
        }
        i = k > i ? k : i + 1;
    }
    return 0;
}

/* Имя тега равно строке / одной из строк списка (список кончается NULL). */
static int is(const char *a, const char *b) { return !strcmp(a, b); }
static int is_any(const char *n, const char *const *list) { for (; *list; list++) if (!strcmp(n, *list)) return 1; return 0; }

/* Теги-блоки без особого смысла для нас: просто начинают новую строку. */
static const char *const blocks[] = {"div", "section", "article", "header", "footer", "nav", "main", "aside", "form",
    "fieldset", "figure", "figcaption", "address", "details", "summary", "dl", "caption", "legend",
    "hgroup", "tbody", "thead", "tfoot", NULL};
/* Теги без закрывающего — в стек не кладутся. */
static const char *const voids[] = {"meta", "link", "wbr", "col", "area", "param", "source", "embed", "track",
    "keygen", "isindex", NULL};
/* Теги, содержимое которых пропускаем до закрывающего тега. */
static const char *const skips[] = {"script", "style", "template", "svg", "math", "iframe", "object",
    "video", "audio", "canvas", "map", "noembed", "noframes", NULL};

static char skip_until[16];              /* пропускать всё до «</этого_тега»; пусто — не пропускаем */

/* «Не встраивать»: компилятор любит вставлять тело небольшой функции прямо в место вызова. Для
 * обработчиков тегов, которые зовёт parse, это плохо: parse вызывает себя через таблицы, и
 * буферы обработчика оказались бы в стеке на каждом уровне вложенности, даже когда не нужны
 * (на плате 04.11 стек кончился). С NOINLINE их место в стеке занято, только пока они работают. */
#define NOINLINE __attribute__((noinline))

#define LIST_INDENT 24                   /* отступ уровня списка: маркер у левого края, текст правее */
#define TABLE_DEPTH_MAX 4                /* таблица в таблице… глубже — без столбцов */
#define NEST_MAX 6                       /* колонки flex и grid в колонках… глубже — сверху вниз (свой счётчик,
                                          * не общий с таблицами: таблица в 4 колонках — всё равно таблица; стек задачи
                                          * 12 КБ: уровень parse → flex_row → render_range — ~0,5 КБ, ESP-сборка,
                                          * gcc -fstack-usage 07.11; массивы — в куче) */

static void parse(const uint8_t *b, size_t n);
static size_t skip_markup(const uint8_t *b, size_t n, size_t i);
static size_t read_tag(const uint8_t *b, size_t n, size_t i, char *name, int *closing, size_t *a0, size_t *a1);
static size_t find_close(const uint8_t *b, size_t n, size_t i, const char *name);

/* Число из начала строки (без знака); не число — 0. */
static int num(const char *s) {
    int v = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9' && v < 100000) v = v * 10 + (*s++ - '0');
    return v;
}

/* --- цвета ---------------------------------------------------------------------------------- */

static int hexd(int c) {
    c = lower(c);
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Смесь цвета c с цветом под ним u; a — непрозрачность c (0 — не видно, 255 — сплошной). */
static uint32_t mix(uint32_t c, uint32_t u, int a) {
    uint32_t r = 0;
    for (int s = 0; s < 24; s += 8) {
        uint32_t x = (c >> s & 255u) * (uint32_t)a + (u >> s & 255u) * (uint32_t)(255 - a);
        r |= (x / 255u) << s;
    }
    return r;
}

/* Цвет CSS (css.c: все имена CSS, #rgb(a), rgb(), hsl(), currentColor — это cur) поверх цвета
 * under: полупрозрачный — смешан с ним. 0 — не цвет или почти прозрачный. Чистый чёрный
 * отдаётся как 0x000001: 0 в стиле значит «своего цвета нет», а на экране разницы не видно
 * (RGB565 младших битов не хранит). */
static int css_rgb(const char *v, uint32_t cur, uint32_t under, uint32_t *out) {
    uint32_t c;
    int a;
    if (!css_color(v, cur, &c, &a) || a < 16) return 0;
    if (a < 250) c = mix(c, under, a);
    *out = c ? c : 1;
    return 1;
}

/* Цвет из атрибута (bgcolor=, color=, text=, link=…): как в CSS и ещё «rrggbb» без # (так
 * писали старые сайты); −1 — не цвет. */
static int32_t parse_color(const char *s) {
    while (*s == ' ') s++;
    uint32_t c;
    if (css_rgb(s, top()->fg, doc_bg(), &c)) return (int32_t)c;
    size_t n = 0;
    while (hexd((unsigned char)s[n]) >= 0) n++;
    if (n != 6 || (s[n] && s[n] != ' ' && s[n] != ';')) return -1;
    int32_t v = 0;
    for (int i = 0; i < 6; i++) v = v << 4 | hexd((unsigned char)s[i]);
    return v ? v : 1;
}

/* Начертания по размеру, от мелкого к крупному: шаг для <small>, <big>, smaller, larger. */
static const uint8_t face_order[6] = {FACE_CAPTION, FACE_SMALL, FACE_BODY, FACE_H3, FACE_H2, FACE_H1};
static unsigned face_step(unsigned face, int d) {
    int i = 2;
    for (int k = 0; k < 6; k++) if (face_order[k] == face) i = k;
    i += d;
    return face_order[i < 0 ? 0 : i > 5 ? 5 : i];
}

/* Размер шрифта CSS (px, pt, em, rem, %, calc(…); слова small, large…) → начертание; −1 — не
 * понят. Обычный текст сайтов — 16 px, у нас он основной 20 px; крупнее и мельче — ближайшее
 * начертание. Проценты и em — от 16 (не от размера родителя: начертаний всего шесть). */
static int css_face(const char *v, unsigned cur) {
    static const struct { const char *w; uint8_t f; } words[] = {
        {"xx-small", FACE_CAPTION}, {"x-small", FACE_CAPTION}, {"small", FACE_SMALL}, {"medium", FACE_BODY},
        {"large", FACE_H3}, {"x-large", FACE_H2}, {"xx-large", FACE_H1}, {"xxx-large", FACE_H1}};
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) if (is(v, words[i].w)) return words[i].f;
    if (is(v, "smaller")) return (int)face_step(cur, -1);
    if (is(v, "larger")) return (int)face_step(cur, 1);
    int px;
    if (css_len(v, 16, &px) != 1 || px < 0 || px > 500) return -1;
    return px >= 26 ? FACE_H1 : px >= 20 ? FACE_H2 : px >= 17 ? FACE_H3 : px >= 14 ? FACE_BODY : px >= 12 ? FACE_SMALL
                                                                                                   : FACE_CAPTION;
}

/* align= или text-align: строк блока. */
static void set_align(const char *v) {
    char w[12];
    size_t k = 0;
    while (v[k] && k < sizeof(w) - 1) { w[k] = (char)lower((unsigned char)v[k]); k++; }
    w[k] = 0;
    if (!strncmp(w, "center", 6)) doc_align(DOC_CENTER);
    else if (is(w, "right")) doc_align(DOC_RIGHT);
    else if (is(w, "left") || is(w, "justify")) doc_align(DOC_LEFT);
}

/* --- стек открытых тегов ----------------------------------------------------------------------- */

/* Положить новую запись — копию верхней; полон стек — NULL (тег тогда стиль не меняет). */
static st_t *push(const char *name) {
    if (sn >= ST_MAX) return NULL;
    stk[sn] = stk[sn - 1];
    st_t *e = &stk[sn++];
    snprintf(e->tag, sizeof(e->tag), "%s", name);
    memset(&e->el, 0, sizeof(e->el));
    e->mr = 0;
    e->cw = e->ch = 0;
    e->mb_css = -1;
    e->anon = e->ovh = 0;
    e->fclear = 0;
    e->fmark = (int16_t)doc_float_mark();
    e->pos = 0;
    e->block = e->box = e->list = e->css = 0;
    e->pre_was = (uint8_t)pre;
    return e;
}

/* Снять верхнюю запись: блок с фоном закрыть, вернуть выравнивание, отступ, <pre> и уровень списка,
 * какими они были до тега. */
static void pop_one(void) {
    st_t *e = &stk[sn - 1];
    if (e->block || e->box) { space = 0; doc_flush(0); }
    /* блок вмещает свои плавающие блоки (как clearfix у сайтов: «.row::after { clear: both }» —
     * ::after у нас нет, а сайты на него рассчитывают); плавающие картинки — нет: текст следующих
     * абзацев их обтекает, как у браузеров */
    if (e->fclear) doc_float_clear(e->fmark, 3);
    if (e->css) doc_cssbox_end();                     /* коробка открыта после фона тега (pre) */
    if (e->box) doc_box_end(4);
    if (e->block) { doc_align(e->align); doc_indent(e->indent); pre = e->pre_was; }
    if (e->list && depth > 0) depth--;
    if (!e->block && e->mr > 0) doc_inline(e->mr, 0, 0, -1);   /* поле справа строчного тега */
    css_vars_cut(sn + 1);                             /* переменные CSS этого тега (глубина = номер + 2) */
    sn--;
}

/* Где в стеке ближайший открытый тег name (выше дна floor_n); −1 — не открыт. */
static int find_open(const char *name) {
    for (int i = sn - 1; i > floor_n && i > 0; i--) if (is(stk[i].tag, name)) return i;
    return -1;
}
static void close_to(int i) { while (sn > i) pop_one(); }

/* Незакрытый <p> закрывается любым блочным тегом (так HTML и задуман: </p> необязателен) — если
 * между ним и верхом стека нет других блоков. */
static void close_p(void) {
    int i = find_open("p");
    if (i < 0) return;
    for (int k = i + 1; k < sn; k++) if (stk[k].block) return;
    block(12);
    close_to(i);
}

/* --- CSS: что делают свойства --------------------------------------------------------------------
 * Таблицы стилей (<style>, внешние <link>, @import) разбирает и подбирает элементам css.c (там же
 * словарь: селектор, специфичность, отпечаток). Здесь — что делают свойства подошедших правил:
 * стиль текста (цвет, фон, жирность, размер, подчёркивание, регистр букв), вид маркера списка,
 * коробка блока (поля, рамка, размеры — doc_cssbox_begin), скрытие (display: none и приёмы
 * «спрятать от глаз, оставить для читалок»). Порядок — как в CSS: свойства правил по весу их
 * селекторов (при равенстве — по порядку в таблицах), потом style="…" тега, потом всё с
 * «!important»; последнее главнее. Наследуются (копией записи стека, push) цвет, размер,
 * жирность, выравнивание, регистр букв, вид маркера; коробка у каждого тега своя. Позиция —
 * поток сверху вниз; float встаёт у края (float_box), position: absolute — на своё место вне
 * потока (abs_box), position: fixed — в потоке (у нас страница, а не окно с прокруткой). */
static char last_display;                /* display последнего apply_style: 'n' none, 'i' строчный, 'b' иное */
static int margin_left;                  /* поле слева строчного тега из CSS (последний apply_style) */
static uint32_t cur_fg;                  /* цвет текста элемента — для currentColor */
static int len_wbase, len_hbase;         /* от чего считать проценты ширины и высоты (−1 — не от чего) */
static int gap_before = -1;              /* промежуток перед блоком до его собственного (open_block) */

/* Вид маркера пункта списка (list-style-type); LS_AUTO — по списку: <ol> — номера, <ul> — точки. */
enum { LS_AUTO, LS_NONE, LS_DISC, LS_DEC, LS_LALPHA, LS_UALPHA, LS_LROMAN, LS_UROMAN };

/* Быстрая проверка «может ли такое свойство быть в строке»: по отпечатку имени — один бит из 64.
 * Строку свойств просматриваем один раз (dmask_make), а потом не ищем в ней свойства, которых
 * там точно нет (иначе полсотни css_get на каждое правило каждого элемента). */
static uint64_t dmask;
static uint64_t pbit(const char *s, size_t n) { return 1ull << (css_hash_ci(s, n) & 63u); }
static void dmask_make(const char *d) {
    dmask = 0;
    for (const char *p = d; *p;) {
        while (*p == ' ' || *p == ';') p++;
        const char *q = p;
        while (*q && *q != ':' && *q != ';') q++;
        size_t n = (size_t)(q - p);
        while (n && p[n - 1] == ' ') n--;
        if (*q == ':' && n) dmask |= pbit(p, n);
        p = q;
        if (*p != ':') continue;
        int par = 0;
        char qu = 0;
        for (p++; *p; p++) {                           /* значение — до «;» вне скобок и кавычек */
            if (qu) { if (*p == qu) qu = 0; }
            else if (*p == '"' || *p == '\'') qu = *p;
            else if (*p == '(') par++;
            else if (*p == ')') { if (par) par--; }
            else if (*p == ';' && !par) break;
        }
    }
}
/* Значение свойства prop (css_get: строчными, var() подставлены), если оно может быть в строке. */
static int get(const char *css, const char *prop, char *v, size_t cap) {
    if (!(dmask & pbit(prop, strlen(prop)))) return 0;
    return css_get(css, prop, v, cap);
}

/* Следующее слово значения (скобки — целиком: «calc(100% - 20px)», «rgb(0 0 0 / 50%)») — в w. */
static int word(const char **pp, char *w, size_t cap) {
    const char *p = *pp;
    while (*p == ' ') p++;
    if (!*p) return 0;
    size_t k = 0;
    int d = 0;
    while (*p && (d || *p != ' ')) {
        if (*p == '(') d++;
        else if (*p == ')' && d) d--;
        if (k + 1 < cap) w[k++] = *p;
        p++;
    }
    w[k] = 0;
    *pp = p;
    return 1;
}

/* --- коробка блока (doc_box_t) ---
 * Собирается в bx и bs (apply_decls), открывается в open_box. Размеры в bx — как их сказал CSS
 * (width — у содержимого или, при box-sizing: border-box, по внешнему краю); в doc.c они уходят
 * по внешнему краю. Проценты ширины, полей и margin — от ширины места (len_wbase), высоты — от
 * заданной высоты предка (len_hbase). */
static doc_box_t bx;
static struct {
    uint8_t w, h, maxw, minw, minh, maxh;     /* свойство задано (числом) */
    uint8_t w_auto, h_auto;                   /* width / height: auto (у картинки — её размер) */
    uint8_t border_box;                       /* box-sizing: border-box */
    uint8_t bstyle;                           /* стиль рамки: 0 — не сказан (рамки нет), 1 — виден, 2 — none */
    uint8_t bnone;                            /* рамку явно убрали (border: 0 / none) — для полей форм */
    uint8_t bg, pl, mt, mb;                   /* фон, padding-left, margin сверху и снизу заданы */
    uint8_t pad;                              /* padding задан (какой-нибудь стороны) */
    uint8_t collapse, bspace_set;             /* таблица: border-collapse: collapse; border-spacing задан */
    int bspace;                               /* border-spacing (по горизонтали) */
    uint8_t ovh, abs, off, clip;              /* overflow: hidden; position: absolute; за краем; обрезан */
    uint8_t fixed;                            /* position: fixed */
    int minw_v, minh_v, maxh_v;               /* min-width, min-height, max-height */
    int indent;                               /* text-indent (> 0) */
    uint8_t disp, fcol, wrap, just;           /* display: 'f' flex, 'g' grid; column; flex-wrap; justify-content */
    uint8_t aic;                              /* align-items: center */
    int cgap, rgap;                           /* gap: между столбцами и строками */
    int grow, shrink, basis, order;           /* flex-grow и flex-shrink ×100; flex-basis (−1 — auto); order */
    char gcols[160];                          /* grid-template-columns */
    uint8_t flt, clr, rel;                    /* float ('l', 'r'); clear (1 левые, 2 правые, 3 оба); position: relative */
    uint8_t lset, rset;                       /* left, right заданы */
    int lv, rv, dx;                           /* left, right; сдвиг position: relative по горизонтали */
    uint8_t tset, bset;                       /* top, bottom заданы */
    int tv, bv;                               /* top, bottom */
    int tx, ty, txp, typ;                     /* transform: translate — точки; проценты своего размера ×100 */
    int ar;                                   /* aspect-ratio × 1000 (0 — нет) */
} bs;
static void box_reset(void) {
    memset(&bx, 0, sizeof(bx));
    memset(&bs, 0, sizeof(bs));
    bs.shrink = 100;
    bs.basis = -1;
}

/* Число CSS с дробью ×100 (flex-grow: 0.5 → 50); −1 — не число. */
static int x100(const char *v) {
    int w = 0, f = 0, d = 0, any = 0;
    while (*v >= '0' && *v <= '9') { if (w < 100000) w = w * 10 + (*v - '0'); v++; any = 1; }
    if (*v == '.') for (v++; *v >= '0' && *v <= '9'; v++, any = 1) if (d < 2) { f = f * 10 + (*v - '0'); d++; }
    if (!any || *v) return -1;
    return w * 100 + (d == 1 ? f * 10 : f);
}

/* flex: none | auto | рост [сжатие] [основа] (CSS: «flex: 1» — это 1 1 0%). */
static void css_flex(const char *v) {
    if (is(v, "none")) { bs.grow = 0; bs.shrink = 0; bs.basis = -1; return; }
    if (is(v, "auto")) { bs.grow = 100; bs.shrink = 100; bs.basis = -1; return; }
    if (is(v, "initial")) { bs.grow = 0; bs.shrink = 100; bs.basis = -1; return; }
    char w[48];
    int nums = 0, basis = 0, x, any_basis = 0;
    for (const char *p = v; word(&p, w, sizeof(w));) {
        int k = x100(w);
        if (k >= 0) { if (!nums) bs.grow = k; else bs.shrink = k; nums++; continue; }
        int r = css_len(w, len_wbase, &x);
        if (r) { basis = r > 0 ? (x > 0 ? x : 0) : -1; any_basis = 1; }
    }
    if (!nums && any_basis) bs.grow = 100;            /* «flex: 200px» — 1 1 200px */
    if (nums < 2) bs.shrink = 100;
    bs.basis = basis;
}

/* Одна длина (padding-top…): 1 — да, *out — не меньше 0, не больше cap. */
static int one_len(const char *v, int base, int cap, int *out) {
    int x;
    if (css_len(v, base, &x) != 1) return 0;
    *out = x < 0 ? 0 : x > cap ? cap : x;
    return 1;
}

/* 1–4 значения «верх право низ лево» (как у margin и padding) → out[4]; au[4] — где auto.
 * Отрицательные — 0 (сдвигать влево за край мы не умеем). 0 — не длины (inherit и подобное). */
static int css_sides(const char *v, int out[4], uint8_t au[4], int cap) {
    int val[4], a[4], k = 0;
    char w[64];
    for (const char *p = v; k < 4 && word(&p, w, sizeof(w));) {
        int x = 0, r = css_len(w, len_wbase, &x);
        if (!r) return 0;
        a[k] = r < 0;
        val[k++] = r < 0 || x < 0 ? 0 : x > cap ? cap : x;
    }
    if (!k) return 0;
    static const uint8_t map[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
    for (int s = 0; s < 4; s++) {
        out[s] = val[map[k - 1][s]];
        if (au) au[s] = (uint8_t)a[map[k - 1][s]];
    }
    return 1;
}

static const char *const side_name[4] = {"top", "right", "bottom", "left"};
static int *border_side(int s) { return s == 0 ? &bx.bt : s == 1 ? &bx.br : s == 2 ? &bx.bb : &bx.bl; }
static int *pad_side(int s) { return s == 0 ? &bx.pt : s == 1 ? &bx.pr : s == 2 ? &bx.pb : &bx.pl; }
static const char *const bstyles[] = {"solid", "dashed", "dotted", "double", "groove", "ridge", "inset", "outset", NULL};

/* Рамка «1px solid #ccc» (части в любом порядке) — на стороны mask (бит 0 верх, 1 право, 2 низ,
 * 3 лево). Без толщины — medium (3 точки), без цвета — цвет текста; без стиля (solid, dashed…)
 * рамки нет, как у браузеров; none и hidden — нет. Пунктир и двойная — сплошной линией. */
static void css_border(const char *v, int mask) {
    char w[48];
    int width = 3, style = 0;
    uint32_t c = cur_fg ? cur_fg : 1;
    for (const char *p = v; word(&p, w, sizeof(w));) {
        int x;
        if (is(w, "none") || is(w, "hidden")) style = 2;
        else if (is_any(w, bstyles)) style = 1;
        else if (is(w, "thin")) width = 1;
        else if (is(w, "medium")) width = 3;
        else if (is(w, "thick")) width = 5;
        else if (css_len(w, -1, &x) == 1) width = x < 0 ? 0 : x > 15 ? 15 : x;
        else css_rgb(w, cur_fg, doc_bg(), &c);
    }
    if (style == 1) bs.bstyle = 1;
    if (style != 1) width = 0;
    for (int s = 0; s < 4; s++) if (mask >> s & 1) *border_side(s) = width;
    if (width) bx.bc = c;
    else if (mask == 15) bs.bnone = 1;
}

/* Свойства коробки из строки свойств — в bx и bs (у любого тега: картинки и поля форм берут
 * отсюда размеры, строчные теги — ширину для процентов у вложенных картинок). */
static void css_box(const char *css, int blockish) {
    char v[96], nm[24];
    int s[4], x;
    uint8_t au[4];
    if (get(css, "padding", v, sizeof(v)) && css_sides(v, s, NULL, 300)) {
        for (int k = 0; k < 4; k++) *pad_side(k) = s[k];
        bs.pl = 1;
        bs.pad = 1;
    }
    for (int k = 0; k < 4; k++) {
        snprintf(nm, sizeof(nm), "padding-%s", side_name[k]);
        if (get(css, nm, v, sizeof(v)) && one_len(v, len_wbase, 300, &x)) { *pad_side(k) = x; bs.pad = 1; if (k == 3) bs.pl = 1; }
    }
    if (blockish) {                                   /* поля снаружи строчного тега — css_margins */
        if (get(css, "margin", v, sizeof(v)) && css_sides(v, s, au, 300)) {
            bx.mt = s[0]; bx.mr = s[1]; bx.mb = s[2]; bx.ml = s[3];
            bx.auto_r = au[1]; bx.auto_l = au[3];
            bs.mt = bs.mb = 1;
        }
        if (get(css, "margin-inline", v, sizeof(v)) && css_sides(v, s, au, 300)) {   /* слева, справа */
            bx.ml = s[0]; bx.mr = s[1]; bx.auto_l = au[0]; bx.auto_r = au[1];
        }
        if (get(css, "margin-top", v, sizeof(v)) && one_len(v, len_wbase, 300, &x)) { bx.mt = x; bs.mt = 1; }
        if (get(css, "margin-bottom", v, sizeof(v)) && one_len(v, len_wbase, 300, &x)) { bx.mb = x; bs.mb = 1; }
        int r;
        if (get(css, "margin-left", v, sizeof(v)) && (r = css_len(v, len_wbase, &x)) != 0) {
            bx.auto_l = r < 0;
            bx.ml = r > 0 && x > 0 ? (x > 300 ? 300 : x) : 0;
        }
        if (get(css, "margin-right", v, sizeof(v)) && (r = css_len(v, len_wbase, &x)) != 0) {
            bx.auto_r = r < 0;
            bx.mr = r > 0 && x > 0 ? (x > 300 ? 300 : x) : 0;
        }
    }
    /* логические свойства: inline — по строке (start — слева), block — поперёк (start — сверху) */
    static const struct { const char *name; int side; } lpad[] = {
        {"padding-block-start", 0}, {"padding-inline-end", 1}, {"padding-block-end", 2}, {"padding-inline-start", 3}};
    for (int k = 0; k < 4; k++)
        if (get(css, lpad[k].name, v, sizeof(v)) && one_len(v, len_wbase, 300, &x)) {
            *pad_side(lpad[k].side) = x;
            bs.pad = 1;
            if (lpad[k].side == 3) bs.pl = 1;
        }
    if (get(css, "padding-inline", v, sizeof(v)) && css_sides(v, s, NULL, 300)) { bx.pl = s[0]; bx.pr = s[1]; bs.pad = bs.pl = 1; }
    if (get(css, "padding-block", v, sizeof(v)) && css_sides(v, s, NULL, 300)) { bx.pt = s[0]; bx.pb = s[1] ? s[1] : s[0]; bs.pad = 1; }
    if (blockish) {
        if (get(css, "margin-block-start", v, sizeof(v)) && one_len(v, len_wbase, 300, &x)) { bx.mt = x; bs.mt = 1; }
        if (get(css, "margin-block-end", v, sizeof(v)) && one_len(v, len_wbase, 300, &x)) { bx.mb = x; bs.mb = 1; }
        if (get(css, "margin-block", v, sizeof(v)) && css_sides(v, s, NULL, 300)) { bx.mt = s[0]; bx.mb = s[1]; bs.mt = bs.mb = 1; }
        int r;
        if (get(css, "margin-inline-start", v, sizeof(v)) && (r = css_len(v, len_wbase, &x)) != 0) {
            bx.auto_l = r < 0;
            bx.ml = r > 0 && x > 0 ? (x > 300 ? 300 : x) : 0;
        }
        if (get(css, "margin-inline-end", v, sizeof(v)) && (r = css_len(v, len_wbase, &x)) != 0) {
            bx.auto_r = r < 0;
            bx.mr = r > 0 && x > 0 ? (x > 300 ? 300 : x) : 0;
        }
    }
    if (get(css, "border", v, sizeof(v))) css_border(v, 15);
    static const struct { const char *name; int mask; } lbor[] = {
        {"border-block-start", 1}, {"border-inline-end", 2}, {"border-block-end", 4}, {"border-inline-start", 8},
        {"border-block", 5}, {"border-inline", 10}};
    for (int k = 0; k < 6; k++)
        if (get(css, lbor[k].name, v, sizeof(v))) css_border(v, lbor[k].mask);
    for (int k = 0; k < 4; k++) {
        snprintf(nm, sizeof(nm), "border-%s", side_name[k]);
        if (get(css, nm, v, sizeof(v))) css_border(v, 1 << k);
    }
    if (get(css, "border-width", v, sizeof(v)) && css_sides(v, s, NULL, 15)) {
        for (int k = 0; k < 4; k++) *border_side(k) = s[k];
        if (!s[0] && !s[1] && !s[2] && !s[3]) bs.bnone = 1;
    }
    for (int k = 0; k < 4; k++) {
        snprintf(nm, sizeof(nm), "border-%s-width", side_name[k]);
        if (get(css, nm, v, sizeof(v)) && one_len(v, -1, 15, &x)) *border_side(k) = x;
        snprintf(nm, sizeof(nm), "border-%s-style", side_name[k]);
        if (get(css, nm, v, sizeof(v))) {
            if (is(v, "none") || is(v, "hidden")) *border_side(k) = 0;
            else bs.bstyle = 1;
        }
    }
    if (get(css, "border-style", v, sizeof(v))) {
        char w[24];
        const char *p = v;
        word(&p, w, sizeof(w));
        if (is(w, "none") || is(w, "hidden")) { bx.bt = bx.br = bx.bb = bx.bl = 0; bs.bstyle = 2; bs.bnone = 1; }
        else bs.bstyle = 1;
    }
    uint32_t c;
    if (get(css, "border-color", v, sizeof(v))) {
        char w[48];
        const char *p = v;
        if (word(&p, w, sizeof(w)) && css_rgb(w, cur_fg, doc_bg(), &c)) bx.bc = c;
    }
    for (int k = 0; k < 4; k++) {                    /* цвет у рамки один — какой сказали последним */
        snprintf(nm, sizeof(nm), "border-%s-color", side_name[k]);
        if (get(css, nm, v, sizeof(v)) && css_rgb(v, cur_fg, doc_bg(), &c)) bx.bc = c;
    }
    if (get(css, "border-radius", v, sizeof(v))) {
        char w[48];
        const char *p = v;
        if (word(&p, w, sizeof(w))) {
            if (strchr(w, '%')) bx.radius = 16;      /* «50%» — круг или «таблетка»: просто скруглённая */
            else if (css_len(w, -1, &x) == 1) bx.radius = x < 0 ? 0 : x > 64 ? 64 : x;
        }
    }
    if (get(css, "box-sizing", v, sizeof(v))) bs.border_box = is(v, "border-box");
    int r;
    if ((get(css, "width", v, sizeof(v)) || get(css, "inline-size", v, sizeof(v))) && (r = css_len(v, len_wbase, &x)) != 0) {
        bs.w = r > 0; bs.w_auto = r < 0;
        bx.w = r > 0 && x > 0 ? x : 0;
    }
    if (get(css, "max-width", v, sizeof(v)) || get(css, "max-inline-size", v, sizeof(v))) {
        if (is(v, "none")) { bs.maxw = 0; bx.maxw = 0; }
        else if (css_len(v, len_wbase, &x) == 1) { bs.maxw = 1; bx.maxw = x > 0 ? x : 0; }
    }
    if (get(css, "min-width", v, sizeof(v)) && css_len(v, len_wbase, &x) == 1) { bs.minw = x > 0; bs.minw_v = x; }
    if ((get(css, "height", v, sizeof(v)) || get(css, "block-size", v, sizeof(v))) && (r = css_len(v, len_hbase, &x)) != 0) {
        bs.h = r > 0; bs.h_auto = r < 0;
        bx.h = r > 0 && x > 0 ? x : 0;
    }
    if (get(css, "min-height", v, sizeof(v)) && css_len(v, len_hbase, &x) == 1) { bs.minh = x > 0; bs.minh_v = x; }
    if (get(css, "max-height", v, sizeof(v))) {
        if (is(v, "none")) bs.maxh = 0;
        else if (css_len(v, len_hbase, &x) == 1) { bs.maxh = 1; bs.maxh_v = x; }
    }
}

/* Фон из background(-color): среди картинки url(…), повторов и мест — слово-цвет; у градиента —
 * его первый цвет. 1 — фон сказан: *rgb, или *none = 1 (прозрачный, none). */
static int css_bg(const char *v, uint32_t *rgb, int *none) {
    char w[96];
    *none = 0;
    for (const char *p = v; word(&p, w, sizeof(w));) {
        if (is(w, "none")) { *none = 1; return 1; }
        if (!strncmp(w, "url(", 4)) continue;
        const char *g = strstr(w, "gradient(");
        uint32_t c;
        int a;
        if (g) {                                      /* первый цвет внутри скобок градиента */
            for (const char *q = g + 9; *q; q++)
                if ((q[-1] == '(' || q[-1] == ',' || q[-1] == ' ') && css_color(q, cur_fg, &c, &a) && a >= 16) {
                    *rgb = a < 250 ? mix(c, doc_bg(), a) : c;
                    if (!*rgb) *rgb = 1;
                    return 1;
                }
            continue;
        }
        if (css_color(w, cur_fg, &c, &a)) {
            if (a < 16) { *none = 1; return 1; }
            *rgb = a < 250 ? mix(c, doc_bg(), a) : c;
            if (!*rgb) *rgb = 1;
            return 1;
        }
    }
    return 0;
}

/* Поля слева и справа строчного тега: margin (1–4 значения), margin-left, margin-right. Ссылки,
 * разделённые только полями (подвал Google: «#… a{margin:0 12px}»), иначе слипались бы в одно
 * слово. Не больше 64 точек. */
static void css_margins(const char *css, int *left, int *right) {
    char v[96];
    int s[4], x;
    uint8_t au[4];
    if (get(css, "margin", v, sizeof(v)) && css_sides(v, s, au, 64)) { *left = s[3]; *right = s[1]; }
    if (get(css, "margin-left", v, sizeof(v)) && one_len(v, len_wbase, 64, &x)) *left = x;
    if (get(css, "margin-right", v, sizeof(v)) && one_len(v, len_wbase, 64, &x)) *right = x;
}

/* Жирность: bold, bolder, 600…900 — 1; normal, lighter, 100…500 — 0; иное — −1. */
static int weight_bold(const char *w) {
    if (is(w, "bold") || is(w, "bolder")) return 1;
    if (is(w, "normal") || is(w, "lighter")) return 0;
    if (w[0] >= '1' && w[0] <= '9') {
        const char *p = w;
        while (*p >= '0' && *p <= '9') p++;
        if (!*p) return num(w) >= 600;
    }
    return -1;
}

/* font: [наклон] [жирность] размер[/интервал] шрифт — жирность и размер (сокращение сбрасывает
 * жирность в normal, если её не сказали, — как у браузеров). */
static void css_font(st_t *e, const char *v) {
    if (is(v, "inherit") || is(v, "initial") || is(v, "unset") || is(v, "revert")) return;
    char w[64];
    int bold = 0;
    for (const char *p = v; word(&p, w, sizeof(w));) {
        int b = weight_bold(w);
        if (b >= 0 && !is(w, "normal")) { bold = b; continue; }
        if (is(w, "normal") || is(w, "italic") || is(w, "oblique") || is(w, "small-caps")) continue;
        char *sl = strchr(w, '/');
        if (sl) *sl = 0;
        int f = css_face(w, e->face);
        if (f < 0) return;                            /* «font: menu» и подобное — не трогать */
        e->face = (uint8_t)f;
        break;                                        /* дальше — имя шрифта */
    }
    if (bold) e->flags |= PAGE_BOLD;
    else e->flags &= (uint8_t)~PAGE_BOLD;
}

/* list-style(-type): вид маркера; −1 — не сказан. */
static int list_style(const char *v) {
    char w[48];
    int r = -1;
    for (const char *p = v; word(&p, w, sizeof(w));) {
        if (is(w, "none")) r = LS_NONE;
        else if (is(w, "disc") || is(w, "circle") || is(w, "square") || !strncmp(w, "disclosure", 10) || w[0] == '"' || w[0] == '\'')
            r = LS_DISC;
        else if (!strncmp(w, "decimal", 7)) r = LS_DEC;
        else if (is(w, "lower-alpha") || is(w, "lower-latin")) r = LS_LALPHA;
        else if (is(w, "upper-alpha") || is(w, "upper-latin")) r = LS_UALPHA;
        else if (is(w, "lower-roman")) r = LS_LROMAN;
        else if (is(w, "upper-roman")) r = LS_UROMAN;
    }
    return r;
}

/* inset: 1–4 значения «верх право низ лево» (как у margin); проценты верха и низа — от высоты блока
 * отсчёта, права и лева — от его ширины; auto — сторона не задана. */
static void css_inset(const char *v) {
    char w[64];
    int k = 0;
    for (const char *p = v; k < 4 && word(&p, w, sizeof(w));) k++;
    if (!k) return;
    static const uint8_t map[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
    uint8_t *f[4] = {&bs.tset, &bs.rset, &bs.bset, &bs.lset};
    int *o[4] = {&bs.tv, &bs.rv, &bs.bv, &bs.lv};
    for (int s = 0; s < 4; s++) {                     /* сторона s — значение номер map[k-1][s] */
        const char *p = v;
        for (int q = 0; q <= map[k - 1][s]; q++) word(&p, w, sizeof(w));
        int x = 0;
        *f[s] = css_len(w, (s & 1) ? len_wbase : len_hbase, &x) == 1;
        *o[s] = x;
    }
}

/* Один сдвиг translate: «-50%» — проценты своего размера (×100), «10px» — точки. */
static void translate_one(const char *w, int y) {
    char u[24];
    int neg = *w == '-', x;
    const char *z = w + (*w == '-' || *w == '+');
    size_t l = strlen(z);
    if (l > 1 && z[l - 1] == '%' && l < sizeof(u)) {
        memcpy(u, z, l - 1);
        u[l - 1] = 0;
        int k = x100(u);
        if (k < 0) return;
        if (neg) k = -k;
        if (y) bs.typ = k; else bs.txp = k;
        return;
    }
    if (css_len(w, -1, &x) == 1) { if (y) bs.ty = x; else bs.tx = x; }
}

/* transform: … translate(x, y) / translateX(x) / translateY(y) / translate3d(x, y, z) … — сдвиг
 * (bs.tx, bs.ty — точки; bs.txp, bs.typ — проценты своего размера). scale, rotate — мимо. */
static void css_translate(const char *v) {
    bs.tx = bs.ty = bs.txp = bs.typ = 0;
    for (const char *p = strstr(v, "translate"); p; p = strstr(p + 9, "translate")) {
        const char *q = p + 9;
        int ax = 0;                                   /* 0 — x и y, 1 — только x, 2 — только y */
        if (*q == 'x') { ax = 1; q++; }
        else if (*q == 'y') { ax = 2; q++; }
        else if (!strncmp(q, "3d", 2)) q += 2;
        if (*q != '(') continue;
        q++;
        for (int k = 0; k < 2; k++) {
            char w[40];
            size_t t = 0;
            int d = 0;
            while (*q == ' ') q++;
            while (*q && (d || (*q != ',' && *q != ')'))) {
                if (*q == '(') d++;
                else if (*q == ')') d--;
                if (t < sizeof(w) - 1) w[t++] = *q;
                q++;
            }
            while (t && w[t - 1] == ' ') t--;
            w[t] = 0;
            translate_one(w, ax == 2 || (ax == 0 && k == 1));
            if (ax || *q != ',') break;
            q++;
        }
    }
}

/* Свойства CSS (строка «color: red; …») — в запись стиля e и коробку bx. Возвращает 1, если
 * элемент скрыт (visibility: hidden, opacity: 0, text-indent далеко влево). */
static int apply_decls(st_t *e, const char *css, int blockish) {
    char v[96];
    int x;
    dmask_make(css);
    cur_fg = e->fg;
    if (get(css, "display", v, sizeof(v))) {
        /* скрыт ли тег, решает последнее display (style="display:block" тега главнее правила
         * «div{display:none}» — так Google прячет всё, кроме своей надписи в <noscript>) */
        last_display = is(v, "none") ? 'n' : !strncmp(v, "inline", 6) || is(v, "contents") ? 'i' : is(v, "list-item") ? 'l' : 'b';
        bs.disp = (uint8_t)(is(v, "flex") ? 'f' : is(v, "grid") ? 'g' : 0);
        if (last_display == 'n') return 0;
    }
    if (get(css, "visibility", v, sizeof(v)) && (is(v, "hidden") || is(v, "collapse"))) return 1;
    if (get(css, "opacity", v, sizeof(v))) {          /* совсем прозрачный — не виден */
        int nz = 0, digits = 0;
        for (const char *p = v; *p && *p != '%'; p++) { if (*p >= '1' && *p <= '9') nz = 1; if (*p >= '0' && *p <= '9') digits = 1; }
        if (digits && !nz) return 1;
    }
    uint32_t c;
    int none;
    if (get(css, "color", v, sizeof(v)) && css_rgb(v, e->fg, e->bg ? e->bg : doc_bg(), &c)) e->fg = cur_fg = c;
    for (int k = 0; k < 2; k++) {                     /* background, потом background-color (главнее) */
        if (!get(css, k ? "background-color" : "background", v, sizeof(v)) || !css_bg(v, &c, &none)) continue;
        if (!blockish) e->bg = none ? 0 : c;
        else { bx.bg = none ? 0 : c; bs.bg = !none; }   /* блок — фоном коробки (open_box) */
    }
    css_box(css, blockish);
    if (get(css, "font", v, sizeof(v))) css_font(e, v);
    if (get(css, "font-weight", v, sizeof(v))) {
        int b = weight_bold(v);
        if (b > 0) e->flags |= PAGE_BOLD;
        else if (!b) e->flags &= (uint8_t)~PAGE_BOLD;
    }
    if (get(css, "text-decoration", v, sizeof(v)) || get(css, "text-decoration-line", v, sizeof(v))) {
        if (strstr(v, "none")) e->flags &= (uint8_t)~(PAGE_UNDERLINE | PAGE_STRIKE);
        if (strstr(v, "underline")) e->flags |= PAGE_UNDERLINE;
        if (strstr(v, "line-through")) e->flags |= PAGE_STRIKE;
    }
    if (get(css, "font-size", v, sizeof(v))) {
        int f = css_face(v, e->face);
        if (f >= 0) e->face = (uint8_t)f;
    }
    if (blockish == 1 && get(css, "text-align", v, sizeof(v))) set_align(v);
    if (get(css, "text-transform", v, sizeof(v)))
        e->tt = (uint8_t)(is(v, "uppercase") ? 1 : is(v, "lowercase") ? 2 : is(v, "capitalize") ? 3 : 0);
    int ls;
    if ((get(css, "list-style-type", v, sizeof(v)) || get(css, "list-style", v, sizeof(v))) && (ls = list_style(v)) >= 0)
        e->ls = (uint8_t)ls;
    if (get(css, "white-space", v, sizeof(v))) {
        if (blockish == 1) pre = !strncmp(v, "pre", 3) || is(v, "break-spaces");
        e->nowrap = is(v, "nowrap") || is(v, "pre");                /* наследуется: копия записи стека */
    }
    /* flex и grid: контейнер (направление, перенос, промежутки, свободное место) и ребёнок */
    if (get(css, "flex-direction", v, sizeof(v))) bs.fcol = !strncmp(v, "column", 6);
    if (get(css, "flex-wrap", v, sizeof(v))) bs.wrap = !strncmp(v, "wrap", 4);
    if (get(css, "flex-flow", v, sizeof(v))) { bs.fcol = strstr(v, "column") != NULL; bs.wrap = strstr(v, "wrap") && !strstr(v, "nowrap"); }
    if (get(css, "gap", v, sizeof(v)) || get(css, "grid-gap", v, sizeof(v))) {
        char w[48];
        const char *p = v;
        if (word(&p, w, sizeof(w)) && css_len(w, len_wbase, &x) == 1) bs.rgap = bs.cgap = x > 0 ? (x > 200 ? 200 : x) : 0;
        if (word(&p, w, sizeof(w)) && css_len(w, len_wbase, &x) == 1) bs.cgap = x > 0 ? (x > 200 ? 200 : x) : 0;
    }
    if ((get(css, "column-gap", v, sizeof(v)) || get(css, "grid-column-gap", v, sizeof(v))) && css_len(v, len_wbase, &x) == 1)
        bs.cgap = x > 0 ? (x > 200 ? 200 : x) : 0;
    if ((get(css, "row-gap", v, sizeof(v)) || get(css, "grid-row-gap", v, sizeof(v))) && css_len(v, len_wbase, &x) == 1)
        bs.rgap = x > 0 ? (x > 200 ? 200 : x) : 0;
    if (get(css, "justify-content", v, sizeof(v)))
        bs.just = (uint8_t)(is(v, "center") ? 1 : is(v, "flex-end") || is(v, "end") || is(v, "right") ? 2 :
                            is(v, "space-between") ? 3 : is(v, "space-around") ? 4 : is(v, "space-evenly") ? 5 : 0);
    if (get(css, "align-items", v, sizeof(v))) bs.aic = is(v, "center");
    if (get(css, "flex", v, sizeof(v))) css_flex(v);
    int k;
    if (get(css, "flex-grow", v, sizeof(v)) && (k = x100(v)) >= 0) bs.grow = k;
    if (get(css, "flex-shrink", v, sizeof(v)) && (k = x100(v)) >= 0) bs.shrink = k;
    if (get(css, "flex-basis", v, sizeof(v))) {
        int r = css_len(v, len_wbase, &x);
        if (r) bs.basis = r > 0 ? (x > 0 ? x : 0) : -1;
        else if (is(v, "content")) bs.basis = -1;
    }
    if (get(css, "order", v, sizeof(v))) bs.order = v[0] == '-' ? -num(v + 1) : num(v);
    if (get(css, "border-collapse", v, sizeof(v))) bs.collapse = is(v, "collapse");
    if (get(css, "border-spacing", v, sizeof(v)) && css_len(v, -1, &x) == 1) { bs.bspace = x < 0 ? 0 : x > 32 ? 32 : x; bs.bspace_set = 1; }
    if (get(css, "grid-template-columns", v, sizeof(v))) snprintf(bs.gcols, sizeof(bs.gcols), "%s", is(v, "none") ? "" : v);
    if (get(css, "text-indent", v, sizeof(v)) && css_len(v, len_wbase, &x) == 1) {
        if (x <= -500) return 1;                      /* текст «уехал» за левый край — его не видно */
        bs.indent = x > 0 ? (x > 200 ? 200 : x) : 0;
    }
    if (get(css, "position", v, sizeof(v))) {
        bs.abs = is(v, "absolute");
        bs.fixed = is(v, "fixed");
        bs.rel = is(v, "relative") || is(v, "sticky");
    }
    if (get(css, "inset", v, sizeof(v))) css_inset(v);
    if (get(css, "top", v, sizeof(v)) && css_len(v, len_hbase, &x) == 1) { bs.tset = 1; bs.tv = x; }
    if (get(css, "bottom", v, sizeof(v)) && css_len(v, len_hbase, &x) == 1) { bs.bset = 1; bs.bv = x; }
    if (get(css, "transform", v, sizeof(v))) css_translate(v);
    if (get(css, "left", v, sizeof(v)) && css_len(v, len_wbase, &x) == 1) { bs.lset = 1; bs.lv = x; }
    if (get(css, "right", v, sizeof(v)) && css_len(v, len_wbase, &x) == 1) { bs.rset = 1; bs.rv = x; }
    if (get(css, "float", v, sizeof(v))) bs.flt = (uint8_t)(is(v, "left") || is(v, "inline-start") ? 'l' : is(v, "right") || is(v, "inline-end") ? 'r' : 0);
    if (get(css, "clear", v, sizeof(v)))
        bs.clr = (uint8_t)(is(v, "left") || is(v, "inline-start") ? 1 : is(v, "right") || is(v, "inline-end") ? 2 : is(v, "both") ? 3 : 0);
    if (get(css, "aspect-ratio", v, sizeof(v))) {    /* «3/2», «1.5», «auto 3/2» */
        const char *p = v;
        while (*p && !(*p >= '0' && *p <= '9')) p++;
        int a = x100(p) >= 0 ? x100(p) : num(p) * 100, b = 100;
        const char *sl = strchr(p, '/');
        if (sl) {
            const char *q = sl + 1;
            while (*q == ' ') q++;
            char t[16];
            size_t k = 0;
            while ((q[k] >= '0' && q[k] <= '9') || q[k] == '.') { if (k < sizeof(t) - 1) t[k] = q[k]; k++; }
            t[k < sizeof(t) ? k : sizeof(t) - 1] = 0;
            b = x100(t) > 0 ? x100(t) : 100;
            char u[16];
            k = 0;
            while ((p[k] >= '0' && p[k] <= '9') || p[k] == '.') { if (k < sizeof(u) - 1) u[k] = p[k]; k++; }
            u[k < sizeof(u) ? k : sizeof(u) - 1] = 0;
            a = x100(u) > 0 ? x100(u) : 0;
        }
        bs.ar = a > 0 && b > 0 ? (int)((int64_t)a * 1000 / b) : 0;
    }
    static const char *const offs[] = {"left", "top", "right", "inset-inline-start"};
    for (int k = 0; k < 4; k++)
        if (get(css, offs[k], v, sizeof(v)) && css_len(v, len_wbase, &x) == 1 && x <= -500) bs.off = 1;
    if (get(css, "clip", v, sizeof(v)) && strstr(v, "rect(")) {   /* rect(0 0 0 0), rect(1px, 1px, 1px, 1px) */
        int big = 0;
        for (const char *p = strstr(v, "rect(") + 5; *p && *p != ')'; p++) if (*p >= '2' && *p <= '9') big = 1;
        if (!big) bs.clip = 1;
    }
    if (get(css, "clip-path", v, sizeof(v)) && (strstr(v, "inset(50%") || strstr(v, "inset(100%") || strstr(v, "circle(0")))
        bs.clip = 2;
    if (get(css, "overflow", v, sizeof(v)) || get(css, "overflow-x", v, sizeof(v)) || get(css, "overflow-y", v, sizeof(v)))
        if (is(v, "hidden") || is(v, "clip")) bs.ovh = 1;
    if (!blockish) {
        int l = margin_left, r = e->mr;
        css_margins(css, &l, &r);
        margin_left = l;
        e->mr = (int16_t)r;
    }
    return 0;
}

/* Приёмы «спрятать от глаз» (CSS не скрывает элемент прямо, но его не видно): «только для
 * читалок экрана» — position: absolute и clip / размер 1 × 1; далеко за краем экрана
 * (left: -9999px); свёрнутый блок — height: 0 или max-height: 0 с overflow: hidden (без полей:
 * «height: 0; padding-bottom: 56%» — рамка видео или картинки нужной пропорции, она видна). */
static int box_hidden(void) {
    if (bs.clip == 2) return 1;
    if ((bs.abs || bs.fixed) && (bs.off || bs.clip == 1)) return 1;
    if ((bs.abs || bs.fixed) && bs.w && bs.h && bx.w <= 1 && bx.h <= 1) return 1;
    if (bs.ovh && bx.pt + bx.pb == 0 && ((bs.h && bx.h <= 1) || (bs.maxh && bs.maxh_v <= 1))) return 1;
    if (bs.ovh && bs.w && bx.w <= 1 && bx.pl + bx.pr == 0) return 1;
    return 0;
}

/* Коробка блока e из bx: есть поля, рамка, ширина, высота или скругление — коробка CSS
 * (doc_cssbox_begin, вложенные — можно); только фон — такая же коробка без полей (по CSS фон
 * полей не добавляет: их задаёт только padding).
 * Размеры — по внешнему краю (так их ждёт doc.c): к ширине содержимого (content-box) добавить
 * поля и рамку. min-width главнее max-width, как у браузеров. */
static void open_box(st_t *e) {
    if (bs.bstyle != 1) bx.bt = bx.br = bx.bb = bx.bl = 0;   /* стиль рамки не сказан — её нет */
    int pad_lr = bx.pl + bx.pr + bx.bl + bx.br, pad_tb = bx.pt + bx.pb + bx.bt + bx.bb, room = doc_width();
    int w = bs.w ? bx.w : 0, maxw = bs.maxw ? bx.maxw : 0, minw = bs.minw ? bs.minw_v : 0;
    int h = bs.h ? bx.h : 0, minh = bs.minh ? bs.minh_v : 0, maxh = bs.maxh ? bs.maxh_v : 0;
    if (!bs.border_box) {
        if (bs.w) w += pad_lr;
        if (maxw) maxw += pad_lr;
        if (minw) minw += pad_lr;
        if (h) h += pad_tb;
        if (minh) minh += pad_tb;
        if (maxh) maxh += pad_tb;
    }
    if (w && maxw && w > maxw) w = maxw;
    if (w && w < minw) w = minw;
    if (h < minh) h = minh;
    if (maxh && h > maxh) h = maxh;                    /* содержимое длиннее — всё равно видно */
    if (h > 4000) h = 4000;
    bx.w = w >= room ? 0 : w;                         /* во всю ширину — как без width */
    bx.maxw = maxw >= room ? 0 : maxw;
    bx.h = h;
    int any = bx.bt || bx.br || bx.bb || bx.bl || bx.pt || bx.pb || bx.pl || bx.pr || bx.w || bx.maxw || bx.h ||
              bx.radius || bx.mt || bx.mb || bx.ml || bx.mr || (bx.auto_l && bx.auto_r && (bx.w || bx.maxw)) ||
              (bs.bg && bx.bg);
    if (any) {
        if (!bs.bg) bx.bg = 0;
        if (!e->css && doc_cssbox_begin(&bx)) e->css = 1;
    }
    if (bs.indent > 0) doc_hang(-bs.indent);           /* первая строка — правее (text-indent) */
}

static int float_side;                   /* open_element → parse: плавающий блок — у правого края */
/* Ячейка таблицы (номер в стеке, render_range из table): поля, рамку и фон её CSS рисует таблица на
 * всю высоту строки — сама ячейка коробку не открывает. */
static int cell_flat = -1;
/* Раскладка детей flex и grid рядом — ниже (flex_row); apply_style только отмечает контейнер. */
static int flex_at = -1;                 /* номер в стеке контейнера, чьих детей пора ставить рядом */
static int flex_child = -1;              /* номер в стеке, где встанет ребёнок: его width — у колонки */
static struct {
    uint8_t grid, wrap, just, clip;      /* grid; перенос строк; justify-content (0 start, 1 center, 2 end,
                                          * 3 space-between, 4 space-around, 5 space-evenly); лишнее не видно */
    int cgap, rgap;                      /* промежутки между столбцами и строками */
    char cols[160];                      /* grid-template-columns */
} fc;

/* Свойства подошедших правил (list), style тега, потом всё с «!important» — в e, bx и bs. 1 — скрыт. */
static int run_decls(st_t *e, const char **list, int m, const char *style, int blockish) {
    int hidden = 0;
    for (int i = 0; i < m; i++) hidden |= apply_decls(e, list[i], blockish);
    if (style) hidden |= apply_decls(e, style, blockish);
    css_important = 1;                                /* второй проход: «!important» главнее всего */
    for (int i = 0; i < m; i++) if (strchr(list[i], '!')) hidden |= apply_decls(e, list[i], blockish);
    if (style && strchr(style, '!')) hidden |= apply_decls(e, style, blockish);
    css_important = 0;
    return hidden || last_display == 'n' || box_hidden();
}

/* --- position: absolute ---
 * Абсолютный элемент места в потоке не занимает: он встаёт относительно «коробки с полями» (padding
 * box) блока отсчёта — ближайшего предка с position (relative, absolute…), без него — экрана
 * 1024 × 768 от верха страницы. left/top (или right/bottom) — от его краёв; ширина — width или «всё
 * между left и right», без них — по содержимому; высота — height или «между top и bottom».
 * transform: translate() сдвигает ещё на точки или проценты своего размера (центровка «left: 50%;
 * transform: translateX(-50%)»). Высота блока отсчёта бывает не известна заранее (растёт по
 * содержимому, а оно ещё не разложено) — тогда сторона, которой нужна эта высота (bottom, top в
 * процентах), не задана: элемент — на том месте по высоте, где стоял бы в потоке, и его сдвиг
 * translate по высоте тоже не делается (он считается от того края, которого нет). */
typedef struct {
    int ok;                              /* элемент — absolute (последний apply_style) */
    int cx, cy, cw, ch;                  /* блок отсчёта (ch 0 — высота не известна) */
    int w, h;                            /* внешний размер (−1 — по содержимому) */
    uint8_t lset, rset, tset, bset;
    int lv, rv, tv, bv, tx, ty, txp, typ;
} absg_t;
static absg_t abs_g;

static void abs_box_geom(int cx, int cy, int cw, int ch) {
    absg_t *g = &abs_g;
    memset(g, 0, sizeof(*g));
    g->ok = 1;
    g->cx = cx; g->cy = cy; g->cw = cw; g->ch = ch;
    int pad_lr = bx.pl + bx.pr + (bs.bstyle == 1 ? bx.bl + bx.br : 0), pad_tb = bx.pt + bx.pb + (bs.bstyle == 1 ? bx.bt + bx.bb : 0);
    g->w = bs.w ? bx.w + (bs.border_box ? 0 : pad_lr) : bs.lset && bs.rset ? cw - bs.lv - bs.rv - bx.ml - bx.mr : -1;
    g->h = bs.h ? bx.h + (bs.border_box ? 0 : pad_tb) : bs.tset && bs.bset && ch > 0 ? ch - bs.tv - bs.bv : -1;
    if (g->w <= 0) g->w = -1;
    if (g->h <= 0) g->h = -1;
    g->lset = bs.lset; g->rset = bs.rset; g->tset = bs.tset; g->bset = bs.bset && ch > 0;
    g->lv = bs.lv + bx.ml; g->rv = bs.rv + bx.mr; g->tv = bs.tv + bx.mt; g->bv = bs.bv + bx.mb;
    g->tx = bs.tx; g->ty = bs.ty; g->txp = bs.txp; g->typ = bs.typ;
}

/* Где встать абсолютному размером w × h (h ≤ 0 — не известна): по left/right и top/bottom от блока
 * отсчёта; сторона не задана — где стоял бы в потоке (doc_here). */
static void abs_where(const absg_t *g, int w, int h, int *x, int *y) {
    int hx, hy, hw;
    doc_here(&hx, &hy, &hw);
    int yset = g->tset || (g->bset && h > 0);
    *x = g->lset ? g->cx + g->lv : g->rset ? g->cx + g->cw - g->rv - w : hx;
    *y = g->tset ? g->cy + g->tv : yset ? g->cy + g->ch - g->bv - h : hy;
    *x += g->tx + (int)((int64_t)w * g->txp / 10000);
    if (yset && h > 0) *y += g->ty + (int)((int64_t)h * g->typ / 10000);
}

/* --- элементы для селекторов: <html> и <body> — предки всех ---
 * Они не лежат в стеке тегов (у нас дно стека — стиль страницы по умолчанию), но правила
 * «body.home .menu», «.kit a» (класс у body) и переменные «:root { --x: … }» должны их видеть. */
static css_el_t html_el, body_el;
static int have_html, have_body;
static int root_depth = -1;              /* ≥ 0 — разбирается <html> (0) или <body> (1) */
static uint32_t html_bg;                 /* фон из правил для <html> (если у <body> своего нет) */
/* Атрибуты тега <html> (class, id, lang…): его стиль считается не сразу, а при первом элементе
 * страницы — когда таблицы стилей из <head> уже прочитаны (иначе «:root { --x: … }» из <style>
 * ещё неизвестен). */
static char html_attr[600];
static size_t html_an;
static int html_seen;
static void html_tag(const char *a, size_t n);

/* Атрибут элемента для селекторов [имя=значение] (css.h). */
typedef struct { const char *a; size_t n; } actx_t;
static int attr_cb(void *ctx, const char *name, char *out, size_t cap) {
    const actx_t *c = ctx;
    return c->a ? attr(c->a, c->n, name, out, cap) : 0;
}

/* Стиль тега name: подходящие правила таблиц стилей (css_match), потом style="…" самого тега,
 * потом всё «!important». Переменные --x из них — на глубину этого тега (видны ему и вложенным).
 * Возвращает 1, если тег скрыт. Большие буферы — static: стек задачи невелик, а сама функция
 * себя не вызывает (кроме html_tag в самом начале — до того, как буферы заняты). */
static int apply_style(st_t *e, const char *name, const char *a, size_t n, int blockish) {
    if (!have_html && root_depth < 0) html_tag(html_seen ? html_attr : NULL, html_an);
    static char cls[256], id[64], style[1024];
    static const char *list[96];
    last_display = 0;
    margin_left = 0;
    box_reset();
    cls[0] = id[0] = 0;
    if (a) { attr(a, n, "class", cls, sizeof(cls)); attr(a, n, "id", id, sizeof(id)); }
    css_el_make(&e->el, name, cls, id);
    /* родители — открытые теги под этим (сам тег, если он уже на стеке, родителем не считается) */
    int idx = e >= stk && e < stk + sn ? (int)(e - stk) : sn;
    int vd = root_depth >= 0 ? root_depth : idx + 2;  /* глубина переменных: html 0, body 1, теги — с 2 */
    const css_el_t *anc[ST_MAX + 2];
    int na = 0, wb = doc_measuring() ? -1 : doc_width(), hb = -1, wstop = 0, hstop = 0;
    if (root_depth < 0) {
        for (int k = idx - 1; k >= 1; k--) {
            const st_t *p = &stk[k];
            if (!p->anon) anc[na++] = &p->el;             /* обёртка колонки flex — не предок для CSS */
            /* проценты: ширина — у ближайшего строчного предка со своей шириной (иначе — место
             * в блоке), высота — у ближайшего предка со своей высотой */
            if (!wstop) { if (p->cw > 0 && !p->block) { wb = p->cw; wstop = 1; } else if (p->block) wstop = 1; }
            if (!hstop) { if (p->ch > 0) { hb = p->ch; hstop = 1; } else if (p->block) hstop = 1; }
        }
        if (have_body) anc[na++] = &body_el;
    }
    if (have_html && root_depth != 0) anc[na++] = &html_el;
    len_wbase = wb;
    len_hbase = hb;
    actx_t ac = {a, n};
    int m = css_match(&e->el, attr_cb, &ac, anc, na, part_now, list, (int)(sizeof(list) / sizeof(list[0])));
#ifdef TSAFARI_HOST
    /* стенд: TSAFARI_STYLELOG=часть_класса — какие правила подошли таким элементам */
    if (getenv("TSAFARI_STYLELOG") && strstr(cls, getenv("TSAFARI_STYLELOG"))) {
        printf("style <%s class=\"%.60s\"> depth %d, %d ancestors, %d rules (doc %d, ext %d):\n", name, cls, idx, na, m,
               css_rules(0), css_rules(1));
        for (int i = 0; i < m; i++) printf("    %.150s\n", list[i]);
    }
#endif
    int has_style = a && attr(a, n, "style", style, sizeof(style));
    css_vars_cut(vd);
    for (int i = 0; i < m; i++) css_vars_take(list[i], vd);
    if (has_style) css_vars_take(style, vd);
    static st_t e0;                                   /* запись до правил — для второго прохода */
    e0 = *e;
    int hidden = run_decls(e, list, m, has_style ? style : NULL, blockish);
    /* position: absolute — проценты (left: 50%, width: 100%) от «коробки с полями» блока отсчёта:
     * ближайшего предка с position, без него — экрана; правила — ещё раз с этой основой (с записи,
     * какой она была до них: «font-size: 1.2em» дважды не умножать) */
    abs_g.ok = 0;
    if (bs.abs && !hidden && root_depth < 0) {
        int cx = 0, cy = 0, cwid = TS_W, chei = TS_H;
        for (int k = idx - 1; k >= 1; k--)
            if (stk[k].pos) { cx = stk[k].cbx; cy = stk[k].cby; cwid = stk[k].cbw; chei = stk[k].cbh > 0 ? stk[k].cbh : 0; break; }
        int hb2 = chei > 0 ? chei : -1;
        if (cwid != len_wbase || hb2 != len_hbase) {
            len_wbase = cwid;
            len_hbase = hb2;
            *e = e0;
            box_reset();
            last_display = 0;
            margin_left = 0;
            hidden = run_decls(e, list, m, has_style ? style : NULL, blockish);
        }
        if (!hidden) abs_box_geom(cx, cy, cwid, chei);
    }
    /* aspect-ratio: одна сторона задана — другая по пропорции (флажок 18 × 3/2 → 18 × 12) */
    if (bs.ar > 0 && bs.w && !bs.h) { bx.h = (int)((int64_t)bx.w * 1000 / bs.ar); bs.h = 1; }
    else if (bs.ar > 0 && bs.h && !bs.w && bx.h > 0) { bx.w = (int)((int64_t)bx.h * bs.ar / 1000); bs.w = 1; }
    /* position: relative — сдвиг на left (или назад на right); место в раскладке — прежнее */
    bs.dx = bs.rel ? (bs.lset ? bs.lv : bs.rset ? -bs.rv : 0) : 0;
    if (blockish == 1 && idx == flex_child) {         /* ребёнок flex или grid: его ширина — ширина колонки */
        bs.w = bs.maxw = bs.minw = bs.w_auto = 0;
        bx.auto_l = bx.auto_r = 0;
    }
    e->ovh = bs.ovh;
    if (!blockish) {                                  /* поля и рамка строчного тега — место слева и справа */
        int l = margin_left + bx.pl + (bs.bstyle == 1 ? bx.bl : 0), r = e->mr + bx.pr + (bs.bstyle == 1 ? bx.br : 0);
        margin_left = l > 64 ? 64 : l;
        e->mr = (int16_t)(r > 64 ? 64 : r);
    }
    /* своя ширина и высота содержимого — для процентов у вложенных (<span> шириной 20px, в нём
     * картинка width: 100% — тоже 20) */
    int pad_lr = bx.pl + bx.pr + (bs.bstyle == 1 ? bx.bl + bx.br : 0), pad_tb = bx.pt + bx.pb + (bs.bstyle == 1 ? bx.bt + bx.bb : 0);
    /* абсолютный между top и bottom (inset: 0) — такой высоты (его фон — во весь блок отсчёта) */
    if (abs_g.ok && abs_g.h > 0 && !bs.h) { bs.h = 1; bx.h = bs.border_box ? abs_g.h : abs_g.h - pad_tb; }
    int cw = bs.w ? bx.w - (bs.border_box ? pad_lr : 0) : 0, ch = bs.h ? bx.h - (bs.border_box ? pad_tb : 0) : 0;
    e->cw = (int16_t)(cw > 0 ? (cw > 4000 ? 4000 : cw) : 0);
    e->ch = (int16_t)(ch > 0 ? (ch > 4000 ? 4000 : ch) : 0);
    /* высота его «коробки с полями» — для абсолютных внутри (open_box bx меняет); без height —
     * пока только поля (cb_settle решит) */
    int cb_h = !bs.h ? -(bx.pt + bx.pb) : bs.border_box ? bx.h - (bs.bstyle == 1 ? bx.bt + bx.bb : 0) : bx.h + bx.pt + bx.pb;
    int flat = blockish == 1 && idx == cell_flat;    /* ячейка таблицы: коробку рисует table */
    if (blockish == 1 && !flat) {
        /* margin сверху и снизу из CSS заменяют промежутки блока по умолчанию (12 у абзаца…):
         * «p { margin: 0 }» — абзацы вплотную, как у браузеров */
        if (bs.mt && gap_before >= 0) doc_set_gap(gap_before);
        e->mb_css = (int16_t)(bs.mb ? bx.mb : -1);
    }
    gap_before = -1;
    if (blockish == 1 && !hidden && !flat) open_box(e);
    /* position задан — блок отсчёта для абсолютных внутри: его «коробка с полями» */
    if (!hidden && (bs.rel || bs.abs || bs.fixed)) {
        int hx, hy, hw;
        doc_here(&hx, &hy, &hw);
        if (blockish == 1 && e->css) { hx -= bx.pl; hy -= bx.pt; hw += bx.pl + bx.pr; }
        e->pos = 1;
        e->cbx = hx; e->cby = hy; e->cbw = hw;
        e->cbh = cb_h;
        if (!blockish && e->cw) e->cbw = e->cw;
    }
    /* колонка flex (column) с align-items: center — её дети по середине строки */
    if (blockish == 1 && !hidden && bs.disp == 'f' && bs.fcol && bs.aic) doc_align(DOC_CENTER);
    /* flex (в строку) или grid со столбцами: детей этого блока разложит рядом flex_row — parse или
     * render_range увидят flex_at сразу после того, как тег открыт */
    if (blockish == 1 && !hidden && idx == sn - 1 && flex_depth < NEST_MAX &&
        ((bs.disp == 'f' && !bs.fcol) || (bs.disp == 'g' && bs.gcols[0]))) {
        flex_at = idx;
        fc.grid = bs.disp == 'g';
        fc.wrap = bs.wrap;
        fc.just = bs.just;
        fc.cgap = bs.cgap;
        fc.rgap = bs.rgap;
        fc.clip = (uint8_t)(bs.ovh || (idx > 0 && stk[idx - 1].ovh));
        snprintf(fc.cols, sizeof(fc.cols), "%s", bs.gcols);
    }
    return hidden;
}

/* Блочный тег: закрыть незакрытый абзац, закончить строку, запомнить, что вернуть при закрытии. */
static st_t *open_block(const char *name, int gap, const char *a, size_t n) {
    close_p();
    block(0);
    gap_before = doc_get_gap();                       /* margin-top из CSS заменит gap (apply_style) */
    block(gap);
    st_t *e = push(name);
    if (!e) return NULL;
    e->block = 1;
    e->align = (int8_t)doc_get_align();
    e->indent = (int16_t)doc_get_indent();
    e->pre_was = (uint8_t)pre;
    char v[16];
    if (attr(a, n, "align", v, sizeof(v))) set_align(v);
    return e;
}

/* <html>: правила для html и :root — их переменные (--x) видны всей странице, цвет текста — всем,
 * фон — странице, если у <body> своего нет. a = NULL — тега <html> в странице нет. */
static void html_tag(const char *a, size_t n) {
    if (have_html) return;
    have_html = 1;
    st_t probe = stk[0];
    probe.bg = 0;
    root_depth = 0;
    apply_style(&probe, "html", a, n, 0);
    root_depth = -1;
    html_el = probe.el;
    html_bg = probe.bg;
    if (probe.fg != stk[0].fg) for (int k = 0; k < sn; k++) stk[k].fg = probe.fg;
}

/* <body>: цвета текста и ссылок, фон. У страницы из одного документа фон — у всей страницы; у
 * фрейма — прямоугольник под его частью составной страницы и своя рамка (для сглаживания). */
static void body_tag(const char *a, size_t n) {
    char v[48];
    int32_t c, bg = -1;
    if (!have_html) html_tag(html_seen ? html_attr : NULL, html_an);
    if (attr(a, n, "text", v, sizeof(v)) && (c = parse_color(v)) >= 0)
        for (int k = 0; k < sn; k++) stk[k].fg = (uint32_t)c;
    if (attr(a, n, "link", v, sizeof(v)) && (c = parse_color(v)) >= 0) link_rgb = (uint32_t)c;
    if (attr(a, n, "vlink", v, sizeof(v)) && (c = parse_color(v)) >= 0) vlink_rgb = (uint32_t)c;
    if (attr(a, n, "bgcolor", v, sizeof(v))) bg = parse_color(v);
    st_t probe = stk[0];                              /* таблицы стилей и style="…" у body */
    probe.bg = 0;
    root_depth = 1;
    apply_style(&probe, "body", a, n, 0);
    root_depth = -1;
    body_el = probe.el;
    have_body = 1;
    if (probe.fg != stk[0].fg) for (int k = 0; k < sn; k++) stk[k].fg = probe.fg;
    if (probe.bg) bg = (int32_t)probe.bg;
    else if (bg < 0 && html_bg) bg = (int32_t)html_bg;
    if (bg < 0 || doc_measuring()) return;
    body_bg = (uint32_t)bg;
    if (part_now < 0) {
        doc_set_bg((uint32_t)bg);
    } else if (body_rect < 0) {
        block(0);
        body_top = doc_y();
        body_rect = page_rect(doc_x(), body_top, doc_width(), 0, (uint32_t)bg);
        doc_frame_push(doc_x() + 8, doc_width() - 16, body_top + 8, (uint32_t)bg);
    }
}

/* <link rel=stylesheet href=… media=…>: адрес внешней таблицы стилей — в очередь (css.h); грузит
 * их app.c, когда страница уже на экране, и раскладывает страницу заново. «alternate stylesheet»
 * (запасной вид, его включает читатель) и disabled — не действуют, как у браузеров. */
static void link_tag(const char *a, size_t n) {
    static char v[URL_MAX], abs_url[URL_MAX];
    char rel[64] = "", md[128], dis[8];
    attr(a, n, "rel", rel, sizeof(rel));
    for (char *p = rel; *p; p++) *p = (char)lower((unsigned char)*p);
    int sheet = strstr(rel, "stylesheet") && !strstr(rel, "alternate");
    if (!sheet || attr(a, n, "disabled", dis, sizeof(dis))) return;
    if (attr(a, n, "media", md, sizeof(md)) && !css_media(md, strlen(md))) return;
    if (attr(a, n, "href", v, sizeof(v)) && v[0] && url_resolve(base, v, abs_url, sizeof(abs_url))) css_link(abs_url, part_now);
}

/* <img>: пока картинки не загружаются — подпись alt в квадратных скобках. */
/* --- формы (что это — в form.h) ---------------------------------------------------------------
 * Элемент формы стоит в строке как встроенный блок (doc_inline): его размер даёт form_size, а
 * заводится он (form_add) только при настоящей раскладке — при измерении ячеек таблицы нужно
 * лишь место. Скрытые элементы (type=hidden, display: none) места не занимают, но значение
 * отправляют, как у браузеров. */

/* Пробел перед элементом, если в тексте перед ним был пробел. */
static void inline_space(void) {
    if (space && !doc_empty()) {
        doc_style_t p = style_plain();
        doc_text(" ", 1, &p);
    }
    space = 0;
}

/* Буквы (не байты) в строке UTF-8. */
static int utf8_letters(const char *s) {
    int n = 0;
    for (; *s; s++) if (((uint8_t)*s & 0xC0) != 0x80) n++;
    return n;
}

/* Текст из куска страницы (содержимое <option>, <textarea>, <button>) в UTF-8: теги
 * выбрасываются, кодировка переводится, сущности раскрываются; keep = 1 — пробелы и переводы
 * строк как есть (textarea), иначе схлопываются. Строка живёт до следующего вызова. */
#define CT_RAW 4096
static char *ct_mem;                     /* сырые байты (CT_RAW) и текст UTF-8 (до 3 × CT_RAW) */
static const char *content_text(const uint8_t *p, size_t n, int keep) {
    if (!ct_mem && !(ct_mem = ts_alloc(4 * CT_RAW + 1))) return "";
    char *raw = ct_mem, *out = ct_mem + CT_RAW;
    size_t r = 0;
    for (size_t i = 0; i < n && r < CT_RAW; i++) {
        if (p[i] == '<') {
            char nm[16];
            int cl;
            size_t a0, a1, j = read_tag(p, n, i, nm, &cl, &a0, &a1);
            if (j) { i = j - 1; continue; }
        }
        raw[r++] = (char)p[i];
    }
    size_t len = entities_decode(out, charset_to_utf8(cs, (const uint8_t *)raw, r, out));
    if (!keep) {                                       /* пробелы подряд — один, по краям — прочь */
        size_t o = 0;
        int sp = 0;
        for (size_t i = 0; i < len; i++) {
            char c = out[i];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { sp = o > 0; continue; }
            if (sp) out[o++] = ' ';
            sp = 0;
            out[o++] = c;
        }
        len = o;
    }
    out[len] = 0;
    return out;
}

/* Поставить элемент в строку (видимый) или только завести (скрытый). */
static void control(const form_new_t *f, int hidden) {
    int w, h, d, max_w = doc_width();
    form_size(f, max_w, &w, &h, &d);
    int id = doc_measuring() ? -1 : form_add(f, max_w);
    if (hidden) return;
    shown_n = 1;
    inline_space();
    doc_inline(w, h, d, id);
}

static int look_tt;                      /* text-transform элемента формы (css_look) — для подписи */
static int ctl_fill;                     /* следующее поле формы — шириной во всё место (ребёнок flex) */
/* Вид элемента формы с сайта: ширина и высота, фон и цвет текста — из таблиц стилей и style,
 * «border: 0» / «none» — без рамки. Возвращает 1, если элемент скрыт (display: none). */
static int css_look(form_new_t *f, const char *name, const char *a, size_t n) {
    st_t probe = *top();
    probe.bg = 0;
    int hidden = apply_style(&probe, name, a, n, 0);
    f->fill = probe.bg;
    f->ink = probe.fg != top()->fg ? probe.fg : 0;
    if (bs.w && bx.w > 0) f->width = bx.w < doc_width() ? bx.w : doc_width();
    if (ctl_fill && !doc_measuring()) f->width = doc_width();   /* ребёнок flex — во всю колонку */
    ctl_fill = 0;
    look_tt = probe.tt;
    if (bs.h && bx.h > 0) f->height = bx.h;
    if (bs.bnone) f->no_border = 1;
    return hidden;
}

/* <form>: куда (action; пусто — адрес страницы), каким способом (method), куда показать ответ
 * (target). Формы заводятся только при настоящей раскладке. */
static void form_tag(const char *a, size_t n) {
    if (doc_measuring()) return;
    static char v[URL_MAX], abs_url[URL_MAX];         /* static — см. «стек» в tag() */
    char tg[32], m[8] = "";
    if (!attr(a, n, "action", v, sizeof(v)) || !v[0] || !url_resolve(base, v, abs_url, sizeof(abs_url)))
        snprintf(abs_url, sizeof(abs_url), "%s", base);
    attr(a, n, "method", m, sizeof(m));
    if (!attr(a, n, "target", tg, sizeof(tg))) snprintf(tg, sizeof(tg), "%s", base_target);
    form_open(abs_url, lower((unsigned char)m[0]) == 'p', tg, part_now, cs);
}

/* <input>: вид по type (незнакомый — строка текста, как у браузеров: email, search, number…). */
static void input_tag(const char *a, size_t n) {
    char type[24] = "text", name[128] = "", value[512], ph[160] = "", alt[160] = "", v[16];
    attr(a, n, "type", type, sizeof(type));
    for (char *p = type; *p; p++) *p = (char)lower((unsigned char)*p);
    form_new_t f;
    memset(&f, 0, sizeof(f));
    f.type = is(type, "password") ? FC_PASSWORD : is(type, "checkbox") ? FC_CHECKBOX : is(type, "radio") ? FC_RADIO :
             is(type, "submit") ? FC_SUBMIT : is(type, "reset") ? FC_RESET : is(type, "button") ? FC_BUTTON :
             is(type, "image") ? FC_IMAGE : is(type, "hidden") ? FC_HIDDEN : is(type, "file") ? FC_FILE : FC_TEXT;
    attr(a, n, "name", name, sizeof(name));
    f.name = name;
    if (attr(a, n, "value", value, sizeof(value))) f.value = value;
    if (attr(a, n, "placeholder", ph, sizeof(ph)) && ph[0]) f.placeholder = ph;
    f.checked = attr(a, n, "checked", v, sizeof(v));
    f.disabled = attr(a, n, "disabled", v, sizeof(v));
    f.readonly = attr(a, n, "readonly", v, sizeof(v));
    if (attr(a, n, "size", v, sizeof(v))) f.size = num(v);
    if (attr(a, n, "maxlength", v, sizeof(v))) f.maxlength = num(v);
    if (f.type == FC_IMAGE) f.label = attr(a, n, "alt", alt, sizeof(alt)) && alt[0] ? alt : "Submit";
    int hidden = css_look(&f, "input", a, n);
    if (f.value && (f.type == FC_SUBMIT || f.type == FC_BUTTON || f.type == FC_RESET)) tt_apply(value, strlen(value), look_tt);
    control(&f, f.type == FC_HIDDEN || hidden);
}

/* Пункты <option> (и заголовки <optgroup> — недоступными пунктами) внутри <select>: id ≥ 0 —
 * добавить их в список; возвращает длину самой длинной подписи в буквах. */
static int options(const uint8_t *b, size_t n, int id) {
    int longest = 0;
    size_t i = 0;
    char label[256], value[256], v[8];
    while (i < n) {
        if (b[i] != '<') { i++; continue; }
        char nm[16];
        int cl;
        size_t a0, a1, j = read_tag(b, n, i, nm, &cl, &a0, &a1);
        if (!j) { i++; continue; }
        if (cl || (!is(nm, "option") && !is(nm, "optgroup"))) { i = j; continue; }
        const char *a = (const char *)b + a0;
        size_t an = a1 - a0;
        if (is(nm, "optgroup")) {
            if (attr(a, an, "label", label, sizeof(label)) && label[0]) {
                int k = utf8_letters(label);
                if (k > longest) longest = k;
                if (id >= 0) form_option(id, label, "", 0, 1);
            }
            i = j;
            continue;
        }
        size_t k = j;                                  /* текст пункта — до следующего пункта */
        while (k < n) {
            char m[16];
            int c2;
            size_t x0, x1;
            if (b[k] == '<' && read_tag(b, n, k, m, &c2, &x0, &x1) && (is(m, "option") || is(m, "optgroup") || is(m, "select")))
                break;
            k++;
        }
        if (!attr(a, an, "label", label, sizeof(label)) || !label[0])
            snprintf(label, sizeof(label), "%s", content_text(b + j, k - j, 0));
        int hv = attr(a, an, "value", value, sizeof(value));
        int l = utf8_letters(label);
        if (l > longest) longest = l;
        if (id >= 0) form_option(id, label, hv ? value : NULL, attr(a, an, "selected", v, sizeof(v)), attr(a, an, "disabled", v, sizeof(v)));
        i = k;
    }
    return longest;
}

/* <select>…</select>: список выбора; content — байты между тегами. */
NOINLINE static void select_tag(const char *a, size_t an, const uint8_t *content, size_t n) {
    char name[128] = "", v[8];
    form_new_t f;
    memset(&f, 0, sizeof(f));
    f.type = FC_SELECT;
    attr(a, an, "name", name, sizeof(name));
    f.name = name;
    f.disabled = attr(a, an, "disabled", v, sizeof(v));
    f.cols = options(content, n, -1);
    int hidden = css_look(&f, "select", a, an);
    int w, h, d, max_w = doc_width();
    form_size(&f, max_w, &w, &h, &d);
    int id = doc_measuring() ? -1 : form_add(&f, max_w);
    if (id >= 0) options(content, n, id);
    if (hidden) return;
    shown_n = 1;
    inline_space();
    doc_inline(w, h, d, id);
}

/* <textarea>…</textarea>: текст внутри — как есть (перевод строки сразу за тегом — мимо). */
NOINLINE static void textarea_tag(const char *a, size_t an, const uint8_t *content, size_t n) {
    char name[128] = "", ph[160] = "", v[16];
    form_new_t f;
    memset(&f, 0, sizeof(f));
    f.type = FC_TEXTAREA;
    attr(a, an, "name", name, sizeof(name));
    f.name = name;
    if (attr(a, an, "placeholder", ph, sizeof(ph)) && ph[0]) f.placeholder = ph;
    if (attr(a, an, "rows", v, sizeof(v))) f.rows = num(v);
    if (attr(a, an, "cols", v, sizeof(v))) f.cols = num(v);
    if (attr(a, an, "maxlength", v, sizeof(v))) f.maxlength = num(v);
    f.disabled = attr(a, an, "disabled", v, sizeof(v));
    f.readonly = attr(a, an, "readonly", v, sizeof(v));
    int hidden = css_look(&f, "textarea", a, an);
    const char *text = content_text(content, n, 1);
    if (text[0] == '\r') text++;
    if (text[0] == '\n') text++;
    f.value = text;
    control(&f, hidden);
}

/* <button>…</button>: подпись — текст внутри (картинки и теги — мимо), вид — по type
 * (по умолчанию «отправить»). */
NOINLINE static void button_tag(const char *a, size_t an, const uint8_t *content, size_t n) {
    char type[16] = "submit", name[128] = "", value[256] = "", label[256], v[8];
    attr(a, an, "type", type, sizeof(type));
    for (char *p = type; *p; p++) *p = (char)lower((unsigned char)*p);
    form_new_t f;
    memset(&f, 0, sizeof(f));
    f.type = is(type, "reset") ? FC_RESET : is(type, "button") ? FC_BUTTON : FC_SUBMIT;
    attr(a, an, "name", name, sizeof(name));
    f.name = name;
    attr(a, an, "value", value, sizeof(value));
    f.value = value;
    snprintf(label, sizeof(label), "%s", content_text(content, n, 0));
    if (!label[0] && !attr(a, an, "aria-label", label, sizeof(label))) attr(a, an, "title", label, sizeof(label));
    f.label = label[0] ? label : NULL;
    f.disabled = attr(a, an, "disabled", v, sizeof(v));
    int hidden = css_look(&f, "button", a, an);
    tt_apply(label, strlen(label), look_tt);
    control(&f, hidden);
}

/* Размер из атрибута width / height: точки или проценты места full; 0 — нет. */
static int size_attr(const char *a, size_t n, const char *name, int full) {
    char v[16];
    if (!attr(a, n, name, v, sizeof(v))) return 0;
    int x = num(v);
    return x <= 0 ? 0 : strchr(v, '%') ? full * x / 100 : x;
}

/* --- картинки data: (сами байты картинки — в адресе: «data:image/svg+xml,%3Csvg…», «data:image/
 * png;base64,iVBOR…») ---
 * Разбираются при раскладке в хранилище страницы (байты по ключу «data:XXXXXXXXXXXXXXXX» —
 * отпечаток адреса и его длина), а image.c и app.c работают с ключом, как с адресом: app.c,
 * увидев «data:», берёт байты отсюда (html_data_copy) вместо сети. Хранилище — до новой страницы. */
#define DATA_MAX 64
#define DATA_BYTES (2u << 20)
static struct { char key[24]; uint8_t *b; uint32_t n; } dimg[DATA_MAX];
static int dimg_n;
static uint32_t dimg_bytes;

void html_data_clear(void) {
    for (int i = 0; i < dimg_n; i++) ts_core->free(dimg[i].b);
    dimg_n = 0;
    dimg_bytes = 0;
}

uint8_t *html_data_copy(const char *key, size_t *len) {
    for (int i = 0; i < dimg_n; i++)
        if (!strcmp(dimg[i].key, key)) {
            uint8_t *c = ts_alloc(dimg[i].n ? dimg[i].n : 1);
            if (!c) return NULL;
            memcpy(c, dimg[i].b, dimg[i].n);
            *len = dimg[i].n;
            return c;
        }
    return NULL;
}

static int b64v(int c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
           c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
}

/* src тега (атрибуты a, n) — «data:…»: разобрать в хранилище, ключ — в key. 0 — не вышло. */
static int data_put(const char *a, size_t n, char *key, size_t cap) {
    char *s = ts_alloc(3 * n + 1);                    /* адрес целиком (он длиннее URL_MAX) */
    if (!s) return 0;
    attr(a, n, "src", s, 3 * n + 1);
    size_t sl = strlen(s);
    snprintf(key, cap, "data:%08x%08x", (unsigned)css_hash(s, sl), (unsigned)sl);
    for (int i = 0; i < dimg_n; i++) if (!strcmp(dimg[i].key, key)) { ts_core->free(s); return 1; }
    const char *comma = strchr(s, ',');
    int ok = 0;
    if (comma && dimg_n < DATA_MAX) {
        int b64 = strstr(s, ";base64") && strstr(s, ";base64") < comma;
        const char *p = comma + 1;
        uint8_t *out = ts_alloc(strlen(p) + 1);
        size_t o = 0;
        if (out) {
            if (b64) {
                uint32_t acc = 0;
                int bits = 0;
                for (; *p; p++) {
                    int v = b64v((unsigned char)*p);
                    if (v < 0) continue;                  /* «=», пробелы, переводы строк */
                    acc = acc << 6 | (uint32_t)v;
                    bits += 6;
                    if (bits >= 8) { bits -= 8; out[o++] = (uint8_t)(acc >> bits); }
                }
            } else {
                for (; *p; p++) {                         /* %XX — байт, остальное — как есть */
                    if (*p == '%' && hexd((unsigned char)p[1]) >= 0 && hexd((unsigned char)p[2]) >= 0) {
                        out[o++] = (uint8_t)(hexd((unsigned char)p[1]) << 4 | hexd((unsigned char)p[2]));
                        p += 2;
                    } else {
                        out[o++] = (uint8_t)*p;
                    }
                }
            }
            if (o && dimg_bytes + o <= DATA_BYTES) {
                snprintf(dimg[dimg_n].key, sizeof(dimg[dimg_n].key), "%.21s", key);   /* «data:» и 16 цифр */
                dimg[dimg_n].b = out;
                dimg[dimg_n].n = (uint32_t)o;
                dimg_n++;
                dimg_bytes += (uint32_t)o;
                ok = 1;
            } else {
                ts_core->free(out);
            }
        }
    }
    ts_core->free(s);
    return ok;
}

static void place_widget(int id, int x, int y);    /* ниже: встроенный блок — на место */

/* <img src=… alt=… width=… height=…>: место под картинку в строке (image.h); низ картинки — на
 * базовой линии, как у браузеров. Картинки без адреса (или с встроенными данными «data:») — их
 * подпись в квадратных скобках, как раньше. Буферы адресов — static (см. «стек» в tag()). */
static void image_tag(const char *a, size_t n) {
    static char src[URL_MAX], abs_url[URL_MAX];
    char alt[160] = "";
    attr(a, n, "alt", alt, sizeof(alt));
    st_t probe = *top();
    if (apply_style(&probe, "img", a, n, 0)) return;  /* скрыта правилами CSS */
    int flt = bs.flt, ml = margin_left, mr = probe.mr;
    static absg_t ag;                                 /* position: absolute — место от блока отсчёта (static: стек) */
    ag = abs_g;
    ag.ok = ag.ok && sn != flex_child;
    int got = attr(a, n, "src", src, sizeof(src));
    if (got && !strncmp(src, "data:", 5)) {           /* картинка в самом адресе — в хранилище (data_put) */
        if (!data_put(a, n, abs_url, sizeof(abs_url))) got = 0;
    } else if (got && (!src[0] || !url_resolve(base, src, abs_url, sizeof(abs_url)))) {
        got = 0;
    }
    if (!got) {
        if (alt[0]) {
            char b[166];
            snprintf(b, sizeof(b), "[%s]", alt);
            out_dim(b);
        }
        return;
    }
    int full = doc_width();
    int aw = size_attr(a, n, "width", full), ah = size_attr(a, n, "height", full);
    /* width и height из CSS (классы сайта, style тега) главнее атрибутов, как у браузеров; auto —
     * свой размер картинки (пропорция сохраняется); проценты — от ширины места или предка со
     * своей шириной (<span> 20 × 15, в нём картинка 100% × 100% — тоже 20 × 15) */
    if (bs.w) aw = bx.w;
    else if (bs.w_auto) aw = 0;
    /* height: auto при заданной ширине — высота по пропорции атрибутов width и height (так браузеры
     * держат место под ещё не пришедшую картинку) */
    int at_w = size_attr(a, n, "width", 0), at_h = size_attr(a, n, "height", 0);
    if (bs.h) ah = bx.h;
    else if (bs.h_auto) ah = aw > 0 && at_w > 0 && at_h > 0 ? (int)((int64_t)aw * at_h / at_w) : 0;
    if (ag.ok && ag.w > 0) aw = ag.w;                 /* между left и right */
    if (ag.ok && ag.h > 0) ah = ag.h;
    if (bs.minw && aw && aw < bs.minw_v) aw = bs.minw_v;
    int max_w = full;
    if (bs.maxw && bx.maxw > 0 && bx.maxw < max_w) max_w = bx.maxw;
    if (bs.maxh && bs.maxh_v > 0 && ah > bs.maxh_v) ah = bs.maxh_v;
    int w, h;
    int id = image_add(abs_url, alt, aw, ah, max_w, top()->link, !doc_measuring(), &w, &h);
    shown_n = 1;
    /* float у картинки: у края, текст обтекает её (и следующие абзацы — тоже, как у браузеров) */
    if (ag.ok) {                                      /* вне потока: места в строке не занимает */
        if (!doc_measuring() && id >= 0) {
            int x, y;
            abs_where(&ag, w, h, &x, &y);
            place_widget(id, x, y);
        }
        return;
    }
    if (flt && !doc_measuring() && sn != flex_child && id >= 0) {
        int x, y;
        doc_float_place(w + ml + mr, flt == 'r', &x, &y);
        place_widget(id, x + ml, y);
        doc_float_add(x, y, w + ml + mr, h + 4, flt == 'r');
        return;
    }
    inline_space();
    doc_inline(w, h, 0, id);
}

/* Встроенные блоки страницы: номер меньше FORM_ID_LIMIT — элемент формы, от IMAGE_ID_BASE —
 * картинка. */
static void place_widget(int id, int x, int y) {
    if (id >= 0 && id < FORM_ID_LIMIT) form_place(id, x, y, doc_bg());
    else if (id >= IMAGE_ID_BASE) image_place(id, x, y, doc_bg());
}
static void paint_widget(int id, int x, int y, int w, int h, uint32_t bg) {
    if (id >= 0 && id < FORM_ID_LIMIT) form_paint(id, x, y, w, h, bg);
    else if (id >= IMAGE_ID_BASE) image_paint(id, x, y, w, h, bg);
}

/* Номер пункта n маркером вида ls: «3. », «c. », «C. », «iii. », «III. ». */
static int list_marker(int ls, int n, char *o) {
    char b[24];
    int k = 0;
    if ((ls == LS_LALPHA || ls == LS_UALPHA) && n > 0) {
        char t[8];
        int m = 0;
        while (n > 0 && m < 7) { n--; t[m++] = (char)((ls == LS_UALPHA ? 'A' : 'a') + n % 26); n /= 26; }
        while (m) b[k++] = t[--m];
    } else if ((ls == LS_LROMAN || ls == LS_UROMAN) && n > 0 && n < 4000) {
        static const int val[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
        static const char *const sym[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
        for (int i = 0; i < 13; i++)
            while (n >= val[i]) {
                for (const char *q = sym[i]; *q; q++) b[k++] = (char)(ls == LS_UROMAN ? *q - 32 : *q);
                n -= val[i];
            }
    } else {
        k = snprintf(b, sizeof(b), "%d", n);
    }
    b[k] = 0;
    return snprintf(o, 24, "%s. ", b);
}

/* Открывающий тег. Возвращает 1, если тег скрыт (display: none). */
static int open_element(const char *name, const char *a, size_t n) {
    static char v[URL_MAX];                           /* static — см. «стек» в tag() */
    st_t *e;
    if (is(name, "br")) { space = 0; if (doc_empty()) doc_flush(PAGE_LINE_H); else doc_flush(0); return 0; }
    if (is(name, "hr")) { close_p(); block(0); doc_rule(COL_RULE, 1, 12); return 0; }
    if (is(name, "img")) { image_tag(a, n); return 0; }
    if (is(name, "input")) { input_tag(a, n); return 0; }
    if (is(name, "link")) { link_tag(a, n); return 0; }
    if (is(name, "html")) {                            /* стиль — позже (html_attr) */
        if (!html_seen && !have_html && n < sizeof(html_attr)) { memcpy(html_attr, a, n); html_an = n; html_seen = 1; }
        return 0;
    }
    if (is(name, "form")) form_tag(a, n);              /* и дальше — как блок */
    if (is_any(name, blocks) || is(name, "p") || is(name, "center") || is(name, "li") || is(name, "ul") ||
        is(name, "ol") || is(name, "dl") || (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2])) {
        /* блок, которому сайт сказал display: inline («<form style="display:inline">» в строке
         * меню vtrd.in) — строчный тег: без новой строки и промежутков */
        st_t probe = *top();
        int hidden = apply_style(&probe, name, a, n, 0);
        if (!hidden && abs_g.ok && sn != flex_child && flex_depth < NEST_MAX) return 4;   /* position: absolute */
        if (!hidden && bs.clr) doc_float_clear(0, bs.clr);                /* clear: ниже плавающих */
        /* float: блок встанет у края, остальное обтечёт его (float_box из parse); ребёнок flex или
         * сам плавающий, которого сейчас раскладывают, — нет (float у них не действует) */
        if (!hidden && bs.flt && sn != flex_child && flex_depth < NEST_MAX) {
            float_side = bs.flt == 'r';
            return 3;
        }
        if (last_display == 'i') {
            if (!(e = push(name))) return 0;
            return apply_style(e, name, a, n, 0) || hidden;
        }
    }
    if (is_any(name, voids) || is(name, "html") || is(name, "head")) return 0;
    if (is(name, "body")) { body_tag(a, n); return 0; }
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        static const uint8_t faces[7] = {0, FACE_H1, FACE_H2, FACE_H3, FACE_BODY, FACE_SMALL, FACE_SMALL};
        int level = name[1] - '0';
        if (!(e = open_block(name, level <= 2 ? 18 : 14, a, n))) return 0;
        e->face = faces[level];
        if (level >= 4) e->flags |= PAGE_BOLD;
        return apply_style(e, name, a, n, 1);
    }
    if (is(name, "p")) {
        if (!(e = open_block(name, 12, a, n))) return 0;
        return apply_style(e, name, a, n, 1);
    }
    if (is(name, "center")) {
        if (!(e = open_block(name, 4, a, n))) return 0;
        doc_align(DOC_CENTER);
        return apply_style(e, name, a, n, 1);
    }
    if (is(name, "blockquote")) {                     /* цитата: светлая заливка и черта слева (Codex) */
        if (!(e = open_block(name, 10, a, n))) return 0;
        gap_before = -1;                              /* блок с фоном уже начат — промежуток прежний */
        if (!doc_measuring() && doc_box_begin(COL_SOFT, 16, 10, COL_LINK)) e->box = 1;
        return apply_style(e, name, a, n, 1);
    }
    if (is(name, "pre") || is(name, "listing") || is(name, "xmp") || is(name, "plaintext")) {
        if (!(e = open_block(name, 10, a, n))) return 0;
        pre = 1;                                      /* блок кода: заливка и поля 16 точек (Codex) */
        pre_open = 1;
        gap_before = -1;
        if (!doc_measuring() && doc_box_begin(COL_CODE, 16, 12, 0)) e->box = 1;
        return apply_style(e, name, a, n, 1);
    }
    if (is(name, "ul") || is(name, "ol") || is(name, "dir") || is(name, "menu")) {
        if (!(e = open_block(name, depth ? 2 : 8, a, n))) return 0;
        if (depth < 15) {
            depth++;
            e->list = 1;
            list_kind[depth] = name[0] == 'o';
            list_num[depth] = attr(a, n, "start", v, 16) ? num(v) - 1 : 0;
        }
        e->ls = LS_AUTO;                              /* у <ul>/<ol> маркер свой (так у браузеров) */
        int hidden = apply_style(e, name, a, n, 1);
        /* отступ списка — padding-left (у браузеров 40px, у нас LIST_INDENT); сайт сказал свой
         * padding — он уже в коробке списка (open_box) */
        if (!bs.pl) doc_indent(doc_get_indent() + LIST_INDENT);
        return hidden;
    }
    if (is(name, "li")) {                             /* прежний пункт этого списка — закрыть */
        int i = find_open("li");
        if (i >= 0) {
            int inner = 0;
            for (int k = i + 1; k < sn; k++) if (stk[k].list) inner = 1;
            if (!inner) { block(2); close_to(i); }
        }
        if (!(e = open_block("li", 4, a, n))) return 0;
        if (apply_style(e, name, a, n, 1)) return 1;     /* стиль пункта — до маркера: маркер его цвета */
        int x = 0;
        if (depth) {
            x = attr(a, n, "value", v, 16) ? num(v) : list_num[depth] + 1;
            list_num[depth] = x;
        }
        /* list-style: none или display не list-item (block, flex…) — без маркера, как у браузеров */
        int ls = e->ls != LS_AUTO ? e->ls : depth && list_kind[depth] ? LS_DEC : LS_DISC;
        if (ls == LS_NONE || (last_display && last_display != 'l')) return 0;
        /* Пункт без списка (<li> без <ul>, так меню у vtrd.in): отступ, как у пункта списка, —
         * иначе маркер висел бы левее края ячейки, за её фоном. */
        if (!depth) doc_indent(doc_get_indent() + LIST_INDENT);
        /* маркер «висит» слева от текста пункта: точка или номер. Точка — бирюзовая (дизайн
         * Codex), если у текста цвет страницы по умолчанию; у сайта со своими цветами — цвета
         * текста, как у браузеров (иначе на чужом фоне — чужие бирюзовые точки). */
        doc_hang(LIST_INDENT);
        doc_style_t st = style_plain();
        if (ls != LS_DISC) {
            char mark[24];
            int k = list_marker(ls, x ? x : 1, mark);
            doc_text(mark, (unsigned)k, &st);
        } else {
            if (st.fg == COL_TEXT) st.fg = COL_LINK;
            doc_text("• ", 4, &st);
        }
        return 0;
    }
    if (is(name, "dt") || is(name, "dd")) {
        int i = find_open("dt");
        if (i < 0) i = find_open("dd");
        if (i >= 0) { block(2); close_to(i); }
        if (!(e = open_block(name, name[1] == 't' ? 6 : 2, a, n))) return 0;
        if (name[1] == 't') e->flags |= PAGE_BOLD;
        else doc_indent(doc_get_indent() + LIST_INDENT);
        return apply_style(e, name, a, n, 1);
    }
    if (is_any(name, blocks) || is(name, "tr") || is(name, "td") || is(name, "th") || is(name, "table")) {
        /* таблица глубже TABLE_DEPTH_MAX — просто блоками */
        if (!(e = open_block(name, 4, a, n))) return 0;
        int32_t c;
        if (attr(a, n, "bgcolor", v, 16) && (c = parse_color(v)) >= 0 && !doc_measuring() &&
            doc_box_begin((uint32_t)c, 4, 2, 0)) {
            e->box = 1;
            gap_before = -1;
        }
        return apply_style(e, name, a, n, 1);
    }
    /* строчные теги */
    if (!(e = push(name))) return 0;
    if (is(name, "b") || is(name, "strong")) e->flags |= PAGE_BOLD;
    else if (is(name, "u") || is(name, "ins")) e->flags |= PAGE_UNDERLINE;
    else if (is(name, "s") || is(name, "strike") || is(name, "del")) e->flags |= PAGE_STRIKE;
    else if (is(name, "small")) e->face = (uint8_t)face_step(e->face, -1);
    else if (is(name, "big")) e->face = (uint8_t)face_step(e->face, 1);
    else if (is(name, "sup") || is(name, "sub")) e->face = FACE_CAPTION;
    else if (is(name, "mark")) { e->bg = 0xFFE873; e->fg = 0x000001; }   /* как у браузеров: чёрным на жёлтом */
    else if (is(name, "font")) {
        int32_t c;
        if (attr(a, n, "color", v, 32) && (c = parse_color(v)) >= 0) e->fg = (uint32_t)c;
        if (attr(a, n, "size", v, 8)) {
            /* размеры 1…7, обычный — 3; «+1», «−1» — от обычного */
            static const uint8_t sizes[8] = {FACE_BODY, FACE_CAPTION, FACE_SMALL, FACE_BODY, FACE_H3, FACE_H2, FACE_H1, FACE_H1};
            int k = num(v + (v[0] == '+' || v[0] == '-')), sz = v[0] == '+' ? 3 + k : v[0] == '-' ? 3 - k : k;
            e->face = sizes[sz < 1 ? 1 : sz > 7 ? 7 : sz];
        }
    }
    static char a_url[URL_MAX];
    char tg[32];
    int href = 0;
    if (is(name, "a") && attr(a, n, "href", v, sizeof(v)) && url_resolve(base, v, a_url, sizeof(a_url))) {
        if (!attr(a, n, "target", tg, sizeof(tg))) snprintf(tg, sizeof(tg), "%s", base_target);
        e->fg = visited_cb && visited_cb(a_url) ? vlink_rgb : link_rgb;
        e->flags |= PAGE_UNDERLINE;
        href = 1;
    }
    int hidden = apply_style(e, name, a, n, 0);
    /* position: absolute — и строчный тег встаёт блоком на своё место (parse → abs_box); ссылку
     * тогда заведёт его разбор там */
    if (!hidden && abs_g.ok && sn - 1 != flex_child && flex_depth < NEST_MAX) {
        sn--;
        return 4;
    }
    if (href) e->link = (int16_t)(doc_measuring() ? -1 : add_link(a_url, tg));
    /* CSS сделал строчный тег блоком («a { display: block }», «span { display: flex }»): с новой
     * строки, со своей коробкой (поля, рамка, фон), дети flex — рядом */
    if (!hidden && (last_display == 'b' || last_display == 'l')) {
        space = 0;
        block(0);
        e->block = 1;
        e->align = (int8_t)doc_get_align();
        e->indent = (int16_t)doc_get_indent();
        e->pre_was = (uint8_t)pre;
        gap_before = doc_get_gap();
        return apply_style(e, name, a, n, 1);
    }
    if (margin_left > 0 && !hidden) doc_inline(margin_left, 0, 0, -1);   /* поле слева — пустой блок */
    return hidden;
}

/* Закрывающий тег: закончить строку (у блоков), снять запись тега и всё, что над ней.
 * Промежуток после блока — по виду тега. «</p>» без «<p>» — пустой абзац, как у браузеров. */
static void close_element(const char *name) {
    int gap = -1;
    if (is(name, "form") && !doc_measuring()) form_close();
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) gap = 10;
    else if (is(name, "p")) gap = 12;
    else if (is(name, "li") || is(name, "dd") || is(name, "dt")) gap = 2;
    else if (is(name, "ul") || is(name, "ol") || is(name, "dir") || is(name, "menu")) gap = depth > 1 ? 2 : 8;
    else if (is(name, "blockquote") || is(name, "pre") || is(name, "listing") || is(name, "xmp")) gap = 10;
    else if (is(name, "center") || is_any(name, blocks) || is(name, "tr") || is(name, "td") || is(name, "th") ||
             is(name, "table")) gap = 4;
    int i = find_open(name);
    if (i < 0) {
        if (is(name, "p")) block(12);
        return;
    }
    if (!stk[i].block && !stk[i].box) gap = -1;          /* блок с display: inline — без новой строки */
    else if (stk[i].block && stk[i].mb_css >= 0 && gap >= 0) gap = stk[i].mb_css;   /* margin-bottom из CSS */
    if (gap >= 0) block(gap);
    close_to(i);
    if (gap >= 0) block(gap);
}

/* Конец тега: позиция за «>», начиная с i. */
static size_t tag_end(const uint8_t *b, size_t n, size_t i) {
    while (i < n && b[i] != '>') i++;
    return i < n ? i + 1 : n;
}

/* Найти закрывающий «</name» для тега, открытого перед позицией i, с учётом вложенных таких же
 * тегов (div в div); комментарии и скрипты пропускаются. Позиция «<» или n. */
static size_t find_match(const uint8_t *b, size_t n, size_t i, const char *name) {
    int nest = 1;
    while (i < n) {
        if (b[i] != '<') { i++; continue; }
        size_t j = skip_markup(b, n, i);
        if (j) { i = j; continue; }
        char nm[16];
        int cl;
        size_t a0, a1;
        j = read_tag(b, n, i, nm, &cl, &a0, &a1);
        if (!j) { i++; continue; }
        if (!cl && (is(nm, "script") || is(nm, "style"))) { i = find_close(b, n, j, nm); continue; }
        if (is(nm, name)) {
            if (!cl) nest++;
            else if (--nest == 0) return i;
        }
        i = j;
    }
    return n;
}

/* --- таблицы ------------------------------------------------------------------------------------
 * Таблицу нельзя выводить потоком: ширина столбца зависит от всех его ячеек. Поэтому таблица
 * разбирается целиком (table):
 *   1) находим строки <tr> и ячейки <td>/<th> (у вложенных таблиц — свои, их пропускаем);
 *   2) меряем каждую ячейку (doc_measure_push): самое длинное слово — меньше нельзя, вся строка
 *      без переносов — больше не нужно;
 *   3) делим ширину между столбцами: всем хватает «без переносов» — так и даём; иначе каждому
 *      его минимум и остаток пропорционально тому, сколько ему не хватает;
 *   4) рисуем строку за строкой: каждая ячейка — своя рамка (doc_frame_push), высота строки — по
 *      самой высокой ячейке; фон ячейки — прямоугольник под ней, рамка таблицы (border) — тонкие
 *      линии, как в дизайне Codex, заголовок <th> — со светлой заливкой. */
#define TC_MAX 512
#define TROW_MAX 256
#define TCOL_MAX 32
typedef struct {
    uint32_t a, b;                       /* содержимое ячейки: байты [a, b) */
    const char *at;                      /* атрибуты её тега */
    uint16_t an, row, col, span;
    uint8_t th;
    int min, max, rect;
    uint8_t pt, pr, pb, pl;              /* поля ячейки (padding из CSS; без него — по таблице) */
    uint8_t bt, br, bb, bl;              /* рамка ячейки по сторонам (CSS) */
    uint32_t bc;                         /* её цвет */
    int32_t cbg;                         /* фон из CSS (−1 — нет) */
    int cssw;                            /* ширина из CSS (0 — нет) */
} tcell_t;
typedef struct {
    tcell_t cell[TC_MAX];
    struct { const char *at; uint16_t an; } row[TROW_MAX];
    int colmin[TCOL_MAX], colmax[TCOL_MAX], colw[TCOL_MAX], colx[TCOL_MAX + 1];
} table_t;

static void render_range(const uint8_t *b, size_t n, int bold, const char *tagname, const char *at, size_t an);

/* Смесь цветов: a и k/16 от b (заливка заголовка таблицы — фон страницы с примесью цвета текста:
 * на светлой странице светлая, на тёмной — тёмная, и текст читается на обеих). */
static uint32_t blend(uint32_t a, uint32_t b, int k) {
    uint32_t c = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int x = (int)((a >> sh) & 255), y = (int)((b >> sh) & 255);
        c |= (uint32_t)(x + (y - x) * k / 16) << sh;
    }
    return c ? c : 1;
}

/* Ширина из атрибута width: «680» — точки, «50%» — доля от full; 0 — не задана. */
static int width_attr(const char *a, size_t n, int full) {
    char v[16];
    if (!a || !attr(a, n, "width", v, sizeof(v))) return 0;
    int x = num(v);
    return strchr(v, '%') ? full * x / 100 : x;
}

NOINLINE static void table(const uint8_t *b, size_t ta, size_t tb, const char *attrs, size_t an) {
    table_t *t = ts_alloc(sizeof(table_t));
    if (!t) return;
    memset(t->colmin, 0, sizeof(t->colmin));
    memset(t->colmax, 0, sizeof(t->colmax));
    int nc = 0, row = -1, col = 0, ncols = 0, open = -1, form_end = 0;
    /* Таблица в таблице не в ячейке (<tr><table>… — ошибка в HTML, так у vtrd.in страница
     * релиза): браузеры выносят её перед внешней таблицей. Без этого всё содержимое пропадало. */
    struct { uint32_t a, b; } foster[8];
    int nfoster = 0;
    size_t cap_a = 0, cap_b = 0, i = ta;
    char v[16];
    /* 1) строки и ячейки */
    while (i < tb) {
        if (b[i] != '<') { i++; continue; }
        size_t j = skip_markup(b, tb, i);
        if (j) { i = j; continue; }
        char nm[16];
        int cl;
        size_t a0, a1;
        j = read_tag(b, tb, i, nm, &cl, &a0, &a1);
        if (!j) { i++; continue; }
        if (!cl && (is(nm, "script") || is(nm, "style"))) { i = find_close(b, tb, j, nm); continue; }
        if (!cl && is(nm, "table")) {
            size_t e = tag_end(b, tb, find_match(b, tb, j, "table"));
            if (open < 0 && nfoster < 8) { foster[nfoster].a = (uint32_t)i; foster[nfoster].b = (uint32_t)e; nfoster++; }
            i = e;
            continue;
        }
        int is_cell = is(nm, "td") || is(nm, "th"), is_row = is(nm, "tr");
        if ((is_cell || is_row || is(nm, "caption")) && open >= 0) { t->cell[open].b = (uint32_t)i; open = -1; }
        if (is_row && !cl) {
            if (row + 1 < TROW_MAX) {
                row++;
                col = 0;
                t->row[row].at = (const char *)b + a0;
                t->row[row].an = (uint16_t)(a1 - a0);
            }
        } else if (is_cell && !cl && nc < TC_MAX) {
            if (row < 0) { row = 0; t->row[0].at = NULL; t->row[0].an = 0; }
            int span = attr((const char *)b + a0, a1 - a0, "colspan", v, sizeof(v)) ? num(v) : 1;
            if (span < 1) span = 1;
            if (col + span > TCOL_MAX) span = TCOL_MAX - col;
            if (span > 0) {
                tcell_t *c = &t->cell[nc];
                c->a = (uint32_t)j; c->b = (uint32_t)tb;
                c->at = (const char *)b + a0; c->an = (uint16_t)(a1 - a0);
                c->row = (uint16_t)row; c->col = (uint16_t)col; c->span = (uint16_t)span;
                c->th = nm[1] == 'h';
                c->rect = -1;
                open = nc++;
                col += span;
                if (col > ncols) ncols = col;
            }
        } else if (is(nm, "caption")) {
            if (!cl) cap_a = j; else if (cap_a && !cap_b) cap_b = i;
        } else if (is(nm, "form") && open < 0) {
            /* <form> между строками таблицы (так часто на старых сайтах): форма открывается до
             * ячеек, а закрывается после всей таблицы — её элементы стоят в ячейках */
            if (!cl) form_tag((const char *)b + a0, a1 - a0);
            else form_end = 1;
        }
        i = j;
    }
    if (open >= 0) t->cell[open].b = (uint32_t)tb;
    int nrows = row + 1;
    for (int k = 0; k < nfoster; k++) render_range(b + foster[k].a, foster[k].b - foster[k].a, 0, "div", NULL, 0);
    if (!nc) { ts_core->free(t); if (form_end && !doc_measuring()) form_close(); return; }
    /* сама таблица — на стек, пока разбираются её ячейки: для правил «table td», «.news td» и
     * цвета текста, заданного таблице (фон таблицы — её прямоугольник, не фон под буквами) */
    st_t *te = push("table");
    /* CSS самой таблицы: ширина, рамка, промежуток между ячейками (border-spacing), слияние рамок */
    int css_w = 0, tbw[4] = {0, 0, 0, 0}, collapse = 0, css_space = -1;
    uint32_t tbc = 0;
    int32_t css_tbg = -1;
    if (te) {
        apply_style(te, "table", attrs, an, 0);
        if (te->bg) css_tbg = (int32_t)te->bg;
        te->bg = 0;
        if (bs.w && bx.w > 0) css_w = bx.w;
        if (bs.bstyle == 1) { tbw[0] = bx.bt; tbw[1] = bx.br; tbw[2] = bx.bb; tbw[3] = bx.bl; tbc = bx.bc; }
        collapse = bs.collapse;
        if (bs.bspace_set) css_space = bs.bspace;
    }

    int border = attr(attrs, an, "border", v, sizeof(v)) ? (v[0] ? num(v) : 1) : 0;
    int pad_x = border ? 12 : 6, pad_y = border ? 5 : 1;  /* без рамки — зазор, как у браузеров */
    if (attr(attrs, an, "cellpadding", v, sizeof(v))) pad_x = pad_y = num(v) > 24 ? 24 : num(v);
    int spacing = attr(attrs, an, "cellspacing", v, sizeof(v)) ? (num(v) > 16 ? 16 : num(v)) : 0;
    if (css_space >= 0) spacing = css_space;          /* CSS главнее атрибута */
    if (collapse) spacing = 0;
    int line = border ? 1 : 0, gap = spacing + line;
    int32_t tbg = attr(attrs, an, "bgcolor", v, sizeof(v)) ? parse_color(v) : -1;
    if (css_tbg >= 0) tbg = css_tbg;
    /* CSS ячеек: поля (padding — вместо полей таблицы), рамки по сторонам, фон, ширина. У слитых рамок
     * (border-collapse: collapse) левая и верхняя рамка ячейки — это правая и нижняя соседа: их нет */
    for (int k = 0; k < nc; k++) {
        tcell_t *c = &t->cell[k];
        st_t probe = *top();
        apply_style(&probe, c->th ? "th" : "td", c->at, c->an, 2);
        c->pt = c->pb = (uint8_t)pad_y;
        c->pl = c->pr = (uint8_t)pad_x;
        if (bs.pad) {
            c->pt = (uint8_t)(bx.pt > 60 ? 60 : bx.pt); c->pb = (uint8_t)(bx.pb > 60 ? 60 : bx.pb);
            c->pl = (uint8_t)(bx.pl > 60 ? 60 : bx.pl); c->pr = (uint8_t)(bx.pr > 60 ? 60 : bx.pr);
        }
        c->bt = c->br = c->bb = c->bl = 0;
        if (bs.bstyle == 1) {
            c->bt = (uint8_t)bx.bt; c->br = (uint8_t)bx.br; c->bb = (uint8_t)bx.bb; c->bl = (uint8_t)bx.bl;
            c->bc = bx.bc;
            if (collapse && c->col) c->bl = 0;
            if (collapse && c->row) c->bt = 0;
        }
        c->cbg = bs.bg && bx.bg ? (int32_t)bx.bg : -1;
        c->cssw = bs.w && bx.w > 0 ? bx.w + (bs.border_box ? 0 : c->pl + c->pr + c->bl + c->br) : 0;
    }
    if (collapse) { tbw[1] = tbw[2] = 0; }               /* правая и нижняя рамка таблицы — у ячеек */

    /* 2) мерки ячеек */
    for (int k = 0; k < nc; k++) {
        tcell_t *c = &t->cell[k];
        doc_measure_push();
        doc_frame_push(0, 30000, 0, 0);
        cell_flat = sn;
        render_range(b + c->a, c->b - c->a, c->th, c->th ? "th" : "td", c->at, c->an);
        cell_flat = -1;
        doc_frame_pop();
        doc_measure_pop(&c->min, &c->max);
        int extra = c->pl + c->pr + c->bl + c->br;
        c->min += extra;
        c->max += extra;
        int w = c->cssw ? c->cssw : width_attr(c->at, c->an, 0);
        if (w > c->min) c->min = w;
        if (w) c->max = w > c->min ? w : c->min;
        if (c->max < c->min) c->max = c->min;
    }
    for (int k = 0; k < nc; k++) {
        tcell_t *c = &t->cell[k];
        if (c->span != 1) continue;
        if (c->min > t->colmin[c->col]) t->colmin[c->col] = c->min;
        if (c->max > t->colmax[c->col]) t->colmax[c->col] = c->max;
    }
    for (int k = 0; k < nc; k++) {                    /* ячейки на несколько столбцов: недостачу поровну */
        tcell_t *c = &t->cell[k];
        if (c->span == 1) continue;
        int smin = (c->span - 1) * gap, smax = smin;
        for (int q = c->col; q < c->col + c->span; q++) { smin += t->colmin[q]; smax += t->colmax[q]; }
        for (int q = c->col; q < c->col + c->span; q++) {
            if (c->min > smin) t->colmin[q] += (c->min - smin) / c->span;
            if (c->max > smax) t->colmax[q] += (c->max - smax) / c->span;
        }
    }
    int summin = 0, summax = 0;
    for (int q = 0; q < ncols; q++) {
        if (t->colmax[q] < t->colmin[q]) t->colmax[q] = t->colmin[q];
        summin += t->colmin[q];
        summax += t->colmax[q];
    }
    int gaps = (ncols + 1) * gap + tbw[1] + tbw[3], avail = doc_width(), want = css_w ? css_w : width_attr(attrs, an, avail);
    if (doc_measuring()) {                            /* таблица внутри измеряемой ячейки */
        doc_measure_add(summin + gaps, (want > summin ? want : summax) + gaps);
        if (te) { css_vars_cut(sn + 1); sn--; }
        ts_core->free(t);
        return;
    }

    /* 3) ширины столбцов */
    int width = want ? (want > avail ? avail : want) : (summax + gaps > avail ? avail : summax + gaps);
    int inner = width - gaps;
    if (inner < ncols) inner = ncols;
    for (int q = 0; q < ncols; q++) {
        if (inner >= summax)
            t->colw[q] = t->colmax[q] + (want && summax ? (int)((int64_t)(inner - summax) * t->colmax[q] / summax) : 0);
        else if (inner > summin)
            t->colw[q] = t->colmin[q] + (int)((int64_t)(t->colmax[q] - t->colmin[q]) * (inner - summin) / (summax - summin));
        else
            t->colw[q] = summin ? (int)((int64_t)t->colmin[q] * inner / summin) : inner / ncols;
    }
    width = gaps;
    for (int q = 0; q < ncols; q++) width += t->colw[q];

    /* 4) вывод */
    int x0 = doc_x();
    if (attr(attrs, an, "align", v, sizeof(v))) {
        if (!strncmp(v, "center", 6)) x0 += (avail - width) / 2;
        else if (lower((unsigned char)v[0]) == 'r') x0 += avail - width;
    } else if (doc_get_align() == DOC_CENTER) {
        x0 += (avail - width) / 2;
    }
    if (x0 < doc_x()) x0 = doc_x();
    int y = doc_y();
    doc_skip_to(y);
    if (cap_b > cap_a) {
        doc_frame_push(x0, width, y, 0);
        doc_align(DOC_CENTER);
        render_range(b + cap_a, cap_b - cap_a, 0, "caption", NULL, 0);
        y = doc_frame_pop() + 4;
    }
    t->colx[0] = x0 + tbw[3] + gap;
    for (int q = 0; q < ncols; q++) t->colx[q + 1] = t->colx[q] + t->colw[q] + gap;
    if (border) page_rect(x0, y, width, 1, COL_LINE);
    int ytab = y;                                     /* верх таблицы — для её рамки CSS */
    y += gap + tbw[0];
    int k = 0;
    for (int r = 0; r < nrows; r++) {
        int top = y, bottom = y, first = k;
        const char *ra = t->row[r].at;
        int rn = t->row[r].an;
        int32_t rbg = ra && attr(ra, (size_t)rn, "bgcolor", v, sizeof(v)) ? parse_color(v) : -1;
        char ral[12] = "";
        if (ra) attr(ra, (size_t)rn, "align", ral, sizeof(ral));
        for (; k < nc && t->cell[k].row == r; k++) {
            tcell_t *c = &t->cell[k];
            int cx = t->colx[c->col], cw = t->colx[c->col + c->span] - cx - gap;
            int32_t bg = c->cbg >= 0 ? c->cbg : attr(c->at, c->an, "bgcolor", v, sizeof(v)) ? parse_color(v) : -1;
            if (bg < 0) bg = rbg;
            if (bg < 0) bg = tbg;
            if (bg < 0 && c->th && border) bg = (int32_t)blend(body_bg ? body_bg : page_bg(), stk[0].fg, 2);
            c->rect = bg >= 0 ? page_rect(cx, top, cw, 0, (uint32_t)bg) : -1;
            int inner_w = cw - c->pl - c->pr - c->bl - c->br;
            doc_frame_push(cx + c->bl + c->pl, inner_w > 1 ? inner_w : 1, top + c->bt + c->pt, bg >= 0 ? (uint32_t)bg : 0);
            char al[12];
            if (attr(c->at, c->an, "align", al, sizeof(al))) set_align(al);
            else if (ral[0]) set_align(ral);
            cell_flat = sn;
            render_range(b + c->a, c->b - c->a, c->th, c->th ? "th" : "td", c->at, c->an);
            cell_flat = -1;
            int bot = doc_frame_pop() + c->pb + c->bb;
            if (bot > bottom) bottom = bot;
        }
        for (int q = first; q < k; q++) if (t->cell[q].rect >= 0) page_rect_height(t->cell[q].rect, bottom - top);
        for (int q = first; q < k; q++) {             /* рамки ячеек CSS — на всю высоту строки */
            const tcell_t *c = &t->cell[q];
            int cx = t->colx[c->col], cw = t->colx[c->col + c->span] - cx - gap;
            if (c->bt) page_rect(cx, top, cw, c->bt, c->bc);
            if (c->bb) page_rect(cx, bottom - c->bb, cw, c->bb, c->bc);
            if (c->bl) page_rect(cx, top, c->bl, bottom - top, c->bc);
            if (c->br) page_rect(cx + cw - c->br, top, c->br, bottom - top, c->bc);
        }
        if (border) {
            for (int q = first; q < k; q++)
                if (t->cell[q].col) page_rect(t->colx[t->cell[q].col] - gap, top, 1, bottom - top, COL_LINE);
            page_rect(x0, bottom, width, 1, COL_LINE);
        }
        y = bottom + gap;
    }
    y += tbw[2];
    if (tbw[0]) page_rect(x0, ytab, width, tbw[0], tbc);              /* рамка самой таблицы (CSS) */
    if (tbw[2]) page_rect(x0, y - tbw[2], width, tbw[2], tbc);
    if (tbw[3]) page_rect(x0, ytab, tbw[3], y - ytab, tbc);
    if (tbw[1]) page_rect(x0 + width - tbw[1], ytab, tbw[1], y - ytab, tbc);
    if (te) { css_vars_cut(sn + 1); sn--; }
    ts_core->free(t);
    if (form_end && !doc_measuring()) form_close();
    doc_skip_to(y);
    block(12);
}

/* --- flex и grid: элементы рядом, колонками ---------------------------------------------------
 * Контейнер с display: flex (направление row — по умолчанию) или display: grid раскладывает своих
 * детей не друг под другом, а рядом: так сайты делают колонки (основная часть 70 % и боковая
 * панель 30 % — «.content { display: flex }» и ширины детей). Устроено как таблица в
 * одну строку (table): каждый ребёнок — «ячейка» со своей рамкой doc_frame_push, его байты
 * разбираются в ней (render_range).
 * Ширины, как в CSS flexbox: «желаемая» ширина ребёнка — flex-basis или width (проценты — от
 * ширины контейнера), без них — по содержимому (мерка, как у ячеек таблицы); свободное место
 * делят flex-grow, нехватку — flex-shrink; flex-wrap: wrap — не влезшие уходят на следующую
 * строку; gap — промежутки; justify-content — где оставить свободное место; order — порядок.
 * grid: столбцы из grid-template-columns (точки, проценты, доли fr, repeat(), minmax()),
 * дети — по порядку слева направо, строками.
 * Не умеем: выравнивание по высоте (align-items) и растягивание фона детей до высоты строки,
 * column (это и так поток сверху вниз), охват нескольких столбцов grid. */
#define FX_MAX 48
typedef struct {
    uint32_t s, e;                       /* байты ребёнка (элемента или текста) в содержимом контейнера */
    int order, grow, shrink;             /* grow и shrink — ×100 */
    int basis;                           /* желаемая внешняя ширина; −1 — по содержимому */
    int w, min, max;                     /* итоговая ширина; мерки содержимого */
    uint8_t abs;                         /* position: absolute — не колонка: на своё место (abs_box) */
    int dx;                                  /* сдвиг position: relative (место — прежнее) */
    uint8_t self;                        /* ребёнок — простой тег: он сам и есть колонка (cs…ce — его содержимое) */
    uint32_t cs, ce, at;                 /* содержимое между его тегами; атрибуты (смещение в b) */
    uint16_t an;
    char tag[16];
    uint8_t ctl;                         /* поле или кнопка формы: во всю ширину колонки */
} fitem_t;
static int rr_anon;                      /* следующая обёртка render_range — невидима для селекторов */
static int scan_quiet;                   /* flex_scan только смотрит (cb_settle): <link> и <script> не трогать */
static void abs_box(const uint8_t *b, size_t n);   /* ниже: абсолютный блок — на своё место */

/* Ребёнок f — в текущую рамку (колонку). У CSS дети flex и grid — блоки, даже <span>: простой тег
 * (div, span, section…) сам становится рамкой ячейки (render_range с его именем и атрибутами) — его
 * фон, поля и рамка — во всю колонку. Тег со своим смыслом (ссылка, картинка, список, заголовок,
 * форма) и текст разбираются как есть под невидимой обёрткой: селекторы («.row > a») видят
 * настоящего родителя. */
static void flex_item(const uint8_t *b, const fitem_t *f, int al) {
    doc_align(al);
    ctl_fill = f->ctl;
    rr_flex = 1;
    if (f->self) {
        flex_child = sn;
        render_range(b + f->cs, f->ce - f->cs, 0, f->tag, (const char *)b + f->at, f->an);
    } else {
        rr_anon = 1;
        flex_child = sn + 1;
        render_range(b + f->s, f->e - f->s, 0, "-flex", NULL, 0);
    }
    flex_child = -1;
    ctl_fill = 0;
}

/* Столбцы grid-template-columns на ширину avail: точки и проценты — как есть, доли fr — остаток;
 * repeat(n, …), repeat(auto-fill | auto-fit, minmax(200px, 1fr)) — сколько влезет; minmax(a, b) —
 * b (доля — доля), auto — доля 1. Возвращает число столбцов (0 — шаблона нет). */
NOINLINE static int grid_tracks(const char *cols, int avail, int gap, int *w, int max) {
    int px[FX_MAX], fr[FX_MAX], k = 0;
    char t[96], u[96];
    for (const char *p = cols; k < max && word(&p, t, sizeof(t));) {
        int times = 1;
        const char *one = t;
        if (!strncmp(t, "repeat(", 7)) {             /* repeat(3, 1fr) / repeat(auto-fill, minmax(200px, 1fr)) */
            const char *c = strchr(t, ',');
            if (!c) continue;
            snprintf(u, sizeof(u), "%.*s", (int)strlen(c + 1) - 1, c + 1);
            if (!strncmp(t + 7, "auto", 4)) {
                char m[48];
                const char *q = u;
                int mn = 0;
                word(&q, m, sizeof(m));
                if (!strncmp(m, "minmax(", 7)) { size_t l = strcspn(m + 7, ","); memmove(m, m + 7, l); m[l] = 0; }
                if (css_len(m, avail, &mn) != 1 || mn < 16) mn = 200;
                times = (avail + gap) / (mn + gap);
                snprintf(u, sizeof(u), "1fr");
            } else {
                times = num(t + 7);
            }
            if (times < 1) times = 1;
            one = u;
        }
        char tr[96];
        snprintf(tr, sizeof(tr), "%s", one);
        while (*tr == ' ') memmove(tr, tr + 1, strlen(tr));
        if (!strncmp(tr, "minmax(", 7)) {             /* minmax(a, b) — по b */
            const char *c = strchr(tr, ',');
            if (c) { snprintf(u, sizeof(u), "%s", c + 1); u[strlen(u) ? strlen(u) - 1 : 0] = 0; snprintf(tr, sizeof(tr), "%s", u); }
            while (*tr == ' ') memmove(tr, tr + 1, strlen(tr));
        }
        int f = 0, x = 0;
        const char *e = tr;
        while ((*e >= '0' && *e <= '9') || *e == '.') e++;
        if (!strncmp(e, "fr", 2)) f = num(tr) ? num(tr) * 100 : 100;
        else if (is(tr, "auto") || !strncmp(tr, "min-content", 11) || !strncmp(tr, "max-content", 11) ||
                 css_len(tr, avail, &x) != 1) f = 100;
        for (int r = 0; r < times && k < max; r++) { px[k] = f ? 0 : x; fr[k] = f; k++; }
    }
    if (!k) return 0;
    int fixed = (k - 1) * gap, frs = 0;
    for (int i = 0; i < k; i++) { fixed += px[i]; frs += fr[i]; }
    int rest = avail - fixed > 0 ? avail - fixed : 0;
    for (int i = 0; i < k; i++) w[i] = fr[i] ? (frs ? rest * fr[i] / frs : 0) : px[i];
    return k;
}

/* Дети контейнера (байты b, n) — в it: элементы и куски текста; <style> — таблица стилей,
 * <script> — мимо; скрытые CSS — мимо. Отдельной функцией: её буферы (копия стиля для пробы, имена)
 * нужны только здесь, а flex_row вызывает себя через render_range — её кадр должен быть мал. */
NOINLINE static int flex_scan(const uint8_t *b, size_t n, fitem_t *it, int measuring, int max) {
    int k = 0;
    size_t i = 0;
    while (i < n && k < max) {
        if (b[i] != '<') {
            size_t j = i;
            int vis = 0;
            for (; j < n && b[j] != '<'; j++) if (b[j] != ' ' && b[j] != '\n' && b[j] != '\r' && b[j] != '\t') vis = 1;
            if (vis) {
                fitem_t *f = &it[k++];
                memset(f, 0, sizeof(*f));
                f->s = (uint32_t)i; f->e = (uint32_t)j; f->shrink = 100; f->basis = -1;
            }
            i = j;
            continue;
        }
        size_t j = skip_markup(b, n, i);
        if (j) { i = j; continue; }
        char nm[16];
        int cl;
        size_t a0, a1;
        j = read_tag(b, n, i, nm, &cl, &a0, &a1);
        if (!j) { i++; continue; }
        if (cl) { i = j; continue; }
        if (is(nm, "style")) {                        /* таблица стилей — уже разобрана (prescan) */
            i = tag_end(b, n, find_close(b, n, j, "style"));
            continue;
        }
        if (is_any(nm, voids) || is(nm, "br")) {      /* <link>, <meta>… — не дети-колонки */
            if (is(nm, "link") && !scan_quiet) link_tag((const char *)b + a0, a1 - a0);
            i = j;
            continue;
        }
        char ty[16];
        if (is(nm, "input") && attr((const char *)b + a0, a1 - a0, "type", ty, sizeof(ty)) && !strcmp(ty, "hidden")) {
            i = j;
            continue;
        }
        if (is(nm, "script") || is(nm, "template")) {
            if (is(nm, "script") && !scan_quiet) script_n++;
            i = tag_end(b, n, find_close(b, n, j, nm));
            continue;
        }
        size_t m = 0, end = j;                        /* m — начало его закрывающего тега */
        if (!is(nm, "img") && !is(nm, "input") && !is(nm, "hr")) {
            m = find_match(b, n, j, nm);
            end = tag_end(b, n, m);
        }
        st_t probe = *top();                          /* CSS ребёнка: ширина, flex, order, скрыт ли */
        int hid = apply_style(&probe, nm, (const char *)b + a0, a1 - a0, 2);
        if (!hid) {
            fitem_t *f = &it[k++];
            memset(f, 0, sizeof(*f));
            f->s = (uint32_t)i; f->e = (uint32_t)end;
            f->order = bs.order; f->grow = bs.grow; f->shrink = bs.shrink; f->basis = -1;
            f->abs = bs.abs;
            f->dx = bs.dx;
            f->ctl = (uint8_t)(is(nm, "button") || is(nm, "input") || is(nm, "select") || is(nm, "textarea"));
            static const char *const plain[] = {"div", "span", "section", "article", "aside", "header", "footer", "nav",
                                                "main", "figure", "figcaption", "label", "p", NULL};
            if (m && is_any(nm, plain)) {
                f->self = 1;
                f->cs = (uint32_t)j; f->ce = (uint32_t)m;
                f->at = (uint32_t)a0; f->an = (uint16_t)(a1 - a0 > 65535 ? 65535 : a1 - a0);
                snprintf(f->tag, sizeof(f->tag), "%s", nm);
            }
            int pad = bs.border_box ? 0 : bx.pl + bx.pr + (bs.bstyle == 1 ? bx.bl + bx.br : 0);
            int want = bs.basis >= 0 ? bs.basis : bs.w ? bx.w : -1;
            if (want >= 0) {
                if (bs.maxw && want > bx.maxw) want = bx.maxw;
                f->basis = want + pad + bx.ml + bx.mr;
            }
        }
        i = end;
    }
    return k;
}

/* Дети контейнера (байты b, n — между его тегами) — рядом (flex row) или по столбцам (grid). */
NOINLINE static void flex_row(const uint8_t *b, size_t n) {
    /* всё большое — в куче: flex_row вызывает себя через render_range, а стек задачи невелик */
    struct fx_mem { fitem_t it[FX_MAX]; int tw[FX_MAX]; typeof(fc) c; } *mem = ts_alloc(sizeof(struct fx_mem));
    if (!mem) { parse(b, n); return; }
    fitem_t *it = mem->it;
    int *tw = mem->tw;
    mem->c = fc;
    typeof(fc) *cp = &mem->c;
    int k = 0, avail = doc_width(), measuring = doc_measuring(), al = doc_get_align();
    k = flex_scan(b, n, it, measuring, FX_MAX);
    int kall = k;                                     /* абсолютные — в конце (сортировка ниже), не колонки */
    for (int q = 1; q < k; q++) {                     /* order — устойчиво, вставками */
        fitem_t v = it[q];
        int p = q - 1;
        while (p >= 0 && (it[p].abs > v.abs || (it[p].abs == v.abs && it[p].order > v.order))) { it[p + 1] = it[p]; p--; }
        it[p + 1] = v;
    }
    while (k > 0 && it[k - 1].abs) k--;
    /* 2) мерки содержимого — у тех, чья ширина не задана (при измерении таблицы — у всех) */
    for (int q = 0; q < k; q++) {
        fitem_t *f = &it[q];
        if (!measuring && (cp->grid || f->basis >= 0)) { f->max = f->basis > 0 ? f->basis : 0; f->min = 1; continue; }
        doc_measure_push();
        doc_frame_push(0, 30000, 0, 0);
        flex_item(b, f, al);
        doc_frame_pop();
        doc_measure_pop(&f->min, &f->max);
    }
    if (measuring) {                                  /* контейнер внутри измеряемой ячейки таблицы */
        int mn = 0, mx = 0;
        for (int q = 0; q < k; q++) {
            if (cp->wrap || cp->grid) { if (it[q].min > mn) mn = it[q].min; }
            else mn += it[q].min + (q ? cp->cgap : 0);
            mx += it[q].max + (q ? cp->cgap : 0);
        }
#ifdef TSAFARI_HOST
        if (getenv("TSAFARI_FLEXLOG")) printf("flex measure depth %d items %d min %d max %d indent %d\n", flex_depth, k, mn, mx, doc_get_indent());
#endif
        doc_measure_add(mn, mx);
        ts_core->free(mem);
        return;
    }
    /* 3) строки: по ширинам, переносы, свободное место */
    int ncols = cp->grid ? grid_tracks(cp->cols, avail, cp->cgap, tw, FX_MAX) : 0;
    if (cp->grid && ncols < 1) ncols = 1, tw[0] = avail;
    int x0 = doc_x(), y = doc_y(), start = 0;
    doc_skip_to(y);
    while (start < k) {
        int cnt = 0, sum = 0;
        if (cp->grid) {
            cnt = k - start < ncols ? k - start : ncols;
            for (int q = 0; q < cnt; q++) { it[start + q].w = tw[q]; sum += tw[q] + (q ? cp->cgap : 0); }
        } else {
            for (int q = start; q < k; q++) {
                int hyp = it[q].basis >= 0 ? it[q].basis : it[q].max;
                if (hyp > avail && it[q].basis < 0) hyp = avail;
                if (cp->wrap && cnt && sum + cp->cgap + hyp > avail) break;
                it[q].w = hyp;
                sum += (cnt ? cp->cgap : 0) + hyp;
                cnt++;
            }
            int free = avail - sum, tg = 0, ts = 0;
            for (int q = start; q < start + cnt; q++) { tg += it[q].grow; ts += it[q].shrink * it[q].w / 100; }
            if (free > 0 && tg > 0) {                 /* свободное — тем, у кого flex-grow */
                int given = 0, last = start;
                for (int q = start; q < start + cnt; q++) {
                    int add = (int)((int64_t)free * it[q].grow / tg);
                    it[q].w += add;
                    given += add;
                    if (it[q].grow) last = q;
                }
                it[last].w += free - given;
            } else if (free < 0 && ts > 0) {
                /* нехватка — с тех, у кого flex-shrink, по весу «сжатие × ширина», но не уже их
                 * наименьшей ширины по содержимому (min-width: auto у CSS); упёрся — остаток с других */
                int need = -free;
                for (int pass = 0; pass < 4 && need > 0; pass++) {
                    int64_t wsum = 0;
                    for (int q = start; q < start + cnt; q++)
                        if (it[q].w > it[q].min) wsum += (int64_t)it[q].shrink * it[q].w;
                    if (!wsum) break;
                    int took = 0;
                    for (int q = start; q < start + cnt; q++) {
                        if (it[q].w <= it[q].min || !it[q].shrink) continue;
                        int cut = (int)((int64_t)need * it[q].shrink * it[q].w / wsum);
                        if (cut < 1) cut = 1;
                        if (cut > it[q].w - it[q].min) cut = it[q].w - it[q].min;
                        it[q].w -= cut;
                        took += cut;
                    }
                    need -= took;
                }
            }
            sum = (cnt - 1) * cp->cgap;
            for (int q = start; q < start + cnt; q++) sum += it[q].w;
        }
        int free = avail - sum, lead = 0, between = cp->cgap;
        if (free > 0 && !cp->grid) {
            if (cp->just == 1) lead = free / 2;
            else if (cp->just == 2) lead = free;
            else if (cp->just == 3 && cnt > 1) between += free / (cnt - 1);
            else if (cp->just == 4) { lead = free / (2 * cnt); between += free / cnt; }
            else if (cp->just == 5) { lead = free / (cnt + 1); between += free / (cnt + 1); }
        }
#ifdef TSAFARI_HOST
        if (getenv("TSAFARI_FLEXLOG")) {
            printf("flex depth %d avail %d grid %d wrap %d line %d..%d:", flex_depth, avail, cp->grid, cp->wrap, start, start + cnt);
            for (int q = start; q < start + cnt; q++) printf(" [%.*s] w%d b%d mx%d", 30, (const char *)b + it[q].s, it[q].w, it[q].basis, it[q].max);
            printf("\n");
        }
#endif
        /* 4) вывод: каждый ребёнок — в своей колонке; не влезший в контейнер с overflow: hidden
         * не виден (карусели: видна первая картинка), без него — уходит на следующую строку */
        int x = x0 + lead, bottom = y, shown = 0;
        for (int q = start; q < start + cnt; q++) {
            int w = it[q].w;
            if (x - x0 >= avail && shown) {
                if (cp->clip) continue;
                cnt = q - start;                      /* остальные — следующей строкой */
                break;
            }
            if (x - x0 + w > avail) w = avail - (x - x0);
            if (w < 8) w = 8;
            doc_frame_push(x + it[q].dx, w, y, 0);
            flex_item(b, &it[q], al);
            int bt = doc_frame_pop();
            if (bt > bottom) bottom = bt;
            x += w + between;
            shown++;
        }
        if (!cnt) cnt = 1;
        start += cnt;
        y = bottom + (start < k ? cp->rgap : 0);
        doc_skip_to(y);
    }
    for (int q = k; q < kall; q++) abs_box(b + it[q].s, it[q].e - it[q].s);
    ts_core->free(mem);
}

/* Абсолютный блок (b, n — от его тега до закрывающего): своя рамка на своём месте (abs_where), поток
 * страницы не сдвигается. Разложен — как ребёнок flex (flex_item: своя рамка, её ширина — его
 * ширина). Ширина без width и без пары left/right — по содержимому («сжатие по содержимому» CSS),
 * не шире блока отсчёта. При измерении (ячейки таблицы, колонки) его нет: он места не занимает. */
NOINLINE static void abs_box(const uint8_t *b, size_t n) {
    static fitem_t one[2];
    if (doc_measuring()) return;
    int al = doc_get_align();
    if (flex_scan(b, n, one, 0, 1) < 1 || !abs_g.ok) return;   /* скрыт CSS */
    fitem_t f = one[0];                               /* копии: вложенные абсолютные трут one и abs_g */
    absg_t g = abs_g;
    int w = g.w;
    if (w < 0) {
        int mn = 0, mx = 0;
        void *held = doc_para_hold();                 /* мерка тоже начинается с вывода набранного */
        doc_measure_push();
        doc_frame_push(0, 30000, 0, 0);
        flex_item(b, &f, al);
        doc_frame_pop();
        doc_measure_pop(&mn, &mx);
        doc_para_back(held);
        int room = g.cw - (g.lset ? g.lv : 0) - (g.rset ? g.rv : 0);
        w = mx < room ? mx : room > mn ? room : mn;
    }
    if (w < 1) w = 1;
    int x, y;
    abs_where(&g, w, g.h, &x, &y);
    void *para = doc_para_hold();                     /* строка, посреди которой он стоял, — не обрывать */
#ifdef TSAFARI_HOST
    if (getenv("TSAFARI_FLEXLOG"))
        printf("absolute cb %d,%d %dx%d -> %d,%d w %d h %d [%.40s]\n", g.cx, g.cy, g.cw, g.ch, x, y, w, g.h, (const char *)b);
#endif
    doc_frame_push(x, w, y, 0);
    flex_item(b, &f, al);
    doc_frame_pop();
    doc_para_back(para);
}

/* Блок отсчёта без своей высоты (cbh < 0 — известны только его поля): все его дети вне потока
 * (position: absolute или скрыты), текста нет — его высота и есть поля («коробка пропорции»:
 * padding-bottom: 56%, внутри — абсолютная картинка во всю коробку); иначе высота — по содержимому,
 * заранее не известна (0). b, n — его содержимое. */
static fitem_t cb_items[FX_MAX];
NOINLINE static void cb_settle(const uint8_t *b, size_t n) {
    st_t *e = top();
    if (e->cbh >= 0) return;
    scan_quiet = 1;
    int k = flex_scan(b, n, cb_items, 0, FX_MAX), flow = k >= FX_MAX;
    scan_quiet = 0;
    for (int q = 0; q < k; q++) if (!cb_items[q].abs) flow = 1;
    e->cbh = flow ? 0 : -e->cbh;
}

/* --- float: плавающий блок ---------------------------------------------------------------------
 * Блок с CSS float встаёт у левого (правого) края места, рядом с прежними плавающими, а не влезает —
 * ниже них (doc_float_place); строки и коробки после него обтекают его (doc.c). Ширина — width из
 * CSS, без неё — по содержимому (не шире места). Разложен — как ребёнок flex (flex_item: своя рамка,
 * его width — уже ширина рамки), потом его прямоугольник занимает место (doc_float_add). Блок, в
 * котором были плавающие, при закрытии опускается ниже них (pop_one, fclear). Так встают меню
 * «li { float: left }» и колонки «.col-md-8 { float: left; width: 66.67% }». */
NOINLINE static void float_box(const uint8_t *b, size_t n, int right) {
    static fitem_t one[2];
    int measuring = doc_measuring(), al = doc_get_align();
    if (flex_scan(b, n, one, measuring, 1) < 1) return;   /* скрыт CSS */
    fitem_t f = one[0];                                   /* копия: вложенные плавающие трут one */
    int mn = 0, mx = 0;
    if (f.basis < 0 || measuring) {
        doc_measure_push();
        doc_frame_push(0, 30000, 0, 0);
        flex_item(b, &f, al);
        doc_frame_pop();
        doc_measure_pop(&mn, &mx);
        if (f.basis >= 0) mn = mx = f.basis;
    }
    if (measuring) { doc_measure_float(mn, mx); return; }
    int avail = doc_width(), w = f.basis >= 0 ? f.basis : mx;
    if (w > avail) w = avail;
    if (w < 1) w = 1;
#ifdef TSAFARI_HOST
    if (getenv("TSAFARI_FLEXLOG"))
        printf("float %c basis %d min %d max %d avail %d -> w %d [%.40s]\n", right ? 'r' : 'l', f.basis, mn, mx, avail, w,
               (const char *)b);
#endif
    int x, y;
    doc_float_place(w, right, &x, &y);
    doc_frame_push(x + f.dx, w, y, 0);
    flex_item(b, &f, al);
    int bottom = doc_frame_pop();
    doc_float_add(x, y, w, bottom - y, right);
    for (int k = sn - 1; k >= 1; k--)                      /* блок-родитель вместит плавающие */
        if (stk[k].block) { stk[k].fclear = 1; break; }
}

/* Разобрать кусок страницы (ячейку, подпись таблицы) в текущую рамку как отдельный документ:
 * теги, открытые снаружи, не закрываются (дно floor_n), а свои незакрытые закрываются в конце;
 * <th> — жирным. */
static void render_range(const uint8_t *b, size_t n, int bold, const char *tagname, const char *at, size_t an) {
    int save_sn = sn, save_floor = floor_n, save_pre = pre, save_space = space, save_depth = depth;
    char save_skip[sizeof(skip_until)];
    memcpy(save_skip, skip_until, sizeof(skip_until));
    skip_until[0] = 0;
    floor_n = sn;
    st_t *e = push("td");
    flex_at = -1;
    if (e) {
        e->anon = (uint8_t)rr_anon;
        e->block = 1;
        e->align = (int8_t)doc_get_align();
        e->indent = (int16_t)doc_get_indent();
        e->bg = 0;
        if (bold) e->flags |= PAGE_BOLD;
        apply_style(e, tagname, at, an, 1);           /* правила для td/th и style="…" ячейки */
    }
    rr_anon = 0;
    if (e && e->pos && e->cbh < 0) cb_settle(b, n);
    int fx = e && flex_at == sn - 1;                  /* ячейка (колонка) сама flex: её дети — рядом */
    flex_at = -1;
    pre = 0;
    space = 0;
    int as_flex = rr_flex;                            /* колонка flex — свой счётчик, ячейка таблицы — свой */
    rr_flex = 0;
    if (as_flex) flex_depth++;
    else table_depth++;
    if (fx) flex_row(b, n);
    else parse(b, n);
    if (as_flex) flex_depth--;
    else table_depth--;
    space = 0;
    doc_flush(0);
    while (sn > save_sn) pop_one();
    floor_n = save_floor;
    pre = save_pre;
    space = save_space;
    depth = save_depth;
    memcpy(skip_until, save_skip, sizeof(skip_until));
}

/* Что делать с тегом name (closing = 1 — «</name>»; a и n — текст атрибутов). Возвращает 1,
 * если содержимое тега надо пропустить (display: none).
 * СТЕК. Таблица в таблице разбирается рекурсивно (parse → table → render_range → parse), и стек
 * задачи браузера на ESP невелик (app.c, start). Буферы адресов по 512 байт в разборе тега
 * (здесь, в open_element, form_tag) — static: разбор идёт только в задаче браузера, а сам тег
 * разбор страницы заново не начинает, так что второй вызов их не застанет занятыми. */
NOINLINE static int tag(const char *name, int closing, const char *a, size_t n) {
    static char v[URL_MAX], abs_url[URL_MAX];
    if (is(name, "title")) { in_title = !closing; return 0; }
    if (!closing && is(name, "base")) {
        if (attr(a, n, "href", v, sizeof(v)) && url_resolve(base, v, abs_url, sizeof(abs_url))) strcpy(base, abs_url);
        attr(a, n, "target", base_target, sizeof(base_target));
        return 0;
    }
    /* фрейм внутри обычного разбора (набор фреймов, вложенный слишком глубоко) — ссылкой
     * «[frame: имя]» на его страницу во всё окно */
    if (!closing && is(name, "frame")) {
        char nm[32] = "", label[48];
        int id = attr(a, n, "src", v, sizeof(v)) && url_resolve(base, v, abs_url, sizeof(abs_url)) && !doc_measuring()
                     ? add_link(abs_url, "_top") : -1;
        attr(a, n, "name", nm, sizeof(nm));
        snprintf(label, sizeof(label), "[frame%s%s]", nm[0] ? ": " : "", nm);
        block(4);
        out_link(label, id);
        block(4);
        return 0;
    }
    if (is_any(name, skips)) {
        if (closing) return 0;
        if (is(name, "script")) script_n++;
        if (is(name, "iframe")) {                     /* встроенный фрейм — ссылкой на его страницу */
            out_link("[frame]", attr(a, n, "src", v, sizeof(v)) && url_resolve(base, v, abs_url, sizeof(abs_url)) &&
                                    !doc_measuring() ? add_link(abs_url, "_top") : -1);
        }
        else if (is(name, "video")) out_dim("[video]");
        else if (is(name, "audio")) out_dim("[audio]");
        strcpy(skip_until, name);
        return 0;
    }
    if (closing) { close_element(name); return 0; }
    return open_element(name, a, n);
}

/* Найти «</name» без учёта регистра, начиная с i; вернуть позицию «<» или n (не нашли). */
static size_t find_close(const uint8_t *b, size_t n, size_t i, const char *name) {
    size_t nl = strlen(name);
    for (; i + 2 + nl <= n; i++) {
        if (b[i] != '<' || b[i + 1] != '/') continue;
        size_t k = 0;
        while (k < nl && lower(b[i + 2 + k]) == name[k]) k++;
        if (k == nl) return i;
    }
    return n;
}

/* Сброс для новой страницы целиком: ссылки и заголовок. */
static void reset_links(void) {
    link_n = 0; pool_len = 0;
    title[0] = 0; title_len = 0; title_lock = 0;
    part_now = -1;
}

/* Сброс разбора для очередного документа (страницы или фрейма): адрес, стек тегов (на дне —
 * стиль по умолчанию из дизайна Codex), цвета ссылок, отступ, выравнивание. */
static void reset_part(const char *url) {
    snprintf(base, sizeof(base), "%s", url);
    base_target[0] = 0;
    pre = in_title = space = depth = 0;
    skip_until[0] = 0;
    table_depth = flex_depth = 0;
    memset(&stk[0], 0, sizeof(stk[0]));
    stk[0].fg = COL_TEXT;
    stk[0].face = FACE_BODY;
    stk[0].link = -1;
    sn = 1;
    floor_n = 0;
    link_rgb = COL_LINK;
    vlink_rgb = COL_VISITED;
    body_rect = -1;
    body_bg = 0;
    css_doc_reset();                                  /* правила <style> — у каждого документа свои */
    css_vars_cut(0);
    have_html = have_body = html_seen = 0;
    html_an = 0;
    html_bg = 0;
    gap_before = -1;
    doc_indent(0);
    doc_align(DOC_LEFT);
}

/* «<!-- … -->», «<!DOCTYPE …>», «<?xml …>» с позиции i: вернуть позицию за ними; 0 — не они. */
static size_t skip_markup(const uint8_t *b, size_t n, size_t i) {
    if (i + 3 < n && b[i + 1] == '!' && b[i + 2] == '-' && b[i + 3] == '-') {   /* комментарий */
        size_t j = i + 4;
        while (j + 2 < n && !(b[j] == '-' && b[j + 1] == '-' && b[j + 2] == '>')) j++;
        return j + 3;
    }
    if (i + 1 < n && (b[i + 1] == '!' || b[i + 1] == '?')) {                   /* DOCTYPE, <?xml> */
        while (i < n && b[i] != '>') i++;
        return i + 1;
    }
    return 0;
}

/* Тег с позиции i («<» в b[i]): имя строчными (до 15 букв) — в name, закрывающий ли — в
 * *closing, атрибуты — b[*a0 … *a1). Возвращает позицию за «>»; 0 — это не тег («<» —
 * просто знак). Тег: «<», может быть «/», имя (буквы, цифры, «-»), атрибуты до «>»; «>»
 * внутри кавычек значения атрибута тег не заканчивает. */
static size_t read_tag(const uint8_t *b, size_t n, size_t i, char *name, int *closing, size_t *a0, size_t *a1) {
    *closing = i + 1 < n && b[i + 1] == '/';
    size_t k = i + 1 + (size_t)*closing;
    if (k >= n || !((b[k] >= 'a' && b[k] <= 'z') || (b[k] >= 'A' && b[k] <= 'Z'))) return 0;
    size_t nl = 0;
    while (k < n && ((b[k] >= 'a' && b[k] <= 'z') || (b[k] >= 'A' && b[k] <= 'Z') || (b[k] >= '0' && b[k] <= '9') || b[k] == '-')) {
        if (nl < 15) name[nl++] = (char)lower(b[k]);
        k++;
    }
    name[nl] = 0;
    *a0 = k;
    char q = 0;
    while (k < n && (q || b[k] != '>')) {                      /* конец тега — вне кавычек */
        if (q) { if (b[k] == q) q = 0; }
        else if (b[k] == '"' || b[k] == '\'') q = (char)b[k];
        k++;
    }
    *a1 = k;
    return k + 1;
}

/* Главный цикл разбора (описан в начале файла). <table> разбирается целиком (table) — до своего
 * «</table>»; тег со стилем display: none пропускается вместе с содержимым. */
#ifdef TSAFARI_HOST
size_t html_host_parsed;                 /* стенд: сколько байт разобрал parse (раскладка в разы дороже страницы?) */
#endif
static void parse(const uint8_t *b, size_t n) {
    size_t i = 0;
#ifdef TSAFARI_HOST
    html_host_parsed += n;
#endif
    while (i < n) {
        if (skip_until[0]) {                                   /* до </script> и подобных */
            i = find_close(b, n, i, skip_until);
            skip_until[0] = 0;
            continue;
        }
        if (b[i] != '<') {
            size_t j = i;
            while (j < n && b[j] != '<') j++;
            text(b + i, j - i);
            i = j;
            continue;
        }
        size_t j = skip_markup(b, n, i);
        if (j) { i = j; continue; }
        char name[16];
        int closing;
        size_t a0, a1;
        j = read_tag(b, n, i, name, &closing, &a0, &a1);
        if (!j) { text(b + i, 1); i++; continue; }             /* «<» — просто знак */
        const char *a = (const char *)b + a0;
        if (!closing && is(name, "style")) {                   /* таблица стилей — уже разобрана (prescan) */
            i = find_close(b, n, j, "style");
            continue;
        }
        if (!closing && (is(name, "select") || is(name, "textarea") || is(name, "button"))) {
            size_t end = find_close(b, n, j, name);           /* содержимое — до своего закрывающего */
            if (is(name, "select")) select_tag(a, a1 - a0, b + j, end - j);
            else if (is(name, "textarea")) textarea_tag(a, a1 - a0, b + j, end - j);
            else button_tag(a, a1 - a0, b + j, end - j);
            i = tag_end(b, n, end);
            continue;
        }
        if (!closing && is(name, "table") && table_depth < TABLE_DEPTH_MAX) {
            close_p();
            size_t end = find_match(b, n, j, "table");
            table(b, j, end, a, a1 - a0);
            i = tag_end(b, n, end);
            continue;
        }
        int sn0 = sn;
        int act = tag(name, closing, a, a1 - a0);
        if (act == 4) {                                        /* position: absolute — вне потока */
            size_t m = find_match(b, n, j, name), end = tag_end(b, n, m);
            abs_box(b + i, end - i);
            i = end;
            continue;
        }
        if (act == 3) {                                        /* float: блок — у края, обтекание */
            size_t m = find_match(b, n, j, name), end = tag_end(b, n, m);
            float_box(b + i, end - i, float_side);
            i = end;
            continue;
        }
        if (act) {                                             /* display: none — содержимое мимо */
            size_t end = find_match(b, n, j, name);
#ifdef TSAFARI_HOST
            if (getenv("TSAFARI_HIDELOG")) printf("hidden <%s %.*s> at %u skips %u bytes\n", name, (int)(a1 - a0 > 80 ? 80 : a1 - a0), a, (unsigned)i, (unsigned)(end - j));
#endif
            close_element(name);
            i = tag_end(b, n, end);
            continue;
        }
        /* открылся блок отсчёта без своей высоты — сразу узнать, не из одних ли полей он */
        if (!act && !closing && sn > sn0 && top()->pos && top()->cbh < 0) {
            size_t m = find_match(b, n, j, name);
            cb_settle(b + j, m - j);
        }
        if (flex_at >= 0) {                                    /* flex или grid: дети — рядом */
            int at = flex_at;
            flex_at = -1;
            if (!closing && at == sn - 1) {
                size_t end = find_match(b, n, j, name);
                flex_row(b + j, end - j);
                close_element(name);
                i = tag_end(b, n, end);
                continue;
            }
        }
        i = j;
    }
}

/* Просмотр «набор ли это фреймов»: ищем <frameset> и <frame src=…>, не рисуя ничего.
 * Встретился <body> раньше <frameset> — обычная страница, дальше не смотрим. */
int html_frames(const uint8_t *b, size_t n, const char *content_type, const char *url,
                html_frame_t *out, int max) {
    if (content_type && !strncmp(content_type, "text/plain", 10)) return 0;
    snprintf(base, sizeof(base), "%s", url);
    cs = charset_detect(content_type, b, n);
    int count = 0, frameset = 0;
    size_t i = 0;
    while (i < n && count < max) {
        if (b[i] != '<') { i++; continue; }
        size_t j = skip_markup(b, n, i);
        if (j) { i = j; continue; }
        char name[16];
        int closing;
        size_t a0, a1;
        j = read_tag(b, n, i, name, &closing, &a0, &a1);
        if (!j) { i++; continue; }
        i = j;
        if (closing) continue;
        const char *a = (const char *)b + a0;
        size_t an = a1 - a0;
        char v[URL_MAX];
        if (is(name, "script") || is(name, "style")) { i = find_close(b, n, i, name); continue; }
        if (is(name, "body") && !frameset) return 0;
        if (is(name, "frameset")) frameset = 1;
        else if (is(name, "base")) {
            char abs_url[URL_MAX];
            if (attr(a, an, "href", v, sizeof(v)) && url_resolve(base, v, abs_url, sizeof(abs_url))) strcpy(base, abs_url);
        } else if (is(name, "frame") && frameset && attr(a, an, "src", v, sizeof(v)) && v[0] &&
                   url_resolve(base, v, out[count].url, sizeof(out[count].url))) {
            if (!attr(a, an, "name", out[count].name, sizeof(out[count].name))) out[count].name[0] = 0;
            count++;
        }
    }
    return frameset ? count : 0;
}

void html_begin(void) {
    shown_n = script_n = 0;
    css_names_reset();                                /* имена всех документов страницы (prescan) */
    reset_links();
    form_clear();
    image_layout_begin();                             /* места картинок — заново (сами картинки — в app.c) */
    page_set_painter(paint_widget);
    doc_set_inline_cb(place_widget);
    doc_begin(COL_BG);
}

/* Черта перед каждым фреймом, кроме первого. */
static void part_start(const char *url, int part) {
    if (part > 0) { block(0); doc_indent(0); doc_rule(COL_RULE, 1, 12); }
    reset_part(url);
    part_now = part;
    title_lock = title_len > 0;
}

/* Конец документа: закрыть все незакрытые теги; у фрейма с фоном <body> — дописать высоту фона. */
static void part_finish(void) {
    block(0);
    while (sn > 1) pop_one();
    doc_float_clear(0, 3);                            /* страница — ниже всех плавающих */
    if (body_rect >= 0) {
        int bottom = doc_frame_pop() + 8;
        page_rect_height(body_rect, bottom - body_top);
        doc_skip_to(bottom);
        body_rect = -1;
    }
    block(12);
}

/* Имена документа для фильтра правил CSS (css.h): теги, классы и id всех элементов — правила
 * с другими именами к нему не подойдут, их можно не хранить. */
static void prescan(const uint8_t *b, size_t n) {
    static char cls[256], id[64];
    static uint32_t sheet_at[64], sheet_end[64];      /* <style> документа: где (разбор — после имён) */
    int ns = 0;
    size_t i = 0;
    while (i < n) {
        const uint8_t *lt = memchr(b + i, '<', n - i);
        if (!lt) break;
        i = (size_t)(lt - b);
        size_t j = skip_markup(b, n, i);
        if (j) { i = j; continue; }
        char name[16];
        int cl;
        size_t a0, a1;
        j = read_tag(b, n, i, name, &cl, &a0, &a1);
        if (!j) { i++; continue; }
        i = j;
        if (cl) continue;
        css_name_tag(name);
        const char *a = (const char *)b + a0;
        if (attr(a, a1 - a0, "class", cls, sizeof(cls))) css_name_classes(cls);
        if (attr(a, a1 - a0, "id", id, sizeof(id))) css_name_id(id);
        /* <style> (с подходящим media) — сразу в правила: в CSS таблица стилей действует на весь
         * документ, где бы ни стояла (в конце <body>, в <noscript>), а не только на то, что ниже */
        if (is(name, "style")) {
            size_t end = find_close(b, n, i, "style");
            char md[128];
            if (ns < 64 && !(attr(a, a1 - a0, "media", md, sizeof(md)) && !css_media(md, strlen(md)))) {
                sheet_at[ns] = (uint32_t)i;
                sheet_end[ns] = (uint32_t)end;
                ns++;
            }
            i = end;
            continue;
        }
        if (is(name, "script") || is(name, "textarea")) i = find_close(b, n, i, name);
    }
    /* таблицы стилей — после того, как собраны все имена (правила для имён, которых нет, — мимо) */
    for (int k = 0; k < ns; k++) css_doc_sheet((const char *)b + sheet_at[k], sheet_end[k] - sheet_at[k], base, part_now);
}

/* Простой текст (text/plain) выводится целиком как <pre>. */
void html_part(const uint8_t *b, size_t n, const char *content_type, const char *url, int part) {
    part_start(url, part);
    cs = charset_detect(content_type, b, n);
    if (content_type && !strncmp(content_type, "text/plain", 10)) { pre = 1; text(b, n); }
    else { prescan(b, n); parse(b, n); }
    part_finish();
}

void html_part_note(const char *url, const char *what, int part) {
    part_start(url, part);
    doc_style_t d = {COL_DIM, 0, 0, -1};
    doc_text(what, (unsigned)strlen(what), &d);
    doc_text(": ", 2, &d);
    doc_text(url, (unsigned)strlen(url), &d);
    block(12);
}

void html_end(void) {
    block(PAGE_MARGIN);
    part_now = -1;
}

/* Страница без своего содержимого, которую строит программа JavaScript (как zxart.ee: в ней
 * только <app-root></app-root> и скрипты): вместо пустого листа — объяснение. Вид — дизайн
 * Codex, кадр 11a «This page needs JavaScript»; кнопок JS-службы пока нет — службы ещё нет. */
static void js_page(const char *url) {
    reset_links();
    form_clear();
    image_clear();
    image_layout_begin();
    reset_part(url);
    doc_begin(COL_BG);
    doc_style_t cap = {COL_LINK, 0, 0, -1, FACE_CAPTION}, h = {COL_TEXT, 0, 0, -1, FACE_H1},
                m = {COL_DIM, 0, 0, -1, FACE_BODY}, t = {COL_TEXT, 0, 0, -1, FACE_BODY};
    static const char *const lines[] = {
        "This site builds its pages with a JavaScript program,",
        "and TSafari does not run JavaScript.",
        "A JS service that prepares readable pages is planned.",
    };
    doc_indent(48);                                    /* подпись — правее глобуса, как у Codex */
    static const char cap_text[] = "PAGE / JAVASCRIPT", head[] = "This page needs JavaScript";
    doc_text(cap_text, sizeof(cap_text) - 1, &cap);
    doc_flush(10);
    doc_indent(0);
    doc_text(head, sizeof(head) - 1, &h);
    doc_flush(4);
    doc_text(url, (unsigned)strlen(url), &m);
    doc_flush(14);
    doc_rule(COL_RULE, 1, 24);
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        doc_text(lines[i], (unsigned)strlen(lines[i]), &t);
        doc_flush(0);
    }
    doc_flush(PAGE_LINE_H);
    const char *hint = "Try the site's mobile or plain version, or another address (Win).";
    doc_text(hint, (unsigned)strlen(hint), &m);
    doc_flush(0);
    snprintf(title, sizeof(title), "%s", head);
}

void html_render(const uint8_t *b, size_t n, const char *content_type, const char *url) {
    html_begin();
    html_part(b, n, content_type, url, -1);
    if (!shown_n && script_n) { js_page(url); return; }
    html_end();
}

/* Страница ошибки: заголовок, адрес, подробности и подсказка, что делать дальше. */
void html_error_page(const char *url, const char *what, const char *detail) {
    reset_links();
    form_clear();
    image_clear();
    image_layout_begin();
    reset_part(url);
    doc_begin(COL_BG);
    doc_style_t h = {COL_TEXT, 0, 0, -1, FACE_H2}, t = {COL_TEXT, 0, 0, -1, FACE_BODY}, d = {COL_DIM, 0, 0, -1, FACE_BODY};
    doc_flush(PAGE_LINE_H);
    doc_text(what, (unsigned)strlen(what), &h);
    doc_flush(14);
    doc_text(url, (unsigned)strlen(url), &t);
    doc_flush(8);
    doc_text(detail, (unsigned)strlen(detail), &d);
    doc_flush(14);
    const char *hint = "F5 — try again, Win — another address.";
    doc_text(hint, (unsigned)strlen(hint), &d);
    doc_flush(0);
    snprintf(title, sizeof(title), "%s", what);
}
