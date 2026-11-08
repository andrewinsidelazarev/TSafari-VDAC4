/* Значки ICO: оглавление и картинки BMP внутри (что такое ICO — ico.h).
 *
 * Как устроена картинка BMP в значке (числа — младшим байтом вперёд):
 *   заголовок (40 байт и больше): длина заголовка, ширина, высота (вдвое больше настоящей: точки и
 *   маска вместе), плоскости, бит на точку (1, 4, 8, 24, 32), сжатие (0 — нет; 3 — маски цветов у
 *   32 бит, они стандартные — пропускаем), …, число цветов палитры (смещение 32);
 *   палитра — для 1, 4, 8 бит: по 4 байта на цвет (синий, зелёный, красный, 0);
 *   точки — строками снизу вверх, каждая строка дополнена до 4 байт;
 *   маска И — тоже снизу вверх, по биту на точку, строка до 4 байт; 1 — точку не рисовать.
 * У 32 бит четвёртый байт точки — прозрачность (альфа); если она у всех точек 0 — значок старый,
 * прозрачность берётся из маски. */
#include <string.h>
#include "ico.h"
#include "ts.h"

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

int ico_is(const uint8_t *p, size_t n) {
    return n >= 22 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || p[2] == 2) && p[3] == 0 && le16(p + 4) > 0;
}

/* Картинка номер i оглавления: размер, бит на точку, где лежит. 0 — испорчена (не в файле). */
static int entry(const uint8_t *p, size_t n, unsigned i, int *w, int *h, unsigned *bpp, uint32_t *off,
                 uint32_t *size, int *png) {
    const uint8_t *e = p + 6 + 16u * i;
    *w = e[0] ? e[0] : 256;                              /* 0 в оглавлении — 256 */
    *h = e[1] ? e[1] : 256;
    *bpp = le16(e + 6);
    *size = le32(e + 8);
    *off = le32(e + 12);
    if (*off < 6u || *size < 8u || *off > n || *size > n - *off) return 0;
    const uint8_t *d = p + *off;
    *png = !memcmp(d, "\x89PNG", 4);
    if (!*png) {                                         /* BMP: размер и цвета — из её заголовка */
        int bw, bh;
        if (!ico_bmp_size(d, *size, &bw, &bh)) return 0;
        *w = bw;
        *h = bh;
        *bpp = le16(d + 14);
    }
    return 1;
}

int ico_pick(const uint8_t *p, size_t n, int want, uint32_t *off, uint32_t *size, int *png) {
    if (!ico_is(p, n)) return 0;
    unsigned count = le16(p + 4);
    if (6u + 16u * count > n) count = (unsigned)((n - 6u) / 16u);   /* обрезанное оглавление — что есть */
    int best = -1, best_w = 0;
    unsigned best_bpp = 0;
    for (unsigned i = 0; i < count; i++) {
        int w, h, pg;
        unsigned bpp;
        uint32_t o, s;
        if (!entry(p, n, i, &w, &h, &bpp, &o, &s, &pg)) continue;
        int side = w > h ? w : h;
        int better;
        if (best < 0) better = 1;
        else if (side == best_w) better = bpp > best_bpp;
        else if (want > 0 && side >= want && best_w >= want) better = side < best_w;   /* оба не меньше места — меньший */
        else if (want > 0 && side >= want) better = 1;                                /* этот дотягивает до места */
        else if (want > 0 && best_w >= want) better = 0;
        else better = side > best_w;                                                   /* оба меньше — больший */
        if (better) { best = (int)i; best_w = side; best_bpp = bpp; *off = o; *size = s; *png = pg; }
    }
    return best >= 0;
}

int ico_bmp_size(const uint8_t *d, size_t n, int *w, int *h) {
    if (n < 40) return 0;
    uint32_t hs = le32(d);
    int32_t bw = (int32_t)le32(d + 4), bh = (int32_t)le32(d + 8);
    unsigned bpp = le16(d + 14), comp = le32(d + 16);
    if (hs < 40 || hs > n || bw <= 0 || bw > 256 || bh == 0) return 0;
    if (bh < 0) bh = -bh;                                /* «сверху вниз» — бывает редко */
    if (bh / 2 <= 0 || bh / 2 > 256) return 0;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) return 0;
    if (comp != 0 && !(comp == 3 && bpp == 32)) return 0;
    *w = bw;
    *h = bh / 2;
    return 1;
}

