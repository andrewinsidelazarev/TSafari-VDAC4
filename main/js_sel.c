/* Селекторы для скриптов — см. js_sel.h.
 *
 * Разбор: текст селектора → «составные» (тег, #id, .классы, [атрибуты], :псевдоклассы), связанные
 * справа налево комбинаторами (пробел — потомок, «>» — ребёнок, «+» — следующий брат, «~» — любой
 * следующий брат). Проверка идёт справа налево, как у браузеров: сначала сам элемент, потом его
 * предки или братья. Всё разобранное — в небольшом статическом буфере (селекторы короткие). */
#include <string.h>
#include <stdint.h>
#include "js_sel.h"

typedef struct sl sl_t;
typedef struct { const char *name, *value; char op, ci; } sa_t;   /* op: 'e' есть, '=', '~', '|', '^', '$', '*' */
enum {
    P_FIRST = 1, P_LAST, P_ONLY, P_FIRST_T, P_LAST_T, P_ONLY_T, P_NTH, P_NTH_LAST, P_NTH_T, P_NTH_LAST_T,
    P_NOT, P_IS, P_ROOT, P_EMPTY, P_CHECKED, P_DISABLED, P_ENABLED, P_LINK, P_NEVER, P_ALWAYS, P_SCOPE,
    P_REQUIRED, P_OPTIONAL
};
typedef struct { uint8_t kind; int a, b; sl_t *sub; } sp_t;
typedef struct sc {
    const char *tag, *id;                 /* NULL — любой */
    const char *cls[8];
    sa_t at[6];
    sp_t ps[6];
    uint8_t ncls, nat, nps;
    char comb;                            /* что связывает с left: ' ', '>', '+', '~' */
    struct sc *left;
} sc_t;
struct sl { sc_t *it[16]; int n; };

#define ARENA 8192
static uint64_t arena_mem[ARENA / 8];    /* uint64_t — выравнивание */
static size_t an;
static sl_t *sel;
static dom_id scope_el;

static void *anew(size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (an + n > ARENA) return NULL;
    void *p = (char *)arena_mem + an;
    an += n;
    memset(p, 0, n);
    return p;
}

