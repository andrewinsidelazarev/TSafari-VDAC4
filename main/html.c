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
    char cls[24], id[16];                /* class и id тега — для правил CSS с родителем («.menu a») */
    uint32_t fg, bg;                     /* цвет букв; фон под буквами (0 — без своего фона) */
    uint8_t face, flags;                 /* начертание (font.h); PAGE_BOLD, PAGE_UNDERLINE, PAGE_STRIKE */
    int16_t link;                        /* номер ссылки или −1 */
    uint8_t block, box, pre_was, list;   /* блочный; открыл блок с фоном; <pre> до него; открыл список */
    int8_t align;                        /* выравнивание до него */
    int16_t indent;                      /* отступ до него */
    int16_t mr;                          /* строчный тег: поле справа (CSS margin), точки */
} st_t;
static st_t stk[ST_MAX];
static int sn, floor_n;                  /* записей в стеке; ниже floor_n не закрывать */
static uint32_t link_rgb, vlink_rgb;     /* цвета ссылок страницы (<body link= vlink=>) */
static int body_rect = -1, body_top;     /* фон <body> фрейма: прямоугольник и его верх */
static uint32_t body_bg;                 /* фон <body> (0 — фон страницы по умолчанию) */
static int table_depth;                  /* вложенность таблиц при разборе */
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
    doc_style_t s = {t->fg, t->bg, t->flags, t->link, t->face};
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
                if (o) out_utf8(w, o);
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
    if (o) out_utf8(w, o);
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
    "fieldset", "figure", "figcaption", "address", "details", "summary", "dl", "caption", "noscript", "legend",
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

/* --- цвета и CSS ---------------------------------------------------------------------------- */

static const struct { const char *name; uint32_t rgb; } color_names[] = {
    {"black", 0x000001}, {"white", 0xFFFFFF}, {"red", 0xFF0000}, {"green", 0x008000}, {"blue", 0x0000FF},
    {"yellow", 0xFFFF00}, {"gray", 0x808080}, {"grey", 0x808080}, {"silver", 0xC0C0C0}, {"maroon", 0x800000},
    {"navy", 0x000080}, {"purple", 0x800080}, {"teal", 0x008080}, {"olive", 0x808000}, {"lime", 0x00FF00},
    {"aqua", 0x00FFFF}, {"cyan", 0x00FFFF}, {"fuchsia", 0xFF00FF}, {"magenta", 0xFF00FF}, {"orange", 0xFFA500},
    {"brown", 0xA52A2A}, {"pink", 0xFFC0CB}, {"gold", 0xFFD700}, {"violet", 0xEE82EE}, {"indigo", 0x4B0082},
    {"darkred", 0x8B0000}, {"darkgreen", 0x006400}, {"darkblue", 0x00008B}, {"darkgray", 0xA9A9A9},
    {"darkgrey", 0xA9A9A9}, {"lightgray", 0xD3D3D3}, {"lightgrey", 0xD3D3D3}, {"beige", 0xF5F5DC},
    {"khaki", 0xF0E68C}, {"crimson", 0xDC143C}, {"coral", 0xFF7F50}, {"tomato", 0xFF6347}, {"salmon", 0xFA8072},
    {"skyblue", 0x87CEEB}, {"steelblue", 0x4682B4}, {"lightblue", 0xADD8E6}, {"lightgreen", 0x90EE90},
    {"lightyellow", 0xFFFFE0}, {"ivory", 0xFFFFF0}, {"whitesmoke", 0xF5F5F5}, {"gainsboro", 0xDCDCDC},
};

