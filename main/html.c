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
 * понимаем. Поля форм выводятся метками вроде «[Найти]»: этап 1 их ещё не отправляет.
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
#include "charset.h"
#include "url.h"
#include "ui_design.h"
#include "ts.h"

/* Цвета страницы — из дизайна Codex. */
#define COL_TEXT TSA_UI_COLOR_INK_RGB888
#define COL_DIM  TSA_UI_COLOR_MUTED_RGB888
#define COL_LINK TSA_UI_COLOR_LINK_RGB888
#define COL_RULE TSA_UI_COLOR_LINE_RGB888
#define COL_BG   TSA_UI_COLOR_PAPER_RGB888

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
static int link_id;                      /* внутри <a>: номер ссылки, иначе −1 */
static int heading;                      /* внутри <h1>…<h6>: уровень 1…6, иначе 0 */
static int quote;                        /* глубина вложенных <blockquote> */
static int space;                        /* был пробел, ещё не выведенный (схлопывание пробелов) */
static int cell;                         /* номер ячейки в строке таблицы */
static int title_len;
/* Вложенные списки: для каждого уровня — нумерованный ли (<ol>) и номер текущего пункта. */
static int list_kind[16], list_num[16], depth;
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

/* Стиль текста по тому, что сейчас открыто: ссылка — синяя и подчёркнутая, крупные
 * заголовки (h1, h2) — подчёркнутые (крупного шрифта пока нет). */
static doc_style_t style_now(void) {
    doc_style_t s = {COL_TEXT, 0, 0, -1};
    if (link_id >= 0) { s.fg = COL_LINK; s.flags = PAGE_UNDERLINE; s.link = link_id; }
    else if (heading && heading <= 2) s.flags = PAGE_UNDERLINE;
    return s;
}

/* Текст страницы — в раскладку текущим стилем. */
static void out_utf8(const char *s, size_t n) {
    doc_style_t st = style_now();
    doc_text(s, (unsigned)n, &st);
}
/* Служебная пометка бледным цветом («[video]», «[Найти]», номер пункта списка). */
static void out_dim(const char *s) {
    doc_style_t st = {COL_DIM, 0, 0, -1};
    if (space && !doc_empty()) doc_text(" ", 1, &st);
    space = 0;
    doc_text(s, (unsigned)strlen(s), &st);
}

/* Служебная пометка-ссылка («[frame]»); id < 0 — просто бледная пометка. Пробел перед ней —
 * без подчёркивания. */
