/* Страница: список элементов, палитра и кольцо строк в памяти видеочипа.
 *
 * ПОЧЕМУ ВСЯ СТРАНИЦА — ОДНА КАРТИНКА. Можно было бы рисовать каждое слово отдельной командой
 * видеочипа, но замер на модели ядра Slot1 (docs/ARCHITECTURE.md) показал: так кадр рисуется
 * 28–53 мс — дольше, чем идёт кадр (17 мс), картинка дёргалась бы. А целая страница одной
 * картинкой с байтом на точку рисуется за 11–12,5 мс. Поэтому ESP32 сам «рисует» текст в
 * массив байтов (как художник на холсте) и отправляет готовые строки точек в видеочип.
 *
 * ПАЛИТРА. Байт точки — не цвет, а номер цвета в таблице из 256 цветов (палитре). Так точка
 * занимает 1 байт вместо 2. Палитра поделена так:
 *   0…31   — «сплошные» цвета (фон страницы, фоны блоков, линии);
 *   32…255 — пары «цвет на цвете фона»: L оттенков от чуть-чуть цвета до чистого цвета
 *            (оттенок k — фон + (цвет − фон) · k / L). Буквам нужно 15 оттенков (точка глифа —
 *            4 бита, 0…15), краям фигур (рамки полей, кружки переключателей) хватает 4. Пары
 *            берутся подряд из общего запаса; пока места много — по 15 оттенков, к концу —
 *            по 8, потом по 4: так на странице помещается до 60 пар, а не 14 (у vtrd.in пары
 *            кончались, и «чёрный на сером» подменялся «белым на сером»).
 * Точка глифа v (0…15) даёт оттенок k = v · L / 15 с округлением (у пары на 15 оттенков k = v);
 * номер цвета — начало пары + k − 1, k = 0 — чистый фон, точку не трогаем. Если места в палитре
 * нет совсем, берётся самая похожая из заведённых пар.
 * Палитр в памяти две: новая страница пишет свою во вторую, пока на экране первая.
 *
 * ВСТРОЕННЫЕ ЭЛЕМЕНТЫ (поля форм, кнопки) — прямоугольник страницы, который рисует не page.c,
 * а тот, кто его поставил (form.c): при каждой отрисовке строк page.c зовёт painter, и тот рисует
 * функциями page_paint_… в те же строки. Так поле с набранным текстом можно перерисовать
 * (page_redraw), не раскладывая страницу заново.
 *
 * КОЛЬЦО. Страница бывает в десятки раз выше экрана, а памяти видеочипа хватает на 896 строк.
 * Поэтому храним только кусок вокруг видимой части. Строка страницы y лежит в строке кольца
 * y mod 896 (остаток от деления): строки идут по кругу, как часы — после 895 снова 0.
 * Видно 768 строк, начиная с shown. При прокрутке верх сдвигается за раз не больше чем на 128
 * строк — тогда новые строки дописываются только в те 128 строк кольца, которых сейчас не видно,
 * и экран не портится, пока по нему идёт рисование. Если видимая часть доходит до конца
 * кольца (строка 895), картинка выводится двумя кусками: низ кольца и его начало. */
#include <string.h>
#include "page.h"
#include "font.h"
#include "ts.h"

enum { ITEM_RECT, ITEM_TEXT, ITEM_WIDGET, ITEM_BOX };  /* прямоугольник, строка текста, встроенный
                                          * элемент, коробка CSS (fg — заливка, bg — рамка, flags —
                                          * её толщина, face — радиус углов, glyph_at — цвет под ней,
                                          * count ≠ 0 — толщины сторон по 4 бита: верх право низ лево) */

/* Один элемент страницы. Глифы строк текста хранятся отдельно, в общем массиве pool:
 * у элемента только номер первого глифа (glyph_at) и их число (count). У встроенного элемента
 * в glyph_at — его номер (id), в bg — цвет фона под ним. */
typedef struct {
    uint8_t kind, flags, face;
    int16_t link;
    int16_t x;
    uint16_t w, h;
    int32_t y;
    uint32_t fg, bg;
    uint32_t glyph_at;
    uint16_t count;
} item_t;

