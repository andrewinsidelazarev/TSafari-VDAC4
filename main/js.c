/* JavaScript страницы — см. js.h.
 *
 * Здесь — то, что удобнее на C: память движка, разбор селекторов (js_sel.c), обход и правка
 * дерева (dom.c), правила CSS для getComputedStyle (css.c), список скриптов и их запуск. Сам DOM,
 * каким его видят скрипты (Node, Element, классы, события, таймеры, window), — на JavaScript, в
 * main/js_prelude.js: так его в разы проще писать и читать. Пролог получает объект «N» с
 * функциями отсюда (N.parent(id), N.setAttr(id, имя, значение)…) — узел для них просто номер.
 *
 * ПАМЯТЬ. QuickJS берёт и отдаёт память тысячами мелких кусков (объект — десятки байт). Брать
 * каждый у прошивки дорого (у неё на кусок свой заголовок) и опасно (забытый кусок останется до
 * перезагрузки). Поэтому своя куча: мелкие куски (до 1 КБ) — из больших блоков по 64 КБ по
 * размерам-«классам» (свободные — в списки по классу), крупные — у прошивки, в своём списке. Конец
 * страницы — все блоки отдаются целиком, что бы движок ни забыл. */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "quickjs.h"
#include "js.h"
#include "dom.h"
#include "js_sel.h"
#include "css.h"
#include "url.h"
#include "ts.h"
#include "js_prelude.h"
#ifdef TSAFARI_HOST
#include <stdlib.h>
#endif

/* --- куча движка ---------------------------------------------------------------------------- */

#define CHUNK (64u * 1024u)
#define NCLS 17
static const uint16_t cls_size[NCLS] = {16, 24, 32, 40, 48, 64, 80, 96, 128, 160, 192, 256, 320, 384, 512, 768, 1024};
#define BIG 0xFFFFu
typedef struct { uint32_t cls, size; } bh_t;          /* перед каждым куском: класс (BIG — крупный), размер */
typedef struct big { struct big *prev, *next; bh_t h; } big_t;
static uint8_t cls_of[129];                             /* (размер + 7) / 8 → класс */
static void *free_list[NCLS];
static uint8_t *chunk, *chunks;                         /* текущий блок; список всех (первые 8 байт — следующий) */
static size_t chunk_used;
static big_t *bigs;
static size_t heap_now, heap_peak;

static void heap_init(void) {
    int c = 0;
    for (int k = 0; k <= 128; k++) {
        while (c < NCLS - 1 && cls_size[c] < k * 8) c++;
        cls_of[k] = (uint8_t)c;
    }
}

/* Запас PSRAM системе. В начале js_run — один раз: самый большой свободный кусок PSRAM (проба
 * убывающими размерами, взять и сразу отдать) минус RESERVE — бюджет движка (budget). Блоки движок
 * берёт у прошивки, пока их сумма в бюджете; больше — «нет памяти» (скрипт падает с ошибкой), а
 * сеть, Wi-Fi и картинки не остаются без памяти. Проба на каждый блок (так было в ELF 48337974)
 * сама на миг забирала мегабайт — свободной PSRAM на плате 08.11 оставалось 30 КБ. (Функции «сколько
 * свободно» прошивка модулям не даёт.) */
#define RESERVE (1024u * 1024u)
static int starved;                                     /* отказали из-за бюджета — в журнал */
static size_t budget, taken;                            /* бюджет движка; взято блоками у прошивки */
static size_t budget_find(void) {
    size_t s = 4u * 1024u * 1024u;
    for (; s >= 512u * 1024u; s -= 256u * 1024u) {
        void *p = ts_alloc(s);
        if (p) { ts_core->free(p); break; }
    }
    return s > RESERVE + 256u * 1024u ? s - RESERVE : 256u * 1024u;
}
static void *sys_alloc(size_t n) {
    if (taken + n > budget) { starved = 1; return NULL; }
    void *p = ts_alloc(n);
    if (!p) { starved = 1; return NULL; }
    taken += n;
    return p;
}

static void *h_malloc(void *o, size_t n) {
    (void)o;
    if (!n) n = 1;
    if (n <= 1024) {
        int c = cls_of[(n + 7) / 8];
        uint8_t *p = free_list[c];
        if (p) {
            free_list[c] = *(void **)p;
        } else {
            size_t need = sizeof(bh_t) + cls_size[c];
            if (!chunk || chunk_used + need > CHUNK) {
                uint8_t *b = sys_alloc(CHUNK);
                if (!b) return NULL;
                *(uint8_t **)b = chunks;
                chunks = b;
                chunk = b;
                chunk_used = 8;
            }
            bh_t *h = (bh_t *)(chunk + chunk_used);
            chunk_used += need;
            h->cls = (uint32_t)c;
            p = (uint8_t *)(h + 1);
        }
        ((bh_t *)p - 1)->size = cls_size[c];
        heap_now += cls_size[c];
        if (heap_now > heap_peak) heap_peak = heap_now;
        return p;
    }
    big_t *b = sys_alloc(sizeof(big_t) + n);
    if (!b) return NULL;
    b->h.cls = BIG;
    b->h.size = (uint32_t)n;
    b->prev = NULL;
    b->next = bigs;
    if (bigs) bigs->prev = b;
    bigs = b;
    heap_now += n;
    if (heap_now > heap_peak) heap_peak = heap_now;
    return b + 1;
}

