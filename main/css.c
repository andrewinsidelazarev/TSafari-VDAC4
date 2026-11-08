/* css.c — упрощённый CSS браузера («алфавит CSS», 07.11). Что умеет и чего нет — в css.h и в
 * docs/ARCHITECTURE.md (таблица поддержки).
 *
 * СЛОВАРЬ.
 * - Правило: «селектор { свойства }». Селектор говорит, к каким элементам относятся свойства.
 * - Простой селектор (здесь — compound): «div.menu#top[lang=ru]» — тег, классы, id, атрибут.
 *   Цепочка: «.nav > ul li» — простые селекторы через «комбинаторы»: пробел — «где-то внутри»
 *   (потомок), «>» — «прямо внутри» (ребёнок).
 * - Специфичность: «вес» селектора. Из двух правил для одного элемента главнее то, у которого
 *   больше id, при равенстве — больше классов (и атрибутов), потом — тегов; при полном
 *   равенстве — то, что ниже в таблицах. Здесь вес — одно число: id × 2^20 + классы × 2^10 + теги.
 * - Отпечаток (хэш): число из строки (FNV-1a). Сравнивать числа быстрее, чем строки; совпадение
 *   отпечатков у разных строк возможно, но редко (32 бита) и стоит лишь лишнего правила.
 *
 * Чтобы не перебирать для каждого элемента все правила, они разложены по «корзинам» (BUCKETS)
 * по ключу своего простого селектора: id, иначе первый класс, иначе тег; без них — общий
 * ключ 0. Элемент смотрит только корзины своих id, классов, тега и общую. */
#include <stdio.h>
#include <string.h>
#include "css.h"
#include "url.h"
#include "ts.h"

#define SALT_TAG 0x85EBCA6Bu             /* отпечатки тегов и id «подсолены» — не путаются с классами */
#define SALT_ID  0x9E3779B9u
#define BUCKETS  512                      /* корзин правил (степень двойки) */
#define RULES_MAX 8000                    /* правил в наборе не больше (память) */
#define POOL_MAX (1u << 20)               /* текста свойств в наборе не больше 1 МБ */
#define CAND_MAX 192                      /* подходящих элементу правил не больше */
#define VIEW_W 1024                       /* экран для @media, vw и vh */
#define VIEW_H 768

/* СТЕК. Разбор страницы рекурсивный (таблицы и колонки flex: parse → … → render_range → parse), а
 * стек задачи браузера — 12 КБ (app.c). Подбор правил (css_match) и разбор таблицы стилей (sheet,
 * вложенные @media — рекурсия) зовутся из самой глубины, поэтому их большие буферы — static: сами
 * себя эти функции не вызывают, задача у разбора одна. ESP-сборка, gcc -fstack-usage 07.11:
 * css_match был 1728 байт, sheet — 1168 на уровень. */
static int low(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int sp(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int digit(int c) { return c >= '0' && c <= '9'; }

/* --- отпечатки ----------------------------------------------------------------------------- */

uint32_t css_hash(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h ? h : 1;
}
uint32_t css_hash_ci(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)low((unsigned char)s[i]); h *= 16777619u; }
    return h ? h : 1;
}
static uint32_t k_tag(const char *s, size_t n) { uint32_t h = css_hash_ci(s, n) ^ SALT_TAG; return h ? h : 1; }
static uint32_t k_id(const char *s, size_t n) { uint32_t h = css_hash(s, n) ^ SALT_ID; return h ? h : 1; }

void css_el_make(css_el_t *el, const char *tag, const char *cls, const char *id) {
    memset(el, 0, sizeof(*el));
    if (tag && tag[0]) el->tag = k_tag(tag, strlen(tag));
    if (id && id[0]) el->id = k_id(id, strlen(id));
    for (const char *p = cls ? cls : ""; *p;) {
        while (sp((unsigned char)*p)) p++;
        const char *q = p;
        while (*q && !sp((unsigned char)*q)) q++;
        if (q > p && el->ncls < CSS_EL_CLS) {
            uint32_t h = css_hash(p, (size_t)(q - p));
            int dup = 0;
            for (int k = 0; k < el->ncls; k++) dup |= el->cls[k] == h;
            if (!dup) el->cls[el->ncls++] = h;
        }
        p = q;
    }
}

/* --- имена документа (фильтр правил) ---------------------------------------------------------
 * «Фильтр Блума»: 16384 бита, каждое имя ставит два бита (по двум частям отпечатка). Нет хоть
 * одного бита — имени в документе точно нет; оба есть — скорее всего есть. */
static uint32_t names[512];
static int names_on;                     /* фильтр включён (имена документа собраны) */
static void name_add(uint32_t h) {
    names[(h & 16383u) >> 5] |= 1u << (h & 31u);
    uint32_t g = h >> 14;
    names[(g & 16383u) >> 5] |= 1u << (g & 31u);
}
static int name_has(uint32_t h) {
    uint32_t g = h >> 14;
    return (names[(h & 16383u) >> 5] >> (h & 31u) & 1u) && (names[(g & 16383u) >> 5] >> (g & 31u) & 1u);
}
void css_names_reset(void) {
    memset(names, 0, sizeof(names));
    names_on = 1;
    name_add(k_tag("html", 4));
    name_add(k_tag("body", 4));
}
void css_name_tag(const char *t) { if (t[0]) name_add(k_tag(t, strlen(t))); }
void css_name_id(const char *id) { if (id[0]) name_add(k_id(id, strlen(id))); }
void css_name_classes(const char *c) {
    for (const char *p = c; *p;) {
        while (sp((unsigned char)*p)) p++;
        const char *q = p;
        while (*q && !sp((unsigned char)*q)) q++;
        if (q > p) name_add(css_hash(p, (size_t)(q - p)));
        p = q;
    }
}

/* --- наборы правил ----------------------------------------------------------------------------
 * Два набора: ext — из внешних таблиц (живут до новой страницы), doc — из <style> документа
 * (заново при каждой раскладке). У каждого: правила r, простые селекторы c (у правила — подряд,
 * свой — первым, потом родитель, дед…), текст pool (свойства и атрибуты селекторов). */
typedef struct {
    uint32_t tag, id;                    /* 0 — любой */
    uint32_t cls[4];
    uint32_t attr;                       /* [имя op значение]: «имя\0значение\0» в pool; 0 — нет */
    uint8_t ncls, op, nocase;            /* op: 'e' есть, '=' равно, '~' слово, '|' приставка с «-», ^ $ * */
} cmp_t;

typedef struct {
    uint32_t decl, spec, key;            /* свойства (в pool), вес, ключ корзины */
    int32_t next;                        /* следующее правило той же корзины; −1 — нет */
    uint32_t cmp;                        /* первый простой селектор (свой) */
    uint8_t ncmp, child;                 /* их число; бит j — между j и j+1 стоит «>» */
    int8_t part;                         /* фрейм (у внешних таблиц) */
} rule_t;

typedef struct {
    rule_t *r;
    unsigned n, cap;
    cmp_t *c;
    unsigned cn, ccap;
    char *pool;
    unsigned pn, pcap;
    int32_t *head;                       /* BUCKETS начал цепочек корзин */
} rs_t;
static rs_t ext_rs, doc_rs;

static int grow(void **p, unsigned *cap, unsigned need, size_t item, unsigned first) {
    if (need <= *cap) return 1;
    unsigned nc = *cap ? *cap : first;
    while (nc < need) nc *= 2;
    void *q = ts_core->realloc(*p, (size_t)nc * item);
    if (!q) return 0;
    *p = q;
    *cap = nc;
    return 1;
}

static void rs_reset(rs_t *rs) {
    rs->n = rs->cn = 0;
    rs->pn = 1;                          /* смещение 0 в pool значит «нет» */
    if (rs->head) memset(rs->head, 0xFF, BUCKETS * sizeof(int32_t));
}

/* Строка в pool (с нулём на конце); 0 — нет места. */
static uint32_t pool_put(rs_t *rs, const char *s, size_t n) {
    if (!rs->pn) rs->pn = 1;
    if (rs->pn + n + 1 > POOL_MAX || !grow((void **)&rs->pool, &rs->pcap, rs->pn + (unsigned)n + 1, 1, 16384)) return 0;
    uint32_t at = rs->pn;
    memcpy(rs->pool + at, s, n);
    rs->pool[at + n] = 0;
    rs->pn += (unsigned)n + 1;
    return at;
}