static item_t *items;                    /* элементы страницы (растущий массив в PSRAM) */
static unsigned item_n, item_cap;        /* сколько занято и сколько места */
static uint16_t *pool;                   /* глифы всех строк текста подряд */
static unsigned pool_n, pool_cap;
static uint32_t bg_rgb;                  /* цвет фона страницы 0xRRGGBB */
static uint8_t bg_index;                 /* его номер в палитре */
static int height;                       /* высота страницы в точках */

#define PAL_SOLIDS 32u
#define PAL_PAIRS  64u
static uint16_t pal[256];                /* палитра в формате видеочипа RGB565 */
static uint32_t pal_rgb[256];            /* те же цвета в 0xRRGGBB — для поиска похожего */
static unsigned solid_n, pair_n;         /* занято сплошных цветов и пар */
static unsigned pal_next;                /* первая свободная запись после пар */
static uint32_t pair_fg[PAL_PAIRS], pair_bg[PAL_PAIRS];
static uint8_t pair_base[PAL_PAIRS], pair_lv[PAL_PAIRS];
static void (*painter)(int id, int x, int y, int w, int h, uint32_t bg);   /* рисует встроенные элементы */
static unsigned pal_slot;                /* какая из двух палитр в памяти видеочипа наша (0 или 1) */
static int pal_dirty;                    /* палитра менялась — отправить её в видеочип */
/* Строки рисуются полосами по 32 строки в массив band (32 КБ), полоса уходит в видеочип,
 * потом рисуется следующая — так не нужен массив на всю страницу. */
#define BAND_ROWS 32
static uint8_t *band;
/* В кольце сейчас нарисованы строки страницы [ring_lo, ring_hi); на экране — с shown;
 * ring_valid = 0 — в кольце ещё ничего от этой страницы нет. */
static int ring_lo, ring_hi, shown, ring_valid;

/* Цвет 0xRRGGBB (по 8 бит на канал) → RGB565: 5 старших бит красного, 6 зелёного, 5 синего. */
static uint16_t rgb565(uint32_t c) {
    return (uint16_t)((((c >> 19) & 0x1F) << 11) | (((c >> 10) & 0x3F) << 5) | ((c >> 3) & 0x1F));
}
/* «Расстояние» между цветами: сумма квадратов разниц каналов. Чем меньше, тем цвета похожее. */
static uint32_t dist(uint32_t a, uint32_t b) {
    int dr = (int)((a >> 16) & 255) - (int)((b >> 16) & 255);
    int dg = (int)((a >> 8) & 255) - (int)((b >> 8) & 255);
    int db = (int)(a & 255) - (int)(b & 255);
    return (uint32_t)(dr * dr + dg * dg + db * db);
}
/* Сплошной цвет: уже есть такой (сплошной или оттенок пары) — его номер; нет — новый
 * сплошной; его 32 места заняты — из свободной части палитры, за оттенками пар (страницы с CSS
 * берут много своих цветов: фоны, рамки, полупрозрачные смеси; 07.11 светло-серая сетка таблицы
 * получала ближайший занятый цвет, и на плате линий не было видно);
 * места нет совсем — ближайший из занятых. */
#define PAL_KEEP 24                      /* столько записей в конце оставить оттенкам новых пар */
static uint8_t pal_solid(uint32_t rgb) {
    uint16_t c = rgb565(rgb);
    unsigned best = 0;
    uint32_t best_d = UINT32_MAX;
    for (unsigned i = 0; i < 256; i++) {
        if ((i >= solid_n && i < PAL_SOLIDS) || i >= pal_next) continue;
        if (pal[i] == c) return (uint8_t)i;
        uint32_t d = dist(pal_rgb[i], rgb);
        if (d < best_d) { best_d = d; best = i; }
    }
    if (solid_n < PAL_SOLIDS) {
        pal[solid_n] = c; pal_rgb[solid_n] = rgb; pal_dirty = 1;
        return (uint8_t)solid_n++;
    }
    if (pal_next + PAL_KEEP < 256u) {
        unsigned i = pal_next++;
        pal[i] = c; pal_rgb[i] = rgb; pal_dirty = 1;
        return (uint8_t)i;
    }
    return (uint8_t)best;
}

