/* Шрифт: распаковка атласа, поиск глифа по коду, композиция диакритик, UTF-8.
 * Правила — как у TXTVIEW2: строгий UTF-8, знаки 0300–036F садятся на предыдущую букву, если
 * готовая буква есть в шрифте; иначе — отдельной клеткой на пунктирном кружке. */
#include "font.h"
#include "ts.h"
#include "miniz_tinfl.h"

static uint8_t *atlas;                   /* распакованный атлас в PSRAM, NULL — ещё не распакован */

/* Распаковка: tinfl — маленький распаковщик zlib из библиотеки miniz (third_party/miniz).
 * Ему нужна своя рабочая структура (~11 КБ) — берём её на время и сразу отдаём. */
int font_init(void) {
    if (atlas) return 1;
    uint8_t *out = ts_alloc(FONT_L4_BYTES);
    tinfl_decompressor *inflator = ts_alloc(sizeof(tinfl_decompressor));
    if (!out || !inflator) return 0;
    tinfl_init(inflator);
    size_t in_bytes = FONT_L4_ZLIB_BYTES, out_bytes = FONT_L4_BYTES;
    tinfl_status st = tinfl_decompress(inflator, font_l4_zlib, &in_bytes, out, out, &out_bytes,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    ts_core->free(inflator);
    if (st != TINFL_STATUS_DONE || out_bytes != FONT_L4_BYTES) {
        ts_log("tsafari: font atlas inflate failed (%d, %u bytes)", (int)st, (unsigned)out_bytes);
        ts_core->free(out);
        return 0;
    }
    atlas = out;
    return 1;
}

/* Глиф номер N лежит в атласе с байта N × FONT_GLYPH_BYTES (все клетки одного размера). */
const uint8_t *font_glyph(unsigned glyph) {
    if (glyph >= FONT_GLYPHS) glyph = FONT_REPLACEMENT;
    return atlas + (size_t)glyph * FONT_GLYPH_BYTES;
}

/* Поиск глифа по коду. Частые коды (латиница, кириллица, греческий…) — сразу из таблицы
 * font_fast. Остальные — «двоичным поиском» в отсортированной font_codes: смотрим в середину,
 * отбрасываем ту половину, где кода точно нет, и так далее. Из 2278 кодов нужный находится
 * за 12 шагов. Кодов больше 0xFFFF (эмодзи и редкие письменности) в этом шрифте нет. */
unsigned font_lookup(uint32_t code) {
    if (code < FONT_FAST_LIMIT) return font_fast[code];
    if (code > 0xFFFF) return FONT_REPLACEMENT;
    unsigned lo = 0, hi = FONT_CODES;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (font_codes[mid] < code) lo = mid + 1; else hi = mid;
    }
    return lo < FONT_CODES && font_codes[lo] == code ? lo : FONT_REPLACEMENT;
}

/* Тоже двоичный поиск: ключ — пара (глиф буквы, код знака), склеенная в одно 32-битное число. */
unsigned font_combine(unsigned base, uint32_t mark) {
    unsigned lo = 0, hi = FONT_COMPOSE;
    uint32_t key = ((uint32_t)base << 16) | mark;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        uint32_t k = ((uint32_t)font_compose[mid * 3] << 16) | font_compose[mid * 3 + 1];
        if (k < key) lo = mid + 1; else hi = mid;
    }
    if (lo < FONT_COMPOSE && font_compose[lo * 3] == base && font_compose[lo * 3 + 1] == mark)
        return font_compose[lo * 3 + 2];
    return FONT_NONE;
}

/* Как устроен UTF-8: по старшим битам первого байта видно, сколько байтов у символа.
 *   0xxxxxxx                    — 1 байт (коды до 0x7F, латиница);
 *   110xxxxx 10xxxxxx           — 2 байта (до 0x7FF: кириллица, греческий…);
 *   1110xxxx 10xxxxxx 10xxxxxx  — 3 байта (до 0xFFFF: почти всё остальное);
 *   11110xxx + три 10xxxxxx     — 4 байта (эмодзи и редкие знаки).
 * Биты x собираются в код. «Строгий» разбор отвергает подделки: лишне длинную запись
 * (код меньше min), коды больше 0x10FFFF и D800–DFFF (в UTF-8 они запрещены). */
uint32_t utf8_next(const char **p, const char *end) {
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t c = s[0];
    unsigned n = 0;
    uint32_t min = 0;
    if (c < 0x80) { *p += 1; return c; }
    if ((c & 0xE0) == 0xC0) { n = 1; c &= 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 2; c &= 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 3; c &= 0x07; min = 0x10000; }
    else { *p += 1; return 0xFFFD; }
    if ((const char *)s + n >= end) { *p += 1; return 0xFFFD; }          /* оборванная */
    for (unsigned i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p += 1; return 0xFFFD; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) { *p += 1; return 0xFFFD; }
    *p += n + 1;
    return c;
}

/* Символ за символом: надстрочный знак (0300–036F) пробуем слить с прошлым глифом;
 * не вышло — он станет отдельной клеткой (знак над пунктирным кружком). */
unsigned font_shape(const char *text, unsigned bytes, uint16_t *glyphs, unsigned max) {
    const char *p = text, *end = text + bytes;
    unsigned n = 0;
    while (p < end && n < max) {
        uint32_t c = utf8_next(&p, end);
        if (c >= 0x300 && c <= 0x36F && n) {
            unsigned done = font_combine(glyphs[n - 1], c);
            if (done != FONT_NONE) { glyphs[n - 1] = (uint16_t)done; continue; }
        }
        glyphs[n++] = (uint16_t)font_lookup(c);
    }
    return n;
}