static int low(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int hexv(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

typedef struct { const char *s; size_t n, i; int bad; } ps_t;
static int peek(ps_t *p) { return p->i < p->n ? (unsigned char)p->s[p->i] : 0; }

/* Символ UTF-8 кода c — в буфер */
static size_t utf8(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | (c >> 12 & 63)); o[2] = (char)(0x80 | (c >> 6 & 63));
    o[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* Знак после «\»: «\:» — «:», «\31 » — код 0x31 (до шести цифр, пробел после — часть записи) */
static size_t escape(ps_t *p, char *o) {
    p->i++;
    if (p->i >= p->n) return 0;
    if (hexv(peek(p)) >= 0) {
        uint32_t c = 0;
        for (int k = 0; k < 6 && hexv(peek(p)) >= 0; k++) c = c * 16 + (uint32_t)hexv(p->s[p->i++]);
        if (ws(peek(p))) p->i++;
        return utf8(c ? c : 0xFFFD, o);
    }
    o[0] = p->s[p->i++];
    return 1;
}

/* Имя (тега, класса, id, атрибута): буквы, цифры, «-», «_», не-ASCII, «\…» */
static const char *ident(ps_t *p, int lowercase) {
    char buf[128];
    size_t k = 0;
    while (p->i < p->n && k < sizeof(buf) - 4) {
        int c = peek(p);
        if (c == '\\') { k += escape(p, buf + k); continue; }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c >= 0x80))
            break;
        buf[k++] = (char)(lowercase ? low(c) : c);
        p->i++;
    }
    if (!k) { p->bad = 1; return NULL; }
    char *s = anew(k + 1);
    if (!s) { p->bad = 1; return NULL; }
    memcpy(s, buf, k);
    return s;
}

static const char *quoted(ps_t *p) {
    char q = (char)peek(p), buf[256];
    size_t k = 0;
    p->i++;
    while (p->i < p->n && peek(p) != q && k < sizeof(buf) - 4) {
        if (peek(p) == '\\') { k += escape(p, buf + k); continue; }
        buf[k++] = p->s[p->i++];
    }
    if (peek(p) != q) { p->bad = 1; return NULL; }
    p->i++;
    char *s = anew(k + 1);
    if (!s) { p->bad = 1; return NULL; }
    memcpy(s, buf, k);
    return s;
}

static void skip_ws(ps_t *p) { while (ws(peek(p))) p->i++; }
static sl_t *parse_list(ps_t *p, int close);

/* an+b: «odd», «even», «3», «2n+1», «-n+3», «n» */
static void nth(ps_t *p, int *a, int *b) {
    skip_ws(p);
    char t[32];
    size_t k = 0;
    while (p->i < p->n && peek(p) != ')' && k < sizeof(t) - 1) {
        if (!ws(peek(p))) t[k++] = (char)low(peek(p));
        p->i++;
    }
    t[k] = 0;
    if (!strcmp(t, "odd")) { *a = 2; *b = 1; return; }
    if (!strcmp(t, "even")) { *a = 2; *b = 0; return; }
    const char *s = t;
    int sign = 1, num = 0, have = 0;
    if (*s == '+' || *s == '-') sign = *s++ == '-' ? -1 : 1;
    while (*s >= '0' && *s <= '9') { num = num * 10 + (*s++ - '0'); have = 1; }
    if (*s == 'n') {
        *a = sign * (have ? num : 1);
        s++;
        *b = 0;
        if (*s == '+' || *s == '-') {
            int sg = *s++ == '-' ? -1 : 1, v = 0, h2 = 0;
            while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); h2 = 1; }
            if (!h2) p->bad = 1;
            *b = sg * v;
        }
    } else {
        if (!have) p->bad = 1;
        *a = 0;
        *b = sign * num;
    }
    if (*s) p->bad = 1;
}

static int pseudo(ps_t *p, sc_t *c) {
    if (c->nps >= 6) return 0;
    sp_t *x = &c->ps[c->nps];
    const char *name = ident(p, 1);
    if (!name) return 0;
    static const struct { const char *n; uint8_t k; } simple[] = {
        {"first-child", P_FIRST}, {"last-child", P_LAST}, {"only-child", P_ONLY}, {"first-of-type", P_FIRST_T},
        {"last-of-type", P_LAST_T}, {"only-of-type", P_ONLY_T}, {"root", P_ROOT}, {"empty", P_EMPTY},
        {"checked", P_CHECKED}, {"selected", P_CHECKED}, {"disabled", P_DISABLED}, {"enabled", P_ENABLED},
        {"link", P_LINK}, {"any-link", P_LINK}, {"scope", P_SCOPE}, {"required", P_REQUIRED},
        {"optional", P_OPTIONAL}, {"defined", P_ALWAYS},
        {"hover", P_NEVER}, {"active", P_NEVER}, {"focus", P_NEVER}, {"focus-visible", P_NEVER},
        {"focus-within", P_NEVER}, {"visited", P_NEVER}, {"target", P_NEVER}, {"indeterminate", P_NEVER},
        {"placeholder-shown", P_NEVER}, {"autofill", P_NEVER}, {"fullscreen", P_NEVER}, {"modal", P_NEVER},
    };
    if (peek(p) != '(') {
        for (unsigned k = 0; k < sizeof(simple) / sizeof(simple[0]); k++)
            if (!strcmp(name, simple[k].n)) { x->kind = simple[k].k; c->nps++; return 1; }
        return 0;
    }
    p->i++;
    if (!strcmp(name, "not") || !strcmp(name, "is") || !strcmp(name, "where") || !strcmp(name, "matches")) {
        x->kind = name[0] == 'n' ? P_NOT : P_IS;
        x->sub = parse_list(p, ')');
        if (!x->sub || p->bad) return 0;
    } else if (!strncmp(name, "nth-", 4)) {
        x->kind = !strcmp(name, "nth-child") ? P_NTH : !strcmp(name, "nth-last-child") ? P_NTH_LAST
                : !strcmp(name, "nth-of-type") ? P_NTH_T : !strcmp(name, "nth-last-of-type") ? P_NTH_LAST_T : 0;
        if (!x->kind) return 0;
        nth(p, &x->a, &x->b);
        if (p->bad) return 0;
    } else {
        return 0;                                       /* :has(), :lang(), :dir()… — не умеем */
    }
    skip_ws(p);
    if (peek(p) != ')') return 0;
    p->i++;
    c->nps++;
    return 1;
}

