/* Дерево документа (DOM) для скриптов страницы — см. dom.h.
 *
 * Как хранится. Все узлы — в одном массиве nd[] (номер узла — его место в массиве), связи —
 * номерами: родитель, первый и последний ребёнок, соседи. Строки (имена тегов, атрибуты, текст)
 * — подряд в одном буфере sb[], узел хранит смещение. Атрибуты элемента — подряд в массиве at[]:
 * пара «смещение имени, смещение значения». Так дерево страницы в 300 КБ занимает ~0,5–1 МБ
 * PSRAM, без тысяч мелких выделений памяти. Изменение (новое значение атрибута, новый текст)
 * дописывает строку в конец буфера; старая остаётся до следующей страницы — так проще и быстрее,
 * а скрипты меняют немного.
 *
 * Разбор HTML — упрощённые правила HTML5 (полные — сотни страниц стандарта): какие теги пустые
 * (<br>, <img>…), чьё содержимое — просто текст (<script>, <style>, <textarea>…), какие
 * открытые теги закрываются сами (<p> перед блоком, <li> перед следующим <li>, ячейки и строки
 * таблиц), что <html>, <head> и <body> есть всегда. Внутри <noscript> — текст: скрипты у нас
 * выполняются, а браузер со скриптами <noscript> не показывает. */
#include <string.h>
#include "dom.h"
#include "html.h"
#include "ts.h"

typedef struct {
    uint32_t parent, first, last, next, prev;
    uint32_t str, len;          /* элемент: имя тега (смещение в sb); текст и комментарий: данные, длина */
    uint32_t attr;              /* элемент: первый атрибут в at[] */
    uint16_t nattr, acap;       /* атрибутов; мест под них подряд */
    uint8_t type, foreign;      /* DOM_…; внутри <svg>/<math> (имена — как в тексте, «/>» закрывает) */
    void *wrap;                 /* объект скрипта (js.c) */
} dn_t;
typedef struct { uint32_t name, value; } da_t;

static dn_t *nd;
static uint32_t nn, ncap;               /* узлов (с нулевым «нет узла»), мест */
static da_t *at;
static uint32_t na, acap;
static char *sb;
static uint32_t sn, scap;
static int oom;                          /* памяти не хватило: дерево неполное */
static dom_id d_html, d_head, d_body;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* Вырастить массив *p до need элементов (вдвое за раз). */
static int grow(void **p, uint32_t *cap, uint32_t need, size_t item, uint32_t first) {
    if (need <= *cap) return 1;
    uint32_t c = *cap ? *cap : first;
    while (c < need) c *= 2;
    void *q = *p ? ts_core->realloc(*p, (size_t)c * item) : ts_alloc((size_t)c * item);
    if (!q) { oom = 1; return 0; }
    *p = q;
    *cap = c;
    return 1;
}

/* Строка — в буфер (с нулём в конце); смещение. 0 — пустая строка (нет памяти — тоже она). */
static uint32_t s_put(const char *s, size_t n) {
    if (!n) return 0;
    if (!grow((void **)&sb, &scap, sn + (uint32_t)n + 1, 1, 64 * 1024)) return 0;
    uint32_t o = sn;
    memcpy(sb + o, s, n);
    sb[o + n] = 0;
    sn += (uint32_t)n + 1;
    return o;
}
static const char *S(uint32_t o) { return sb ? sb + o : ""; }

static dom_id n_new(int type) {
    if (!grow((void **)&nd, &ncap, nn + 1, sizeof(dn_t), 1024)) return 0;
    dom_id id = nn++;
    memset(&nd[id], 0, sizeof(dn_t));
    nd[id].type = (uint8_t)type;
    return id;
}
static int ok_id(dom_id n) { return n && n < nn; }

void dom_free(void) {
    if (nd) ts_core->free(nd);
    if (at) ts_core->free(at);
    if (sb) ts_core->free(sb);
    nd = NULL; at = NULL; sb = NULL;
    nn = ncap = na = acap = sn = scap = 0;
    d_html = d_head = d_body = 0;
    oom = 0;
}
int dom_alive(void) { return nn > 1; }
size_t dom_bytes(void) { return (size_t)ncap * sizeof(dn_t) + (size_t)acap * sizeof(da_t) + scap; }
uint32_t dom_count(void) { return nn ? nn - 1 : 0; }