/* Пара «цвет fg на фоне bg»: где её оттенки в палитре и сколько их (L). */
typedef struct { uint8_t base, levels; } pair_t;

/* Пара на want оттенков (15 — буквы, 4 — края фигур). Такая уже есть — она (сколько бы у неё
 * ни было оттенков); нет — новая: на 15 оттенков, пока в палитре свободно больше 55 записей,
 * на 8 — больше 20, иначе на 4; места нет совсем — самая похожая из заведённых. */
static pair_t pal_pair_lv(uint32_t fg, uint32_t bg, unsigned want) {
    pair_t r = {PAL_SOLIDS, 15};
    unsigned best = 0;
    uint32_t best_d = UINT32_MAX;
    for (unsigned p = 0; p < pair_n; p++) {
        if (pair_fg[p] == fg && pair_bg[p] == bg) { r.base = pair_base[p]; r.levels = pair_lv[p]; return r; }
        uint32_t d = dist(pair_fg[p], fg) + dist(pair_bg[p], bg);
        if (d < best_d) { best_d = d; best = p; }
    }
    unsigned free = 256u - pal_next, lv;
    if (want >= 15 && free >= 15u + 40u) lv = 15;
    else if (want >= 8 && free >= 8u + 12u) lv = 8;
    else if (free >= 4u) lv = 4;
    else lv = 0;
    if (!lv || pair_n == PAL_PAIRS) {
        if (pair_n) { r.base = pair_base[best]; r.levels = pair_lv[best]; }
        return r;
    }
    unsigned base = pal_next;
    for (unsigned k = 1; k <= lv; k++) {
        uint32_t c = 0;
        for (unsigned sh = 0; sh <= 16; sh += 8) {
            int f = (int)((fg >> sh) & 255), b = (int)((bg >> sh) & 255);
            c |= (uint32_t)(b + ((f - b) * (int)k + (int)lv / 2) / (int)lv) << sh;
        }
        pal[base + k - 1] = rgb565(c); pal_rgb[base + k - 1] = c;
    }
    pair_fg[pair_n] = fg; pair_bg[pair_n] = bg;
    pair_base[pair_n] = (uint8_t)base; pair_lv[pair_n] = (uint8_t)lv;
    pair_n++;
    pal_next += lv;
    pal_dirty = 1;
    r.base = (uint8_t)base; r.levels = (uint8_t)lv;
    return r;
}
static pair_t pal_pair(uint32_t fg, uint32_t bg) { return pal_pair_lv(fg, bg, 15); }

/* Новый пустой элемент в конце списка. Массив растёт удвоением: 256, 512, 1024… — так
 * перевыделять память приходится редко. NULL — память кончилась. */
static item_t *new_item(void) {
    if (item_n == item_cap) {
        unsigned cap = item_cap ? item_cap * 2 : 256;
        item_t *p = ts_core->realloc(items, cap * sizeof(item_t));
        if (!p) return NULL;
        items = p; item_cap = cap;
    }
    item_t *it = &items[item_n++];
    memset(it, 0, sizeof(*it));
    it->link = -1;
    return it;
}

void page_begin(uint32_t bg) {
    item_n = 0; pool_n = 0; height = 0;
    bg_rgb = bg;
    solid_n = 0; pair_n = 0; pal_next = PAL_SOLIDS;
    memset(pal, 0, sizeof(pal));
    pal_slot ^= 1u;
    bg_index = pal_solid(bg);
    pal_dirty = 1;
    ring_valid = 0;
}

uint32_t page_bg(void) { return bg_rgb; }
void page_set_bg(uint32_t bg) { bg_rgb = bg; bg_index = pal_solid(bg); ring_valid = 0; }
int page_height(void) { return height; }
int page_top(void) { return shown; }

/* Возвращает номер элемента: высоту фона блока узнают позже, когда кончится его содержимое,
 * и тогда дописывают её через page_rect_height. */
int page_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (w <= 0 || h < 0) return -1;
    item_t *it = new_item();
    if (!it) return -1;
    it->kind = ITEM_RECT; it->x = (int16_t)x; it->y = y; it->w = (uint16_t)w; it->h = (uint16_t)h;
    it->fg = rgb;
    if (y + h > height) height = y + h;
    return (int)(it - items);
}

