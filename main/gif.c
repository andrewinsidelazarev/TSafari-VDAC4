/* Картинки GIF (что это — в gif.h).
 *
 * УСТРОЙСТВО ФАЙЛА GIF:
 *   «GIF89a», размер «холста» (ширина, высота), флаги: есть ли общая палитра и её размер;
 *   общая палитра — по 3 байта (красный, зелёный, синий) на цвет;
 *   дальше блоки: «!» (0x21) — расширение (у 0xF9 — номер прозрачного цвета), «,» (0x2C) — кадр:
 *   где он на холсте, его размер, своя палитра (если есть), «через строку» (interlace) ли, и сами
 *   номера точек, сжатые LZW, кусками до 255 байт; «;» (0x3B) — конец файла.
 *
 * LZW — «словарь, растущий на ходу»: код — номер слова в словаре; первые 2^N кодов — одиночные
 * номера цветов, потом два служебных (очистить словарь, конец), а каждый новый код — это прежнее
 * слово плюс первая буква следующего. Длина кода растёт от N+1 до 12 бит по мере роста словаря.
 * Слово раскрывается с конца (по цепочке «префикс + последняя буква») в стек, потом выводится. */
#include <string.h>
#include "gif.h"
#include "ts.h"

int gif_is(const uint8_t *p, size_t n) {
    return n >= 13 && !memcmp(p, "GIF8", 4) && (p[4] == '7' || p[4] == '9') && p[5] == 'a';
}

int gif_size(const uint8_t *p, size_t n, int *w, int *h) {
    if (!gif_is(p, n)) return 0;
    *w = p[6] | p[7] << 8;
    *h = p[8] | p[9] << 8;
    return *w > 0 && *h > 0;
}

/* Чтение кодов LZW из кусков данных кадра: [длина][байты]…[0]. */
typedef struct {
    const uint8_t *p, *end;
    unsigned left;                       /* байт осталось в текущем куске */
    uint32_t bits;                       /* накопленные биты (младшие — первыми) */
    int nbits;
} bits_t;

static int next_code(bits_t *b, int size) {
    while (b->nbits < size) {
        if (!b->left) {                  /* следующий кусок */
            if (b->p >= b->end || !*b->p) return -1;
            b->left = *b->p++;
        }
        if (b->p >= b->end) return -1;
        b->bits |= (uint32_t)*b->p++ << b->nbits;
        b->nbits += 8;
        b->left--;
    }
    int c = (int)(b->bits & ((1u << size) - 1));
    b->bits >>= size;
    b->nbits -= size;
    return c;
}

/* Номера точек кадра fw × fh — в idx (построчно, interlace уже разложен). 0 — испорчено. */
static int lzw(const uint8_t *p, const uint8_t *end, int min_size, uint8_t *idx, int fw, int fh, int interlace) {
    static uint16_t prefix[4096];
    static uint8_t suffix[4096], stack[4097];
    if (min_size < 2 || min_size > 8) return 0;
    bits_t b = {p, end, 0, 0, 0};
    int clear = 1 << min_size, eoi = clear + 1, size = min_size + 1, next = clear + 2, prev = -1;
    uint8_t first = 0;
    long total = (long)fw * fh, out = 0;
    int row = 0, col = 0, pass = 0;
    static const int start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
    for (int i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = (uint8_t)i; }
    while (out < total) {
        int code = next_code(&b, size);
        if (code < 0 || code == eoi) break;
        if (code == clear) { size = min_size + 1; next = clear + 2; prev = -1; continue; }
        int sp = 0, c = code;
        if (prev < 0) {                                  /* первый код после очистки — одиночный */
            if (code >= clear) return 0;
            stack[sp++] = (uint8_t)code;
            first = (uint8_t)code;
        } else {
            if (code > next || (code == next && next >= 4096)) return 0;
            if (code == next) { stack[sp++] = first; c = prev; }   /* слово «прежнее + его первая буква» */
            while (c >= clear) {
                if (sp >= 4096 || c >= 4096) return 0;
                stack[sp++] = suffix[c];
                c = prefix[c];
            }
            stack[sp++] = (uint8_t)c;
            first = (uint8_t)c;
            if (next < 4096) {
                prefix[next] = (uint16_t)prev;
                suffix[next] = first;
                next++;
                if (next == (1 << size) && size < 12) size++;
            }
        }
        prev = code;
        while (sp && out < total) {                      /* слово — в точки, с конца стека */
            idx[(size_t)row * fw + col] = stack[--sp];
            out++;
            if (++col == fw) {
                col = 0;
                if (!interlace) row++;
                else {                                    /* через строку: 4 прохода */
                    row += step[pass];
                    while (row >= fh && pass < 3) row = start[++pass];
                }
                if (row >= fh) row = fh - 1;
            }
        }
    }
    return out > 0;
}