/* --- структура --- */
int dom_type(dom_id n) { return ok_id(n) ? nd[n].type : 0; }
dom_id dom_parent(dom_id n) { return ok_id(n) ? nd[n].parent : 0; }
dom_id dom_first(dom_id n) { return ok_id(n) ? nd[n].first : 0; }
dom_id dom_last(dom_id n) { return ok_id(n) ? nd[n].last : 0; }
dom_id dom_next(dom_id n) { return ok_id(n) ? nd[n].next : 0; }
dom_id dom_prev(dom_id n) { return ok_id(n) ? nd[n].prev : 0; }
dom_id dom_html(void) { return d_html; }
dom_id dom_head(void) { return d_head; }
dom_id dom_body(void) { return d_body; }
int dom_contains(dom_id a, dom_id b) {
    for (; ok_id(b); b = nd[b].parent) if (b == a) return 1;
    return 0;
}
const char *dom_tag(dom_id n) { return ok_id(n) && nd[n].type == DOM_ELEMENT ? S(nd[n].str) : ""; }
const char *dom_data(dom_id n, size_t *len) {
    if (!ok_id(n) || (nd[n].type != DOM_TEXT && nd[n].type != DOM_COMMENT && nd[n].type != DOM_DOCTYPE)) {
        if (len) *len = 0;
        return "";
    }
    if (len) *len = nd[n].len;
    return S(nd[n].str);
}
int dom_set_data(dom_id n, const char *s, size_t len) {
    if (!ok_id(n) || nd[n].type == DOM_ELEMENT || nd[n].type == DOM_DOCUMENT) return 0;
    uint32_t o = s_put(s, len);
    if (len && !o) return 0;
    nd[n].str = o;
    nd[n].len = (uint32_t)len;
    return 1;
}
void *dom_wrap(dom_id n) { return ok_id(n) ? nd[n].wrap : NULL; }
void dom_set_wrap(dom_id n, void *w) { if (ok_id(n)) nd[n].wrap = w; }

/* --- связи --- */
void dom_detach(dom_id n) {
    if (!ok_id(n) || !nd[n].parent) return;
    dn_t *x = &nd[n], *p = &nd[x->parent];
    if (x->prev) nd[x->prev].next = x->next; else p->first = x->next;
    if (x->next) nd[x->next].prev = x->prev; else p->last = x->prev;
    x->parent = x->next = x->prev = 0;
}

static void link_before(dom_id parent, dom_id n, dom_id before) {
    dn_t *p = &nd[parent], *x = &nd[n];
    x->parent = parent;
    if (before && nd[before].parent == parent) {
        x->next = before;
        x->prev = nd[before].prev;
        if (x->prev) nd[x->prev].next = n; else p->first = n;
        nd[before].prev = n;
    } else {
        x->prev = p->last;
        x->next = 0;
        if (p->last) nd[p->last].next = n; else p->first = n;
        p->last = n;
    }
}

int dom_insert(dom_id parent, dom_id node, dom_id before) {
    if (!ok_id(parent) || !ok_id(node) || dom_contains(node, parent)) return 0;
    if (before && (!ok_id(before) || nd[before].parent != parent)) before = 0;
    if (nd[node].type == DOM_FRAGMENT) {                /* фрагмент — вставляются его дети */
        dom_id c = nd[node].first;
        while (c) {
            dom_id nx = nd[c].next;
            dom_detach(c);
            link_before(parent, c, before);
            c = nx;
        }
        return 1;
    }
    if (before == node) before = nd[node].next;
    dom_detach(node);
    link_before(parent, node, before);
    return 1;
}

/* --- атрибуты --- */
int dom_attr_count(dom_id n) { return ok_id(n) && nd[n].type == DOM_ELEMENT ? nd[n].nattr : 0; }
const char *dom_attr_name(dom_id n, int i) {
    return i >= 0 && i < dom_attr_count(n) ? S(at[nd[n].attr + (uint32_t)i].name) : "";
}
const char *dom_attr_value(dom_id n, int i) {
    return i >= 0 && i < dom_attr_count(n) ? S(at[nd[n].attr + (uint32_t)i].value) : "";
}
static int attr_find(dom_id n, const char *name) {
    for (int i = 0; i < dom_attr_count(n); i++) if (!strcmp(S(at[nd[n].attr + (uint32_t)i].name), name)) return i;
    return -1;
}
const char *dom_get_attr(dom_id n, const char *name) {
    int i = attr_find(n, name);
    return i < 0 ? NULL : S(at[nd[n].attr + (uint32_t)i].value);
}

