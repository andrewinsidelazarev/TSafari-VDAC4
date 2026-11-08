/* Адреса (как устроен адрес — в url.h). Превращение ссылок в полные адреса — по RFC 3986
 * (раздел 5.2) в упрощённом виде: только схемы http и https; фрагмент «#…» отбрасывается
 * (переход к месту внутри страницы — позже). Все функции пишут в буфер out не больше cap
 * байт и возвращают 0, если не поместилось, — чтобы длинный адрес не вылез за массив. */
#include <string.h>
#include "url.h"

/* Латинская буква → строчная (в ASCII строчные на 32 дальше заглавных). */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
/* Строка s начинается с p без учёта регистра (p — строчными). */
static int starts_ci(const char *s, const char *p) {
    for (; *p; s++, p++)
        if (lower((unsigned char)*s) != *p) return 0;
    return 1;
}
/* Дописать len байт s в конец out (там уже *n байт) и закончить нулём; 0 — нет места. */
static int put(char *out, size_t cap, size_t *n, const char *s, size_t len) {
    if (*n + len + 1 > cap) return 0;
    memcpy(out + *n, s, len);
    *n += len;
    out[*n] = 0;
    return 1;
}

int url_is_internal(const char *u) { return starts_ci(u, "tsafari:"); }

/* Поиск — DuckDuckGo HTML (работает без JS и GET-запросом); поисковик в настройках — позже.
 * Слова кладутся в адрес после «?q=». В адресе можно не всё: пробел записывается «+», а
 * любой байт, кроме латиницы, цифр и «-._», — «процентной записью» %XX (XX — байт в
 * шестнадцатеричном виде). Русская буква в UTF-8 — два байта, значит два %XX. */
static int search(const char *q, size_t len, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    if (!put(out, cap, &n, "https://html.duckduckgo.com/html/?q=", 36)) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)q[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_') {
            if (!put(out, cap, &n, (const char *)&c, 1)) return 0;
        } else if (c == ' ') {
            if (!put(out, cap, &n, "+", 1)) return 0;
        } else {
            char e[3] = {'%', hex[c >> 4], hex[c & 15]};
            if (!put(out, cap, &n, e, 3)) return 0;
        }
    }
    return 1;
}

/* Пробелы по краям отбросить; со схемой — как есть; с «://» и чужой схемой — не адрес;
 * с пробелом или без точки — поиск («погода», «zx spectrum»); иначе — https:// + набранное. */
int url_from_input(const char *t, char *out, size_t cap) {
    while (*t == ' ') t++;
    size_t len = strlen(t);
    while (len && t[len - 1] == ' ') len--;
    if (!len || len >= cap) return 0;
    if (starts_ci(t, "tsafari:") || starts_ci(t, "http://") || starts_ci(t, "https://")) {
        memcpy(out, t, len); out[len] = 0;
        return 1;
    }
    int dot = 0, space = 0;
    for (size_t i = 0; i < len; i++) {
        if (t[i] == '.') dot = 1;
        if (t[i] == ' ') space = 1;
        if (t[i] == ':' && i + 2 < len && t[i + 1] == '/' && t[i + 2] == '/') return 0;   /* ftp:// и т. п. */
    }
    if (space || !dot) return search(t, len, out, cap);
    size_t n = 0;
    return put(out, cap, &n, "https://", 8) && put(out, cap, &n, t, len);
}

/* Убрать «.» и «..» из пути (RFC 3986, 5.2.4) на месте; path — от первого «/».
 * «/./» — «этот же каталог», просто выбрасываем; «/../» — «на каталог выше»: стираем
 * последний уже записанный каталог. Пример: /a/b/../c/./d → /a/c/d. Читаем (in) и пишем (out)
 * в одном массиве: результат никогда не длиннее исходного, поэтому out не обгоняет in. */
static void dot_segments(char *path) {
    char *in = path, *out = path;
    while (*in) {
        if (in[0] == '/' && in[1] == '.' && (in[2] == '/' || in[2] == 0)) {
            in += 2;
            if (!*in) *out++ = '/';
            continue;
        }
        if (in[0] == '/' && in[1] == '.' && in[2] == '.' && (in[3] == '/' || in[3] == 0)) {
            in += 3;
            while (out > path && *--out != '/') {}
            if (!*in) *out++ = '/';
            continue;
        }
        do *out++ = *in++; while (*in && *in != '/');
    }
    if (out == path) *out++ = '/';
    *out = 0;
}