static int attribute(ps_t *p, sc_t *c) {
    if (c->nat >= 6) return 0;
    sa_t *a = &c->at[c->nat];
    p->i++;
    skip_ws(p);
    a->name = ident(p, 1);
    if (!a->name) return 0;
    skip_ws(p);
    a->op = 'e';
    int ch = peek(p);
    if (ch == '=') { a->op = '='; p->i++; }
    else if ((ch == '~' || ch == '|' || ch == '^' || ch == '$' || ch == '*') && p->i + 1 < p->n && p->s[p->i + 1] == '=') {
        a->op = (char)ch;
        p->i += 2;
    }
    if (a->op != 'e') {
        skip_ws(p);
        a->value = peek(p) == '"' || peek(p) == '\'' ? quoted(p) : ident(p, 0);
        if (!a->value) return 0;
        skip_ws(p);
        if (low(peek(p)) == 'i' || low(peek(p)) == 's') { a->ci = (char)(low(peek(p)) == 'i'); p->i++; skip_ws(p); }
    }
    if (peek(p) != ']') return 0;
    p->i++;
    c->nat++;
    return 1;
}

static sc_t *compound(ps_t *p) {
    sc_t *c = anew(sizeof(sc_t));
    if (!c) return NULL;
    int any = 0;
    if (peek(p) == '*') { p->i++; any = 1; }
    else if (peek(p) && (peek(p) == '\\' || peek(p) == '-' || peek(p) == '_' || peek(p) >= 0x80 ||
                         (low(peek(p)) >= 'a' && low(peek(p)) <= 'z'))) {
        if (!(c->tag = ident(p, 1))) return NULL;
        any = 1;
    }
    for (;;) {
        int ch = peek(p);
        if (ch == '#') { p->i++; if (!(c->id = ident(p, 0))) return NULL; }
        else if (ch == '.') { p->i++; if (c->ncls >= 8 || !(c->cls[c->ncls++] = ident(p, 0))) return NULL; }
        else if (ch == '[') { if (!attribute(p, c)) return NULL; }
        else if (ch == ':') {
            p->i++;
            if (peek(p) == ':') return NULL;            /* ::before и прочие части элемента — не элементы */
            if (!pseudo(p, c)) return NULL;
        } else break;
        any = 1;
    }
    return any ? c : NULL;
}

static sc_t *complex_sel(ps_t *p) {
    skip_ws(p);
    sc_t *right = compound(p);
    if (!right) return NULL;
    for (;;) {
        size_t before = p->i;
        skip_ws(p);
        int ch = peek(p);
        if (!ch || ch == ',' || ch == ')') return right;
        char comb = ' ';
        if (ch == '>' || ch == '+' || ch == '~') { comb = (char)ch; p->i++; skip_ws(p); }
        else if (p->i == before) return NULL;           /* два составных без пробела между ними */
        sc_t *next = compound(p);
        if (!next) return NULL;
        next->left = right;
        next->comb = comb;
        right = next;
    }
}

static sl_t *parse_list(ps_t *p, int close) {
    sl_t *l = anew(sizeof(sl_t));
    if (!l) return NULL;
    for (;;) {
        sc_t *c = complex_sel(p);
        if (!c || l->n >= 16) return NULL;
        l->it[l->n++] = c;
        skip_ws(p);
        if (peek(p) == ',') { p->i++; continue; }
        if ((close && peek(p) == close) || (!close && p->i >= p->n)) return l;
        return NULL;
    }
}