int page_box(int x, int y, int w, int h, uint32_t fill, uint32_t border, int bw, int radius, uint32_t under) {
    if (w <= 0 || h < 0) return -1;
    item_t *it = new_item();
    if (!it) return -1;
    it->kind = ITEM_BOX; it->x = (int16_t)x; it->y = y; it->w = (uint16_t)w; it->h = (uint16_t)h;
    it->fg = fill; it->bg = border;
    it->flags = (uint8_t)(bw < 0 ? 0 : bw > 32 ? 32 : bw);
    it->face = (uint8_t)(radius < 0 ? 0 : radius > 64 ? 64 : radius);
    it->glyph_at = under;
    if (y + h > height) height = y + h;
    return (int)(it - items);
}

void page_box_sides(int index, int t, int r, int b, int l) {
    if (index < 0 || (unsigned)index >= item_n || items[index].kind != ITEM_BOX) return;
    int s[4] = {t, r, b, l};
    for (int k = 0; k < 4; k++) s[k] = s[k] < 0 ? 0 : s[k] > 15 ? 15 : s[k];
    items[index].count = (uint16_t)(s[0] << 12 | s[1] << 8 | s[2] << 4 | s[3]);
    items[index].flags = 0;
    items[index].face = 0;
}

void page_rect_height(int index, int h) {
    if (index < 0 || (unsigned)index >= item_n || h < 0) return;
    items[index].h = (uint16_t)h;
    if (items[index].y + h > height) height = items[index].y + h;
}

/* Глифы копируются в общий массив pool (он растёт удвоением, как items). Ширина строки —
 * число глифов × шаг начертания: шрифт моноширинный, все буквы одного начертания одной ширины. */
void page_text(int x, int y, unsigned face, const uint16_t *glyphs, unsigned count, uint32_t fg, uint32_t bg,
               unsigned flags, int link) {
    if (face >= FACE_COUNT) face = FACE_BODY;
    const font_face_t *f = &font_faces[face];
    if (!count) return;
    if (pool_n + count > pool_cap) {
        unsigned cap = pool_cap ? pool_cap : 4096;
        while (cap < pool_n + count) cap *= 2;
        uint16_t *p = ts_core->realloc(pool, cap * sizeof(uint16_t));
        if (!p) return;
        pool = p; pool_cap = cap;
    }
    item_t *it = new_item();
    if (!it) return;
    memcpy(pool + pool_n, glyphs, count * sizeof(uint16_t));
    it->kind = ITEM_TEXT; it->flags = (uint8_t)flags; it->face = (uint8_t)face; it->link = (int16_t)link;
    it->x = (int16_t)x; it->y = y; it->w = (uint16_t)(count * f->w); it->h = f->h;
    it->fg = fg; it->bg = bg; it->glyph_at = pool_n; it->count = (uint16_t)count;
    pool_n += count;
    if (y + f->h > height) height = y + f->h;
}

int page_widget(int x, int y, int w, int h, int id, uint32_t bg, int link) {
    if (w <= 0 || h <= 0) return -1;
    item_t *it = new_item();
    if (!it) return -1;
    it->kind = ITEM_WIDGET; it->x = (int16_t)x; it->y = y; it->w = (uint16_t)w; it->h = (uint16_t)h;
    it->glyph_at = (uint32_t)id; it->bg = bg; it->link = (int16_t)link;
    if (y + h > height) height = y + h;
    return (int)(it - items);
}

void page_set_painter(void (*paint)(int id, int x, int y, int w, int h, uint32_t bg)) { painter = paint; }

int page_widget_at(int x, int y) {
    int py = shown + y;
    for (unsigned i = 0; i < item_n; i++) {
        const item_t *it = &items[i];
        if (it->kind == ITEM_WIDGET && py >= it->y && py < it->y + it->h && x >= it->x && x < it->x + it->w)
            return (int)it->glyph_at;
    }
    return -1;
}

/* Перебираем строки текста со ссылками и ищем ту, в чей прямоугольник попала точка. Точка
 * экрана (x, y) на странице — это (x, shown + y): страница прокручена на shown строк. */