/* Путь с «?запросом» с p: путь начинается с «/», точки убраны, запрос на месте. room — места в
 * буфере с p. 0 — не поместилось. */
static int normalize_path(char *p, size_t room) {
    if (*p != '/') {                                    /* пусто или сразу «?» — путь «/» */
        size_t len = strlen(p);
        if (len + 2 > room) return 0;
        memmove(p + 1, p, len + 1);
        p[0] = '/';
    }
    size_t q = strcspn(p, "?");
    char save = p[q];
    char *query = p + q + (save ? 1 : 0);
    p[q] = 0;
    dot_segments(p);                                    /* короче не становится длиннее */
    if (save) {
        size_t m = strlen(p);
        p[m] = '?';
        memmove(p + m + 1, query, strlen(query) + 1);
    }
    return 1;
}

/* Виды ссылок, от самых полных к самым коротким:
 *   https://host/path  — полный адрес: берём как есть;
 *   //host/path        — тот же способ (схема), другой сервер;
 *   (пусто)            — та же страница;
 *   ?query             — та же страница с другими параметрами;
 *   /path              — тот же сервер, путь от корня;
 *   path               — тот же сервер и каталог текущей страницы.
 * В конце путь чистится от «.» и «..». */
int url_resolve(const char *base, const char *ref, char *out, size_t cap) {
    while (*ref == ' ' || *ref == '\t' || *ref == '\n' || *ref == '\r') ref++;
    size_t rlen = strcspn(ref, "#");
    while (rlen && (ref[rlen - 1] == ' ' || ref[rlen - 1] == '\n' || ref[rlen - 1] == '\r')) rlen--;
    size_t n = 0;
    /* схема у ссылки: буквы, затем буквы, цифры, «+-.», а за ними «:» */
    size_t k = 0;
    while (k < rlen && ((ref[k] >= 'a' && ref[k] <= 'z') || (ref[k] >= 'A' && ref[k] <= 'Z') ||
                        (k && ((ref[k] >= '0' && ref[k] <= '9') || ref[k] == '+' || ref[k] == '-' || ref[k] == '.'))))
        k++;
    if (k && k < rlen && ref[k] == ':') {
        if (url_is_internal(ref)) return put(out, cap, &n, ref, rlen);       /* свои страницы браузера tsafari:… */
        if (!starts_ci(ref, "http://") && !starts_ci(ref, "https://")) return 0;   /* mailto:, javascript: … */
        if (!put(out, cap, &n, ref, rlen)) return 0;
        char *p = strstr(out, "://") + 3;
        p += strcspn(p, "/?");
        return normalize_path(p, cap - (size_t)(p - out));
    }
    /* части базы: схема://хост | путь | ?запрос */
    const char *auth = strstr(base, "://");
    if (!auth) return 0;
    auth += 3;
    const char *path = auth + strcspn(auth, "/?#");
    size_t scheme_len = (size_t)(auth - base);
    size_t blen = strcspn(base, "#");
    if (rlen && ref[0] == '/' && rlen > 1 && ref[1] == '/') {                 /* //хост/… */
        if (!put(out, cap, &n, base, scheme_len - 2) || !put(out, cap, &n, ref, rlen)) return 0;
        return 1;
    }
    if (!put(out, cap, &n, base, (size_t)(path - base))) return 0;            /* схема://хост */
    size_t path_at = n;
    if (!rlen) {                                                               /* та же страница */
        if (!put(out, cap, &n, path, blen - (size_t)(path - base))) return 0;
        if (n == path_at && !put(out, cap, &n, "/", 1)) return 0;
        return 1;
    }
    if (ref[0] == '?') {
        size_t plen = strcspn(path, "?#");
        if (!put(out, cap, &n, plen ? path : "/", plen ? plen : 1) || !put(out, cap, &n, ref, rlen)) return 0;
        return 1;
    }
    if (ref[0] != '/') {                                                       /* относительно каталога */
        size_t plen = strcspn(path, "?#"), dir = 0;
        for (size_t i = 0; i < plen; i++) if (path[i] == '/') dir = i + 1;
        if (!dir) { if (!put(out, cap, &n, "/", 1)) return 0; }
        else if (!put(out, cap, &n, path, dir)) return 0;
    }
    if (!put(out, cap, &n, ref, rlen)) return 0;
    return normalize_path(out + path_at, cap - path_at);
}