static void h_free(void *o, void *p) {
    (void)o;
    if (!p) return;
    bh_t *h = (bh_t *)p - 1;
    heap_now -= h->size;
    if (h->cls != BIG) {
        *(void **)p = free_list[h->cls];
        free_list[h->cls] = p;
        return;
    }
    big_t *b = (big_t *)p - 1;
    if (b->prev) b->prev->next = b->next; else bigs = b->next;
    if (b->next) b->next->prev = b->prev;
    taken -= sizeof(big_t) + h->size;
    ts_core->free(b);
}

static size_t h_usable(const void *p) { return p ? ((const bh_t *)p - 1)->size : 0; }

static void *h_calloc(void *o, size_t c, size_t s) {
    void *p = h_malloc(o, c * s);
    if (p) memset(p, 0, c * s);
    return p;
}

static void *h_realloc(void *o, void *p, size_t n) {
    if (!p) return h_malloc(o, n);
    if (!n) { h_free(o, p); return NULL; }
    size_t have = h_usable(p);
    if (n <= have && (((bh_t *)p - 1)->cls == BIG || n > have / 2)) return p;
    void *q = h_malloc(o, n);
    if (!q) return NULL;
    memcpy(q, p, have < n ? have : n);
    h_free(o, p);
    return q;
}

static const JSMallocFunctions heap_fns = {h_calloc, h_malloc, h_free, h_realloc, h_usable};

/* Отдать всю кучу (конец страницы) */
static void heap_drop(void) {
    taken = 0;
    while (chunks) { uint8_t *nx = *(uint8_t **)chunks; ts_core->free(chunks); chunks = nx; }
    while (bigs) { big_t *nx = bigs->next; ts_core->free(bigs); bigs = nx; }
    memset(free_list, 0, sizeof(free_list));
    chunk = NULL;
    chunk_used = 0;
    heap_now = 0;
}

/* --- состояние страницы --------------------------------------------------------------------- */

#define SCRIPTS 96
typedef struct {
    dom_id el;
    char *url;                          /* внешний: адрес (ts_alloc); встроенный — NULL */
    uint8_t *body;                      /* пришедший текст (ts_alloc) */
    size_t len;
    uint8_t state;                      /* 0 встроенный, 1 ждёт загрузки, 2 загружается, 3 пришёл, 4 не пришёл */
    uint8_t module;
} script_t;
static script_t sc[SCRIPTS];
static int scn;
static char base_url[512];
static JSRuntime *rt;
static JSContext *ctx;
static JSValue hooks;                   /* объект пролога: scriptStart, scriptEnd, ready, load, tick */
static uint32_t deadline, t_run;
static int changed, ok_n, fail_n;

#define LOGN 48
static char logs[LOGN][136];
static int log_head, log_tail;
static void jlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void jlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(logs[log_head % LOGN], sizeof(logs[0]), fmt, ap);
    va_end(ap);
    log_head++;
    if (log_head - log_tail > LOGN) log_tail = log_head - LOGN;
}
const char *js_log_next(void) { return log_tail < log_head ? logs[log_tail++ % LOGN] : NULL; }

/* --- функции для пролога: дерево -------------------------------------------------------------- */

static dom_id ID(JSValueConst v) {
    uint32_t x = 0;
    JS_ToUint32(ctx, &x, v);
    return x;
}
static JSValue STR(const char *s) { return JS_NewString(ctx, s ? s : ""); }
/* Строка из значения JS — во временный буфер (освобождается JS_FreeCString) */
#define CSTR(var, val) const char *var = JS_ToCString(ctx, val); if (!var) return JS_EXCEPTION

#define A0 (argc > 0 ? argv[0] : JS_UNDEFINED)
#define A1 (argc > 1 ? argv[1] : JS_UNDEFINED)
#define A2 (argc > 2 ? argv[2] : JS_UNDEFINED)
#define FN(name) static JSValue name(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv)
#define UNUSED (void)c; (void)this_val