/* Атрибут по смещениям строк (уже в буфере); есть — заменить значение. */
static int attr_put(dom_id n, uint32_t name, uint32_t value) {
    dn_t *x = &nd[n];
    for (int i = 0; i < x->nattr; i++)
        if (!strcmp(S(at[x->attr + (uint32_t)i].name), S(name))) { at[x->attr + (uint32_t)i].value = value; return 1; }
    if (x->nattr == x->acap) {                          /* мест подряд нет — переложить в конец */
        uint16_t nc = x->acap ? (uint16_t)(x->acap * 2) : 4;
        if (nc > 4096 || !grow((void **)&at, &acap, na + nc, sizeof(da_t), 4096)) return 0;
        x = &nd[n];
        if (x->nattr) memmove(&at[na], &at[x->attr], x->nattr * sizeof(da_t));
        x->attr = na;
        x->acap = nc;
        na += nc;
    }
    at[x->attr + x->nattr].name = name;
    at[x->attr + x->nattr].value = value;
    x->nattr++;
    return 1;
}
int dom_set_attr(dom_id n, const char *name, const char *value) {
    if (!ok_id(n) || nd[n].type != DOM_ELEMENT || !name[0]) return 0;
    int i = attr_find(n, name);
    uint32_t v = s_put(value, strlen(value));
    if (value[0] && !v) return 0;
    if (i >= 0) { at[nd[n].attr + (uint32_t)i].value = v; return 1; }
    uint32_t k = s_put(name, strlen(name));
    return k ? attr_put(n, k, v) : 0;
}
void dom_remove_attr(dom_id n, const char *name) {
    int i = attr_find(n, name);
    if (i < 0) return;
    dn_t *x = &nd[n];
    memmove(&at[x->attr + (uint32_t)i], &at[x->attr + (uint32_t)i + 1], (size_t)(x->nattr - i - 1) * sizeof(da_t));
    x->nattr--;
}

/* --- новые узлы --- */
static int is_foreign_root(const char *t) { return !strcmp(t, "svg") || !strcmp(t, "math"); }

dom_id dom_create_element(const char *tag) {
    dom_id n = n_new(DOM_ELEMENT);
    if (!n) return 0;
    char low[64];
    size_t l = strlen(tag);
    if (l >= sizeof(low)) l = sizeof(low) - 1;
    for (size_t i = 0; i < l; i++) low[i] = (char)lower((unsigned char)tag[i]);
    low[l] = 0;
    nd[n].str = s_put(low, l);
    nd[n].foreign = (uint8_t)is_foreign_root(low);
    return n;
}
dom_id dom_create_text(const char *s, size_t len) {
    dom_id n = n_new(DOM_TEXT);
    if (n) { nd[n].str = s_put(s, len); nd[n].len = (uint32_t)len; }
    return n;
}
dom_id dom_create_comment(const char *s, size_t len) {
    dom_id n = n_new(DOM_COMMENT);
    if (n) { nd[n].str = s_put(s, len); nd[n].len = (uint32_t)len; }
    return n;
}
dom_id dom_create_fragment(void) { return n_new(DOM_FRAGMENT); }

dom_id dom_clone(dom_id n, int deep) {
    if (!ok_id(n)) return 0;
    dom_id c = n_new(nd[n].type);
    if (!c) return 0;
    nd[c].str = nd[n].str;                              /* строки общие: их не меняют на месте */
    nd[c].len = nd[n].len;
    nd[c].foreign = nd[n].foreign;
    for (int i = 0; i < nd[n].nattr; i++) {
        da_t a = at[nd[n].attr + (uint32_t)i];
        if (!attr_put(c, a.name, a.value)) return c;
    }
    if (deep)
        for (dom_id k = nd[n].first; k; k = nd[k].next) {
            dom_id kc = dom_clone(k, 1);
            if (!kc) break;
            link_before(c, kc, 0);
        }
    return c;
}

/* --- разбор HTML ---------------------------------------------------------------------------- */

static const char *const voids[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                                    "param", "source", "track", "wbr", "keygen", "frame", "basefont", "bgsound", NULL};
static const char *const rawtext[] = {"script", "style", "xmp", "iframe", "noembed", "noframes", "noscript", NULL};
static const char *const rcdata[] = {"textarea", "title", NULL};
/* Начало такого блока закрывает открытый <p> */
static const char *const p_closers[] = {
    "address", "article", "aside", "blockquote", "center", "details", "dialog", "dir", "div", "dl", "fieldset",
    "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hgroup", "hr", "main",
    "menu", "nav", "ol", "p", "pre", "listing", "section", "summary", "table", "ul", "li", "dd", "dt", "xmp", NULL};
/* До <body> — в <head> */
static const char *const head_tags[] = {"base", "basefont", "bgsound", "link", "meta", "style", "script", "title",
                                        "noscript", "template", NULL};
/* Границы «области» поиска открытого тега (закрывающий тег внутри ячейки не закроет тег снаружи таблицы) */
static const char *const scope_stop[] = {"table", "td", "th", "caption", "marquee", "object", "applet", "template",
                                         "html", NULL};

static int in_list(const char *s, const char *const *l) {
    for (; *l; l++) if (!strcmp(s, *l)) return 1;
    return 0;
}