static int hexd(int c) {
    c = lower(c);
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Цвет из «#rgb», «#rrggbb», «rrggbb» (старые сайты пишут без #), «rgb(r, g, b)» или имени;
 * −1 — не цвет. Чистый чёрный отдаётся как 0x000001: 0 в стиле значит «своего фона нет», а на
 * экране разницы не видно (RGB565 младших битов не хранит). */
static int32_t parse_color(const char *s) {
    while (*s == ' ') s++;
    int32_t c = -1;
    const char *h = *s == '#' ? s + 1 : s;
    size_t n = 0;
    while (hexd((unsigned char)h[n]) >= 0) n++;
    int end = h[n] == 0 || h[n] == ' ' || h[n] == ';' || h[n] == '!';
    if (end && (n == 6 || (n == 3 && *s == '#'))) {
        if (n == 6)
            c = hexd(h[0]) << 20 | hexd(h[1]) << 16 | hexd(h[2]) << 12 | hexd(h[3]) << 8 | hexd(h[4]) << 4 | hexd(h[5]);
        else
            c = hexd(h[0]) * 0x110000 | hexd(h[1]) * 0x1100 | hexd(h[2]) * 0x11;
    } else if (lower(s[0]) == 'r' && lower(s[1]) == 'g' && lower(s[2]) == 'b' && (s[3] == '(' || lower(s[3]) == 'a')) {
        int v[3] = {0, 0, 0}, k = 0;
        for (const char *p = s + 4; *p && *p != ')' && k < 3; p++) {
            if (*p >= '0' && *p <= '9') { if (v[k] < 1000) v[k] = v[k] * 10 + (*p - '0'); }
            else if (*p == ',') k++;
        }
        for (k = 0; k < 3; k++) if (v[k] > 255) v[k] = 255;
        c = v[0] << 16 | v[1] << 8 | v[2];
    } else {
        char w[24];
        size_t k = 0;
        while (s[k] && s[k] != ' ' && s[k] != ';' && k < sizeof(w) - 1) { w[k] = (char)lower((unsigned char)s[k]); k++; }
        w[k] = 0;
        for (unsigned i = 0; i < sizeof(color_names) / sizeof(color_names[0]); i++)
            if (is(w, color_names[i].name)) { c = (int32_t)color_names[i].rgb; break; }
    }
    return c == 0 ? 1 : c;
}

/* Значение свойства prop в CSS-строке style («color: red; font-weight: bold») строчными, без
 * «!important»; имя — без учёта регистра. 0 — свойства нет. */
static int css_get(const char *st, const char *prop, char *out, size_t cap) {
    size_t pl = strlen(prop);
    const char *p = st;
    int found = 0;                       /* свойство повторено — главнее последнее, как у браузеров */
    while (*p) {
        while (*p == ' ' || *p == ';' || *p == '\n' || *p == '\t' || *p == '\r') p++;
        const char *name = p;
        while (*p && *p != ':' && *p != ';') p++;
        size_t nl = (size_t)(p - name);
        while (nl && (name[nl - 1] == ' ' || name[nl - 1] == '\t')) nl--;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        const char *val = p;
        while (*p && *p != ';') p++;
        size_t vl = (size_t)(p - val);
        int match = nl == pl;
        for (size_t i = 0; match && i < nl; i++) match = lower((unsigned char)name[i]) == prop[i];
        if (!match) continue;
        for (size_t i = 0; i < vl; i++) if (val[i] == '!') { vl = i; break; }
        while (vl && val[vl - 1] == ' ') vl--;
        if (vl >= cap) vl = cap - 1;
        for (size_t i = 0; i < vl; i++) out[i] = (char)lower((unsigned char)val[i]);
        out[vl] = 0;
        found = 1;
    }
    return found;
}

/* Начертания по размеру, от мелкого к крупному: шаг для <small>, <big>, smaller, larger. */
static const uint8_t face_order[6] = {FACE_CAPTION, FACE_SMALL, FACE_BODY, FACE_H3, FACE_H2, FACE_H1};
static unsigned face_step(unsigned face, int d) {
    int i = 2;
    for (int k = 0; k < 6; k++) if (face_order[k] == face) i = k;
    i += d;
    return face_order[i < 0 ? 0 : i > 5 ? 5 : i];
}

/* Размер шрифта CSS (px, pt, em, %, слова) → начертание; −1 — не понят. Обычный текст сайтов —
 * 16 px, у нас он основной 20 px; крупнее и мельче — ближайшее начертание. */
static int css_face(const char *v, unsigned cur) {
    static const struct { const char *w; uint8_t f; } words[] = {
        {"xx-small", FACE_CAPTION}, {"x-small", FACE_CAPTION}, {"small", FACE_SMALL}, {"medium", FACE_BODY},
        {"large", FACE_H3}, {"x-large", FACE_H2}, {"xx-large", FACE_H1}, {"xxx-large", FACE_H1}};
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) if (is(v, words[i].w)) return words[i].f;
    if (is(v, "smaller")) return (int)face_step(cur, -1);
    if (is(v, "larger")) return (int)face_step(cur, 1);
    int whole = 0, frac = 0, digits = 0, any = 0;
    const char *p = v;
    while (*p >= '0' && *p <= '9') { whole = whole * 10 + (*p++ - '0'); any = 1; }
    if (*p == '.') for (p++; *p >= '0' && *p <= '9'; p++, any = 1) if (digits < 2) { frac = frac * 10 + (*p - '0'); digits++; }
    if (!any || whole > 500) return -1;
    int px100 = whole * 100 + (digits == 1 ? frac * 10 : frac);            /* размер × 100 */
    if (!strncmp(p, "pt", 2)) px100 = px100 * 4 / 3;
    else if (!strncmp(p, "em", 2) || !strncmp(p, "rem", 3)) px100 *= 16;
    else if (*p == '%') px100 = px100 * 16 / 100;
    else if (*p && strncmp(p, "px", 2)) return -1;
    int px = px100 / 100;
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
    e->cls[0] = e->id[0] = 0;
    e->mr = 0;
    e->block = e->box = e->list = 0;
    e->pre_was = (uint8_t)pre;
    return e;
}