int page_link_at(int x, int y) {
    int py = shown + y;
    for (unsigned i = 0; i < item_n; i++) {
        const item_t *it = &items[i];
        if ((it->kind == ITEM_TEXT || it->kind == ITEM_WIDGET) && it->link >= 0 && py >= it->y && py < it->y + it->h &&
            x >= it->x && x < it->x + it->w)                   /* картинка в ссылке — тоже ссылка */
            return it->link;
    }
    return -1;
}

/* --- рисование строк страницы [y0, y0 + rows) в полосу band ---
 * Полоса — массив rows строк по 1024 байта; строка страницы y — строка полосы y − y0.
 * clip_l…clip_r — где по ширине можно рисовать (у текста в поле формы — внутри поля). */
static int clip_l = 0, clip_r = TS_W;
static int cur_y0, cur_rows;             /* какую полосу рисуем сейчас (для page_paint_…) */

/* Залить прямоугольник номером цвета index — только ту его часть, что попала в полосу
 * (a…b по высоте) и в экран (l…r по ширине). */
static void fill(int y0, int rows, int x, int y, int w, int h, uint8_t index) {
    int a = y > y0 ? y : y0, b = y + h < y0 + rows ? y + h : y0 + rows;
    int l = x > clip_l ? x : clip_l, r = x + w < clip_r ? x + w : clip_r;
    for (int row = a; row < b && l < r; row++) memset(band + (size_t)(row - y0) * TS_W + l, index, (size_t)(r - l));
}

/* Один глиф в полосу: клетка cw×ch (stride байт в строке) с левым верхним углом (gx, top).
 * Точка глифа (4 бита) даёт оттенок пары p (как — в начале файла); нулевые точки (чистый фон)
 * пропускаем — там уже фон, залитый раньше. В байте две точки: старшие 4 бита — левая, младшие —
 * правая. Где точка уже закрашена этой же парой цветов (соседний глиф, жирный), остаётся более
 * насыщенная. bold — то же ещё раз на точку правее: синтетический жирный по дизайну Codex. */
static void paint_cell(int y0, int rows, const uint8_t *cell, int cw, int ch, int stride, int gx, int top,
                       pair_t p, int bold) {
    int a = top > y0 ? top : y0, b = top + ch < y0 + rows ? top + ch : y0 + rows;
    for (int row = a; row < b; row++) {
        const uint8_t *src = cell + (row - top) * stride;
        uint8_t *dst = band + (size_t)(row - y0) * TS_W;
        for (int k = 0; k < cw; k++) {
            int v = (src[k >> 1] >> ((k & 1) ? 0 : 4)) & 15;
            int lv = p.levels == 15 ? v : (v * p.levels + 7) / 15;   /* оттенок пары, 0 — фон */
            if (!lv) continue;
            for (int d = 0; d <= bold; d++) {
                int px = gx + k + d;
                if (px < clip_l || px >= clip_r) continue;
                int cur = dst[px] >= p.base && dst[px] < p.base + p.levels ? dst[px] - p.base + 1 : 0;
                if (lv > cur) dst[px] = (uint8_t)(p.base + lv - 1);
            }
        }
    }
}

/* Нарисовать строку текста в полосу: глифы подряд с шагом начертания. Знака нет в
 * начертании — его рисует основной шрифт, по центру клетки и на той же базовой линии.
 * Подчёркивание — на 2 точки ниже базовой линии (у крупных заголовков — толщиной 2). */