#define DEPTH 256
static dom_id st[DEPTH];                 /* открытые элементы; st[0] — куда вставлять, если пусто */
static int sp;
static int fragment;                     /* разбор куска (innerHTML): без <html>/<head>/<body> */
static dom_id frag_before;               /* куда вставлять кусок: перед этим узлом (0 — в конец) */

static dom_id cur(void) { return sp > 0 ? st[sp - 1] : 0; }
static void push(dom_id n) { if (sp < DEPTH) st[sp++] = n; }
static int stack_has(const char *tag, const char *const *stop, int from) {
    for (int i = sp - 1; i >= from; i--) {
        const char *t = S(nd[st[i]].str);
        if (!strcmp(t, tag)) return i;
        if (stop && in_list(t, stop)) return -1;
    }
    return -1;
}
static void pop_to(int i) { if (i >= 0 && i < sp) sp = i; }
static int floor_sp;                     /* ниже не закрывать (кусок; <html>/<body>) */

/* Вставить узел в текущее место */
static void put(dom_id n) {
    dom_id p = cur();
    if (!p) return;
    link_before(p, n, (fragment && p == st[0]) ? frag_before : 0);
}

/* Текст — в текущее место (подряд идущий текст сливается в один узел) */
static void add_text(const char *s, size_t n) {
    if (!n) return;
    dom_id p = cur();
    if (!p) return;
    dom_id last = (fragment && p == st[0] && frag_before) ? nd[frag_before].prev : nd[p].last;
    if (last && nd[last].type == DOM_TEXT) {
        size_t l0 = nd[last].len;
        if (!grow((void **)&sb, &scap, sn + (uint32_t)(l0 + n) + 1, 1, 64 * 1024)) return;
        uint32_t o = sn;
        memcpy(sb + o, sb + nd[last].str, l0);
        memcpy(sb + o + l0, s, n);
        sb[o + l0 + n] = 0;
        sn += (uint32_t)(l0 + n) + 1;
        nd[last].str = o;
        nd[last].len = (uint32_t)(l0 + n);
        return;
    }
    dom_id t = dom_create_text(s, n);
    if (t) put(t);
}

/* <html>, <head>, <body> — есть всегда (у целого документа) */
static void ensure_html(void) {
    if (fragment || d_html) return;
    d_html = dom_create_element("html");
    if (!d_html) return;
    link_before(DOM_DOC, d_html, 0);
    sp = 0;
    push(d_html);
}
static void ensure_head(void) {
    ensure_html();
    if (fragment || d_head || !d_html) return;
    d_head = dom_create_element("head");
    if (d_head) link_before(d_html, d_head, d_body);
}
static void ensure_body(void) {
    ensure_head();
    if (fragment || d_body || !d_html) return;
    d_body = dom_create_element("body");
    if (!d_body) return;
    link_before(d_html, d_body, 0);
    sp = 0;
    push(d_html);
    push(d_body);
    floor_sp = 2;
}

/* Атрибуты тега (текст между именем и «>») — в элемент. Имена — строчными (не внутри svg), значения —
 * с раскрытыми сущностями. Повтор имени — первое значение. */
static void parse_attrs(dom_id el, const char *a, size_t n, int foreign) {
    size_t i = 0;
    char name[64];
    while (i < n) {
        while (i < n && (a[i] == ' ' || a[i] == '\t' || a[i] == '\n' || a[i] == '\r' || a[i] == '\f' || a[i] == '/')) i++;
        if (i >= n) break;
        size_t k = 0;
        while (i < n && a[i] != '=' && a[i] != ' ' && a[i] != '\t' && a[i] != '\n' && a[i] != '\r' && a[i] != '\f' &&
               a[i] != '>' && !(a[i] == '/' && k)) {
            if (k < sizeof(name) - 1) name[k++] = foreign ? a[i] : (char)lower((unsigned char)a[i]);
            i++;
        }
        name[k] = 0;
        while (i < n && (a[i] == ' ' || a[i] == '\t' || a[i] == '\n' || a[i] == '\r')) i++;
        const char *v = "";
        size_t vl = 0;
        if (i < n && a[i] == '=') {
            i++;
            while (i < n && (a[i] == ' ' || a[i] == '\t' || a[i] == '\n' || a[i] == '\r')) i++;
            if (i < n && (a[i] == '"' || a[i] == '\'')) {
                char q = a[i++];
                v = a + i;
                while (i < n && a[i] != q) i++;
                vl = (size_t)(a + i - v);
                if (i < n) i++;
            } else {
                v = a + i;
                while (i < n && a[i] != ' ' && a[i] != '\t' && a[i] != '\n' && a[i] != '\r' && a[i] != '>') i++;
                vl = (size_t)(a + i - v);
            }
        }
        if (!k || attr_find(el, name) >= 0) continue;
        uint32_t kn = s_put(name, k), kv = 0;
        if (vl) {                                       /* значение: сущности раскрыть (на месте, в буфере) */
            kv = s_put(v, vl);
            if (kv) {
                size_t l = memchr(v, '&', vl) ? html_entities_decode(sb + kv, vl) : vl;
                sb[kv + l] = 0;
            }
        }
        if (kn) attr_put(el, kn, kv);
    }
}