int js_sel_compile(const char *s, size_t n) {
    an = 0;
    ps_t p = {s, n, 0, 0};
    sel = parse_list(&p, 0);
    if (p.bad) sel = NULL;
    return sel != NULL;
}

/* --- проверка --- */

static dom_id prev_el(dom_id e) { do e = dom_prev(e); while (e && dom_type(e) != DOM_ELEMENT); return e; }
static dom_id next_el(dom_id e) { do e = dom_next(e); while (e && dom_type(e) != DOM_ELEMENT); return e; }
static dom_id parent_el(dom_id e) { e = dom_parent(e); return dom_type(e) == DOM_ELEMENT ? e : 0; }

static int same_tag(const char *a, const char *b) {
    while (*a && low((unsigned char)*a) == low((unsigned char)*b)) a++, b++;
    return !*a && !*b;
}
static int word_in(const char *list, const char *w) {
    size_t wl = strlen(w);
    for (const char *q = list; *q;) {
        while (ws((unsigned char)*q)) q++;
        const char *e = q;
        while (*e && !ws((unsigned char)*e)) e++;
        if ((size_t)(e - q) == wl && !memcmp(q, w, wl)) return 1;
        q = e;
    }
    return 0;
}
static int eq(const char *a, const char *b, size_t n, int ci) {
    for (size_t i = 0; i < n; i++)
        if (ci ? low((unsigned char)a[i]) != low((unsigned char)b[i]) : a[i] != b[i]) return 0;
    return 1;
}
static int attr_ok(dom_id e, const sa_t *a) {
    const char *v = dom_get_attr(e, a->name);
    if (!v) return 0;
    if (a->op == 'e') return 1;
    size_t vl = strlen(v), wl = strlen(a->value);
    switch (a->op) {
    case '=': return vl == wl && eq(v, a->value, wl, a->ci);
    case '^': return wl && vl >= wl && eq(v, a->value, wl, a->ci);
    case '$': return wl && vl >= wl && eq(v + vl - wl, a->value, wl, a->ci);
    case '*':
        for (size_t i = 0; wl && i + wl <= vl; i++) if (eq(v + i, a->value, wl, a->ci)) return 1;
        return 0;
    case '|': return (vl == wl && eq(v, a->value, wl, a->ci)) || (vl > wl && eq(v, a->value, wl, a->ci) && v[wl] == '-');
    case '~': return word_in(v, a->value);
    default: return 0;
    }
}

/* Номер элемента среди братьев (с 1); of_type — только того же тега; from_end — с конца */
static int index_of(dom_id e, int of_type, int from_end) {
    int k = 1;
    for (dom_id s = from_end ? next_el(e) : prev_el(e); s; s = from_end ? next_el(s) : prev_el(s))
        if (!of_type || same_tag(dom_tag(s), dom_tag(e))) k++;
    return k;
}
static int nth_ok(int k, int a, int b) {
    if (!a) return k == b;
    int d = k - b;
    return d / a >= 0 && d % a == 0;
}
static int is_control(const char *t) {
    return !strcmp(t, "input") || !strcmp(t, "button") || !strcmp(t, "select") || !strcmp(t, "textarea") ||
           !strcmp(t, "option") || !strcmp(t, "optgroup") || !strcmp(t, "fieldset");
}
static int match_list(const sl_t *l, dom_id e);