/* Свойства правила — в pool: комментарии прочь, переводы строк и табуляции — пробелами. */
static uint32_t decl_put(rs_t *rs, const char *d, size_t n) {
    if (!rs->pn) rs->pn = 1;
    if (rs->pn + n + 1 > POOL_MAX || !grow((void **)&rs->pool, &rs->pcap, rs->pn + (unsigned)n + 1, 1, 16384)) return 0;
    uint32_t at = rs->pn;
    char *o = rs->pool + at;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (d[i] == '/' && i + 1 < n && d[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(d[i] == '*' && d[i + 1] == '/')) i++;
            i++;
            continue;
        }
        o[k++] = sp((unsigned char)d[i]) ? ' ' : d[i];
    }
    while (k && o[k - 1] == ' ') k--;
    size_t s = 0;
    while (s < k && o[s] == ' ') s++;
    if (s == k) return 0;                /* свойств нет — правило ни к чему */
    if (s) memmove(o, o + s, k - s);
    k -= s;
    o[k] = 0;
    rs->pn += (unsigned)k + 1;
    return at;
}

/* --- разбор селекторов ------------------------------------------------------------------------- */

static int hexv(int c) {
    c = low(c);
    return digit(c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static int ident_ch(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || digit(c) || c == '-' || c == '_' || c >= 0x80 || c == '\\'; }
static int utf8(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18 & 7)); o[1] = (char)(0x80 | (c >> 12 & 63)); o[2] = (char)(0x80 | (c >> 6 & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* Имя (класса, тега, id) с позиции i до n — в buf; «\» — экранирование: «.md\:flex» — класс
 * «md:flex», «.\31 0» — класс «10» (код знака в шестнадцатеричном виде). Возвращает позицию за ним. */
static size_t ident(const char *s, size_t n, size_t i, char *buf, size_t cap, size_t *len) {
    size_t k = 0;
    while (i < n) {
        int c = (unsigned char)s[i];
        if (c == '\\') {
            if (++i >= n) break;
            if (hexv((unsigned char)s[i]) >= 0) {
                uint32_t cp = 0;
                int d = 0;
                while (i < n && d < 6 && hexv((unsigned char)s[i]) >= 0) { cp = cp * 16 + (uint32_t)hexv((unsigned char)s[i]); i++; d++; }
                if (i < n && sp((unsigned char)s[i])) i++;
                char u[4];
                int m = utf8(cp, u);
                for (int q = 0; q < m; q++) if (k + 1 < cap) buf[k++] = u[q];
            } else {
                if (k + 1 < cap) buf[k++] = s[i];
                i++;
            }
            continue;
        }
        if (!ident_ch(c)) break;
        if (k + 1 < cap) buf[k++] = (char)c;
        i++;
    }
    buf[k] = 0;
    *len = k;
    return i;
}

/* [имя], [имя=значение], [имя~=…], |=, ^=, $=, *=, и «i» в конце — без учёта регистра. */
static int attr_sel(rs_t *rs, const char *s, size_t n, size_t *pi, cmp_t *c) {
    size_t i = *pi + 1;
    while (i < n && sp((unsigned char)s[i])) i++;
    static char name[48], val[128];          /* static — стек (см. начало файла); себя не вызывает */
    size_t nl, vl = 0;
    i = ident(s, n, i, name, sizeof(name), &nl);
    if (!nl) return 0;
    for (size_t k = 0; k < nl; k++) name[k] = (char)low((unsigned char)name[k]);
    while (i < n && sp((unsigned char)s[i])) i++;
    int op = 'e', nocase = 0;
    val[0] = 0;
    if (i < n && s[i] != ']') {
        if (s[i] == '=') { op = '='; i++; }
        else if (i + 1 < n && s[i + 1] == '=' && strchr("~|^$*", s[i])) { op = s[i]; i += 2; }
        else return 0;
        while (i < n && sp((unsigned char)s[i])) i++;
        if (i < n && (s[i] == '"' || s[i] == '\'')) {
            char q = s[i++];
            while (i < n && s[i] != q) {
                if (s[i] == '\\' && i + 1 < n) i++;
                if (vl + 1 < sizeof(val)) val[vl++] = s[i];
                i++;
            }
            i++;
            val[vl] = 0;
        } else {
            i = ident(s, n, i, val, sizeof(val), &vl);
        }
        while (i < n && sp((unsigned char)s[i])) i++;
        if (i < n && (s[i] == 'i' || s[i] == 'I' || s[i] == 's' || s[i] == 'S')) {
            nocase = low((unsigned char)s[i]) == 'i';
            i++;
            while (i < n && sp((unsigned char)s[i])) i++;
        }
    }
    if (i >= n || s[i] != ']') return 0;
    *pi = i + 1;
    if (c->attr) return 1;               /* второй атрибут в одном селекторе — хватит первого */
    static char b[180];
    memcpy(b, name, nl + 1);
    memcpy(b + nl + 1, val, vl + 1);
    c->attr = pool_put(rs, b, nl + 1 + vl);
    c->op = (uint8_t)op;
    c->nocase = (uint8_t)nocase;
    return c->attr != 0;
}

/* Простой селектор с позиции *pi. Состояния, которых у нас нет (:hover, :focus, :first-child…)
 * и «части» элемента (::before) — 0: такое правило пропускается. :root — это <html>; :link —
 * как без него (у ссылки адрес всегда есть); :not(…) — как будто его нет (почти всегда это
 * «кроме редкого случая»). */
static int compound(rs_t *rs, const char *s, size_t n, size_t *pi, cmp_t *c, uint32_t *spec) {
    size_t i = *pi;
    static char buf[96];                      /* static: compound себя не вызывает */
    size_t bl;
    int any = 0;
    memset(c, 0, sizeof(*c));
    while (i < n) {
        int ch = (unsigned char)s[i];
        if (ch == '*') { i++; any = 1; continue; }
        if (ch == '.' || ch == '#') {
            i = ident(s, n, i + 1, buf, sizeof(buf), &bl);
            if (!bl) return 0;
            if (ch == '.') { if (c->ncls < 4) c->cls[c->ncls++] = css_hash(buf, bl); *spec += 1u << 10; }
            else { c->id = k_id(buf, bl); *spec += 1u << 20; }
            any = 1;
            continue;
        }
        if (ch == '[') {
            if (!attr_sel(rs, s, n, &i, c)) return 0;
            *spec += 1u << 10;
            any = 1;
            continue;
        }
        if (ch == ':') {
            int el = i + 1 < n && s[i + 1] == ':';
            i = ident(s, n, i + 1 + (size_t)el, buf, sizeof(buf), &bl);
            for (size_t k = 0; k < bl; k++) buf[k] = (char)low((unsigned char)buf[k]);
            if (i < n && s[i] == '(') {
                int d = 0;
                for (; i < n; i++) {
                    if (s[i] == '(') d++;
                    else if (s[i] == ')' && --d == 0) { i++; break; }
                }
            }
            if (el || !bl) return 0;
            if (!strcmp(buf, "root")) {
                uint32_t h = k_tag("html", 4);
                if (c->tag && c->tag != h) return 0;
                c->tag = h;
                *spec += 1u << 10;
            } else if (!strcmp(buf, "link") || !strcmp(buf, "any-link")) {
                *spec += 1u << 10;
            } else if (strcmp(buf, "not")) {
                return 0;
            }
            any = 1;
            continue;
        }
        if (ident_ch(ch) && !digit(ch)) {
            if (any) return 0;               /* тег — только в начале («div.a», не «.adiv») */
            i = ident(s, n, i, buf, sizeof(buf), &bl);
            c->tag = k_tag(buf, bl);
            *spec += 1;
            any = 1;
            continue;
        }
        break;
    }
    *pi = i;
    return any;
}

/* Есть ли имена простого селектора в документе (фильтр Блума). */
static int names_ok(const cmp_t *c) {
    if (!names_on) return 1;
    if (c->tag && !name_has(c->tag)) return 0;
    if (c->id && !name_has(c->id)) return 0;
    for (int k = 0; k < c->ncls; k++) if (!name_has(c->cls[k])) return 0;
    return 1;
}

/* Селектор (один из списка через запятую) → правило r и его простые селекторы out (свой —
 * первым). Длиннее четырёх — остаются четыре правых (правило станет чуть «шире»). 0 — не
 * понимаем или к документу не подойдёт. */
static int selector(rs_t *rs, const char *s, size_t n, cmp_t *out, rule_t *r) {
    static cmp_t t[8];                        /* static: selector себя не вызывает */
    static uint8_t ch[8];
    memset(ch, 0, sizeof(ch));
    int k = 0;
    uint32_t spec = 0;
    size_t i = 0;
    while (i < n && sp((unsigned char)s[i])) i++;
    while (i < n) {
        if (k == 8) { memmove(t, t + 1, 7 * sizeof(t[0])); memmove(ch, ch + 1, 7); k = 7; }
        if (!compound(rs, s, n, &i, &t[k], &spec)) return 0;
        k++;
        int gap = 0;
        while (i < n && sp((unsigned char)s[i])) { i++; gap = 1; }
        if (i >= n) break;
        if (s[i] == '>') { ch[k - 1] = 1; i++; while (i < n && sp((unsigned char)s[i])) i++; }
        else if (s[i] == '+' || s[i] == '~') return 0;   /* соседи — не понимаем */
        else if (gap) ch[k - 1] = 0;
        else return 0;
    }
    if (!k) return 0;
    int m = k > 4 ? 4 : k;
    r->ncmp = (uint8_t)m;
    r->child = 0;
    for (int j = 0; j < m; j++) {
        out[j] = t[k - 1 - j];
        if (j + 1 < m && ch[k - 2 - j]) r->child |= (uint8_t)(1u << j);
        if (!names_ok(&out[j])) return 0;
    }
    r->spec = spec;
    return 1;
}

/* Правило — в набор и в корзину по ключу своего простого селектора. */
static void commit(rs_t *rs, rule_t *r, const cmp_t *c, uint32_t decl, int part) {
    if (rs->n >= RULES_MAX) return;
    if (!rs->head) {
        if (!(rs->head = ts_alloc(BUCKETS * sizeof(int32_t)))) return;
        memset(rs->head, 0xFF, BUCKETS * sizeof(int32_t));
    }
    if (!grow((void **)&rs->r, &rs->cap, rs->n + 1, sizeof(rule_t), 256) ||
        !grow((void **)&rs->c, &rs->ccap, rs->cn + r->ncmp, sizeof(cmp_t), 512))
        return;
    r->decl = decl;
    r->part = (int8_t)part;
    r->cmp = rs->cn;
    memcpy(rs->c + rs->cn, c, r->ncmp * sizeof(cmp_t));
    rs->cn += r->ncmp;
    r->key = c[0].id ? c[0].id : c[0].ncls ? c[0].cls[0] : c[0].tag;
    unsigned b = r->key & (BUCKETS - 1);
    r->next = rs->head[b];
    rs->head[b] = (int32_t)rs->n;
    rs->r[rs->n++] = *r;
}

/* «a, b.c > d { свойства }»: правило на каждый селектор списка (свойства — одна копия). */
static void rules_add(rs_t *rs, const char *s, size_t n, const char *d, size_t dl, int part) {
    uint32_t decl = 0;
    size_t p = 0;
    while (p < n) {
        size_t q = p;
        int par = 0;
        while (q < n && (s[q] != ',' || par)) { if (s[q] == '(') par++; else if (s[q] == ')' && par) par--; q++; }
        static rule_t r;                      /* static: rules_add себя не вызывает */
        static cmp_t c[4];
        memset(&r, 0, sizeof(r));
        if (selector(rs, s + p, q - p, c, &r)) {
            if (!decl && !(decl = decl_put(rs, d, dl))) return;
            commit(rs, &r, c, decl, part);
        }
        p = q + 1;
    }
}

/* --- разбор таблицы стилей ---------------------------------------------------------------------- */

static size_t skip_comment(const char *s, size_t n, size_t i) {        /* i — на начале комментария */
    i += 2;
    while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
    return i + 2 < n ? i + 2 : n;
}
static size_t skip_string(const char *s, size_t n, size_t i) {         /* i — на кавычке */
    char q = s[i++];
    while (i < n && s[i] != q) { if (s[i] == '\\') i++; i++; }
    return i + 1 < n ? i + 1 : n;
}

/* Конец блока «{ … }» (i — за «{»): позиция за парной «}». *nested — внутри свои «{» (вложенные
 * правила нового CSS — их не понимаем). */
static size_t block_end(const char *s, size_t n, size_t i, int *nested) {
    int d = 1;
    *nested = 0;
    while (i < n) {
        char c = s[i];
        if (c == '"' || c == '\'') { i = skip_string(s, n, i); continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') { i = skip_comment(s, n, i); continue; }
        if (c == '{') { d++; *nested = 1; }
        else if (c == '}' && --d == 0) return i + 1;
        i++;
    }
    return n;
}

/* @supports (условие): «not» и «selector(…)» — считаем «нет», остальное — «да» (что сайт
 * проверяет, у нас обычно либо есть, либо безвредно). */
static int supports_ok(const char *s, size_t n) {
    for (size_t i = 0; i + 3 <= n; i++) {
        if (!strncmp(s + i, "not", 3) && (i == 0 || !ident_ch((unsigned char)s[i - 1])) && (i + 3 == n || !ident_ch((unsigned char)s[i + 3])))
            return 0;
        if (i + 9 <= n && !strncmp(s + i, "selector(", 9)) return 0;
    }
    return 1;
}

static void sheet(rs_t *rs, const char *s, size_t n, const char *base, int part, int lvl);

/* @import url(адрес) [условие media]; — адрес в очередь внешних таблиц. */
static void import_rule(const char *s, size_t n, const char *base, int part) {
    size_t i = 0;
    while (i < n && sp((unsigned char)s[i])) i++;
    static char u[URL_MAX];
    size_t k = 0;
    if (i + 4 <= n && !strncmp(s + i, "url(", 4)) {
        i += 4;
        while (i < n && sp((unsigned char)s[i])) i++;
        char q = i < n && (s[i] == '"' || s[i] == '\'') ? s[i++] : ')';
        while (i < n && s[i] != q && k + 1 < sizeof(u)) u[k++] = s[i++];
        while (i < n && s[i] != ')') i++;
        i++;
    } else if (i < n && (s[i] == '"' || s[i] == '\'')) {
        char q = s[i++];
        while (i < n && s[i] != q && k + 1 < sizeof(u)) u[k++] = s[i++];
        i++;
    } else {
        return;
    }
    u[k] = 0;
    while (k && sp((unsigned char)u[k - 1])) u[--k] = 0;
    if (i < n && !css_media(s + i, n - i)) return;
    static char abs_url[URL_MAX];
    if (base && url_resolve(base, u, abs_url, sizeof(abs_url))) css_link(abs_url, part);
}

/* Таблица стилей s (n байт) — в набор rs. Комментарии и «<!-- -->» — мимо; @media и @supports
 * с подходящим условием, @layer — их правила как обычные; @import — в очередь; остальные
 * @-блоки (@font-face, @keyframes, @page…) — мимо. */
static void sheet(rs_t *rs, const char *s, size_t n, const char *base, int part, int lvl) {
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (sp((unsigned char)c) || c == '}' || c == ';') { i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') { i = skip_comment(s, n, i); continue; }
        if (c == '<' && i + 3 < n && !strncmp(s + i, "<!--", 4)) { i += 4; continue; }
        if (c == '-' && i + 2 < n && !strncmp(s + i, "-->", 3)) { i += 3; continue; }
        size_t a = i;                                     /* «прелюдия» — до «{» или «;» */
        int par = 0;
        while (i < n) {
            char d = s[i];
            if (d == '"' || d == '\'') { i = skip_string(s, n, i); continue; }
            if (d == '/' && i + 1 < n && s[i + 1] == '*') { i = skip_comment(s, n, i); continue; }
            if (d == '(') par++;
            else if (d == ')' && par) par--;
            else if (!par && (d == '{' || d == ';' || d == '}')) break;
            i++;
        }
        if (i >= n) break;
        if (s[i] == '}') { i++; continue; }
        if (s[a] == '@') {
            char name[24];
            size_t nl, p = ident(s, i, a + 1, name, sizeof(name), &nl);
            for (size_t k = 0; k < nl; k++) name[k] = (char)low((unsigned char)name[k]);
            if (s[i] == ';') {
                if (!strcmp(name, "import")) import_rule(s + p, i - p, base, part);
                i++;
                continue;
            }
            int nested;
            size_t e = block_end(s, n, i + 1, &nested), b1 = e > i + 1 ? e - 1 : i + 1;
            if (lvl < 4 && ((!strcmp(name, "media") && css_media(s + p, i - p)) ||
                            (!strcmp(name, "supports") && supports_ok(s + p, i - p)) || !strcmp(name, "layer")))
                sheet(rs, s + i + 1, b1 - (i + 1), base, part, lvl + 1);
            i = e;
            continue;
        }
        if (s[i] == ';') { i++; continue; }
        int nested;
        size_t e = block_end(s, n, i + 1, &nested), d1 = e > i + 1 ? e - 1 : i + 1;
        if (!nested && d1 > i + 1) rules_add(rs, s + a, i - a, s + i + 1, d1 - (i + 1), part);
        i = e;
    }
}

void css_doc_reset(void) { rs_reset(&doc_rs); }
void css_doc_sheet(const char *s, size_t n, const char *base, int part) { sheet(&doc_rs, s, n, base, part, 0); }

/* --- какие правила подходят элементу ------------------------------------------------------------ */

static int word_in(const char *list, const char *w) {
    size_t wl = strlen(w);
    for (const char *p = list; *p;) {
        while (sp((unsigned char)*p)) p++;
        const char *q = p;
        while (*q && !sp((unsigned char)*q)) q++;
        if ((size_t)(q - p) == wl && wl && !strncmp(p, w, wl)) return 1;
        p = q;
    }
    return 0;
}

static int attr_match(const rs_t *rs, const cmp_t *c, css_attr_fn fn, void *ctx) {
    const char *name = rs->pool + c->attr, *want = name + strlen(name) + 1;
    static char v[256], w[128];               /* static: подбор правил себя не вызывает */
    if (!fn || !fn(ctx, name, v, sizeof(v))) return 0;
    if (c->op == 'e') return 1;
    snprintf(w, sizeof(w), "%s", want);
    if (c->nocase) {
        for (char *p = v; *p; p++) *p = (char)low((unsigned char)*p);
        for (char *p = w; *p; p++) *p = (char)low((unsigned char)*p);
    }
    size_t wl = strlen(w), vl = strlen(v);
    switch (c->op) {
    case '=': return !strcmp(v, w);
    case '~': return word_in(v, w);
    case '|': return !strcmp(v, w) || (!strncmp(v, w, wl) && v[wl] == '-');
    case '^': return wl && !strncmp(v, w, wl);
    case '$': return wl && vl >= wl && !strcmp(v + vl - wl, w);
    case '*': return wl && strstr(v, w) != NULL;
    default: return 0;
    }
}

static int cmp_match(const rs_t *rs, const cmp_t *c, const css_el_t *el, css_attr_fn fn, void *ctx) {
    if (c->tag && c->tag != el->tag) return 0;
    if (c->id && c->id != el->id) return 0;
    for (int k = 0; k < c->ncls; k++) {
        int f = 0;
        for (int q = 0; q < el->ncls && !f; q++) f = el->cls[q] == c->cls[k];
        if (!f) return 0;
    }
    if (c->attr && !attr_match(rs, c, fn, ctx)) return 0;   /* у предков атрибутов не знаем — нет */
    return 1;
}

/* Цепочка: свой простой селектор — элементу, остальные — предкам по порядку («>» — прямо
 * следующий предок, пробел — первый подходящий выше). */
static int rule_match(const rs_t *rs, const rule_t *r, const css_el_t *el, css_attr_fn fn, void *ctx,
                      const css_el_t *const *anc, int nanc) {
    const cmp_t *c = rs->c + r->cmp;
    if (!cmp_match(rs, &c[0], el, fn, ctx)) return 0;
    int pos = 0;
    for (int j = 1; j < r->ncmp; j++) {
        if (r->child >> (j - 1) & 1) {
            if (pos >= nanc || !cmp_match(rs, &c[j], anc[pos], NULL, NULL)) return 0;
            pos++;
        } else {
            while (pos < nanc && !cmp_match(rs, &c[j], anc[pos], NULL, NULL)) pos++;
            if (pos >= nanc) return 0;
            pos++;
        }
    }
    return 1;
}

int css_match(const css_el_t *el, css_attr_fn fn, void *ctx, const css_el_t *const *anc, int nanc, int part,
              const char **out, int max) {
    static uint64_t cand[CAND_MAX];           /* static: 1,5 КБ — не в стеке задачи браузера */
    int cn = 0;
    static uint32_t keys[CSS_EL_CLS + 3];
    int kn = 0;
    if (el->id) keys[kn++] = el->id;
    for (int k = 0; k < el->ncls; k++) keys[kn++] = el->cls[k];
    if (el->tag) keys[kn++] = el->tag;
    keys[kn++] = 0;
    for (int s = 0; s < 2; s++) {
        const rs_t *rs = s ? &doc_rs : &ext_rs;
        if (!rs->n || !rs->head) continue;
        for (int k = 0; k < kn; k++) {
            for (int32_t i = rs->head[keys[k] & (BUCKETS - 1)]; i >= 0; i = rs->r[i].next) {
                const rule_t *r = &rs->r[i];
                if (r->key != keys[k] || (!s && r->part != part)) continue;
                if (!rule_match(rs, r, el, fn, ctx, anc, nanc)) continue;
                if (cn < CAND_MAX) cand[cn++] = (uint64_t)r->spec << 32 | (uint64_t)s << 24 | (uint32_t)i;
            }
        }
    }
    for (int i = 1; i < cn; i++) {                    /* по весу, потом по порядку (вставками) */
        uint64_t v = cand[i];
        int j = i - 1;
        while (j >= 0 && cand[j] > v) { cand[j + 1] = cand[j]; j--; }
        cand[j + 1] = v;
    }
    int m = 0;
    for (int i = 0; i < cn && m < max; i++) {
        const rs_t *rs = cand[i] >> 24 & 1 ? &doc_rs : &ext_rs;
        out[m++] = rs->pool + rs->r[cand[i] & 0xFFFFFFu].decl;
    }
    return m;
}

/* --- внешние таблицы стилей ---------------------------------------------------------------------- */

#define LINK_MAX 48
typedef struct {
    char *url;
    int8_t part;
    uint8_t state;                       /* 0 — ждёт, 1 — грузится, 2 — разобрана, 3 — не загрузилась */
} link_t;
static link_t links[LINK_MAX];
static int link_n;

void css_links_clear(void) {
    for (int i = 0; i < link_n; i++) ts_core->free(links[i].url);
    link_n = 0;
    rs_reset(&ext_rs);
}

void css_link(const char *url, int part) {
    if (!url[0] || link_n >= LINK_MAX) return;
    for (int i = 0; i < link_n; i++) if (links[i].part == part && !strcmp(links[i].url, url)) return;
    size_t n = strlen(url);
    char *u = ts_alloc(n + 1);
    if (!u) return;
    memcpy(u, url, n + 1);
    links[link_n].url = u;
    links[link_n].part = (int8_t)part;
    links[link_n].state = 0;
    link_n++;
}

const char *css_link_next(void) {
    for (int i = 0; i < link_n; i++)
        if (links[i].state == 0) { links[i].state = 1; return links[i].url; }
    return NULL;
}

void css_link_done(const char *url, const uint8_t *b, size_t n) {
    for (int i = 0; i < link_n; i++) {
        if (links[i].state != 1 || strcmp(links[i].url, url)) continue;
        links[i].state = b ? 2 : 3;
        if (b) {
            static char base[URL_MAX];               /* адрес — копия: @import может добавить адрес */
            snprintf(base, sizeof(base), "%s", links[i].url);
            sheet(&ext_rs, (const char *)b, n, base, links[i].part, 0);
        }
        return;
    }
}

int css_links_wanted(void) {
    int k = 0;
    for (int i = 0; i < link_n; i++) k += links[i].state < 2;
    return k;
}

void css_links_stop(void) {
    for (int i = 0; i < link_n; i++) if (links[i].state < 2) links[i].state = 3;
}

int css_rules(int ext) { return (int)(ext ? ext_rs.n : doc_rs.n); }

/* --- таблицы стилей в памяти ---------------------------------------------------------------------
 * До MEM_N таблиц, вместе не больше MEM_MAX: не хватает места — уходит та, что дольше всех не
 * спрашивалась (used — «время» последнего обращения). Хранится исходный текст: разобранные
 * правила зависят от имён конкретной страницы (фильтр css_names) и другой странице не годятся. */
#define MEM_N   48
#define MEM_MAX (512u * 1024u)
static struct {
    char *url;
    uint8_t *text;
    uint32_t len, used;
} mem[MEM_N];
static uint32_t mem_total, mem_clock;

static void mem_drop(int i) {
    ts_core->free(mem[i].url);
    ts_core->free(mem[i].text);
    mem_total -= mem[i].len;
    memset(&mem[i], 0, sizeof(mem[i]));
}

const uint8_t *css_mem_get(const char *url, size_t *len) {
    for (int i = 0; i < MEM_N; i++)
        if (mem[i].url && !strcmp(mem[i].url, url)) {
            mem[i].used = ++mem_clock;
            *len = mem[i].len;
            return mem[i].text;
        }
    return NULL;
}

void css_mem_put(const char *url, uint8_t *text, size_t len) {
    if (!text) return;
    size_t ul = strlen(url);
    char *u = len && len <= MEM_MAX / 2 ? ts_alloc(ul + 1) : NULL;
    if (!u) { ts_core->free(text); return; }
    memcpy(u, url, ul + 1);
    for (int i = 0; i < MEM_N; i++)
        if (mem[i].url && !strcmp(mem[i].url, url)) mem_drop(i);       /* «обновить» — заменить */
    for (;;) {
        int free_slot = -1, old = -1;
        for (int i = 0; i < MEM_N; i++) {
            if (!mem[i].url) { if (free_slot < 0) free_slot = i; }
            else if (old < 0 || mem[i].used < mem[old].used) old = i;
        }
        if (free_slot >= 0 && mem_total + len <= MEM_MAX) {
            mem[free_slot].url = u;
            mem[free_slot].text = text;
            mem[free_slot].len = (uint32_t)len;
            mem[free_slot].used = ++mem_clock;
            mem_total += (uint32_t)len;
            return;
        }
        if (old < 0) break;
        mem_drop(old);
    }
    ts_core->free(u);
    ts_core->free(text);
}

int css_mem_trim(void) {
    int any = 0;
    for (int i = 0; i < MEM_N; i++)
        if (mem[i].url) { mem_drop(i); any = 1; }
    return any;
}

/* --- переменные (--x) ---------------------------------------------------------------------------
 * Стек: у каждой записи — отпечаток имени, значение (в vpool) и глубина элемента, который её
 * объявил. Значение var(--x) — ближайшее сверху: так переменная «наследуется» вниз по дереву, а
 * у соседних элементов свои (у каждого контейнера могут быть свои --width, --gap…). */
typedef struct {
    uint32_t name, at;
    uint16_t len;
    uint8_t depth;
} var_t;
static var_t *vars;
static unsigned var_n, var_cap;
static char *vpool;
static unsigned vp_n, vp_cap;

void css_vars_cut(int depth) {
    while (var_n && vars[var_n - 1].depth >= depth) vp_n = vars[--var_n].at;
}

static void var_set(const char *name, size_t nl, const char *v, size_t vl, int depth) {
    if (vl > 1000 || var_n >= 4096 || vp_n + vl > (256u << 10)) return;
    if (!grow((void **)&vars, &var_cap, var_n + 1, sizeof(var_t), 256) || !grow((void **)&vpool, &vp_cap, vp_n + (unsigned)vl, 1, 8192))
        return;
    var_t *e = &vars[var_n++];
    e->name = css_hash_ci(name, nl);
    e->at = vp_n;
    e->len = (uint16_t)vl;
    e->depth = (uint8_t)(depth > 255 ? 255 : depth);
    memcpy(vpool + vp_n, v, vl);
    vp_n += (unsigned)vl;
}

static const char *var_get(uint32_t h, size_t *len) {
    for (unsigned i = var_n; i-- > 0;)
        if (vars[i].name == h) { *len = vars[i].len; return vpool + vars[i].at; }
    return NULL;
}

/* Конец значения свойства: «;» вне скобок и кавычек (в url(data:…;base64,…) «;» — часть адреса). */
static const char *value_end(const char *p) {
    int par = 0;
    char q = 0;
    for (; *p; p++) {
        if (q) { if (*p == q) q = 0; }
        else if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '(') par++;
        else if (*p == ')') { if (par) par--; }
        else if (*p == ';' && !par) break;
    }
    return p;
}

void css_vars_take(const char *d, int depth) {
    const char *p = d;
    while (*p) {
        while (*p == ';' || sp((unsigned char)*p)) p++;
        if (!*p) break;
        const char *name = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') continue;
        size_t nl = (size_t)(p - name);
        while (nl && sp((unsigned char)name[nl - 1])) nl--;
        p++;
        const char *val = p, *e = value_end(p);
        p = e;
        if (nl < 3 || name[0] != '-' || name[1] != '-') continue;
        while (val < e && sp((unsigned char)*val)) val++;
        size_t vl = (size_t)(e - val);
        for (size_t i = 0; i < vl; i++) if (val[i] == '!') { vl = i; break; }
        while (vl && sp((unsigned char)val[vl - 1])) vl--;
        var_set(name, nl, val, vl, depth);
    }
}

/* Значение со строчными буквами, пробелами вместо переводов строк и подставленными var(). 0 —
 * переменной нет и запасного значения нет (свойство тогда недействительно, как у браузеров). */
static int expand(const char *v, size_t n, char *out, size_t cap, size_t *o, int lvl) {
    if (lvl > 6) return 0;
    for (size_t i = 0; i < n;) {
        if (i + 4 <= n && low((unsigned char)v[i]) == 'v' && low((unsigned char)v[i + 1]) == 'a' &&
            low((unsigned char)v[i + 2]) == 'r' && v[i + 3] == '(') {
            size_t p = i + 4, q = p, comma = 0;
            int d = 1;
            for (; q < n; q++) {
                if (v[q] == '(') d++;
                else if (v[q] == ')' && --d == 0) break;
                else if (v[q] == ',' && d == 1 && !comma) comma = q;
            }
            size_t ne = comma ? comma : q;
            while (p < ne && sp((unsigned char)v[p])) p++;
            while (ne > p && sp((unsigned char)v[ne - 1])) ne--;
            size_t vl;
            const char *val = var_get(css_hash_ci(v + p, ne - p), &vl);
            if (val) {
                if (!expand(val, vl, out, cap, o, lvl + 1)) return 0;
            } else if (comma) {
                size_t f = comma + 1;
                while (f < q && sp((unsigned char)v[f])) f++;
                if (!expand(v + f, q - f, out, cap, o, lvl + 1)) return 0;
            } else {
                return 0;
            }
            i = q + 1;
            continue;
        }
        char c = sp((unsigned char)v[i]) ? ' ' : (char)low((unsigned char)v[i]);
        if (!(c == ' ' && (!*o || out[*o - 1] == ' ')) && *o + 1 < cap) out[(*o)++] = c;
        i++;
    }
    return 1;
}

int css_important;

int css_get(const char *st, const char *prop, char *out, size_t cap) {
    size_t pl = strlen(prop);
    const char *p = st, *best = NULL;
    size_t bl = 0;
    while (*p) {
        while (*p == ';' || sp((unsigned char)*p)) p++;
        if (!*p) break;
        const char *name = p;
        while (*p && *p != ':' && *p != ';') p++;
        size_t nl = (size_t)(p - name);
        while (nl && sp((unsigned char)name[nl - 1])) nl--;
        if (*p != ':') continue;
        p++;
        while (sp((unsigned char)*p)) p++;
        const char *val = p;
        p = value_end(p);
        size_t vl = (size_t)(p - val);
        int match = nl == pl;
        for (size_t i = 0; match && i < nl; i++) match = low((unsigned char)name[i]) == prop[i];
        if (!match) continue;
        int imp = 0;
        for (size_t i = 0; i < vl; i++) if (val[i] == '!') { imp = 1; vl = i; break; }
        if (css_important && !imp) continue;
        while (vl && sp((unsigned char)val[vl - 1])) vl--;
        best = val;
        bl = vl;
    }
    if (!best || !cap) return 0;
    size_t o = 0;
    if (!expand(best, bl, out, cap, &o, 0)) return 0;
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    return 1;
}

/* --- длины: числа с единицами и calc() ---------------------------------------------------------
 * Значение длины — пара «точки + проценты» (calc(100% − 20px) иначе не сложить), num — число без
 * единиц (множитель в calc). Разбор — «рекурсивным спуском»: выражение — слагаемые через + и −,
 * слагаемое — множители через * и /, множитель — число, скобки, calc(), min(), max(), clamp(). */
typedef struct {
    float px, pc;
    int num, ok;
} lv_t;
static const char *lp, *le;
static int lbase;
static lv_t lerr(void) { lv_t v = {0, 0, 0, 0}; return v; }
static void lsp(void) { while (lp < le && sp((unsigned char)*lp)) lp++; }
static int lword(const char *w) {
    size_t n = strlen(w);
    if ((size_t)(le - lp) < n) return 0;
    for (size_t i = 0; i < n; i++) if (low((unsigned char)lp[i]) != w[i]) return 0;
    lp += n;
    return 1;
}
static lv_t lexpr(int lvl);

static lv_t lnum(void) {
    lsp();
    const char *s = lp;
    float sign = 1, v = 0;
    int any = 0;
    if (lp < le && (*lp == '+' || *lp == '-')) { if (*lp == '-') sign = -1; lp++; }
    while (lp < le && digit((unsigned char)*lp)) { v = v * 10 + (float)(*lp - '0'); lp++; any = 1; }
    if (lp < le && *lp == '.') {
        float f = 0.1f;
        for (lp++; lp < le && digit((unsigned char)*lp); lp++, f *= 0.1f) { v += (float)(*lp - '0') * f; any = 1; }
    }
    if (!any) { lp = s; return lerr(); }
    if (lp + 1 < le && (*lp == 'e' || *lp == 'E') && (digit((unsigned char)lp[1]) || ((lp[1] == '-' || lp[1] == '+') && lp + 2 < le && digit((unsigned char)lp[2])))) {
        lp++;
        int es = 1, e = 0;
        if (*lp == '-' || *lp == '+') { es = *lp == '-' ? -1 : 1; lp++; }
        while (lp < le && digit((unsigned char)*lp)) { if (e < 30) e = e * 10 + (*lp - '0'); lp++; }
        while (e--) v = es > 0 ? v * 10 : v / 10;
    }
    v *= sign;
    lv_t r = {0, 0, 0, 1};
    char u[8];
    int k = 0;
    if (lp < le && *lp == '%') { lp++; r.pc = v; return r; }
    while (lp < le && k < 7 && ((*lp >= 'a' && *lp <= 'z') || (*lp >= 'A' && *lp <= 'Z'))) u[k++] = (char)low((unsigned char)*lp++);
    u[k] = 0;
    if (!k) { r.px = v; r.num = 1; return r; }
    static const struct { char u[5]; float k; } units[] = {
        {"px", 1}, {"em", 16}, {"rem", 16}, {"pt", 4.0f / 3}, {"pc", 16}, {"in", 96}, {"cm", 37.795f}, {"mm", 3.7795f},
        {"q", 0.9449f}, {"vw", VIEW_W / 100.0f}, {"vh", VIEW_H / 100.0f}, {"vmin", VIEW_H / 100.0f}, {"vmax", VIEW_W / 100.0f},
        {"svw", VIEW_W / 100.0f}, {"lvw", VIEW_W / 100.0f}, {"dvw", VIEW_W / 100.0f}, {"svh", VIEW_H / 100.0f},
        {"lvh", VIEW_H / 100.0f}, {"dvh", VIEW_H / 100.0f}, {"ch", 8}, {"ex", 8}, {"lh", 24}, {"rlh", 24}};
    for (unsigned i = 0; i < sizeof(units) / sizeof(units[0]); i++)
        if (!strcmp(u, units[i].u)) { r.px = v * units[i].k; return r; }
    return lerr();
}

/* Значение в точках (проценты — от lbase; не от чего — ошибка). */
static int lpx(lv_t v, float *px) {
    if (v.pc != 0 && lbase < 0) return 0;
    *px = v.px + (v.pc != 0 ? v.pc * (float)lbase / 100 : 0);
    return 1;
}

static lv_t lfactor(int lvl) {
    lsp();
    if (lvl > 8 || lp >= le) return lerr();
    int fn = 0;                                       /* 1 calc/скобки, 2 min, 3 max, 4 clamp */
    if (*lp == '(') { lp++; fn = 1; }
    else if (lword("calc(") || lword("-webkit-calc(")) fn = 1;
    else if (lword("min(")) fn = 2;
    else if (lword("max(")) fn = 3;
    else if (lword("clamp(")) fn = 4;
    if (!fn) return lnum();
    lv_t a[3];
    int k = 0;
    for (;;) {
        lv_t v = lexpr(lvl + 1);
        if (!v.ok) return lerr();
        if (k < 3) a[k++] = v;
        lsp();
        if (lp < le && *lp == ',') { lp++; continue; }
        break;
    }
    lsp();
    if (lp >= le || *lp != ')') return lerr();
    lp++;
    if (fn == 1) return a[0];
    float x[3];
    for (int i = 0; i < k; i++) if (!lpx(a[i], &x[i])) return lerr();
    lv_t r = {0, 0, a[0].num, 1};
    if (fn == 4) {
        if (k < 3) return lerr();
        float v = x[1] < x[2] ? x[1] : x[2];
        r.px = v > x[0] ? v : x[0];
        return r;
    }
    r.px = x[0];
    for (int i = 1; i < k; i++) r.px = fn == 2 ? (x[i] < r.px ? x[i] : r.px) : (x[i] > r.px ? x[i] : r.px);
    return r;
}

static lv_t lterm(int lvl) {
    lv_t v = lfactor(lvl);
    for (;;) {
        lsp();
        if (!v.ok || lp >= le || (*lp != '*' && *lp != '/')) return v;
        char op = *lp++;
        lv_t w = lfactor(lvl);
        if (!w.ok) return lerr();
        if (op == '*') {
            if (w.num) { v.px *= w.px; v.pc *= w.px; }
            else if (v.num) { float k = v.px; v = w; v.px *= k; v.pc *= k; }
            else return lerr();
        } else {
            if (!w.num || w.px == 0) return lerr();
            v.px /= w.px;
            v.pc /= w.px;
        }
    }
}

static lv_t lexpr(int lvl) {
    lv_t v = lterm(lvl);
    for (;;) {
        lsp();
        if (!v.ok || lp >= le || (*lp != '+' && *lp != '-')) return v;
        float s = *lp++ == '-' ? -1.0f : 1.0f;
        lv_t w = lterm(lvl);
        if (!w.ok) return lerr();
        v.px += s * w.px;
        v.pc += s * w.pc;
        v.num = v.num && w.num;
    }
}

int css_len(const char *v, int base, int *px) {
    while (sp((unsigned char)*v)) v++;
    if (!strncmp(v, "auto", 4)) return -1;
    lp = v;
    le = v + strlen(v);
    lbase = base;
    lv_t r = lexpr(0);
    float x;
    if (!r.ok || !lpx(r, &x)) return 0;
    if (x > 100000) x = 100000;
    if (x < -100000) x = -100000;
    *px = (int)(x + (x >= 0 ? 0.5f : -0.5f));
    return 1;
}

/* --- цвета ---------------------------------------------------------------------------------------- */

static const struct { const char *name; uint32_t rgb; } named[] = {
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4},
    {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"black", 0x000000}, {"blanchedalmond", 0xFFEBCD},
    {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A}, {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0},
    {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E}, {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED},
    {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C}, {"cyan", 0x00FFFF}, {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B},
    {"darkgoldenrod", 0xB8860B}, {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9},
    {"darkkhaki", 0xBDB76B}, {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00},
    {"darkorchid", 0x9932CC}, {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F},
    {"darkslateblue", 0x483D8B}, {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1},
    {"darkviolet", 0x9400D3}, {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969},
    {"dimgrey", 0x696969}, {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0},
    {"forestgreen", 0x228B22}, {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF},
    {"gold", 0xFFD700}, {"goldenrod", 0xDAA520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xADFF2F},
    {"grey", 0x808080}, {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C}, {"indigo", 0x4B0082},
    {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA}, {"lavenderblush", 0xFFF0F5},
    {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6}, {"lightcoral", 0xF08080},
    {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3}, {"lightgreen", 0x90EE90},
    {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A}, {"lightseagreen", 0x20B2AA},
    {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32},
    {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66CDAA},
    {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371},
    {"mediumslateblue", 0x7B68EE}, {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC},
    {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1},
    {"moccasin", 0xFFE4B5}, {"navajowhite", 0xFFDEAD}, {"navy", 0x000080}, {"oldlace", 0xFDF5E6}, {"olive", 0x808000},
    {"olivedrab", 0x6B8E23}, {"orange", 0xFFA500}, {"orangered", 0xFF4500}, {"orchid", 0xDA70D6},
    {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE}, {"palevioletred", 0xDB7093},
    {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F}, {"pink", 0xFFC0CB}, {"plum", 0xDDA0DD},
    {"powderblue", 0xB0E0E6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399}, {"red", 0xFF0000},
    {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1}, {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072},
    {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57}, {"seashell", 0xFFF5EE}, {"sienna", 0xA0522D},
    {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB}, {"slateblue", 0x6A5ACD}, {"slategray", 0x708090},
    {"slategrey", 0x708090}, {"snow", 0xFFFAFA}, {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4}, {"tan", 0xD2B48C},
    {"teal", 0x008080}, {"thistle", 0xD8BFD8}, {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE},
    {"wheat", 0xF5DEB3}, {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00}, {"yellowgreen", 0x9ACD32},
};

/* Числа в скобках rgb(…)/hsl(…): через запятые, пробелы или «/»; *pct — с «%»; углы (deg, turn,
 * rad, grad) — в градусы. Возвращает их число; −1 — не числа. */
static int cargs(const char *p, float *a, int *pct, int max) {
    int k = 0;
    while (*p && *p != ')' && k < max) {
        while (*p == ' ' || *p == ',' || *p == '/') p++;
        if (!*p || *p == ')') break;
        if (!strncmp(p, "none", 4)) { a[k] = 0; pct[k++] = 0; p += 4; continue; }
        float s = 1, v = 0;
        int any = 0;
        if (*p == '-' || *p == '+') { if (*p == '-') s = -1; p++; }
        while (digit((unsigned char)*p)) { v = v * 10 + (float)(*p++ - '0'); any = 1; }
        if (*p == '.') { float f = 0.1f; for (p++; digit((unsigned char)*p); p++, f *= 0.1f) { v += (float)(*p - '0') * f; any = 1; } }
        if (!any) return -1;
        v *= s;
        pct[k] = 0;
        if (*p == '%') { pct[k] = 1; p++; }
        else if (!strncmp(p, "deg", 3)) p += 3;
        else if (!strncmp(p, "turn", 4)) { v *= 360; p += 4; }
        else if (!strncmp(p, "grad", 4)) { v *= 0.9f; p += 4; }
        else if (!strncmp(p, "rad", 3)) { v *= 57.29578f; p += 3; }
        a[k++] = v;
    }
    return k;
}

static int clamp255(float v) { return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5f); }

static uint32_t hsl_rgb(float h, float s, float l) {
    if (h > 1e6f || h < -1e6f) h = 0;
    while (h < 0) h += 360;
    while (h >= 360) h -= 360;
    s = s < 0 ? 0 : s > 100 ? 1 : s / 100;
    l = l < 0 ? 0 : l > 100 ? 1 : l / 100;
    float t = 2 * l - 1, c = (1 - (t < 0 ? -t : t)) * s, hp = h / 60;
    float m2 = hp - 2 * (float)(int)(hp / 2) - 1, x = c * (1 - (m2 < 0 ? -m2 : m2)), m = l - c / 2;
    float r = 0, g = 0, b = 0;
    int seg = (int)hp;
    if (seg == 0) { r = c; g = x; } else if (seg == 1) { r = x; g = c; } else if (seg == 2) { g = c; b = x; }
    else if (seg == 3) { g = x; b = c; } else if (seg == 4) { r = x; b = c; } else { r = c; b = x; }
    return (uint32_t)clamp255((r + m) * 255) << 16 | (uint32_t)clamp255((g + m) * 255) << 8 | (uint32_t)clamp255((b + m) * 255);
}

int css_color(const char *v, uint32_t cur, uint32_t *rgb, int *alpha) {
    while (sp((unsigned char)*v)) v++;
    *alpha = 255;
    if (*v == '#') {
        int d[8], n = 0;
        while (n < 8 && hexv((unsigned char)v[1 + n]) >= 0) { d[n] = hexv((unsigned char)v[1 + n]); n++; }
        if (hexv((unsigned char)v[1 + n]) >= 0 || ident_ch((unsigned char)v[1 + n])) return 0;
        if (n == 3 || n == 4) {
            *rgb = (uint32_t)(d[0] * 0x110000 | d[1] * 0x1100 | d[2] * 0x11);
            if (n == 4) *alpha = d[3] * 17;
            return 1;
        }
        if (n == 6 || n == 8) {
            *rgb = (uint32_t)(d[0] << 20 | d[1] << 16 | d[2] << 12 | d[3] << 8 | d[4] << 4 | d[5]);
            if (n == 8) *alpha = d[6] * 16 + d[7];
            return 1;
        }
        return 0;
    }
    char w[24];
    size_t k = 0;
    while (v[k] && v[k] != '(' && !sp((unsigned char)v[k]) && v[k] != ';' && v[k] != ',' && k < sizeof(w) - 1) {
        w[k] = (char)low((unsigned char)v[k]);
        k++;
    }
    w[k] = 0;
    if (v[k] == '(') {
        float a[4];
        int pc[4];
        int n = cargs(v + k + 1, a, pc, 4);
        if (n < 3) return 0;
        float al = n > 3 ? (pc[3] ? a[3] * 2.55f : a[3] * 255) : 255;
        *alpha = clamp255(al);
        if (!strcmp(w, "rgb") || !strcmp(w, "rgba")) {
            int c[3];
            for (int i = 0; i < 3; i++) c[i] = clamp255(pc[i] ? a[i] * 2.55f : a[i]);
            *rgb = (uint32_t)(c[0] << 16 | c[1] << 8 | c[2]);
            return 1;
        }
        if (!strcmp(w, "hsl") || !strcmp(w, "hsla")) { *rgb = hsl_rgb(a[0], a[1], a[2]); return 1; }
        return 0;
    }
    if (!strcmp(w, "transparent")) { *rgb = 0; *alpha = 0; return 1; }
    if (!strcmp(w, "currentcolor")) { *rgb = cur; return 1; }
    for (unsigned i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if (!strcmp(w, named[i].name)) { *rgb = named[i].rgb; return 1; }
    return 0;
}

/* --- @media ------------------------------------------------------------------------------------- */

/* Длина в условии (px, em…); 1 — да. */
static int mq_len(const char *s, size_t n, float *px) {
    char b[32];
    while (n && sp((unsigned char)*s)) { s++; n--; }
    while (n && sp((unsigned char)s[n - 1])) n--;
    if (!n || n >= sizeof(b)) return 0;
    memcpy(b, s, n);
    b[n] = 0;
    lp = b; le = b + n; lbase = -1;
    lv_t v = lexpr(0);
    if (!v.ok || v.pc != 0) return 0;
    *px = v.px;
    return 1;
}

/* Сравнение «величина экрана op значение» (op: < <= > >= =). */
static int mq_cmp(float have, const char *op, float want) {
    if (!strcmp(op, "<")) return have < want;
    if (!strcmp(op, "<=")) return have <= want;
    if (!strcmp(op, ">")) return have > want;
    if (!strcmp(op, ">=")) return have >= want;
    return have == want;
}

/* Одно условие в скобках: «min-width: 768px», «prefers-color-scheme: dark», «width >= 600px». */
static int mq_feature(const char *f, size_t n) {
    char b[96];
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < sizeof(b); i++) b[k++] = (char)low((unsigned char)f[i]);
    b[k] = 0;
    char *colon = strchr(b, ':');
    if (!colon) {
        /* диапазон: «width >= 600px», «400px <= width <= 700px» */
        char *parts[3], ops[2][3];
        int np = 0, no = 0;
        char *p = b;
        parts[np++] = p;
        while (*p && np < 3) {
            if (*p == '<' || *p == '>' || *p == '=') {
                char *o = p;
                int l = (p[1] == '=') ? 2 : 1;
                if (no < 2) { memcpy(ops[no], o, (size_t)l); ops[no][l] = 0; no++; }
                *p = 0;
                p += l;
                parts[np++] = p;
                continue;
            }
            p++;
        }
        if (no == 0) {                                /* одно слово: (color), (hover) — есть; (grid) — нет */
            char *w = b;
            while (sp((unsigned char)*w)) w++;
            return !strncmp(w, "color", 5) || !strncmp(w, "hover", 5);
        }
        int ok = 1;
        for (int i = 0; i + 1 < np; i++) {
            char *l = parts[i], *r = parts[i + 1];
            while (sp((unsigned char)*l)) l++;
            while (sp((unsigned char)*r)) r++;
            float v;
            int lw = !strncmp(l, "width", 5), lh = !strncmp(l, "height", 6), rw = !strncmp(r, "width", 5), rh = !strncmp(r, "height", 6);
            if ((lw || lh) && mq_len(r, strlen(r), &v)) ok &= mq_cmp(lw ? VIEW_W : VIEW_H, ops[i], v);
            else if ((rw || rh) && mq_len(l, strlen(l), &v)) {
                /* «600px <= width» — то же, что «width >= 600px» */
                const char *o = ops[i];
                const char *rev = !strcmp(o, "<") ? ">" : !strcmp(o, "<=") ? ">=" : !strcmp(o, ">") ? "<" : !strcmp(o, ">=") ? "<=" : "=";
                ok &= mq_cmp(rw ? VIEW_W : VIEW_H, rev, v);
            } else return 0;
        }
        return ok;
    }
    *colon = 0;
    char *name = b, *val = colon + 1;
    while (sp((unsigned char)*name)) name++;
    for (char *e = name + strlen(name); e > name && sp((unsigned char)e[-1]);) *--e = 0;
    while (sp((unsigned char)*val)) val++;
    for (char *e = val + strlen(val); e > val && sp((unsigned char)e[-1]);) *--e = 0;
    float v;
    if (!strcmp(name, "min-width") || !strcmp(name, "min-device-width")) return mq_len(val, strlen(val), &v) && VIEW_W >= v;
    if (!strcmp(name, "max-width") || !strcmp(name, "max-device-width")) return mq_len(val, strlen(val), &v) && VIEW_W <= v;
    if (!strcmp(name, "width") || !strcmp(name, "device-width")) return mq_len(val, strlen(val), &v) && VIEW_W == v;
    if (!strcmp(name, "min-height") || !strcmp(name, "min-device-height")) return mq_len(val, strlen(val), &v) && VIEW_H >= v;
    if (!strcmp(name, "max-height") || !strcmp(name, "max-device-height")) return mq_len(val, strlen(val), &v) && VIEW_H <= v;
    if (!strcmp(name, "height") || !strcmp(name, "device-height")) return mq_len(val, strlen(val), &v) && VIEW_H == v;
    if (!strcmp(name, "orientation")) return !strcmp(val, "landscape");
    if (!strcmp(name, "prefers-color-scheme")) return !strcmp(val, "light");
    if (!strcmp(name, "prefers-reduced-motion") || !strcmp(name, "prefers-contrast") || !strcmp(name, "prefers-reduced-transparency"))
        return !strcmp(val, "no-preference");
    if (!strcmp(name, "hover") || !strcmp(name, "any-hover")) return !strcmp(val, "hover");
    if (!strcmp(name, "pointer") || !strcmp(name, "any-pointer")) return !strcmp(val, "fine");
    if (!strcmp(name, "display-mode")) return !strcmp(val, "browser");
    if (!strcmp(name, "forced-colors")) return !strcmp(val, "none");
    if (!strcmp(name, "scripting")) return !strcmp(val, "none");
    if (!strcmp(name, "update")) return !strcmp(val, "fast");
    if (!strcmp(name, "color") || !strcmp(name, "min-color")) return 1;
    if (strstr(name, "device-pixel-ratio") || !strcmp(name, "min-resolution") || !strcmp(name, "max-resolution") ||
        !strcmp(name, "resolution")) {
        /* экран — 1 точка на точку CSS (96 dpi) */
        float x = 0;
        const char *p = val;
        while (digit((unsigned char)*p)) x = x * 10 + (float)(*p++ - '0');
        if (*p == '.') { float f = 0.1f; for (p++; digit((unsigned char)*p); p++, f *= 0.1f) x += (float)(*p - '0') * f; }
        if (!strncmp(p, "dpi", 3)) x /= 96;
        else if (!strncmp(p, "dpcm", 4)) x /= 37.8f;
        int mn = !strncmp(name, "min", 3) || strstr(name, "min-") != NULL, mx = !strncmp(name, "max", 3) || strstr(name, "max-") != NULL;
        return mn ? 1 >= x : mx ? 1 <= x : x == 1;
    }
    if (!strcmp(name, "min-aspect-ratio") || !strcmp(name, "max-aspect-ratio") || !strcmp(name, "aspect-ratio")) {
        int a = 0, c = 0;
        const char *p = val;
        while (digit((unsigned char)*p)) a = a * 10 + (*p++ - '0');
        while (*p == ' ' || *p == '/') p++;
        while (digit((unsigned char)*p)) c = c * 10 + (*p++ - '0');
        if (!a || !c) return 0;
        long have = (long)VIEW_W * c, want = (long)VIEW_H * a;   /* сравнить 1024/768 и a/c */
        return name[1] == 'i' ? have >= want : name[1] == 'a' ? have <= want : have == want;
    }
    return 0;
}

/* Один запрос: [not|only] [тип] [and (условие)]…; «or» между условиями — хотя бы одно. */
static int mq_one(const char *q, size_t n) {
    int neg = 0, ok = 1, or_mode = 0, any = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && sp((unsigned char)q[i])) i++;
        if (i >= n) break;
        if (q[i] == '(') {
            int d = 0;
            size_t j = i;
            for (; j < n; j++) {
                if (q[j] == '(') d++;
                else if (q[j] == ')' && --d == 0) break;
            }
            int f = mq_feature(q + i + 1, (j > i + 1 ? j : i + 1) - (i + 1));
            ok = or_mode ? (ok || f) : (any ? ok && f : f);
            any = 1;
            or_mode = 0;
            i = j + 1;
            continue;
        }
        char w[16];
        size_t k = 0;
        while (i < n && !sp((unsigned char)q[i]) && q[i] != '(' && k < sizeof(w) - 1) w[k++] = (char)low((unsigned char)q[i++]);
        w[k] = 0;
        while (i < n && !sp((unsigned char)q[i]) && q[i] != '(') i++;
        if (!strcmp(w, "not")) neg = 1;
        else if (!strcmp(w, "only") || !strcmp(w, "and")) continue;
        else if (!strcmp(w, "or")) or_mode = 1;
        else { ok = (any ? ok : 1) && (!strcmp(w, "all") || !strcmp(w, "screen")); any = 1; }
    }
    return neg ? !ok : ok;
}

int css_media(const char *q, size_t n) {
    while (n && sp((unsigned char)*q)) { q++; n--; }
    while (n && sp((unsigned char)q[n - 1])) n--;
    if (!n) return 1;
    size_t p = 0;
    while (p < n) {
        size_t e = p;
        int d = 0;
        while (e < n && (q[e] != ',' || d)) { if (q[e] == '(') d++; else if (q[e] == ')' && d) d--; e++; }
        if (mq_one(q + p, e - p)) return 1;
        p = e + 1;
    }
    return 0;
}