/* Начальный тег name (атрибуты a…a+n) */
static int start_tag(const char *name, const char *a, size_t an, int selfclose, dom_id *made) {
    *made = 0;
    int foreign = cur() && nd[cur()].foreign;
    if (!fragment) {
        if (!strcmp(name, "html")) {                    /* второй <html> — его атрибуты к первому */
            ensure_html();
            if (d_html) parse_attrs(d_html, a, an, 0);
            return 0;
        }
        if (!strcmp(name, "head")) { ensure_head(); return 0; }
        if (!strcmp(name, "body")) {
            int had = d_body != 0;
            ensure_body();
            if (d_body && (had || an)) parse_attrs(d_body, a, an, 0);
            return 0;
        }
    } else if (!strcmp(name, "html") || !strcmp(name, "head") || !strcmp(name, "body")) {
        return 0;
    }
    int to_head = !fragment && !d_body && in_list(name, head_tags);
    if (!fragment) { if (to_head) ensure_head(); else ensure_body(); }
    if (!to_head && !foreign) {
        /* открытые теги, которые этот закрывает */
        int i;
        if (in_list(name, p_closers) && (i = stack_has("p", scope_stop, floor_sp)) >= 0) pop_to(i);
        if (!strcmp(name, "li")) {
            for (i = sp - 1; i >= floor_sp; i--) {
                const char *t = S(nd[st[i]].str);
                if (!strcmp(t, "li")) { pop_to(i); break; }
                if (!strcmp(t, "ul") || !strcmp(t, "ol") || in_list(t, scope_stop)) break;
            }
        } else if (!strcmp(name, "dd") || !strcmp(name, "dt")) {
            for (i = sp - 1; i >= floor_sp; i--) {
                const char *t = S(nd[st[i]].str);
                if (!strcmp(t, "dd") || !strcmp(t, "dt")) { pop_to(i); break; }
                if (!strcmp(t, "dl") || in_list(t, scope_stop)) break;
            }
        } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
            const char *t = sp > floor_sp ? S(nd[cur()].str) : "";
            if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2]) sp--;
        } else if (!strcmp(name, "option") || !strcmp(name, "optgroup")) {
            if (sp > floor_sp && !strcmp(S(nd[cur()].str), "option")) sp--;
            if (!strcmp(name, "optgroup") && sp > floor_sp && !strcmp(S(nd[cur()].str), "optgroup")) sp--;
        } else if (!strcmp(name, "tr")) {
            for (i = sp - 1; i >= floor_sp; i--) {
                const char *t = S(nd[st[i]].str);
                if (!strcmp(t, "tr")) { pop_to(i); break; }
                if (!strcmp(t, "table") || !strcmp(t, "tbody") || !strcmp(t, "thead") || !strcmp(t, "tfoot")) break;
            }
        } else if (!strcmp(name, "td") || !strcmp(name, "th")) {
            for (i = sp - 1; i >= floor_sp; i--) {
                const char *t = S(nd[st[i]].str);
                if (!strcmp(t, "td") || !strcmp(t, "th")) { pop_to(i); break; }
                if (!strcmp(t, "tr") || !strcmp(t, "table")) break;
            }
        } else if (!strcmp(name, "tbody") || !strcmp(name, "thead") || !strcmp(name, "tfoot")) {
            for (i = sp - 1; i >= floor_sp; i--) {
                const char *t = S(nd[st[i]].str);
                if (!strcmp(t, "table")) { pop_to(i + 1); break; }
            }
        } else if (!strcmp(name, "a")) {
            if ((i = stack_has("a", scope_stop, floor_sp)) >= 0) pop_to(i);
        } else if (!strcmp(name, "select")) {
            if ((i = stack_has("select", scope_stop, floor_sp)) >= 0) pop_to(i);
        }
    }
    dom_id el = n_new(DOM_ELEMENT);
    if (!el) return 0;
    nd[el].str = s_put(name, strlen(name));
    nd[el].foreign = (uint8_t)(foreign || is_foreign_root(name));
    parse_attrs(el, a, an, nd[el].foreign);
    if (to_head) link_before(d_head, el, 0);
    else put(el);
    *made = el;
    if ((!nd[el].foreign && in_list(name, voids)) || (nd[el].foreign && selfclose)) return 0;
    if (!nd[el].foreign && (in_list(name, rawtext) || in_list(name, rcdata))) return 2;   /* содержимое — текст */
    if (to_head) return 3;                               /* <title>… в <head> — не на стек (текст выше) */
    push(el);
    return 1;
}