static int pseudo_ok(dom_id e, const sp_t *x) {
    const char *t = dom_tag(e);
    switch (x->kind) {
    case P_FIRST: return !prev_el(e);
    case P_LAST: return !next_el(e);
    case P_ONLY: return !prev_el(e) && !next_el(e);
    case P_FIRST_T: return index_of(e, 1, 0) == 1;
    case P_LAST_T: return index_of(e, 1, 1) == 1;
    case P_ONLY_T: return index_of(e, 1, 0) == 1 && index_of(e, 1, 1) == 1;
    case P_NTH: return nth_ok(index_of(e, 0, 0), x->a, x->b);
    case P_NTH_LAST: return nth_ok(index_of(e, 0, 1), x->a, x->b);
    case P_NTH_T: return nth_ok(index_of(e, 1, 0), x->a, x->b);
    case P_NTH_LAST_T: return nth_ok(index_of(e, 1, 1), x->a, x->b);
    case P_NOT: return !match_list(x->sub, e);
    case P_IS: return match_list(x->sub, e);
    case P_ROOT: return e == dom_html();
    case P_SCOPE: return e == (scope_el ? scope_el : dom_html());
    case P_EMPTY:
        for (dom_id k = dom_first(e); k; k = dom_next(k))
            if (dom_type(k) == DOM_ELEMENT || (dom_type(k) == DOM_TEXT && dom_data(k, NULL)[0])) return 0;
        return 1;
    case P_CHECKED: return dom_get_attr(e, !strcmp(t, "option") ? "selected" : "checked") != NULL;
    case P_DISABLED: return is_control(t) && dom_get_attr(e, "disabled") != NULL;
    case P_ENABLED: return is_control(t) && !dom_get_attr(e, "disabled");
    case P_LINK: return (!strcmp(t, "a") || !strcmp(t, "area")) && dom_get_attr(e, "href");
    case P_REQUIRED: return is_control(t) && dom_get_attr(e, "required") != NULL;
    case P_OPTIONAL: return is_control(t) && !dom_get_attr(e, "required");
    case P_ALWAYS: return 1;
    default: return 0;
    }
}

static int compound_ok(const sc_t *c, dom_id e) {
    if (dom_type(e) != DOM_ELEMENT) return 0;
    if (c->tag && !same_tag(c->tag, dom_tag(e))) return 0;
    if (c->id) { const char *v = dom_get_attr(e, "id"); if (!v || strcmp(v, c->id)) return 0; }
    if (c->ncls) {
        const char *v = dom_get_attr(e, "class");
        if (!v) return 0;
        for (int k = 0; k < c->ncls; k++) if (!word_in(v, c->cls[k])) return 0;
    }
    for (int k = 0; k < c->nat; k++) if (!attr_ok(e, &c->at[k])) return 0;
    for (int k = 0; k < c->nps; k++) if (!pseudo_ok(e, &c->ps[k])) return 0;
    return 1;
}

static int complex_ok(const sc_t *c, dom_id e) {
    if (!compound_ok(c, e)) return 0;
    if (!c->left) return 1;
    switch (c->comb) {
    case '>': { dom_id p = parent_el(e); return p && complex_ok(c->left, p); }
    case '+': { dom_id s = prev_el(e); return s && complex_ok(c->left, s); }
    case '~':
        for (dom_id s = prev_el(e); s; s = prev_el(s)) if (complex_ok(c->left, s)) return 1;
        return 0;
    default:
        for (dom_id p = parent_el(e); p; p = parent_el(p)) if (complex_ok(c->left, p)) return 1;
        return 0;
    }
}

static int match_list(const sl_t *l, dom_id e) {
    for (int k = 0; k < l->n; k++) if (complex_ok(l->it[k], e)) return 1;
    return 0;
}

int js_sel_match(dom_id e, dom_id scope) {
    if (!sel) return 0;
    scope_el = scope;
    return match_list(sel, e);
}

size_t js_sel_find(dom_id root, int first, dom_id *out, size_t max) {
    size_t found = 0;
    if (!sel) return 0;
    scope_el = dom_type(root) == DOM_ELEMENT ? root : 0;
    dom_id k = dom_first(root);
    while (k && k != root) {                            /* по порядку документа, без рекурсии */
        if (dom_type(k) == DOM_ELEMENT && match_list(sel, k)) {
            if (found < max) out[found] = k;
            found++;
            if (first) return found;
        }
        if (dom_first(k)) { k = dom_first(k); continue; }
        while (k && k != root && !dom_next(k)) k = dom_parent(k);
        if (k && k != root) k = dom_next(k);
    }
    return found;
}