/* Снять верхнюю запись: блок с фоном закрыть, вернуть выравнивание, отступ, <pre> и уровень списка,
 * какими они были до тега. */
static void pop_one(void) {
    st_t *e = &stk[sn - 1];
    if (e->block || e->box) { space = 0; doc_flush(0); }
    if (e->box) doc_box_end(4);
    if (e->block) { doc_align(e->align); doc_indent(e->indent); pre = e->pre_was; }
    if (e->list && depth > 0) depth--;
    if (!e->block && e->mr > 0) doc_inline(e->mr, 0, 0, -1);   /* поле справа строчного тега */
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

/* --- таблицы стилей <style> ---------------------------------------------------------------------
 * Правило таблицы стилей — «селектор { свойства }». Из селектора берём последнюю часть («div p»
 * → «p», «a.main» → тег a и класс main, «#menu» → id): этого хватает, чтобы старые сайты получили
 * свои цвета, размеры и подчёркивания. Правила с :hover и подобными (состояния мыши) и блоки
 * @media пропускаются. Свойства правил хранятся строками в общем массиве decls. */
typedef struct {
    char tag[12], cls[24], id[24];       /* что выбирает правило (пусто — любое) */
    char ptag[12], pcls[24], pid[24];    /* родитель из селектора («.menu a» → класс menu); пусто — нет */
    uint8_t parent;                      /* у правила есть родитель */
    uint32_t decl;                       /* где в decls его свойства */
} css_rule_t;
#define RULE_MAX 512
static css_rule_t *rules;
static unsigned rule_n, rule_cap;
static char *decls;
static size_t decl_len, decl_cap;

/* Простой селектор «тег.класс#id» (любая часть может отсутствовать) из s (n байт). 0 — его не
 * понимаем: состояние (:hover и подобные — кроме :link, оно как без него) или [атрибут]. */
static int css_simple(const char *s, size_t n, char *tag, char *cls, char *id) {
    char *dst = tag;
    size_t cap = 12, dn = 0;
    for (size_t t = 0; t < n; t++) {
        char ch = s[t];
        if (ch == ':') {
            if (n - t >= 5 && !strncmp(s + t, ":link", 5)) break;
            return 0;
        }
        if (ch == '[') return 0;
        if (ch == '.' || ch == '#') {
            if ((ch == '.' && cls[0]) || (ch == '#' && id[0])) break;     /* второй класс — хватит первого */
            dst = ch == '.' ? cls : id;
            cap = 24;
            dn = 0;
            continue;
        }
        if (dn + 1 < cap) { dst[dn++] = dst == tag ? (char)lower((unsigned char)ch) : ch; dst[dn] = 0; }
    }
    return tag[0] || cls[0] || id[0];
}

/* Одно правило: селектор sel (sl байт) и свойства d (dl байт). */
static void css_add(const char *sel, size_t sl, const char *d, size_t dl) {
    while (sl && (sel[sl - 1] == ' ' || sel[sl - 1] == '\t' || sel[sl - 1] == '\r' || sel[sl - 1] == '\n')) sl--;
    while (sl && (*sel == ' ' || *sel == '\t' || *sel == '\r' || *sel == '\n')) { sel++; sl--; }
    if (!sl || dl > 2000 || rule_n >= RULE_MAX) return;
    size_t k = sl;                                    /* последняя часть селектора */
    while (k > 0 && !strchr(" >+~\t\r\n", sel[k - 1])) k--;
    css_rule_t r;
    memset(&r, 0, sizeof(r));
    if (!css_simple(sel + k, sl - k, r.tag, r.cls, r.id)) return;
    /* Ближайший родитель («.demoz td», «table a»): без него правило «.demoz td» красило бы все
     * ячейки на странице (у vtrd.in белым по светлому). Соседей «+», «~» не понимаем — мимо. */
    size_t e = k;
    while (e > 0 && strchr(" >\t\r\n", sel[e - 1])) e--;
    if (e < k && strchr(sel + e, '+') && strchr(sel + e, '+') < sel + k) return;
    if (e < k && strchr(sel + e, '~') && strchr(sel + e, '~') < sel + k) return;
    if (e > 0) {
        size_t s = e;
        while (s > 0 && !strchr(" >+~\t\r\n", sel[s - 1])) s--;
        if (!css_simple(sel + s, e - s, r.ptag, r.pcls, r.pid)) return;
        r.parent = !(is(r.ptag, "html") || is(r.ptag, "body")) || r.pcls[0] || r.pid[0];
    }
    if (rule_n == rule_cap) {
        unsigned nc = rule_cap ? rule_cap * 2 : 64;
        css_rule_t *p = ts_core->realloc(rules, nc * sizeof(css_rule_t));
        if (!p) return;
        rules = p; rule_cap = nc;
    }
    if (decl_len + dl + 1 > decl_cap) {
        size_t nc = decl_cap ? decl_cap : 8192;
        while (nc < decl_len + dl + 1) nc *= 2;
        char *p = ts_core->realloc(decls, nc);
        if (!p) return;
        decls = p; decl_cap = nc;
    }
    memcpy(decls + decl_len, d, dl);
    decls[decl_len + dl] = 0;
    r.decl = (uint32_t)decl_len;
    decl_len += dl + 1;
    rules[rule_n++] = r;
}

/* Текст таблицы стилей: комментарии CSS («косая черта со звёздочкой … звёздочка с косой») и «<!-- -->» (старые сайты прятали в них CSS от
 * старых браузеров) — мимо; @import и @charset — до «;»; @media { … } — целиком мимо;
 * «а, b { … }» — правило на каждый селектор. */
NOINLINE static void css_sheet(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '}') { i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t k = i + 2;
            while (k + 1 < n && !(s[k] == '*' && s[k + 1] == '/')) k++;
            i = k + 2;
            continue;
        }
        if (i + 3 < n && !strncmp(s + i, "<!--", 4)) { i += 4; continue; }
        if (i + 2 < n && !strncmp(s + i, "-->", 3)) { i += 3; continue; }
        size_t a = i;
        while (i < n && s[i] != '{' && s[i] != ';') i++;
        if (i >= n) break;
        if (s[a] == '@') {
            if (s[i] == ';') { i++; continue; }
            int nest = 0;
            for (; i < n; i++) {
                if (s[i] == '{') nest++;
                else if (s[i] == '}' && --nest == 0) { i++; break; }
            }
            continue;
        }
        if (s[i] == ';') { i++; continue; }
        size_t sel_end = i, d0 = ++i;
        while (i < n && s[i] != '}') i++;
        size_t d1 = i;
        if (i < n) i++;
        for (size_t p = a; p < sel_end;) {
            size_t q = p;
            while (q < sel_end && s[q] != ',') q++;
            css_add(s + p, q - p, s + d0, d1 - d0);
            p = q + 1;
        }
    }
}