static void paint_glyphs(int y0, int rows, int x, int y, unsigned face, const uint16_t *g, unsigned count,
                         uint32_t fg, uint32_t bg, unsigned flags) {
    const font_face_t *f = &font_faces[face], *body = &font_faces[FACE_BODY];
    pair_t base = pal_pair(fg, bg);
    int bold = (flags & PAGE_BOLD) != 0;
    for (unsigned i = 0; i < count; i++) {
        int gx = x + (int)i * f->w;
        if (gx >= clip_r) break;
        if (gx + f->w <= clip_l) continue;
        const uint8_t *cell = font_glyph_face(face, g[i]);
        if (cell) paint_cell(y0, rows, cell, f->w, f->h, f->stride, gx, y, base, bold);
        else paint_cell(y0, rows, font_glyph(g[i]), body->w, body->h, body->stride, gx + (f->w - body->w) / 2,
                        y + f->baseline - body->baseline, base, bold);
    }
    int w = (int)count * f->w;
    uint8_t solid = (uint8_t)(base.base + base.levels - 1);    /* чистый цвет пары */
    if (flags & PAGE_UNDERLINE)
        fill(y0, rows, x, y + f->baseline + 2, w, f->h > 40 ? 2 : 1, solid);
    if (flags & PAGE_STRIKE)                                      /* на трети высоты строчной буквы */
        fill(y0, rows, x, y + f->baseline - f->baseline / 3, w, f->h > 40 ? 2 : 1, solid);
}
static void paint_text(int y0, int rows, const item_t *it) {
    paint_glyphs(y0, rows, it->x, it->y, it->face, pool + it->glyph_at, it->count, it->fg, it->bg, it->flags);
}

/* --- рисование для встроенных элементов (зовёт painter во время render_rows) --- */

/* Скруглённый прямоугольник цвета fg на фоне bg со сглаженными углами. Точка целиком внутри —
 * чистый fg; точка на скруглении угла делится на 4×4 «пробы», и доля проб внутри круга угла
 * даёт оттенок края (пара на 4 оттенка). Круг — это скругление на всю половину стороны. */
void page_paint_round(int x, int y, int w, int h, int r, uint32_t fg, uint32_t bg) {
    if (w <= 0 || h <= 0 || !band) return;
    if (2 * r > w) r = w / 2;
    if (2 * r > h) r = h / 2;
    int a = y > cur_y0 ? y : cur_y0, b = y + h < cur_y0 + cur_rows ? y + h : cur_y0 + cur_rows;
    int l = x > clip_l ? x : clip_l, rt = x + w < clip_r ? x + w : clip_r;
    if (a >= b || l >= rt) return;
    pair_t s = pal_pair_lv(fg, bg, 4);
    uint8_t full = (uint8_t)(s.base + s.levels - 1);
    int r8 = 8 * r;
    for (int row = a; row < b; row++) {
        uint8_t *dst = band + (size_t)(row - cur_y0) * TS_W;
        if (row >= y + r && row < y + h - r) { memset(dst + l, full, (size_t)(rt - l)); continue; }
        int cy = row < y + r ? y + r : y + h - r;                     /* центр круга угла по y */
        for (int px = l; px < rt; px++) {
            if (px >= x + r && px < x + w - r) { dst[px] = full; continue; }
            int cx = px < x + r ? x + r : x + w - r;
            int in = 0;
            for (int j = 0; j < 4; j++) {
                int dy = row * 8 + 2 * j + 1 - cy * 8;                  /* в восьмых долях точки */
                if ((row < y + r && dy > 0) || (row >= y + h - r && dy < 0)) dy = 0;
                for (int i = 0; i < 4; i++) {
                    int dx = px * 8 + 2 * i + 1 - cx * 8;
                    if ((px < x + r && dx > 0) || (px >= x + w - r && dx < 0)) dx = 0;
                    if (dx * dx + dy * dy <= r8 * r8) in++;
                }
            }
            int q = (in * s.levels + 8) / 16;                         /* 0…16 проб → оттенок пары */
            if (q) dst[px] = (uint8_t)(s.base + q - 1);
        }
    }
}

/* Маска L4 (иконка: 4 бита на точку, как у букв) цветом fg на фоне bg. */
void page_paint_mask(int x, int y, int w, int h, const uint8_t *l4, int stride, uint32_t fg, uint32_t bg) {
    if (!band) return;
    paint_cell(cur_y0, cur_rows, l4, w, h, stride, x, y, pal_pair_lv(fg, bg, 8), 0);
}

/* Глифы начертания face с верхом клетки y, видно только между clip_left и clip_right. */
void page_paint_text(int x, int y, unsigned face, const uint16_t *g, unsigned n, uint32_t fg, uint32_t bg,
                     unsigned flags, int clip_left, int clip_right) {
    if (!band || !n) return;
    if (face >= FACE_COUNT) face = FACE_BODY;
    int ol = clip_l, orr = clip_r;
    clip_l = clip_left > ol ? clip_left : ol;
    clip_r = clip_right < orr ? clip_right : orr;
    if (clip_l < clip_r) paint_glyphs(cur_y0, cur_rows, x, y, face, g, n, fg, bg, flags);
    clip_l = ol; clip_r = orr;
}