/* Цвет палитры (3 байта) → ARGB1555 с непрозрачностью. */
static uint16_t argb1555(const uint8_t *c) {
    return (uint16_t)(0x8000u | (c[0] >> 3) << 10 | (c[1] >> 3) << 5 | c[2] >> 3);
}

int gif_decode(const uint8_t *p, size_t n, int max_w, int max_h, uint16_t **pix, int *ow, int *oh) {
    int sw, sh;
    if (!gif_size(p, n, &sw, &sh)) return 0;
    const uint8_t *end = p + n, *q = p + 13;
    const uint8_t *gpal = NULL;
    int gcount = 0, transparent = -1;
    if (p[10] & 0x80) { gcount = 2 << (p[10] & 7); gpal = q; q += 3 * gcount; }
    while (q < end) {
        if (*q == 0x21) {                                /* расширение */
            if (q + 2 >= end) return 0;
            if (q[1] == 0xF9 && q + 6 < end && q[2] >= 4 && (q[3] & 1)) transparent = q[6];
            q += 2;
            while (q < end && *q) q += 1 + *q;           /* куски до нулевого */
            q++;
            continue;
        }
        if (*q != 0x2C) return 0;                        /* не кадр — испорчен (или «;» без кадра) */
        if (q + 10 > end) return 0;
        int fx = q[1] | q[2] << 8, fy = q[3] | q[4] << 8, fw = q[5] | q[6] << 8, fh = q[7] | q[8] << 8;
        int flags = q[9];
        q += 10;
        const uint8_t *pal = gpal;
        int count = gcount;
        if (flags & 0x80) { count = 2 << (flags & 7); pal = q; q += 3 * count; }
        if (!pal || q >= end || fw <= 0 || fh <= 0) return 0;
        int min_size = *q++;
        uint8_t *idx = ts_alloc((size_t)fw * fh);
        if (!idx) return 0;
        memset(idx, transparent >= 0 ? transparent : 0, (size_t)fw * fh);
        int ok = lzw(q, end, min_size, idx, fw, fh, (flags & 0x40) != 0);
        if (!ok) { ts_core->free(idx); return 0; }
        /* размер результата: холст, уменьшенный до max_w × max_h с сохранением пропорций */
        int tw = sw, th = sh;
        if (max_w > 0 && tw > max_w) { th = (int)((long)th * max_w / tw); tw = max_w; }
        if (max_h > 0 && th > max_h) { tw = (int)((long)tw * max_h / th); th = max_h; }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        uint16_t *out = ts_alloc((size_t)tw * th * 2 + 4);
        if (!out) { ts_core->free(idx); return 0; }
        for (int y = 0; y < th; y++) {
            int cy = (int)((long)y * sh / th) - fy;      /* точка холста → точка кадра */
            for (int x = 0; x < tw; x++) {
                int cx = (int)((long)x * sw / tw) - fx;
                uint16_t v = 0;                          /* вне кадра и прозрачное — прозрачно */
                if (cx >= 0 && cy >= 0 && cx < fw && cy < fh) {
                    int i = idx[(size_t)cy * fw + cx];
                    if (i != transparent && i < count) v = argb1555(pal + 3 * i);
                }
                out[(size_t)y * tw + x] = v;
            }
        }
        ts_core->free(idx);
        *pix = out; *ow = tw; *oh = th;
        return 1;
    }
    return 0;
}