static void end_tag(const char *name) {
    if (!strcmp(name, "html") || !strcmp(name, "body") || !strcmp(name, "head")) return;
    if (!strcmp(name, "br")) { dom_id m; start_tag("br", "", 0, 0, &m); return; }
    const char *const *stop = in_list(name, scope_stop) ? NULL : scope_stop;
    int i = stack_has(name, stop, floor_sp);
    if (i >= 0) pop_to(i);
}

/* Имя тега: буквы, цифры, «-», «:»; строчными, если не внутри svg/math */
static size_t tag_name(const char *b, size_t n, size_t i, char *out, size_t cap, int keep_case) {
    size_t k = 0;
    while (i < n && ((b[i] >= 'a' && b[i] <= 'z') || (b[i] >= 'A' && b[i] <= 'Z') || (b[i] >= '0' && b[i] <= '9') ||
                     b[i] == '-' || b[i] == ':' || b[i] == '_')) {
        if (k < cap - 1) out[k++] = keep_case ? b[i] : (char)lower((unsigned char)b[i]);
        i++;
    }
    out[k] = 0;
    return i;
}

/* Найти «</name» без учёта регистра (конец текста <script>, <style>…); n — если нет */
static size_t find_end(const char *b, size_t n, size_t i, const char *name) {
    size_t l = strlen(name);
    for (; i + 2 + l <= n; i++) {
        if (b[i] != '<' || b[i + 1] != '/') continue;
        size_t k = 0;
        while (k < l && lower((unsigned char)b[i + 2 + k]) == name[k]) k++;
        if (k == l) {
            char c = i + 2 + l < n ? b[i + 2 + l] : '>';
            if (c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') return i;
        }
    }
    return n;
}

static void parse_run(const char *b, size_t n) {
    size_t i = 0;
    char name[64];
    while (i < n && !oom) {
        if (b[i] != '<') {                               /* текст до «<» */
            size_t j = i;
            while (j < n && b[j] != '<') j++;
            int blank = 1;
            for (size_t k = i; k < j && blank; k++)
                if (b[k] != ' ' && b[k] != '\n' && b[k] != '\r' && b[k] != '\t' && b[k] != '\f') blank = 0;
            if (!fragment && !d_body && blank) { i = j; continue; }   /* пробелы до <body> — мимо */
            if (!fragment) ensure_body();
            if (memchr(b + i, '&', j - i)) {                       /* с сущностями — раскрыть в копии */
                char *tmp = ts_alloc(j - i + 1);
                if (!tmp) { oom = 1; break; }
                memcpy(tmp, b + i, j - i);
                add_text(tmp, html_entities_decode(tmp, j - i));
                ts_core->free(tmp);
            } else {
                add_text(b + i, j - i);
            }
            i = j;
            continue;
        }
        if (i + 3 < n && b[i + 1] == '!' && b[i + 2] == '-' && b[i + 3] == '-') {   /* <!-- комментарий --> */
            size_t j = i + 4;
            while (j + 2 < n && !(b[j] == '-' && b[j + 1] == '-' && b[j + 2] == '>')) j++;
            if (j + 2 >= n) j = n;
            dom_id c = dom_create_comment(b + i + 4, j > i + 4 ? j - i - 4 : 0);
            if (c) { if (fragment || d_html) put(c); else link_before(DOM_DOC, c, 0); }
            i = j + 3 < n ? j + 3 : n;
            continue;
        }
        if (i + 1 < n && (b[i + 1] == '!' || b[i + 1] == '?')) {   /* <!DOCTYPE …>, <?xml …?> */
            size_t j = i + 2;
            while (j < n && b[j] != '>') j++;
            if (!fragment && !d_html && (b[i + 1] == '!') && n - i > 9) {
                dom_id d = n_new(DOM_DOCTYPE);
                if (d) {
                    nd[d].str = s_put(b + i + 2, j - i - 2);
                    nd[d].len = (uint32_t)(j - i - 2);
                    link_before(DOM_DOC, d, 0);
                }
            }
            i = j < n ? j + 1 : n;
            continue;
        }
        int closing = i + 1 < n && b[i + 1] == '/';
        size_t s0 = i + 1 + (size_t)closing;
        if (s0 >= n || !((b[s0] >= 'a' && b[s0] <= 'z') || (b[s0] >= 'A' && b[s0] <= 'Z'))) {
            if (!fragment) ensure_body();
            add_text("<", 1);                            /* «<» — просто знак */
            i++;
            continue;
        }
        int keep = cur() && nd[cur()].foreign;
        size_t j = tag_name(b, n, s0, name, sizeof(name), keep);
        /* конец тега — «>» вне кавычек */
        size_t a0 = j;
        char q = 0;
        while (j < n && (q || b[j] != '>')) {
            if (q) { if (b[j] == q) q = 0; }
            else if (b[j] == '"' || b[j] == '\'') q = b[j];
            j++;
        }
        size_t a1 = j;
        int selfclose = a1 > a0 && b[a1 - 1] == '/';
        i = j < n ? j + 1 : n;
        if (closing) {
            if (keep) {                                   /* внутри svg имена — как есть; сравнить строчными */
                for (char *p = name; *p; p++) *p = (char)lower((unsigned char)*p);
                int k = sp - 1;
                for (; k >= floor_sp; k--) {
                    const char *t = S(nd[st[k]].str);
                    size_t m = 0;
                    while (t[m] && lower((unsigned char)t[m]) == name[m]) m++;
                    if (!t[m] && !name[m]) break;
                }
                if (k >= floor_sp) pop_to(k);
            } else {
                end_tag(name);
            }
            continue;
        }
        dom_id el;
        int r = start_tag(name, b + a0, a1 - a0, selfclose, &el);
        if (r == 2 && el) {                              /* <script>, <style>, <textarea>…: до своего конца */
            size_t e = find_end(b, n, i, S(nd[el].str));
            size_t l = e - i;
            if (l) {
                dom_id t = dom_create_text(b + i, l);
                if (t) {
                    if (in_list(S(nd[el].str), rcdata) && memchr(b + i, '&', l)) {
                        nd[t].len = (uint32_t)html_entities_decode(sb + nd[t].str, l);
                        sb[nd[t].str + nd[t].len] = 0;
                    }
                    link_before(el, t, 0);
                }
            }
            i = e;
            while (i < n && b[i] != '>') i++;
            if (i < n) i++;
        }
    }
}

int dom_parse(const char *html, size_t n) {
    dom_free();
    n_new(0);                                            /* номер 0 — «нет узла» (его номер и есть 0) */
    if (oom || n_new(DOM_DOCUMENT) != DOM_DOC) return 0;
    s_put("", 1);                                        /* смещение 0 — пустая строка */
    if (oom) return 0;
    sn = 1;
    sp = 0;
    floor_sp = 0;
    fragment = 0;
    parse_run(html, n);
    ensure_body();
    sp = 0;
    return !oom;
}

int dom_insert_html(dom_id parent, dom_id before, const char *html, size_t n) {
    if (!ok_id(parent)) return 0;
    int save_sp = sp, save_floor = floor_sp, save_frag = fragment;
    dom_id save_before = frag_before, save_st[4];
    memcpy(save_st, st, sizeof(save_st));
    sp = 0;
    push(parent);
    floor_sp = 1;
    fragment = 1;
    frag_before = before && nd[before].parent == parent ? before : 0;
    oom = 0;
    parse_run(html, n);
    int ok = !oom;
    sp = save_sp;
    floor_sp = save_floor;
    fragment = save_frag;
    frag_before = save_before;
    memcpy(st, save_st, sizeof(save_st));
    return ok;
}

/* --- текст и HTML из дерева --- */

size_t dom_text_content(dom_id n, char *out, size_t cap) {
    size_t total = 0;
    if (!ok_id(n)) { if (cap) out[0] = 0; return 0; }
    if (nd[n].type == DOM_TEXT || nd[n].type == DOM_COMMENT) {
        size_t l = nd[n].len;
        if (cap) { size_t c = l < cap - 1 ? l : cap - 1; memcpy(out, S(nd[n].str), c); out[c] = 0; }
        return l;
    }
    dom_id k = nd[n].first;
    while (k && k != n) {                                /* обход в глубину без рекурсии */
        if (nd[k].type == DOM_TEXT) {
            size_t l = nd[k].len;
            if (cap && total < cap - 1) {
                size_t c = l < cap - 1 - total ? l : cap - 1 - total;
                memcpy(out + total, S(nd[k].str), c);
            }
            total += l;
        }
        if (nd[k].first && nd[k].type == DOM_ELEMENT) { k = nd[k].first; continue; }
        while (k && k != n && !nd[k].next) k = nd[k].parent;
        if (k && k != n) k = nd[k].next;
    }
    if (cap) out[total < cap - 1 ? total : cap - 1] = 0;
    return total;
}

/* Буфер вывода, растёт */
typedef struct { char *b; size_t n, cap; int fail; } ob_t;
static void ob_put(ob_t *o, const char *s, size_t n) {
    if (o->fail || !n) return;
    if (o->n + n + 1 > o->cap) {
        size_t c = o->cap ? o->cap : 64 * 1024;
        while (c < o->n + n + 1) c *= 2;
        char *q = o->b ? ts_core->realloc(o->b, c) : ts_alloc(c);
        if (!q) { o->fail = 1; return; }
        o->b = q;
        o->cap = c;
    }
    memcpy(o->b + o->n, s, n);
    o->n += n;
    o->b[o->n] = 0;
}
static void ob_str(ob_t *o, const char *s) { ob_put(o, s, strlen(s)); }

/* Текст с заменой «&», «<», «>» (в значении атрибута — ещё «"») на сущности; неразрывный пробел —
 * «&nbsp;», как было в странице (html.c разбирает его так же). */
static void ob_esc(ob_t *o, const char *s, size_t n, int attr) {
    size_t i = 0, from = 0;
    while (i < n) {
        const char *e = NULL;
        size_t sk = 1;
        unsigned char c = (unsigned char)s[i];
        if (c == '&') e = "&amp;";
        else if (c == '<') e = "&lt;";
        else if (c == '>') e = "&gt;";
        else if (c == '"' && attr) e = "&quot;";
        else if (c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] == 0xA0) { e = "&nbsp;"; sk = 2; }
        if (!e) { i++; continue; }
        ob_put(o, s + from, i - from);
        ob_str(o, e);
        i += sk;
        from = i;
    }
    ob_put(o, s + from, n - from);
}