static void out_link(const char *s, int id) {
    if (id < 0) { out_dim(s); return; }
    doc_style_t plain = {COL_TEXT, 0, 0, -1}, st = {COL_LINK, 0, PAGE_UNDERLINE, id};
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
            if (!o && link_id >= 0) {                          /* пробел перед ссылкой — не подчёркивать */
                doc_style_t plain = {COL_TEXT, 0, 0, -1};
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
    "fieldset", "figure", "figcaption", "address", "center", "details", "summary", "dl", "table", "tbody", "thead",
    "tfoot", "caption", "noscript", "legend", "menu", "hgroup", NULL};
/* Теги, содержимое которых пропускаем до закрывающего тега. */
static const char *const skips[] = {"script", "style", "template", "svg", "math", "iframe", "object", "select",
    "textarea", "video", "audio", "canvas", "map", "noembed", "noframes", NULL};

static char skip_until[16];              /* пропускать всё до «</этого_тега»; пусто — не пропускаем */

/* Что делать с тегом name. closing = 1 — это закрывающий тег «</name>»; a и n — текст его
 * атрибутов. Промежутки (числа в block) — в точках, подобраны на глаз, чтобы блоки читались. */
static void tag(const char *name, int closing, const char *a, size_t n) {
    char v[URL_MAX];
    if (is(name, "title")) { in_title = !closing; return; }
    if (!closing && is(name, "base")) {
        char abs_url[URL_MAX];
        if (attr(a, n, "href", v, sizeof(v)) && url_resolve(base, v, abs_url, sizeof(abs_url))) strcpy(base, abs_url);
        attr(a, n, "target", base_target, sizeof(base_target));
        return;
    }
    /* фрейм внутри обычного разбора (набор фреймов, вложенный слишком глубоко) — ссылкой
     * «[frame: имя]» на его страницу во всё окно */
    if (!closing && is(name, "frame")) {
        char abs_url[URL_MAX], nm[32] = "", label[48];
        int id = attr(a, n, "src", v, sizeof(v)) && url_resolve(base, v, abs_url, sizeof(abs_url)) ? add_link(abs_url, "_top") : -1;
        attr(a, n, "name", nm, sizeof(nm));
        snprintf(label, sizeof(label), "[frame%s%s]", nm[0] ? ": " : "", nm);
        block(4);
        out_link(label, id);
        block(4);
        return;
    }
    if (is_any(name, skips)) {
        if (closing) return;
        if (is(name, "select")) out_dim("[▾]");
        else if (is(name, "textarea")) out_dim("[text]");
        else if (is(name, "iframe")) {                     /* встроенный фрейм — ссылкой на его страницу */
            char abs_url[URL_MAX];
            out_link("[frame]", attr(a, n, "src", v, sizeof(v)) && url_resolve(base, v, abs_url, sizeof(abs_url))
                                    ? add_link(abs_url, "_top") : -1);
        }
        else if (is(name, "video")) out_dim("[video]");
        else if (is(name, "audio")) out_dim("[audio]");
        strcpy(skip_until, name);
        return;
    }
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        block(closing ? 10 : 18);
        heading = closing ? 0 : name[1] - '0';
        return;
    }
    if (is(name, "p")) { block(12); return; }
    if (is(name, "br")) { space = 0; if (doc_empty()) doc_flush(PAGE_LINE_H); else doc_flush(0); return; }
    if (is(name, "hr")) { block(0); doc_rule(COL_RULE, 1, 12); return; }
    if (is(name, "pre") || is(name, "listing") || is(name, "xmp")) { block(10); pre = !closing; return; }
    if (is(name, "ul") || is(name, "ol") || is(name, "dir")) {
        block(6);
        if (!closing && depth < 15) { depth++; list_kind[depth] = name[0] == 'o'; list_num[depth] = 0; doc_indent(doc_get_indent() + 3); }
        else if (closing && depth > 0) { depth--; doc_indent(doc_get_indent() - 3); }
        return;
    }
    /* пункт списка: в нумерованном — номер (value= меняет счёт), иначе — точка «•» */
    if (is(name, "li")) {
        block(2);
        if (closing) return;
        doc_style_t st = {COL_DIM, 0, 0, -1};
        if (depth && list_kind[depth]) {
            char num[12]; int k = 0, x = ++list_num[depth], d[10], m = 0;
            if (attr(a, n, "value", v, 16)) { x = 0; for (char *p = v; *p >= '0' && *p <= '9'; p++) x = x * 10 + (*p - '0'); list_num[depth] = x; }
            do { d[m++] = x % 10; x /= 10; } while (x && m < 9);
            while (m) num[k++] = (char)('0' + d[--m]);
            num[k++] = '.'; num[k++] = ' ';
            doc_text(num, (unsigned)k, &st);
        } else {
            doc_text("• ", 4, &st);
        }
        return;
    }
    if (is(name, "dt")) { block(4); return; }
    if (is(name, "dd")) { block(2); if (!closing) out_dim("  "); return; }
    if (is(name, "blockquote")) { block(8); quote += closing ? -1 : 1; doc_indent(doc_get_indent() + (closing ? -4 : 4)); return; }
    /* таблица — строками: каждая строка таблицы с новой строки, ячейки через « |» */
    if (is(name, "tr")) { block(0); cell = 0; return; }
    if (is(name, "td") || is(name, "th")) { if (!closing && cell++) out_dim(" |"); space = 1; return; }
    if (is_any(name, blocks)) { block(4); return; }
    if (is(name, "a")) {
        if (closing) { link_id = -1; return; }
        char abs_url[URL_MAX], tg[32];
        if (!attr(a, n, "target", tg, sizeof(tg))) snprintf(tg, sizeof(tg), "%s", base_target);
        link_id = attr(a, n, "href", v, sizeof(v)) && url_resolve(base, v, abs_url, sizeof(abs_url)) ? add_link(abs_url, tg) : -1;
        return;
    }
    if (is(name, "img") && !closing) {
        if (attr(a, n, "alt", v, 200) && v[0]) {
            char b[210];
            size_t l = strlen(v);
            b[0] = '['; memcpy(b + 1, v, l); b[l + 1] = ']'; b[l + 2] = 0;
            out_dim(b);
        }
        return;
    }
    if (is(name, "input") && !closing) {
        char type[48] = "text";
        attr(a, n, "type", type, sizeof(type));
        for (char *p = type; *p; p++) *p = (char)lower(*p);
        if (is(type, "hidden")) return;
        if (is(type, "checkbox")) { out_dim(attr(a, n, "checked", v, 8) ? "[x]" : "[ ]"); return; }
        if (is(type, "radio")) { out_dim(attr(a, n, "checked", v, 8) ? "(•)" : "( )"); return; }
        /* кнопка — «[Надпись]», поле — «[значение____]»; snprintf, а не strcpy + strcat: из этой
         * пары компилятор делает stpcpy, а её ядро ESP модулям не даёт */
        char b[96];
        if (is(type, "submit") || is(type, "button") || is(type, "reset") || is(type, "image")) {
            if (!attr(a, n, "value", v, 80) || !v[0]) snprintf(v, 80, "%s", is(type, "reset") ? "Reset" : "Submit");
            snprintf(b, sizeof(b), "[%s]", v);
        } else {
            if ((!attr(a, n, "value", v, 60) || !v[0]) && !attr(a, n, "placeholder", v, 60)) v[0] = 0;
            snprintf(b, sizeof(b), "[%s____]", v);
        }
        out_dim(b);
        return;
    }
    if (is(name, "button")) { out_dim(closing ? "]" : "["); return; }
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

/* Сброс разбора для очередного документа (страницы или фрейма): адрес, открытые теги, отступ. */
static void reset_part(const char *url) {
    snprintf(base, sizeof(base), "%s", url);
    base_target[0] = 0;
    pre = in_title = heading = quote = space = cell = depth = 0;
    link_id = -1;
    skip_until[0] = 0;
    doc_indent(0);
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

/* Главный цикл разбора (описан в начале файла). */
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
        tag(name, closing, (const char *)b + a0, a1 - a0);
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
    reset_links();
    doc_begin(COL_BG);
}

/* Черта перед каждым фреймом, кроме первого. */
static void part_start(const char *url, int part) {
    if (part > 0) { block(0); doc_indent(0); doc_rule(COL_RULE, 1, 12); }
    reset_part(url);
    part_now = part;
    title_lock = title_len > 0;
}

/* Простой текст (text/plain) выводится целиком как <pre>. */
void html_part(const uint8_t *b, size_t n, const char *content_type, const char *url, int part) {
    part_start(url, part);
    cs = charset_detect(content_type, b, n);
    if (content_type && !strncmp(content_type, "text/plain", 10)) { pre = 1; text(b, n); }
    else parse(b, n);
    block(12);
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

void html_render(const uint8_t *b, size_t n, const char *content_type, const char *url) {
    html_begin();
    html_part(b, n, content_type, url, -1);
    html_end();
}

/* Страница ошибки: заголовок, адрес, подробности и подсказка, что делать дальше. */
void html_error_page(const char *url, const char *what, const char *detail) {
    reset_links();
    reset_part(url);
    doc_begin(COL_BG);
    doc_style_t h = {COL_TEXT, 0, PAGE_UNDERLINE, -1}, t = {COL_TEXT, 0, 0, -1}, d = {COL_DIM, 0, 0, -1};
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