/* Есть ли класс cls в атрибуте class="a b c". */
static int has_class(const char *list, const char *cls) {
    size_t cl = strlen(cls);
    for (const char *p = list; *p;) {
        while (*p == ' ') p++;
        const char *q = p;
        while (*q && *q != ' ') q++;
        if ((size_t)(q - p) == cl && !strncmp(p, cls, cl)) return 1;
        p = q;
    }
    return 0;
}

/* Свойства CSS («color: red; …») — в запись стиля e: цвет, фон, жирность, подчёркивание,
 * размер, выравнивание (у блоков). Фон блока — блоком с фоном (doc_box_begin), фон строчного
 * тега — под его текстом. Возвращает 1, если тег скрыт (display: none). */
static char last_display;                /* display последнего apply_style: 'n' none, 'i' inline, 'b' иное */
static int margin_left;                  /* поле слева строчного тега из CSS (последний apply_style) */

/* Длина CSS в точках: px и число как есть, em — 16 точек; auto и прочее — 0. Не больше 64. */
static int css_len(const char *v) {
    int k = num(v);
    if (k <= 0) return 0;
    if (strstr(v, "em")) k *= 16;
    return k > 64 ? 64 : k;
}

/* Поля слева и справа строчного тега: margin (1–4 значения: верх право низ лево),
 * margin-left, margin-right. Ссылки, разделённые только полями (подвал Google:
 * «#… a{margin:0 12px}»), иначе слипались бы в одно слово. */