static void ser(ob_t *o, dom_id n, int page);
static void ser_children(ob_t *o, dom_id n, int page) {
    int raw = nd[n].type == DOM_ELEMENT && !nd[n].foreign &&
              (in_list(S(nd[n].str), rawtext) || !strcmp(S(nd[n].str), "plaintext"));
    for (dom_id k = nd[n].first; k && !o->fail; k = nd[k].next) {
        if (raw && nd[k].type == DOM_TEXT) ob_put(o, S(nd[k].str), nd[k].len);
        else ser(o, k, page);
    }
}
static void ser(ob_t *o, dom_id n, int page) {
    switch (nd[n].type) {
    case DOM_TEXT: ob_esc(o, S(nd[n].str), nd[n].len, 0); return;
    case DOM_COMMENT:
        if (page) return;                                /* html.c комментарии не нужны */
        ob_str(o, "<!--"); ob_put(o, S(nd[n].str), nd[n].len); ob_str(o, "-->");
        return;
    case DOM_DOCTYPE: ob_str(o, "<!"); ob_put(o, S(nd[n].str), nd[n].len); ob_str(o, ">"); return;
    case DOM_DOCUMENT: case DOM_FRAGMENT: ser_children(o, n, page); return;
    case DOM_ELEMENT: break;
    default: return;
    }
    const char *t = S(nd[n].str);
    if (page && !nd[n].foreign && (!strcmp(t, "script") || !strcmp(t, "noscript"))) return;
    int meta_cs = 0;
    if (page && !strcmp(t, "meta")) {                    /* кодировка — теперь UTF-8 */
        const char *he = dom_get_attr(n, "http-equiv");
        if (dom_get_attr(n, "charset") || (he && (he[0] == 'c' || he[0] == 'C'))) meta_cs = 1;
    }
    ob_str(o, "<");
    ob_str(o, t);
    if (meta_cs) {
        ob_str(o, " charset=\"utf-8\"");
    } else {
        for (int i = 0; i < nd[n].nattr && !o->fail; i++) {
            da_t a = at[nd[n].attr + (uint32_t)i];
            ob_str(o, " ");
            ob_str(o, S(a.name));
            ob_str(o, "=\"");
            ob_esc(o, S(a.value), strlen(S(a.value)), 1);
            ob_str(o, "\"");
        }
    }
    if (!nd[n].foreign && in_list(t, voids)) { ob_str(o, ">"); return; }
    if (nd[n].foreign && !nd[n].first) { ob_str(o, "/>"); return; }
    ob_str(o, ">");
    ser_children(o, n, page);
    ob_str(o, "</");
    ob_str(o, t);
    ob_str(o, ">");
}

char *dom_serialize(dom_id n, int outer, int page, size_t *len) {
    ob_t o = {0};
    if (!ok_id(n)) n = DOM_DOC;
    if (ok_id(n)) {                                      /* дерева нет — пустая строка */
        if (outer || nd[n].type == DOM_DOCUMENT) ser(&o, n, page);
        else ser_children(&o, n, page);
    }
    if (!o.b && !o.fail && (o.b = ts_alloc(1)) != NULL) o.b[0] = 0;   /* пусто — пустая строка */
    if (o.fail || !o.b) { if (o.b) ts_core->free(o.b); if (len) *len = 0; return NULL; }
    if (len) *len = o.n;
    return o.b;
}
