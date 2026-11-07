/* SVG — векторные картинки (что это и чего nanosvg не умеет — в svg.h).
 *
 * SVG — это не точки, а текст XML с описанием фигур: «контур из таких-то отрезков и дуг, залить таким
 * цветом». Чтобы показать его, картинку надо «растеризовать» — нарисовать фигуры точками нужного
 * размера. Это делает nanosvg (svg_lib.c): nsvgParse разбирает текст в контуры, nsvgRasterize рисует
 * их со сглаживанием в RGBA. Векторная картинка рисуется сразу в размер места на странице — без
 * потери чёткости, как бы её ни растянули.
 *
 * <use href="#id"> — «нарисуй ещё раз элемент id» (так, например, сделан флаг Великобритании: одна
 * половина креста и она же, повёрнутая). nanosvg его не знает, поэтому до разбора каждый <use>
 * заменяется в тексте копией элемента id внутри группы <g> с преобразованием самого <use>. */
#include <string.h>
#include "svg.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "ts.h"

struct svg {
    NSVGimage *im;                       /* разобранные контуры */
    NSVGrasterizer *rast;                /* рисовальщик (его рабочая память) */
    float k;                             /* свой размер (svg_size) — во столько раз больше размера
                                          * nanosvg: 1, если у корня <svg> есть width или height */
};

/* --- распознать SVG по первым байтам --- */