static void css_margins(const char *css, int *left, int *right) {
    char v[48];
    if (css_get(css, "margin", v, sizeof(v))) {
        int val[4], k = 0;
        for (char *p = v; *p && k < 4;) {
            while (*p == ' ') p++;
            if (!*p) break;
            val[k++] = css_len(p);
            while (*p && *p != ' ') p++;
        }
        if (k == 1) *left = *right = val[0];
        else if (k == 2 || k == 3) *left = *right = val[1];
        else if (k == 4) { *right = val[1]; *left = val[3]; }
    }
    if (css_get(css, "margin-left", v, sizeof(v))) *left = css_len(v);
    if (css_get(css, "margin-right", v, sizeof(v))) *right = css_len(v);
}
static int apply_decls(st_t *e, const char *css, int blockish) {
    char v[48];
    if (css_get(css, "display", v, sizeof(v))) {
        /* скрыт ли тег, решает последнее display (style="display:block" тега главнее правила
         * «div{display:none}» — так Google прячет всё, кроме своей надписи в <noscript>) */
        last_display = is(v, "none") ? 'n' : !strncmp(v, "inline", 6) ? 'i' : 'b';
        if (last_display == 'n') return 0;
    }
    if (css_get(css, "visibility", v, sizeof(v)) && is(v, "hidden")) return 1;
    int32_t c;
    if (css_get(css, "color", v, sizeof(v)) && (c = parse_color(v)) >= 0) e->fg = (uint32_t)c;
    if ((css_get(css, "background-color", v, sizeof(v)) || css_get(css, "background", v, sizeof(v))) &&
        (c = parse_color(v)) >= 0) {
        if (!blockish) e->bg = (uint32_t)c;
        else if (!e->box && !doc_measuring() && doc_box_begin((uint32_t)c, 4, 2, 0)) e->box = 1;
    }
    if (css_get(css, "font-weight", v, sizeof(v))) {
        if (is(v, "bold") || is(v, "bolder") || (v[0] >= '6' && v[0] <= '9' && v[1] == '0')) e->flags |= PAGE_BOLD;
        else e->flags &= (uint8_t)~PAGE_BOLD;
    }
    if (css_get(css, "text-decoration", v, sizeof(v)) || css_get(css, "text-decoration-line", v, sizeof(v))) {
        if (strstr(v, "none")) e->flags &= (uint8_t)~(PAGE_UNDERLINE | PAGE_STRIKE);
        if (strstr(v, "underline")) e->flags |= PAGE_UNDERLINE;
        if (strstr(v, "line-through")) e->flags |= PAGE_STRIKE;
    }
    if (css_get(css, "font-size", v, sizeof(v))) {
        int f = css_face(v, e->face);
        if (f >= 0) e->face = (uint8_t)f;
    }
    if (blockish && css_get(css, "text-align", v, sizeof(v))) set_align(v);
    if (!blockish) {
        int l = margin_left, r = e->mr;
        css_margins(css, &l, &r);
        margin_left = l;
        e->mr = (int16_t)r;
    }
    return 0;
}

/* Стиль тега name: сначала подходящие правила таблиц стилей по порядку, потом style="…"
 * самого тега (он главнее). Возвращает 1, если тег скрыт (display: none). */
static int apply_style(st_t *e, const char *name, const char *a, size_t n, int blockish) {
    int hidden = 0;
    last_display = 0;
    margin_left = 0;
    char cls[96] = "", id[32] = "";
    if (a) { attr(a, n, "class", cls, sizeof(cls)); attr(a, n, "id", id, sizeof(id)); }
    snprintf(e->cls, sizeof(e->cls), "%s", cls);      /* для правил потомков этого тега */
    snprintf(e->id, sizeof(e->id), "%s", id);
    /* родители — открытые теги под этим (сам тег, если он уже на стеке, родителем не считается) */
    int anc = e >= stk && e < stk + sn ? (int)(e - stk) : sn;
    for (unsigned r = 0; r < rule_n; r++) {
        const css_rule_t *R = &rules[r];
        if (R->tag[0] && !is(R->tag, name) && !is(R->tag, "*")) continue;
        if (R->cls[0] && !has_class(cls, R->cls)) continue;
        if (R->id[0] && strcmp(R->id, id)) continue;
        if (R->parent) {
            int k = anc - 1;
            for (; k >= 0; k--) {
                const st_t *p = &stk[k];
                if ((!R->ptag[0] || is(R->ptag, "*") || is(R->ptag, p->tag)) && (!R->pcls[0] || has_class(p->cls, R->pcls)) &&
                    (!R->pid[0] || !strcmp(R->pid, p->id)))
                    break;
            }
            if (k < 0) continue;
        }
        hidden |= apply_decls(e, decls + R->decl, blockish);
    }
    char css[320];
    if (a && attr(a, n, "style", css, sizeof(css))) hidden |= apply_decls(e, css, blockish);
    return hidden || last_display == 'n';
}