/* Коробка CSS: без скругления — прямоугольники (рамка, внутри — заливка), со скруглением — два
 * скруглённых (page_paint_round): рамка к цвету под коробкой, заливка к цвету рамки. */
static void paint_box(const item_t *it) {
    int x = it->x, y = it->y, w = it->w, h = it->h, bw = it->flags, r = it->face;
    if (it->count) {                                   /* стороны разной толщины: рамка, внутри — заливка */
        int t = it->count >> 12 & 15, rt = it->count >> 8 & 15, b = it->count >> 4 & 15, l = it->count & 15;
        fill(cur_y0, cur_rows, x, y, w, h, pal_solid(it->bg));
        if (w > l + rt && h > t + b) fill(cur_y0, cur_rows, x + l, y + t, w - l - rt, h - t - b, pal_solid(it->fg));
        return;
    }
    if (!r) {
        fill(cur_y0, cur_rows, x, y, w, h, pal_solid(bw ? it->bg : it->fg));
        if (bw && w > 2 * bw && h > 2 * bw) fill(cur_y0, cur_rows, x + bw, y + bw, w - 2 * bw, h - 2 * bw, pal_solid(it->fg));
        return;
    }
    if (!bw) { page_paint_round(x, y, w, h, r, it->fg, it->glyph_at); return; }
    page_paint_round(x, y, w, h, r, it->bg, it->glyph_at);
    if (w > 2 * bw && h > 2 * bw) page_paint_round(x + bw, y + bw, w - 2 * bw, h - 2 * bw, r > bw ? r - bw : 0, it->fg, it->bg);
}

/* Отправить полосу в кольцо. Если полоса упирается в конец кольца (строка 895), она идёт
 * двумя кусками: сколько влезло до конца — туда, остаток — в начало кольца. */
static int upload(int y0, int rows) {
    unsigned slot = (unsigned)y0 % PAGE_RING_ROWS;
    unsigned n1 = PAGE_RING_ROWS - slot < (unsigned)rows ? PAGE_RING_ROWS - slot : (unsigned)rows;
    if (!ft_put(RAMG_RING + slot * PAGE_ROW_BYTES, band, n1 * PAGE_ROW_BYTES)) return 0;
    if ((unsigned)rows > n1 && !ft_put(RAMG_RING, band + (size_t)n1 * TS_W, ((unsigned)rows - n1) * PAGE_ROW_BYTES))
        return 0;
    return 1;
}

/* Нарисовать строки страницы [a, b) и отправить их в кольцо. Каждую полосу: залить фоном,
 * пройти по всем элементам и нарисовать те, что её задевают (фоны блоков добавлены раньше
 * текста, поэтому текст ложится поверх них). */
static int render_rows(int a, int b) {
    if (!band && !(band = ts_alloc((size_t)BAND_ROWS * TS_W))) return 0;
    for (int y0 = a; y0 < b; y0 += BAND_ROWS) {
        int rows = b - y0 < BAND_ROWS ? b - y0 : BAND_ROWS;
        memset(band, bg_index, (size_t)rows * TS_W);
        cur_y0 = y0; cur_rows = rows;
        for (unsigned i = 0; i < item_n; i++) {
            const item_t *it = &items[i];
            if (it->kind == ITEM_RECT) {
                if (it->y < y0 + rows && it->y + it->h > y0)
                    fill(y0, rows, it->x, it->y, it->w, it->h, pal_solid(it->fg));
            } else if (it->kind == ITEM_WIDGET) {
                if (painter && it->y < y0 + rows && it->y + it->h > y0)
                    painter((int)it->glyph_at, it->x, it->y, it->w, it->h, it->bg);
            } else if (it->kind == ITEM_BOX) {
                if (it->y < y0 + rows && it->y + it->h > y0) paint_box(it);
            } else if (it->y < y0 + rows && it->y + it->h + 4 > y0) {      /* +4 — подчёркивание */
                paint_text(y0, rows, it);
            }
        }
        if (!upload(y0, rows)) return 0;
    }
    return 1;
}