FN(n_type) { UNUSED; return JS_NewInt32(ctx, dom_type(ID(A0))); }
FN(n_parent) { UNUSED; return JS_NewInt32(ctx, (int32_t)dom_parent(ID(A0))); }
FN(n_first) { UNUSED; return JS_NewInt32(ctx, (int32_t)dom_first(ID(A0))); }
FN(n_last) { UNUSED; return JS_NewInt32(ctx, (int32_t)dom_last(ID(A0))); }
FN(n_next) { UNUSED; return JS_NewInt32(ctx, (int32_t)dom_next(ID(A0))); }
FN(n_prev) { UNUSED; return JS_NewInt32(ctx, (int32_t)dom_prev(ID(A0))); }
FN(n_html) { UNUSED; (void)argc; (void)argv; return JS_NewInt32(ctx, (int32_t)dom_html()); }
FN(n_head) { UNUSED; (void)argc; (void)argv; return JS_NewInt32(ctx, (int32_t)dom_head()); }
FN(n_body) { UNUSED; (void)argc; (void)argv; return JS_NewInt32(ctx, (int32_t)dom_body()); }
FN(n_tag) { UNUSED; return STR(dom_tag(ID(A0))); }
FN(n_data) {
    UNUSED;
    size_t l;
    const char *s = dom_data(ID(A0), &l);
    return JS_NewStringLen(ctx, s, l);
}
FN(n_setdata) {
    UNUSED;
    size_t l;
    const char *s = JS_ToCStringLen(ctx, &l, A1);
    if (!s) return JS_EXCEPTION;
    dom_set_data(ID(A0), s, l);
    JS_FreeCString(ctx, s);
    changed = 1;
    return JS_UNDEFINED;
}
FN(n_kids) {
    UNUSED;
    JSValue a = JS_NewArray(ctx);
    uint32_t i = 0;
    for (dom_id k = dom_first(ID(A0)); k; k = dom_next(k)) JS_SetPropertyUint32(ctx, a, i++, JS_NewInt32(ctx, (int32_t)k));
    return a;
}
FN(n_attrs) {
    UNUSED;
    dom_id n = ID(A0);
    JSValue a = JS_NewArray(ctx);
    for (int i = 0; i < dom_attr_count(n); i++) {
        JS_SetPropertyUint32(ctx, a, (uint32_t)(2 * i), STR(dom_attr_name(n, i)));
        JS_SetPropertyUint32(ctx, a, (uint32_t)(2 * i + 1), STR(dom_attr_value(n, i)));
    }
    return a;
}
FN(n_getattr) {
    UNUSED;
    CSTR(name, A1);
    const char *v = dom_get_attr(ID(A0), name);
    JS_FreeCString(ctx, name);
    return v ? STR(v) : JS_NULL;
}
FN(n_setattr) {
    UNUSED;
    CSTR(name, A1);
    const char *v = JS_ToCString(ctx, A2);
    if (!v) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    dom_set_attr(ID(A0), name, v);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, v);
    changed = 1;
    return JS_UNDEFINED;
}
FN(n_rmattr) {
    UNUSED;
    CSTR(name, A1);
    dom_remove_attr(ID(A0), name);
    JS_FreeCString(ctx, name);
    changed = 1;
    return JS_UNDEFINED;
}
FN(n_create) {
    UNUSED;
    CSTR(tag, A0);
    dom_id n = dom_create_element(tag);
    JS_FreeCString(ctx, tag);
    return JS_NewInt32(ctx, (int32_t)n);
}
FN(n_text) {
    UNUSED;
    size_t l;
    const char *s = JS_ToCStringLen(ctx, &l, A0);
    if (!s) return JS_EXCEPTION;
    dom_id n = argc > 1 && JS_ToBool(ctx, A1) ? dom_create_comment(s, l) : dom_create_text(s, l);
    JS_FreeCString(ctx, s);
    return JS_NewInt32(ctx, (int32_t)n);
}
FN(n_frag) { UNUSED; (void)argc; (void)argv; return JS_NewInt32(ctx, (int32_t)dom_create_fragment()); }
FN(n_clone) { UNUSED; return JS_NewInt32(ctx, (int32_t)dom_clone(ID(A0), JS_ToBool(ctx, A1))); }
FN(n_insert) {
    UNUSED;
    int r = dom_insert(ID(A0), ID(A1), ID(A2));
    if (r && dom_contains(DOM_DOC, ID(A0))) changed = 1;
    return JS_NewBool(ctx, r);
}
FN(n_detach) {
    UNUSED;
    dom_id n = ID(A0);
    if (dom_contains(DOM_DOC, n)) changed = 1;
    dom_detach(n);
    return JS_UNDEFINED;
}
FN(n_inserthtml) {
    UNUSED;
    size_t l;
    const char *s = argc > 2 ? JS_ToCStringLen(ctx, &l, A2) : NULL;
    if (!s) return JS_EXCEPTION;
    int r = dom_insert_html(ID(A0), ID(A1), s, l);
    JS_FreeCString(ctx, s);
    if (dom_contains(DOM_DOC, ID(A0))) changed = 1;
    return JS_NewBool(ctx, r);
}
FN(n_serialize) {
    UNUSED;
    size_t l;
    char *s = dom_serialize(ID(A0), JS_ToBool(ctx, A1), 0, &l);
    if (!s) return JS_ThrowOutOfMemory(ctx);
    JSValue v = JS_NewStringLen(ctx, s, l);
    ts_core->free(s);
    return v;
}
FN(n_textcontent) {
    UNUSED;
    dom_id n = ID(A0);
    size_t l = dom_text_content(n, NULL, 0);
    char *b = ts_alloc(l + 1);
    if (!b) return JS_ThrowOutOfMemory(ctx);
    dom_text_content(n, b, l + 1);
    JSValue v = JS_NewStringLen(ctx, b, l);
    ts_core->free(b);
    return v;
}
FN(n_contains) { UNUSED; return JS_NewBool(ctx, dom_contains(ID(A0), ID(A1))); }