int ico_bmp_decode(const uint8_t *d, size_t n, int tw, int th, uint16_t **pix, int *alpha) {
    int sw, sh;
    if (!ico_bmp_size(d, n, &sw, &sh) || tw <= 0 || th <= 0 || tw > sw || th > sh) return 0;
    uint32_t hs = le32(d), comp = le32(d + 16), nclr = le32(d + 32);
    unsigned bpp = le16(d + 14);
    int top_down = (int32_t)le32(d + 8) < 0;
    const uint8_t *pal = d + hs + (comp == 3 ? 12u : 0u);    /* у сжатия 3 после заголовка — три маски */
    uint32_t npal = bpp <= 8 ? (nclr && nclr <= (1u << bpp) ? nclr : 1u << bpp) : 0;
    uint32_t xs = ((uint32_t)sw * bpp + 31u) / 32u * 4u, as = ((uint32_t)sw + 31u) / 32u * 4u;
    size_t at = (size_t)(pal - d) + npal * 4u;           /* где начинаются точки */
    if (at > n || xs * (uint32_t)sh > n - at) return 0;  /* точек меньше, чем сказано, — испорчена */
    const uint8_t *xr = d + at, *an = xr + xs * (uint32_t)sh;
    int have_mask = as * (uint32_t)sh <= n - at - xs * (uint32_t)sh;   /* у 32 бит маски бывает нет */
    /* 32 бит: есть ли настоящая прозрачность (у старых значков четвёртый байт всюду 0) */
    int own_alpha = 0;
    if (bpp == 32)
        for (int y = 0; y < sh && !own_alpha; y++)
            for (int x = 0; x < sw; x++)
                if (xr[(uint32_t)y * xs + (uint32_t)x * 4u + 3u]) { own_alpha = 1; break; }
    uint8_t *rgba = ts_alloc((size_t)sw * (size_t)sh * 4u);  /* точки как есть: R, G, B, A — сверху вниз */
    if (!rgba) return -1;
    int opaque = 1;
    for (int y = 0; y < sh; y++) {
        uint32_t row = (uint32_t)(top_down ? y : sh - 1 - y);
        const uint8_t *src = xr + row * xs, *m = an + row * as;
        uint8_t *o = rgba + (size_t)y * (size_t)sw * 4u;
        for (int x = 0; x < sw; x++, o += 4) {
            uint8_t r, g, b, a = 255;
            if (bpp >= 24) {
                const uint8_t *q = src + (uint32_t)x * (bpp / 8u);
                b = q[0]; g = q[1]; r = q[2];
                if (bpp == 32 && own_alpha) a = q[3];
            } else {
                uint32_t i = bpp == 8 ? src[x] : bpp == 4 ? (uint32_t)(src[x / 2] >> (x & 1 ? 0 : 4)) & 15u
                                                          : (uint32_t)(src[x / 8] >> (7 - x % 8)) & 1u;
                if (i < npal) { b = pal[i * 4u]; g = pal[i * 4u + 1u]; r = pal[i * 4u + 2u]; }
                else r = g = b = 0;
            }
            if (!own_alpha && have_mask && (m[x / 8] >> (7 - x % 8) & 1)) a = 0;   /* маска И: прозрачно */
            if (a != 255) opaque = 0;
            o[0] = r; o[1] = g; o[2] = b; o[3] = a;
        }
    }
    size_t bytes = ((size_t)tw * (size_t)th * 2u + 3u) & ~(size_t)3u;
    uint16_t *out = ts_alloc(bytes);
    if (!out) { ts_core->free(rgba); return -1; }
    memset(out, 0, bytes);
    /* Уменьшение до tw × th: каждая точка — среднее своего прямоугольника исходных. Цвет усредняется
     * с весом прозрачности (у невидимой точки цвета нет — иначе края тёмные), потом делится обратно. */
    for (int ty = 0; ty < th; ty++) {
        int y0 = ty * sh / th, y1 = (ty + 1) * sh / th;
        if (y1 <= y0) y1 = y0 + 1;
        for (int tx = 0; tx < tw; tx++) {
            int x0 = tx * sw / tw, x1 = (tx + 1) * sw / tw;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t sr = 0, sg = 0, sb = 0, sa = 0, cnt = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    const uint8_t *q = rgba + ((size_t)y * (size_t)sw + (size_t)x) * 4u;
                    sr += (uint32_t)q[0] * q[3]; sg += (uint32_t)q[1] * q[3]; sb += (uint32_t)q[2] * q[3];
                    sa += q[3];
                    cnt++;
                }
            uint32_t a = sa / cnt, r = sa ? sr / sa : 0, g = sa ? sg / sa : 0, b = sa ? sb / sa : 0;
            uint16_t v;
            if (opaque) v = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));          /* RGB565 */
            else v = (uint16_t)((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));    /* ARGB4 */
            out[(size_t)ty * (size_t)tw + (size_t)tx] = v;
        }
    }
    ts_core->free(rgba);
    *pix = out;
    *alpha = !opaque;
    return 1;
}