/* Палитра менялась — отправить её (в ту же половину: новые цвета — в свободных местах,
 * видимые точки старых цветов не меняются). */
static int put_palette(void) {
    if (!pal_dirty) return 1;
    if (!ft_put(RAMG_PAL + pal_slot * 512u, pal, sizeof(pal))) return 0;
    pal_dirty = 0;
    return 1;
}

int page_redraw(int y, int h) {
    if (!ring_valid) return 1;
    int a = y > ring_lo ? y : ring_lo, b = y + h < ring_hi ? y + h : ring_hi;
    if (a < b && !render_rows(a, b)) return 0;
    return put_palette();
}

/* Прокрутка: target — какую строку страницы показать вверху экрана. */
int page_scroll_to(int target) {
    int limit = height > TS_H ? height - TS_H : 0;
    if (target > limit) target = limit;
    if (target < 0) target = 0;
    if (!ring_valid || target > shown + 2 * TS_H || target < shown - 2 * TS_H) {
        /* новая страница или дальний прыжок: экран целиком (видимое при этом переписывается) */
        if (!render_rows(target, target + TS_H)) return 0;
        ring_lo = target; ring_hi = target + TS_H; shown = target; ring_valid = 1;
    } else {
        /* близкий сдвиг: не больше PAGE_STEP за раз (остальное — в следующих кадрах);
         * дорисовываем только то, чего в кольце ещё нет, — снизу или сверху */
        int t = target;
        if (t > shown + PAGE_STEP) t = shown + PAGE_STEP;
        if (t < shown - PAGE_STEP) t = shown - PAGE_STEP;
        /* Строки невидимы только для листа с верхом shown. Прошлый лист видеочип берёт лишь в
         * конце кадра, а до того на экране ещё позапрошлый — с верхом на шаг выше, и для него
         * новые строки попали бы в видимый верх экрана (при быстрой прокрутке там на кадр
         * мелькали чужие строки). Поэтому сначала ждём, пока лист с shown встанет на экран. */
        if (t != shown && !ft_swap_wait()) return 0;
        if (t + TS_H > ring_hi) {
            if (!render_rows(ring_hi, t + TS_H)) return 0;
            ring_hi = t + TS_H;
            if (ring_lo < ring_hi - (int)PAGE_RING_ROWS) ring_lo = ring_hi - (int)PAGE_RING_ROWS;
        }
        if (t < ring_lo) {
            if (!render_rows(t, ring_lo)) return 0;
            ring_lo = t;
            if (ring_hi > ring_lo + (int)PAGE_RING_ROWS) ring_hi = ring_lo + (int)PAGE_RING_ROWS;
        }
        shown = t;
    }
    return put_palette();
}

/* Команды для видеочипа: «бери палитру отсюда, картинку отсюда, рисуй в (0, 0)». Видимые
 * 768 строк начинаются со строки кольца slot; если до конца кольца их меньше (na < 768),
 * вторым куском рисуется начало кольца ниже первого. Цвет белый и непрозрачный — чтобы
 * видеочип не подкрашивал и не просвечивал цвета палитры. */
void page_dl(ft_dl_t *d) {
    unsigned slot = (unsigned)shown % PAGE_RING_ROWS;
    unsigned na = PAGE_RING_ROWS - slot < TS_H ? PAGE_RING_ROWS - slot : TS_H;
    dl_put(d, dl_handle(0));
    dl_put(d, dl_begin(FT_BITMAPS));
    dl_put(d, dl_color_rgb(0xFFFFFF));
    dl_put(d, dl_color_a(255));
    dl_put(d, dl_palette_source(RAMG_PAL + pal_slot * 512u));
    dl_bitmap(d, RAMG_RING + slot * PAGE_ROW_BYTES, FT_PALETTED565, PAGE_ROW_BYTES, TS_W, na);
    dl_put(d, dl_vertex(0, 0));
    if (na < TS_H) {
        dl_bitmap(d, RAMG_RING, FT_PALETTED565, PAGE_ROW_BYTES, TS_W, TS_H - na);
        dl_put(d, dl_vertex(0, (int)na * 16));
    }
    dl_put(d, dl_end());
}