int svg_is(const uint8_t *b, size_t len) {
    size_t i = 0, n = len < 4096 ? len : 4096;
    if (len >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) i = 3;   /* метка UTF-8 */
    while (i < len && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n')) i++;
    if (i >= len || b[i] != '<') return 0;
    for (size_t k = i; k + 5 <= n; k++) {
        if (!memcmp(b + k, "<svg", 4)) return 1;
        if (!memcmp(b + k, "<html", 5) || !memcmp(b + k, "<HTML", 5)) return 0;
    }
    return 0;
}

/* --- текст, который растёт: подстановка <use> --- */

typedef struct { char *p; size_t n, cap; int ok; } text_t;

static void put(text_t *t, const char *s, size_t n) {
    if (!t->ok) return;
    if (t->n + n + 1 > t->cap) {
        size_t c = t->cap ? t->cap * 2 : 4096;
        while (c < t->n + n + 1) c *= 2;
        char *q = ts_core->realloc(t->p, c);
        if (!q) { t->ok = 0; return; }
        t->p = q;
        t->cap = c;
    }
    memcpy(t->p + t->n, s, n);
    t->n += n;
    t->p[t->n] = 0;
}
static void put_str(text_t *t, const char *s) { put(t, s, strlen(s)); }

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* Значение атрибута name (name="…" или name='…') в теге tag…end: *val и *vlen. 0 — нет такого.
 * Имя должно стоять отдельно: «href» не найдётся внутри «xlink:href». */
static int attr_value(const char *tag, const char *end, const char *name, const char **val, size_t *vlen) {
    size_t nl = strlen(name);
    for (const char *p = tag + 1; p + nl < end; p++) {
        if (!is_space(p[-1]) || strncmp(p, name, nl)) continue;
        const char *q = p + nl;
        while (q < end && is_space(*q)) q++;
        if (q >= end || *q != '=') continue;
        q++;
        while (q < end && is_space(*q)) q++;
        if (q >= end || (*q != '"' && *q != '\'')) continue;
        char quote = *q++;
        const char *v = q;
        while (q < end && *q != quote) q++;
        if (q >= end) return 0;
        *val = v;
        *vlen = (size_t)(q - v);
        return 1;
    }
    return 0;
}

/* Конец тега, начатого в p (за «>»); *self — тег закрыт сам («/>»). NULL — тег не закончен. */
static const char *tag_end(const char *p, const char *end, int *self) {
    while (p < end && *p != '>') p++;
    if (p >= end) return NULL;
    *self = p[-1] == '/';
    return p + 1;
}

/* Элемент с id="name" в тексте src…end: его начало («<») и конец (за «>» закрывающего тега или
 * самого тега, если он закрыт сам), имя тега — в tag. 0 — такого нет. Вложенные элементы с тем же
 * именем тега считаются, чтобы найти свой закрывающий тег. */
static int find_element(const char *src, const char *end, const char *id, size_t idn,
                        const char **s0, const char **s1, char *tag, size_t tagcap) {
    for (const char *p = src; p + idn + 5 < end; p++) {
        if (p[0] != 'i' || p[1] != 'd' || p[2] != '=' || (p > src && !is_space(p[-1]))) continue;
        if ((p[3] != '"' && p[3] != '\'') || strncmp(p + 4, id, idn) || p[4 + idn] != p[3]) continue;
        const char *lt = p;
        while (lt > src && *lt != '<' && *lt != '>') lt--;
        if (*lt != '<') continue;
        size_t tn = 0;
        while (lt + 1 + tn < end && !is_space(lt[1 + tn]) && lt[1 + tn] != '>' && lt[1 + tn] != '/' && tn + 1 < tagcap)
            tn++;
        memcpy(tag, lt + 1, tn);
        tag[tn] = 0;
        int self;
        const char *q = tag_end(p, end, &self);
        if (!q) return 0;
        if (!self) {                                     /* найти свой закрывающий тег */
            int depth = 1;
            while (depth > 0) {
                const char *c = q;
                while (c < end && *c != '<') c++;
                if (c >= end) return 0;
                int closing = c[1] == '/';
                const char *nm = c + (closing ? 2 : 1);
                int same = nm + tn <= end && !strncmp(nm, tag, tn) &&
                           (is_space(nm[tn]) || nm[tn] == '>' || nm[tn] == '/');
                int s;
                q = tag_end(c, end, &s);
                if (!q) return 0;
                if (same) depth += closing ? -1 : (s ? 0 : 1);
            }
        }
        *s0 = lt;
        *s1 = q;
        return 1;
    }
    return 0;
}

/* Число из значения атрибута (x="10", y="-2.5px"): только знак, цифры, точка и порядок — единицы
 * отбрасываются (nanosvg в translate() их не ждёт). */
static void put_number(text_t *t, const char *v, size_t n) {
    size_t k = 0;
    while (k < n && (strchr("+-.0123456789eE", v[k]) != NULL)) k++;
    if (k) put(t, v, k);
    else put_str(t, "0");
}

/* Копия текста src (с нулём в конце), где каждый <use href="#id"> заменён на
 * <g transform="(transform <use>) translate(x, y)">копия элемента id</g>. Элемент <symbol>
 * становится <g>. Вложенные <use> раскрываются за несколько проходов (не больше 4, и копия — не
 * больше 8 размеров исходного: <use> может ссылаться и сам на себя). NULL — нет памяти. */
static char *expand_use(const char *src, size_t len) {
    text_t cur = {NULL, 0, 0, 1};
    put(&cur, src, len);
    for (int pass = 0; pass < 4 && cur.ok && strstr(cur.p, "<use"); pass++) {
        text_t out = {NULL, 0, 0, 1};
        const char *p = cur.p, *e = cur.p + cur.n;
        while (p < e) {
            const char *u = strstr(p, "<use");
            if (!u) { put(&out, p, (size_t)(e - p)); break; }
            if (!is_space(u[4]) && u[4] != '/' && u[4] != '>') {   /* <useless…> — не тот тег */
                put(&out, p, (size_t)(u + 4 - p));
                p = u + 4;
                continue;
            }
            put(&out, p, (size_t)(u - p));
            int self;
            const char *te = tag_end(u, e, &self);
            if (!te) { p = e; break; }
            const char *after = te;
            if (!self) {                                 /* <use …>…</use> */
                const char *c = strstr(te, "</use>");
                if (c) after = c + 6;
            }
            const char *v, *s0, *s1;
            size_t vn;
            char tag[32];
            if ((attr_value(u, te, "href", &v, &vn) || attr_value(u, te, "xlink:href", &v, &vn)) && vn > 1 &&
                v[0] == '#' && find_element(cur.p, e, v + 1, vn - 1, &s0, &s1, tag, sizeof(tag))) {
                put_str(&out, "<g transform=\"");
                if (attr_value(u, te, "transform", &v, &vn)) { put(&out, v, vn); put_str(&out, " "); }
                put_str(&out, "translate(");
                if (attr_value(u, te, "x", &v, &vn)) put_number(&out, v, vn); else put_str(&out, "0");
                put_str(&out, ",");
                if (attr_value(u, te, "y", &v, &vn)) put_number(&out, v, vn); else put_str(&out, "0");
                put_str(&out, ")\">");
                size_t n = (size_t)(s1 - s0);
                if (!strcmp(tag, "symbol") && n > 16 && !strncmp(s1 - 9, "</symbol>", 9)) {
                    put_str(&out, "<g");                 /* <symbol …>…</symbol> → <g …>…</g> */
                    put(&out, s0 + 7, n - 7 - 9);
                    put_str(&out, "</g>");
                } else if (strcmp(tag, "symbol")) {
                    put(&out, s0, n);
                }
                put_str(&out, "</g>");
            }
            p = after;
        }
        ts_core->free(cur.p);
        cur = out;
        if (cur.n > 8 * len + 65536) break;
    }
    if (!cur.ok) {
        if (cur.p) ts_core->free(cur.p);
        return NULL;
    }
    return cur.p;
}

/* --- разбор и отрисовка --- */

/* У корня <svg> задан свой размер — width или height не в процентах? Нет — у картинки есть только
 * пропорции (viewBox), а размер на странице задаёт CSS сайта. */
static int root_sized(const char *b, size_t len) {
    const char *end = b + len;
    for (const char *p = b; p + 5 <= end; p++) {
        if (memcmp(p, "<svg", 4) || (!is_space(p[4]) && p[4] != '>')) continue;
        int self;
        const char *te = tag_end(p, end, &self);
        if (!te) return 0;
        const char *v;
        size_t n;
        int w = attr_value(p, te, "width", &v, &n) && n && !memchr(v, '%', n);
        int h = attr_value(p, te, "height", &v, &n) && n && !memchr(v, '%', n);
        return w || h;
    }
    return 0;
}

svg_t *svg_open(const uint8_t *body, size_t len) {
    char *text = expand_use((const char *)body, len);   /* nanosvg портит свой текст: копия */
    if (!text) return NULL;
    NSVGimage *im = nsvgParse(text, "px", 96.0f);
    ts_core->free(text);
    if (!im) return NULL;
    if (im->width <= 0.0f || im->height <= 0.0f) { nsvgDelete(im); return NULL; }
    svg_t *s = ts_alloc(sizeof(*s));
    NSVGrasterizer *r = s ? nsvgCreateRasterizer() : NULL;
    if (!r) {
        if (s) ts_core->free(s);
        nsvgDelete(im);
        return NULL;
    }
    s->im = im;
    s->rast = r;
    s->k = 1.0f;
    if (!root_sized((const char *)body, len))            /* только viewBox — вписать в 300 × 150 */
        s->k = 300.0f / im->width < 150.0f / im->height ? 300.0f / im->width : 150.0f / im->height;
    return s;
}

/* Свой размер SVG. Без width и height (только viewBox) — как у браузеров без CSS: размер по
 * умолчанию CSS 300 × 150, в который картинка вписывается со своими пропорциями (CSS Images 3,
 * «default object size»). Раньше бралась ширина viewBox: флажки языков zx-evolution.com (viewBox
 * 900 × 600, размер — во внешнем CSS сайта, его браузер не грузит) вставали шириной 900 точек и
 * занимали по 280 КБ памяти видеочипа — вдвоём с логотипом почти весь пул картинок (07.11). */
void svg_size(const svg_t *s, float *w, float *h) {
    *w = s->im->width * s->k;
    *h = s->im->height * s->k;
}

/* Полоса строк: nanosvg рисует строки 0…rows − 1 своего «холста», поэтому картинка сдвигается
 * вверх на y0. Каждый вызов заново раскладывает все контуры на отрезки — для значков и логотипов
 * это быстро, зато память — только на полосу, а не на всю картинку. */
int svg_render(svg_t *s, float scale, float tx, float ty, int w, int y0, int rows, uint8_t *rgba) {
    nsvgRasterize(s->rast, s->im, tx, ty - (float)y0, scale * s->k, rgba, w, rows, w * 4);   /* scale — от svg_size */
    return 1;
}

void svg_close(svg_t *s) {
    if (!s) return;
    nsvgDeleteRasterizer(s->rast);
    nsvgDelete(s->im);
    ts_core->free(s);
}