/* Найти по селектору внутри root: массив номеров; first — только первый (или null) */
FN(n_find) {
    UNUSED;
    size_t l;
    const char *s = JS_ToCStringLen(ctx, &l, A1);
    if (!s) return JS_EXCEPTION;
    int ok = js_sel_compile(s, l);
    if (!ok) {
        JSValue e = JS_ThrowSyntaxError(ctx, "'%s' is not a valid selector", s);
        JS_FreeCString(ctx, s);
        return e;
    }
    JS_FreeCString(ctx, s);
    int first = JS_ToBool(ctx, A2);
    static dom_id ids[4096];                   /* static: выше 4 тысяч — второй проход не нужен страницам */
    size_t n = js_sel_find(ID(A0), first, ids, sizeof(ids) / sizeof(ids[0]));
    if (n > sizeof(ids) / sizeof(ids[0])) n = sizeof(ids) / sizeof(ids[0]);
    if (first) return n ? JS_NewInt32(ctx, (int32_t)ids[0]) : JS_NULL;
    JSValue a = JS_NewArray(ctx);
    for (size_t i = 0; i < n; i++) JS_SetPropertyUint32(ctx, a, (uint32_t)i, JS_NewInt32(ctx, (int32_t)ids[i]));
    return a;
}
FN(n_matches) {
    UNUSED;
    size_t l;
    const char *s = JS_ToCStringLen(ctx, &l, A1);
    if (!s) return JS_EXCEPTION;
    int ok = js_sel_compile(s, l);
    if (!ok) {
        JSValue e = JS_ThrowSyntaxError(ctx, "'%s' is not a valid selector", s);
        JS_FreeCString(ctx, s);
        return e;
    }
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, js_sel_match(ID(A0), argc > 2 ? ID(A2) : 0));
}
/* Элемент с таким id (первый по порядку документа) */
FN(n_byid) {
    UNUSED;
    CSTR(want, A0);
    dom_id k = dom_first(DOM_DOC), found = 0;
    while (k && k != DOM_DOC && !found) {
        if (dom_type(k) == DOM_ELEMENT) {
            const char *v = dom_get_attr(k, "id");
            if (v && !strcmp(v, want)) { found = k; break; }
        }
        if (dom_first(k)) { k = dom_first(k); continue; }
        while (k && k != DOM_DOC && !dom_next(k)) k = dom_parent(k);
        if (k && k != DOM_DOC) k = dom_next(k);
    }
    JS_FreeCString(ctx, want);
    return found ? JS_NewInt32(ctx, (int32_t)found) : JS_NULL;
}
/* Порядок двух узлов в документе: −1 — a раньше b, 1 — позже, 0 — один и тот же или в разных деревьях */
static int depth_of(dom_id n) { int d = 0; while (n) { n = dom_parent(n); d++; } return d; }
FN(n_order) {
    UNUSED;
    dom_id a = ID(A0), b = ID(A1);
    if (a == b) return JS_NewInt32(ctx, 0);
    int da = depth_of(a), db = depth_of(b);
    dom_id x = a, y = b;
    while (da > db) { x = dom_parent(x); da--; }
    while (db > da) { y = dom_parent(y); db--; }
    if (x == y) return JS_NewInt32(ctx, x == a ? -1 : 1);       /* один внутри другого: предок раньше */
    while (dom_parent(x) != dom_parent(y)) { x = dom_parent(x); y = dom_parent(y); }
    if (!dom_parent(x)) return JS_NewInt32(ctx, 0);              /* разные деревья */
    for (dom_id s = dom_next(x); s; s = dom_next(s)) if (s == y) return JS_NewInt32(ctx, -1);
    return JS_NewInt32(ctx, 1);
}

/* --- функции для пролога: CSS, адреса, время, журнал ------------------------------------------ */