/* Блочный тег: закрыть незакрытый абзац, закончить строку, запомнить, что вернуть при закрытии. */
static st_t *open_block(const char *name, int gap, const char *a, size_t n) {
    close_p();
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

/* <body>: цвета текста и ссылок, фон. У страницы из одного документа фон — у всей страницы; у
 * фрейма — прямоугольник под его частью составной страницы и своя рамка (для сглаживания). */
static void body_tag(const char *a, size_t n) {
    char v[48];
    int32_t c, bg = -1;
    if (attr(a, n, "text", v, sizeof(v)) && (c = parse_color(v)) >= 0)
        for (int k = 0; k < sn; k++) stk[k].fg = (uint32_t)c;
    if (attr(a, n, "link", v, sizeof(v)) && (c = parse_color(v)) >= 0) link_rgb = (uint32_t)c;
    if (attr(a, n, "vlink", v, sizeof(v)) && (c = parse_color(v)) >= 0) vlink_rgb = (uint32_t)c;
    if (attr(a, n, "bgcolor", v, sizeof(v))) bg = parse_color(v);
    st_t probe = stk[0];                              /* таблицы стилей и style="…" у body */
    probe.bg = 0;
    apply_style(&probe, "body", a, n, 0);
    if (probe.fg != stk[0].fg) for (int k = 0; k < sn; k++) stk[k].fg = probe.fg;
    if (probe.bg) bg = (int32_t)probe.bg;
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

/* Ширина из style="width: …": точки или проценты места; 0 — не задана. */
static int css_width(const char *a, size_t n) {
    char css[240], v[24];
    if (!attr(a, n, "style", css, sizeof(css)) || !css_get(css, "width", v, sizeof(v))) return 0;
    int k = num(v);
    if (k <= 0) return 0;
    if (strchr(v, '%')) return doc_width() * (k > 100 ? 100 : k) / 100;
    if (strstr(v, "em")) return k * 16;
    return k;
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

/* Картинка скрыта правилами CSS (display: none, visibility: hidden)? */
static int css_hidden_img(const char *a, size_t n) {
    st_t probe = *top();
    return apply_style(&probe, "img", a, n, 0);
}

/* Вид элемента формы с сайта: ширина и высота из style, фон и цвет текста — из style и таблиц
 * стилей, «border: 0» / «none» — без рамки. Возвращает 1, если элемент скрыт (display: none). */
static int css_look(form_new_t *f, const char *name, const char *a, size_t n) {
    char css[240], v[24];
    st_t probe = *top();
    probe.bg = 0;
    int hidden = apply_style(&probe, name, a, n, 0);
    f->fill = probe.bg;
    f->ink = probe.fg != top()->fg ? probe.fg : 0;
    f->width = css_width(a, n);
    if (attr(a, n, "style", css, sizeof(css))) {
        if (css_get(css, "height", v, sizeof(v))) f->height = num(v);
        if ((css_get(css, "border", v, sizeof(v)) || css_get(css, "border-width", v, sizeof(v)) ||
             css_get(css, "border-style", v, sizeof(v))) && (v[0] == '0' || !strncmp(v, "none", 4)))
            f->no_border = 1;
    }
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
    f.label = label[0] ? label : NULL;
    f.disabled = attr(a, an, "disabled", v, sizeof(v));
    control(&f, css_look(&f, "button", a, an));
}

/* Размер из атрибута width / height: точки или проценты места full; 0 — нет. */
static int size_attr(const char *a, size_t n, const char *name, int full) {
    char v[16];
    if (!attr(a, n, name, v, sizeof(v))) return 0;
    int x = num(v);
    return x <= 0 ? 0 : strchr(v, '%') ? full * x / 100 : x;
}

/* <img src=… alt=… width=… height=…>: место под картинку в строке (image.h); низ картинки — на
 * базовой линии, как у браузеров. Картинки без адреса (или с встроенными данными «data:») — их
 * подпись в квадратных скобках, как раньше. Буферы адресов — static (см. «стек» в tag()). */
static void image_tag(const char *a, size_t n) {
    static char src[URL_MAX], abs_url[URL_MAX];
    char alt[160] = "", css[200], v[24];
    attr(a, n, "alt", alt, sizeof(alt));
    if (css_hidden_img(a, n)) return;
    if (!attr(a, n, "src", src, sizeof(src)) || !src[0] || !url_resolve(base, src, abs_url, sizeof(abs_url))) {
        if (alt[0]) {
            char b[166];
            snprintf(b, sizeof(b), "[%s]", alt);
            out_dim(b);
        }
        return;
    }
    int full = doc_width();
    int aw = size_attr(a, n, "width", full), ah = size_attr(a, n, "height", full);
    int cw = css_width(a, n);
    if (cw > 0) aw = cw;
    if (attr(a, n, "style", css, sizeof(css)) && css_get(css, "height", v, sizeof(v)) && num(v) > 0 && !strchr(v, '%'))
        ah = num(v);
    int w, h;
    int id = image_add(abs_url, alt, aw, ah, full, top()->link, !doc_measuring(), &w, &h);
    shown_n = 1;
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

/* Открывающий тег. Возвращает 1, если тег скрыт (display: none). */
static int open_element(const char *name, const char *a, size_t n) {
    static char v[URL_MAX];                           /* static — см. «стек» в tag() */
    st_t *e;
    if (is(name, "br")) { space = 0; if (doc_empty()) doc_flush(PAGE_LINE_H); else doc_flush(0); return 0; }
    if (is(name, "hr")) { close_p(); block(0); doc_rule(COL_RULE, 1, 12); return 0; }
    if (is(name, "img")) { image_tag(a, n); return 0; }
    if (is(name, "input")) { input_tag(a, n); return 0; }
    if (is(name, "form")) form_tag(a, n);              /* и дальше — как блок */
    if (is_any(name, blocks) || is(name, "p") || is(name, "center")) {
        /* блок, которому сайт сказал display: inline («<form style="display:inline">» в строке
         * меню vtrd.in) — строчный тег: без новой строки и промежутков */
        st_t probe = *top();
        int hidden = apply_style(&probe, name, a, n, 0);
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
        if (!doc_measuring() && doc_box_begin(COL_SOFT, 16, 10, COL_LINK)) e->box = 1;
        return apply_style(e, name, a, n, 1);
    }
    if (is(name, "pre") || is(name, "listing") || is(name, "xmp") || is(name, "plaintext")) {
        if (!(e = open_block(name, 10, a, n))) return 0;
        pre = 1;                                      /* блок кода: заливка и поля 16 точек (Codex) */
        pre_open = 1;
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
        doc_indent(doc_get_indent() + LIST_INDENT);
        return apply_style(e, name, a, n, 1);
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
        /* Пункт без списка (<li> без <ul>, так меню у vtrd.in): отступ, как у пункта списка, —
         * иначе маркер висел бы левее края ячейки, за её фоном. */
        if (!depth) doc_indent(doc_get_indent() + LIST_INDENT);
        /* маркер «висит» слева от текста пункта: точка или номер. Точка — бирюзовая (дизайн
         * Codex), если у текста цвет страницы по умолчанию; у сайта со своими цветами — цвета
         * текста, как у браузеров (иначе на чужом фоне — чужие бирюзовые точки). */
        doc_hang(LIST_INDENT);
        doc_style_t st = style_plain();
        if (depth && list_kind[depth]) {
            int x = attr(a, n, "value", v, 16) ? num(v) : list_num[depth] + 1;
            list_num[depth] = x;
            char mark[16];
            int k = snprintf(mark, sizeof(mark), "%d. ", x);
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
            doc_box_begin((uint32_t)c, 4, 2, 0))
            e->box = 1;
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
    } else if (is(name, "a")) {
        static char abs_url[URL_MAX];
        char tg[32];
        if (attr(a, n, "href", v, sizeof(v)) && url_resolve(base, v, abs_url, sizeof(abs_url))) {
            if (!attr(a, n, "target", tg, sizeof(tg))) snprintf(tg, sizeof(tg), "%s", base_target);
            e->link = (int16_t)(doc_measuring() ? -1 : add_link(abs_url, tg));
            e->fg = visited_cb && visited_cb(abs_url) ? vlink_rgb : link_rgb;
            e->flags |= PAGE_UNDERLINE;
        }
    }
    int hidden = apply_style(e, name, a, n, 0);
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
    if (te) { apply_style(te, "table", attrs, an, 0); te->bg = 0; }

    int border = attr(attrs, an, "border", v, sizeof(v)) ? (v[0] ? num(v) : 1) : 0;
    int pad_x = border ? 12 : 6, pad_y = border ? 5 : 1;  /* без рамки — зазор, как у браузеров */
    if (attr(attrs, an, "cellpadding", v, sizeof(v))) pad_x = pad_y = num(v) > 24 ? 24 : num(v);
    int spacing = attr(attrs, an, "cellspacing", v, sizeof(v)) ? (num(v) > 16 ? 16 : num(v)) : 0;
    int line = border ? 1 : 0, gap = spacing + line;
    int32_t tbg = attr(attrs, an, "bgcolor", v, sizeof(v)) ? parse_color(v) : -1;

    /* 2) мерки ячеек */
    for (int k = 0; k < nc; k++) {
        tcell_t *c = &t->cell[k];
        doc_measure_push();
        doc_frame_push(0, 30000, 0, 0);
        render_range(b + c->a, c->b - c->a, c->th, c->th ? "th" : "td", c->at, c->an);
        doc_frame_pop();
        doc_measure_pop(&c->min, &c->max);
        c->min += 2 * pad_x;
        c->max += 2 * pad_x;
        int w = width_attr(c->at, c->an, 0);
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
    int gaps = (ncols + 1) * gap, avail = doc_width(), want = width_attr(attrs, an, avail);
    if (doc_measuring()) {                            /* таблица внутри измеряемой ячейки */
        doc_measure_add(summin + gaps, (want > summin ? want : summax) + gaps);
        if (te) sn--;
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
    t->colx[0] = x0 + gap;
    for (int q = 0; q < ncols; q++) t->colx[q + 1] = t->colx[q] + t->colw[q] + gap;
    if (border) page_rect(x0, y, width, 1, COL_LINE);
    y += gap;
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
            int32_t bg = attr(c->at, c->an, "bgcolor", v, sizeof(v)) ? parse_color(v) : -1;
            if (bg < 0) bg = rbg;
            if (bg < 0) bg = tbg;
            if (bg < 0 && c->th && border) bg = (int32_t)blend(body_bg ? body_bg : page_bg(), stk[0].fg, 2);
            c->rect = bg >= 0 ? page_rect(cx, top, cw, 0, (uint32_t)bg) : -1;
            doc_frame_push(cx + pad_x, cw - 2 * pad_x, top + pad_y, bg >= 0 ? (uint32_t)bg : 0);
            char al[12];
            if (attr(c->at, c->an, "align", al, sizeof(al))) set_align(al);
            else if (ral[0]) set_align(ral);
            render_range(b + c->a, c->b - c->a, c->th, c->th ? "th" : "td", c->at, c->an);
            int bot = doc_frame_pop() + pad_y;
            if (bot > bottom) bottom = bot;
        }
        for (int q = first; q < k; q++) if (t->cell[q].rect >= 0) page_rect_height(t->cell[q].rect, bottom - top);
        if (border) {
            for (int q = first; q < k; q++)
                if (t->cell[q].col) page_rect(t->colx[t->cell[q].col] - gap, top, 1, bottom - top, COL_LINE);
            page_rect(x0, bottom, width, 1, COL_LINE);
        }
        y = bottom + gap;
    }
    if (te) sn--;
    ts_core->free(t);
    if (form_end && !doc_measuring()) form_close();
    doc_skip_to(y);
    block(12);
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
    if (e) {
        e->block = 1;
        e->align = (int8_t)doc_get_align();
        e->indent = (int16_t)doc_get_indent();
        e->bg = 0;
        if (bold) e->flags |= PAGE_BOLD;
        apply_style(e, tagname, at, an, 1);           /* правила для td/th и style="…" ячейки */
    }
    pre = 0;
    space = 0;
    table_depth++;
    parse(b, n);
    table_depth--;
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
    table_depth = 0;
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
    rule_n = 0;
    decl_len = 0;
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
static void parse(const uint8_t *b, size_t n) {
    size_t i = 0;
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
        if (!closing && is(name, "style")) {                   /* таблица стилей */
            size_t end = find_close(b, n, j, "style");
            if (!doc_measuring()) css_sheet((const char *)b + j, end - j);
            i = end;
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
        if (tag(name, closing, a, a1 - a0)) {                  /* display: none — содержимое мимо */
            size_t end = find_match(b, n, j, name);
            close_element(name);
            i = tag_end(b, n, end);
            continue;
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
    if (body_rect >= 0) {
        int bottom = doc_frame_pop() + 8;
        page_rect_height(body_rect, bottom - body_top);
        doc_skip_to(bottom);
        body_rect = -1;
    }
    block(12);
}

/* Простой текст (text/plain) выводится целиком как <pre>. */
void html_part(const uint8_t *b, size_t n, const char *content_type, const char *url, int part) {
    part_start(url, part);
    cs = charset_detect(content_type, b, n);
    if (content_type && !strncmp(content_type, "text/plain", 10)) { pre = 1; text(b, n); }
    else parse(b, n);
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
