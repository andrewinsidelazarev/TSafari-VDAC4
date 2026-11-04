/* Кодировки страниц (что это — в charset.h). Как узнать кодировку — по порядку, как у
 * больших браузеров:
 *   1) сервер сказал её в заголовке ответа: «Content-Type: text/html; charset=windows-1251»;
 *   2) файл начинается с «метки порядка байтов» (BOM) EF BB BF — это UTF-8;
 *   3) страница сказала её сама в начале: <meta charset="koi8-r">;
 *   4) никто не сказал — проверяем, похож ли текст на правильный UTF-8. */
#include <string.h>
#include "charset.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
/* n байт a равны строке b без учёта регистра. */
static int eq_ci(const char *a, size_t n, const char *b) {
    if (strlen(b) != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (lower((unsigned char)a[i]) != b[i]) return 0;
    return 1;
}

/* У одной кодировки бывает несколько имён — принимаем все, что встречаются на сайтах.
 * Латиница-1 (iso-8859-1) читается как Windows-1252: так велит стандарт HTML. */
int charset_id(const char *name, size_t len) {
    while (len && (*name == ' ' || *name == '"' || *name == '\'')) { name++; len--; }
    while (len && (name[len - 1] == ' ' || name[len - 1] == '"' || name[len - 1] == '\'' || name[len - 1] == ';')) len--;
    if (eq_ci(name, len, "utf-8") || eq_ci(name, len, "utf8")) return CS_UTF8;
    if (eq_ci(name, len, "windows-1251") || eq_ci(name, len, "cp1251") || eq_ci(name, len, "x-cp1251")) return CS_CP1251;
    if (eq_ci(name, len, "koi8-r") || eq_ci(name, len, "koi8r") || eq_ci(name, len, "koi8-u")) return CS_KOI8R;
    if (eq_ci(name, len, "ibm866") || eq_ci(name, len, "cp866") || eq_ci(name, len, "866")) return CS_CP866;
    if (eq_ci(name, len, "iso-8859-1") || eq_ci(name, len, "windows-1252") || eq_ci(name, len, "latin1") ||
        eq_ci(name, len, "us-ascii") || eq_ci(name, len, "ascii")) return CS_CP1252;
    return -1;
}

/* «charset=…» в строке s (len байт): кодировка или −1. */
static int from_param(const char *s, size_t len) {
    for (size_t i = 0; i + 8 <= len; i++) {
        if (eq_ci(s + i, 8, "charset=")) {
            size_t a = i + 8, b = a;
            while (b < len && s[b] != ';' && s[b] != '"' && s[b] != '\'' && s[b] != '>' && s[b] != ' ' && s[b] != '/') b++;
            if (a < len && (s[a] == '"' || s[a] == '\'')) { a++; b = a; while (b < len && s[b] != '"' && s[b] != '\'') b++; }
            return charset_id(s + a, b - a);
        }
    }
    return -1;
}

int charset_detect(const char *content_type, const uint8_t *body, size_t len) {
    int cs = content_type ? from_param(content_type, strlen(content_type)) : -1;
    if (cs >= 0) return cs;
    if (len >= 3 && body[0] == 0xEF && body[1] == 0xBB && body[2] == 0xBF) return CS_UTF8;
    size_t scan = len < 4096 ? len : 4096;
    cs = from_param((const char *)body, scan);                /* <meta charset> и http-equiv */
    if (cs >= 0) return cs;
    /* без объявления: правильный ли UTF-8? Считаем в первых 64 КБ правильные многобайтовые
     * буквы и ошибки. Ошибок больше — скорее всего русский сайт в Windows-1251, не сказавший
     * кодировку (байты кириллицы 1251 почти никогда не складываются в правильный UTF-8). */
    size_t bad = 0, multi = 0, i = 0, look = len < 65536 ? len : 65536;
    while (i < look) {
        uint8_t c = body[i];
        if (c < 0x80) { i++; continue; }
        unsigned n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
        int ok = n && i + n < look;
        for (unsigned k = 1; ok && k <= n; k++) ok = (body[i + k] & 0xC0) == 0x80;
        if (ok) { multi++; i += n + 1; } else { bad++; i++; }
    }
    return bad > multi ? CS_CP1251 : CS_UTF8;
}

/* Байт меньше 0x80 — тот же код; больше — код из таблицы; дальше — запись кода в UTF-8
 * (как в ui_url, ui.c). */
size_t charset_to_utf8(int cs, const uint8_t *in, size_t n, char *out) {
    if (cs == CS_UTF8) { memcpy(out, in, n); return n; }
    const uint16_t *t = cs == CS_CP1251 ? charset_cp1251 : cs == CS_KOI8R ? charset_koi8r :
                        cs == CS_CP866 ? charset_cp866 : charset_cp1252;
    char *o = out;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = in[i] < 0x80 ? in[i] : t[in[i] - 0x80];
        if (c < 0x80) *o++ = (char)c;
        else if (c < 0x800) { *o++ = (char)(0xC0 | c >> 6); *o++ = (char)(0x80 | (c & 63)); }
        else { *o++ = (char)(0xE0 | c >> 12); *o++ = (char)(0x80 | ((c >> 6) & 63)); *o++ = (char)(0x80 | (c & 63)); }
    }
    return (size_t)(o - out);
}