typedef struct { dom_id n; } actx_t;
static int attr_cb(void *c, const char *name, char *out, size_t cap) {
    const char *v = dom_get_attr(((actx_t *)c)->n, name);
    if (!v) return 0;
    snprintf(out, cap, "%s", v);
    return 1;
}
static void el_of(dom_id n, css_el_t *el) {
    const char *cl = dom_get_attr(n, "class"), *id = dom_get_attr(n, "id");
    css_el_make(el, dom_tag(n), cl ? cl : "", id ? id : "");
}
/* Значение свойства CSS элемента: style="…", иначе последнее из подошедших правил; "" — нет */
FN(n_cssprop) {
    UNUSED;
    dom_id n = ID(A0);
    CSTR(prop, A1);
    static char v[256];
    v[0] = 0;
    const char *st = dom_get_attr(n, "style");
    if (!(st && css_get(st, prop, v, sizeof(v)))) {
        static css_el_t el, ancs[24];
        static const css_el_t *anc[24];
        el_of(n, &el);
        int na = 0;
        for (dom_id p = dom_parent(n); p && dom_type(p) == DOM_ELEMENT && na < 24; p = dom_parent(p)) {
            el_of(p, &ancs[na]);
            anc[na] = &ancs[na];
            na++;
        }
        static const char *list[64];
        actx_t ac = {n};
        int m = css_match(&el, attr_cb, &ac, anc, na, 0, list, 64);
        v[0] = 0;
        for (int i = 0; i < m; i++) {
            static char w[256];
            if (css_get(list[i], prop, w, sizeof(w))) memcpy(v, w, sizeof(v));
        }
    }
    JS_FreeCString(ctx, prop);
    return STR(v);
}
FN(n_media) {
    UNUSED;
    size_t l;
    const char *q = JS_ToCStringLen(ctx, &l, A0);
    if (!q) return JS_EXCEPTION;
    int r = css_media(q, l);
    JS_FreeCString(ctx, q);
    return JS_NewBool(ctx, r);
}
FN(n_resolve) {
    UNUSED;
    CSTR(ref, A0);
    static char out[1024];
    int ok = url_resolve(base_url, ref, out, sizeof(out));
    JS_FreeCString(ctx, ref);
    return ok ? STR(out) : JS_NULL;
}
FN(n_now) { UNUSED; (void)argc; (void)argv; return JS_NewInt32(ctx, (int32_t)(ts_core->millis() - t_run)); }
FN(n_log) {
    UNUSED;
    CSTR(s, A1);
    int32_t level = 0;
    JS_ToInt32(ctx, &level, A0);
    jlog("js %s: %.120s", level >= 2 ? "error" : level == 1 ? "warn" : "log", s);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
FN(n_url) { UNUSED; (void)argc; (void)argv; return STR(base_url); }
/* Скрипт просит перейти (location): адрес — браузеру (js_nav), скрипты — прекратить */
static char nav_url[1024];
static int nav_replace;
static volatile int js_stop;
FN(n_nav) {
    UNUSED;
    CSTR(u, A0);
    snprintf(nav_url, sizeof(nav_url), "%s", u);
    JS_FreeCString(ctx, u);
    int32_t r = 0;
    JS_ToInt32(ctx, &r, A1);
    nav_replace = r;
    js_stop = 1;                                         /* дальше страница — другая */
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry natives[] = {
    JS_CFUNC_DEF("type", 1, n_type),         JS_CFUNC_DEF("parent", 1, n_parent),
    JS_CFUNC_DEF("first", 1, n_first),       JS_CFUNC_DEF("last", 1, n_last),
    JS_CFUNC_DEF("next", 1, n_next),         JS_CFUNC_DEF("prev", 1, n_prev),
    JS_CFUNC_DEF("html", 0, n_html),         JS_CFUNC_DEF("head", 0, n_head),
    JS_CFUNC_DEF("body", 0, n_body),         JS_CFUNC_DEF("tag", 1, n_tag),
    JS_CFUNC_DEF("data", 1, n_data),         JS_CFUNC_DEF("setData", 2, n_setdata),
    JS_CFUNC_DEF("kids", 1, n_kids),         JS_CFUNC_DEF("attrs", 1, n_attrs),
    JS_CFUNC_DEF("getAttr", 2, n_getattr),   JS_CFUNC_DEF("setAttr", 3, n_setattr),
    JS_CFUNC_DEF("rmAttr", 2, n_rmattr),     JS_CFUNC_DEF("create", 1, n_create),
    JS_CFUNC_DEF("text", 2, n_text),         JS_CFUNC_DEF("frag", 0, n_frag),
    JS_CFUNC_DEF("clone", 2, n_clone),       JS_CFUNC_DEF("insert", 3, n_insert),
    JS_CFUNC_DEF("detach", 1, n_detach),     JS_CFUNC_DEF("insertHTML", 3, n_inserthtml),
    JS_CFUNC_DEF("serialize", 2, n_serialize), JS_CFUNC_DEF("textContent", 1, n_textcontent),
    JS_CFUNC_DEF("contains", 2, n_contains), JS_CFUNC_DEF("find", 3, n_find),
    JS_CFUNC_DEF("matches", 3, n_matches),   JS_CFUNC_DEF("byId", 1, n_byid),
    JS_CFUNC_DEF("order", 2, n_order),       JS_CFUNC_DEF("cssProp", 2, n_cssprop),
    JS_CFUNC_DEF("media", 1, n_media),       JS_CFUNC_DEF("resolve", 1, n_resolve),
    JS_CFUNC_DEF("now", 0, n_now),           JS_CFUNC_DEF("log", 2, n_log),
    JS_CFUNC_DEF("url", 0, n_url),           JS_CFUNC_DEF("nav", 2, n_nav),
};

/* --- запуск ----------------------------------------------------------------------------------- */

/* Правила <style> страницы — в css.c (getComputedStyle): имена документа и разбор таблиц.
 * Внешние таблицы там уже есть — их загрузил и разобрал браузер для раскладки этой страницы. */
static void css_from_dom(void) {
    css_names_reset();
    css_doc_reset();
    for (int pass = 0; pass < 2; pass++) {
        dom_id k = dom_first(DOM_DOC);
        while (k && k != DOM_DOC) {
            if (dom_type(k) == DOM_ELEMENT) {
                if (!pass) {
                    const char *v;
                    css_name_tag(dom_tag(k));
                    if ((v = dom_get_attr(k, "class")) != NULL) css_name_classes(v);
                    if ((v = dom_get_attr(k, "id")) != NULL) css_name_id(v);
                } else if (!strcmp(dom_tag(k), "style")) {
                    size_t l = dom_text_content(k, NULL, 0);
                    char *t = ts_alloc(l + 1);
                    if (t) {
                        dom_text_content(k, t, l + 1);
                        css_doc_sheet(t, l, base_url, 0);
                        ts_core->free(t);
                    }
                }
            }
            if (dom_first(k)) { k = dom_first(k); continue; }
            while (k && k != DOM_DOC && !dom_next(k)) k = dom_parent(k);
            if (k && k != DOM_DOC) k = dom_next(k);
        }
    }
}

/* js_stop — просят остановиться (переход на другую страницу, js_abort, или сам скрипт попросил
 * перейти — n_nav). QuickJS спрашивает interrupt время от времени: 1 — прервать скрипт */
static int interrupt(JSContext *r, void *o) {
    (void)r; (void)o;
    return js_stop || (int32_t)(ts_core->millis() - deadline) > 0;
}

static int oom_seen;                                    /* скрипту не хватило памяти */
static void report(const char *where) {
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    if (s && strstr(s, "out of memory")) oom_seen = 1;
    /* исключение null — движку не хватило памяти даже на объект ошибки «out of memory» (стенд 08.11:
     * пролог при бюджете 640–900 КБ кончался так) */
    if (JS_IsNull(e)) oom_seen = 1;
    jlog("js error in %.60s: %.100s", where, s ? s : "?");
    if (s) JS_FreeCString(ctx, s);
    if (JS_IsError(e)) {
        JSValue st = JS_GetPropertyStr(ctx, e, "stack");
        const char *ss = JS_IsUndefined(st) ? NULL : JS_ToCString(ctx, st);
        if (ss && ss[0]) jlog("  at %.120s", ss);
        if (ss) JS_FreeCString(ctx, ss);
        JS_FreeValue(ctx, st);
    }
    JS_FreeValue(ctx, e);
}

static void rejection(JSContext *c, JSValueConst promise, JSValueConst reason, bool handled, void *o) {
    (void)promise; (void)o;
    if (handled) return;
    const char *s = JS_ToCString(c, reason);
    jlog("js unhandled rejection: %.110s", s ? s : "?");
    if (s) JS_FreeCString(c, s);
}

static void drain(void) {
    JSContext *c1;
    for (int k = 0; k < 20000; k++) {
        int r = JS_ExecutePendingJob(rt, &c1);
        if (r <= 0) { if (r < 0) report("promise"); break; }
    }
}

/* Вызвать хук пролога (имя, до двух чисел) */
static JSValue hook(const char *name, int argc, int32_t a, int32_t b) {
    JSValue f = JS_GetPropertyStr(ctx, hooks, name);
    JSValue av[2] = {JS_NewInt32(ctx, a), JS_NewInt32(ctx, b)};
    JSValue r = JS_Call(ctx, f, JS_UNDEFINED, argc, av);
    JS_FreeValue(ctx, f);
    if (JS_IsException(r)) { report(name); r = JS_UNDEFINED; }
    drain();
    return r;
}

static int js_type_ok(dom_id el, int *module) {
    const char *t = dom_get_attr(el, "type");
    *module = 0;
    if (dom_get_attr(el, "nomodule")) return 0;         /* для браузеров без модулей; мы модули знаем */
    if (!t || !t[0]) return 1;
    char low[40];
    size_t k = 0;
    for (; t[k] && k < sizeof(low) - 1; k++) low[k] = (char)(t[k] >= 'A' && t[k] <= 'Z' ? t[k] + 32 : t[k]);
    low[k] = 0;
    if (!strcmp(low, "module")) { *module = 1; return 1; }
    return !strcmp(low, "text/javascript") || !strcmp(low, "application/javascript") ||
           !strcmp(low, "application/x-javascript") || !strcmp(low, "text/ecmascript") ||
           !strcmp(low, "application/ecmascript") || !strcmp(low, "text/jscript");
}

static char *res_html;                  /* итог (js_result) */
static size_t res_len;

static void free_scripts(void) {
    for (int i = 0; i < scn; i++) {
        if (sc[i].url) ts_core->free(sc[i].url);
        if (sc[i].body) ts_core->free(sc[i].body);
    }
    memset(sc, 0, sizeof(sc));
    scn = 0;
}

void js_end(void) {
    if (res_html) { ts_core->free(res_html); res_html = NULL; }
    /* движок не закрываем по правилам (JS_FreeRuntime проверил бы, всё ли он отдал) — вся его
     * память в нашей куче и уходит целиком */
    rt = NULL;
    ctx = NULL;
    hooks = JS_UNDEFINED;
    heap_drop();
    dom_free();
    free_scripts();
}

int js_begin(const char *html, size_t n, const char *url) {
    js_end();
    snprintf(base_url, sizeof(base_url), "%s", url ? url : "");
    log_head = log_tail = 0;
    changed = ok_n = fail_n = 0;
    if (!dom_parse(html, n)) { dom_free(); return -1; }   /* дерево не поместилось в память */
    /* скрипты по порядку документа */
    dom_id k = dom_first(DOM_DOC);
    while (k && k != DOM_DOC) {
        if (dom_type(k) == DOM_ELEMENT && !strcmp(dom_tag(k), "script") && scn < SCRIPTS) {
            int module;
            if (js_type_ok(k, &module)) {
                script_t *s = &sc[scn];
                s->el = k;
                s->module = (uint8_t)module;
                const char *src = dom_get_attr(k, "src");
                if (src && src[0]) {
                    static char abs[1024];          /* static: стек задачи браузера мал */
                    if (url_resolve(base_url, src, abs, sizeof(abs)) && (s->url = ts_alloc(strlen(abs) + 1))) {
                        strcpy(s->url, abs);
                        s->state = 1;
                        scn++;
                    }
                } else {
                    scn++;
                }
            }
        }
        if (dom_first(k)) { k = dom_first(k); continue; }
        while (k && k != DOM_DOC && !dom_next(k)) k = dom_parent(k);
        if (k && k != DOM_DOC) k = dom_next(k);
    }
    if (!scn) { js_end(); return 0; }
    return 1;
}

const char *js_script_next(void) {
    for (int i = 0; i < scn; i++)
        if (sc[i].state == 1) { sc[i].state = 2; return sc[i].url; }
    return NULL;
}
void js_script_body(const char *url, const uint8_t *b, size_t n) {
    for (int i = 0; i < scn; i++) {
        if (sc[i].state != 2 || strcmp(sc[i].url, url)) continue;
        if (n && (sc[i].body = ts_alloc(n + 1))) {
            memcpy(sc[i].body, b, n);
            sc[i].body[n] = 0;                          /* QuickJS ждёт ноль в конце */
            sc[i].len = n;
            sc[i].state = 3;
        } else {
            sc[i].state = 4;
        }
    }
}
void js_script_take(const char *url, uint8_t *b, size_t n) {
    for (int i = 0; i < scn; i++) {
        if (sc[i].state != 2 || strcmp(sc[i].url, url)) continue;
        if (b && n) {
            sc[i].body = b;                             /* сам буфер, без копии (ноль после данных — есть) */
            sc[i].len = n;
            sc[i].state = 3;
            return;
        }
        sc[i].state = 4;
    }
    if (b) ts_core->free(b);                            /* не нужен (страница уже другая) */
}

int js_scripts_wanted(void) {
    int w = 0;
    for (int i = 0; i < scn; i++) w += sc[i].state == 1 || sc[i].state == 2;
    return w;
}

uint32_t js_mem_limit = 3u * 1024u * 1024u;            /* потолок кучи движка (стенд меняет) */
/* Стек задачи скриптов (в PSRAM). Вызов функции JavaScript берёт у интерпретатора QuickJS на ESP
 * 448 байт и ещё её переменные (-fstack-usage, 08.11): 64 КБ кончились на плате у проверки Google
 * («Maximum call stack size exceeded»). */
#define JS_STACK (256u * 1024u)

/* Пролог (или контекст движка) не получился — обычно не хватило памяти: скрипты страницы не
 * выполнялись, все считаются неудачными (в журнале «0 ok, N failed», а не «0 ok, 0 failed»); время —
 * сколько ушло, а не мусор от незаконченного отсчёта. */
static int run_failed(void) {
    fail_n = scn;
    t_run = ts_core->millis() - t_run;
    if (starved) jlog("js: stopped by memory: budget %u KB (1 MB of PSRAM is kept for the system)", (unsigned)(budget / 1024));
    return 0;
}

int js_run(uint32_t budget_ms) {
    if (!scn) return 0;
    t_run = ts_core->millis();
    deadline = t_run + budget_ms;
    fail_n = 0;
    nav_url[0] = 0;
    heap_init();
    heap_peak = 0;
    starved = oom_seen = 0;
    budget = budget_find();
    if (budget > js_mem_limit + 512u * 1024u) budget = js_mem_limit + 512u * 1024u;
    jlog("js: engine budget %u KB", (unsigned)(budget / 1024));
    css_from_dom();
    rt = JS_NewRuntime2(&heap_fns, NULL);
    if (!rt) { jlog("js: no memory for the engine"); return 0; }
    JS_SetMemoryLimit(rt, budget < js_mem_limit ? budget : js_mem_limit);
#ifndef TSAFARI_HOST
    JS_SetMaxStackSize(rt, JS_STACK - 16 * 1024);      /* запас — функциям C под движком */
#endif
    JS_SetInterruptHandler(rt, interrupt, NULL);
    JS_SetHostPromiseRejectionTracker(rt, rejection, NULL);
    ctx = JS_NewContext(rt);
    if (!ctx) { jlog("js: no memory for the context"); oom_seen = 1; return run_failed(); }
    /* пролог: функция (N) → объект хуков */
    JSValue f = JS_Eval(ctx, js_prelude, sizeof(js_prelude) - 1, "prelude", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(f)) { report("prelude"); return run_failed(); }
    JSValue nat = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, nat, natives, sizeof(natives) / sizeof(natives[0]));
    hooks = JS_Call(ctx, f, JS_UNDEFINED, 1, &nat);
    JS_FreeValue(ctx, nat);
    JS_FreeValue(ctx, f);
    if (JS_IsException(hooks)) { report("prelude"); return run_failed(); }
    drain();
    for (int i = 0; i < scn && !js_stop && (int32_t)(ts_core->millis() - deadline) < 0; i++) {
        script_t *s = &sc[i];
        if (s->state == 4 || s->state == 1 || s->state == 2) { fail_n++; continue; }
        char *src = (char *)s->body;
        size_t len = s->len;
        char *own = NULL;
        if (!s->url) {                                  /* встроенный: текст из дерева */
            len = dom_text_content(s->el, NULL, 0);
            if (!(own = ts_alloc(len + 1))) { fail_n++; continue; }
            dom_text_content(s->el, own, len + 1);
            src = own;
        }
        JSValue r = hook("scriptStart", 1, (int32_t)s->el, 0);
        JS_FreeValue(ctx, r);
        char name[80];
        snprintf(name, sizeof(name), "%.70s", s->url ? s->url : "inline");
        uint32_t ts0 = ts_core->millis();
        JSValue v = JS_Eval(ctx, src, len, name, s->module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) { report(name); fail_n++; }
        else ok_n++;
        JS_FreeValue(ctx, v);
        drain();
        uint32_t dt = ts_core->millis() - ts0;
        if (dt >= 200) jlog("js time %u ms: %.70s (%u KB)", (unsigned)dt, name, (unsigned)(len / 1024));   /* долгие — в журнал */
        r = hook("scriptEnd", 0, 0, 0);
        JS_FreeValue(ctx, r);
        if (own) ts_core->free(own);
        if (s->body) { ts_core->free(s->body); s->body = NULL; }   /* текст больше не нужен */
    }
    JS_FreeValue(ctx, hook("ready", 0, 0, 0));
    JS_FreeValue(ctx, hook("load", 0, 0, 0));
    /* таймеры: пока есть время (виртуальные миллисекунды пролога, до 5 с страницы) */
    for (int round = 0; round < 400 && !js_stop && (int32_t)(ts_core->millis() - deadline) < 0; round++) {
        JSValue r = hook("tick", 1, 5000, 0);
        int32_t more = 0;
        JS_ToInt32(ctx, &more, r);
        JS_FreeValue(ctx, r);
        if (!more) break;
    }
    t_run = ts_core->millis() - t_run;
    if (starved) jlog("js: stopped by memory: budget %u KB (1 MB of PSRAM is kept for the system)", (unsigned)(budget / 1024));
    return changed;
}

/* Итог — HTML дерева после скриптов. Его готовит сама задача скриптов (js_task): вывод дерева
 * рекурсивен, а стек задачи браузера мал (12 КБ). Отдаётся один раз (освобождает взявший). */
char *js_result(size_t *len) {
    if (!res_html) res_html = dom_serialize(DOM_DOC, 0, 1, &res_len);   /* стенд и js_host: прямо здесь */
    char *h = res_html;
    if (len) *len = h ? res_len : 0;
    res_html = NULL;
    return h;
}

void js_stats(size_t *heap, size_t *peak, size_t *dom, uint32_t *ms_run, int *scripts_ok, int *scripts_failed) {
    if (heap) *heap = heap_now;
    if (peak) *peak = heap_peak;
    if (dom) *dom = dom_bytes();
    if (ms_run) *ms_run = t_run;
    if (scripts_ok) *scripts_ok = ok_n;
    if (scripts_failed) *scripts_failed = fail_n;
}

/* --- фоновая задача ----------------------------------------------------------------------------
 * Скрипты выполняются не в задаче браузера: та рисует экран и слушает мышь, а скрипты страницы
 * идут секунды. Своя задача: стек 64 КБ в PSRAM (рекурсия движка глубокая, внутренней памяти
 * столько нет; ZM_TASK_STACK_PSRAM, прошивка 0.1.112+), ядро 1, приоритет 0 — ниже обмена с Z80.
 * Задача не пишет журнал прошивки (он во flash, а стек в PSRAM при записи flash недоступен):
 * строки журнала копятся (jlog), их пишет задача браузера (js_log_next). На стенде ПК задачи нет:
 * js_start считает сразу. */
static volatile int js_done_flag = 1;
static uint32_t js_budget;
static int js_res;
static void js_task(void *arg) {
    (void)arg;
    js_res = js_run(js_budget);
    if (js_res && !js_stop) res_html = dom_serialize(DOM_DOC, 0, 1, &res_len);
    __atomic_store_n(&js_done_flag, 1, __ATOMIC_RELEASE);
}
int js_start(uint32_t budget_ms) {
    js_budget = budget_ms;
    js_stop = 0;
    js_res = 0;
    __atomic_store_n(&js_done_flag, 0, __ATOMIC_RELEASE);
#ifdef TSAFARI_HOST
    js_task(NULL);
    return 1;
#else
    if (ts_core->task_start(js_task, NULL, JS_STACK | ZM_TASK_STACK_PSRAM, 0, 1) != ZM_OK) {
        jlog("js: no task for scripts (memory)");
        __atomic_store_n(&js_done_flag, 1, __ATOMIC_RELEASE);
        return 0;
    }
    return 1;
#endif
}
int js_finished(void) { return __atomic_load_n(&js_done_flag, __ATOMIC_ACQUIRE); }
int js_out_of_memory(void) { return oom_seen || starved; }
const char *js_nav(int *replace) {
    if (replace) *replace = nav_replace;
    return nav_url[0] ? nav_url : NULL;
}
void js_abort(void) { js_stop = 1; }
int js_changed(void) { return js_res; }

#ifndef TSAFARI_HOST
/* abort() движка (его внутренняя ошибка). У прошивки для модулей abort нет — и не нужен: сбросить ESP
 * из-за скрипта страницы нельзя. Задача скриптов говорит «кончилась» (без итога — страница остаётся
 * без скриптов) и засыпает навсегда: вернуться в движок ей некуда. Её стек (64 КБ) пропадает до
 * выхода из браузера — так бывает только при ошибке в самом движке. */
void abort(void) {
    jlog("js: engine abort");
    js_res = 0;
    __atomic_store_n(&js_done_flag, 1, __ATOMIC_RELEASE);
    for (;;) ts_core->delay(1000);
}
#endif
