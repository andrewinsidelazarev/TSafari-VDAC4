/* Картинки на странице (как они попадают на экран — в image.h).
 *
 * ГДЕ ЧТО ХРАНИТСЯ.
 *   imgs[]   — картинки страницы: адрес, подпись, состояние, размер, где лежат в RAM_G и в каком
 *              формате. Картинка с одним адресом — одна, даже если стоит на странице много раз.
 *   places[] — места на странице: какая картинка, где (x, y) и какого размера (w, h). Размер места
 *              может отличаться от размера картинки — видеочип её масштабирует.
 *   strs     — строки (адреса, подписи) подряд через нулевой байт.
 *
 * СОПРОЦЕССОР. JPEG и PNG разбирает сопроцессор FT812 (его изображает BL616): ESP32 пишет в
 * кольцо команд RAM_CMD (4 КБ) слова команды и следом байты файла, сколько влезает, сдвигает
 * REG_CMD_WRITE, сопроцессор забирает и сдвигает REG_CMD_READ (так же DOOM VDAC4 кормит BL616
 * своими командами). Флаг OPT_NODL — «в дисплей-лист ничего не пиши»: лист у нас свой.
 *
 * С прошивки BL616 39 (сделана по нашему заданию task_bl616_images.md, 04.11) — своя команда
 * VDAC4 CMD_LOADIMAGE_BOX: «разбери и уложи в рамку w × h, памяти у тебя room байт». Рамка —
 * место картинки на странице: значок 1000×1000 на месте 32×32 ложится сразу 32×32 (2 КБ вместо
 * мегабайта), с честным усреднением точек — без ряби, которую дало бы уменьшение видеочипом.
 * Понимает она и прогрессивный JPEG, и PNG всех видов. Что вышло (размер, формат, палитра) —
 * спрашиваем командой CMD_IMAGEINFO; не вышло — BL616 оставляет в начале RAM_CMD отчёт с
 * причиной («too big: need … bytes»), он идёт в журнал.
 *
 * Со старой прошивкой BL616 (команды нет — «unknown command») — CMD_LOADIMAGE, как у FT812:
 * JPEG «baseline» в RGB565, PNG по 8 бит без «через строку» (серый — L8, цветной — RGB565, с
 * прозрачностью — ARGB4, с палитрой — палитра по 2 байта на цвет, за ней точки по байту) в
 * полный размер; прочее — «недоступна».
 *
 * GIF и WebP раскладывает сам ESP32: GIF — gif.c, WebP — библиотека libwebp (third_party/libwebp,
 * решение пользователя 04.11). libwebp умеет уменьшать картинку прямо при разборе — так большая
 * фотография сразу ложится размером с место на странице и не занимает лишней памяти. */
#include <stdio.h>
#include <string.h>
#include "image.h"
#include "gif.h"
#include "src/webp/decode.h"
#include "page.h"
#include "font.h"
#include "ui_data.h"
#include "ui_design.h"
#include "ts.h"

#define POOL_START 0x114000u             /* RAM_G картинок: после масок раскрытых списков форм */
#define POOL_END   0x200000u
#define RAM_CMD        0x308000u
#define REG_CPURESET   0x302020u
#define REG_CMD_READ   0x3020F8u
#define REG_CMD_WRITE  0x3020FCu
#define CMD_LOADIMAGE  0xFFFFFF24u
#define CMD_LOADIMAGE_BOX 0xFFFFFFB0u    /* BL616 39+: ptr, опции, рамка w | h << 16, room */
#define CMD_IMAGEINFO  0xFFFFFFB1u       /* BL616 39+: 4 слова о последней картинке */
#define IMG_REPORT     0x00474D49u       /* «IMG» — отчёт BL616 об ошибке картинки в начале RAM_CMD */
#define FAULT_UNKNOWN  0x04F2u           /* метка в начале RAM_CMD: «неизвестная команда» */
#define BOX_MAX        2047              /* больше видеочип не рисует (поле BITMAP_SIZE) */
#define OPT_NODL       2u
#define PLACEHOLDER    32                /* сторона заглушки картинки без размера, пока она не пришла */
#define DRAW_MAX       160               /* картинок в кадре — не больше (дисплей-лист: ~10 слов на каждую) */
#define WIDE_MAX       1008              /* шире места на странице картинку ESP32 не раскладывает */

enum { IMG_NEW, IMG_LOADING, IMG_READY, IMG_FAILED };

typedef struct {
    uint32_t url, alt;                   /* строки в strs */
    uint8_t state, fmt, alpha;           /* IMG_…; формат FT (ft.h); есть прозрачность */
    uint16_t w, h;                       /* настоящий размер картинки (0 — ещё неизвестен) */
    uint16_t dw, dh;                     /* размер того, что лежит в RAM_G (GIF бывает уменьшен) */
    uint32_t addr, pal, stride;          /* точки, палитра (0 — нет), байт в строке */
} img_t;
typedef struct {
    int img, x, y, w, h, link;
    int aw, ah, max_w;                   /* размеры из HTML и ширина места — пересчитать размер */
    uint8_t placed, sized;               /* легло на страницу; размер задан в HTML (не от картинки) */
    uint32_t bg;
} place_t;

static img_t *imgs;
static unsigned img_n, img_cap;
static place_t *places;
static unsigned place_n, place_cap;
static char *strs;
static size_t str_len, str_cap;
static uint32_t pool_next = POOL_START;  /* следующий свободный адрес RAM_G картинок */
static int stopped;                      /* «стоп»: больше не грузить */
static int changed;                      /* готовых картинок стало больше — нужен новый лист */

int image_changed(void) { int c = changed; changed = 0; return c; }

static int grow(void **p, unsigned *cap, unsigned need, size_t item) {
    if (need <= *cap) return 1;
    unsigned c = *cap ? *cap * 2 : 32;
    while (c < need) c *= 2;
    void *q = ts_core->realloc(*p, c * item);
    if (!q) return 0;
    *p = q; *cap = c;
    return 1;
}
static uint32_t str_add(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    if (str_len + n > str_cap) {
        size_t c = str_cap ? str_cap * 2 : 4096;
        while (c < str_len + n) c *= 2;
        char *q = ts_core->realloc(strs, c);
        if (!q) return 0;
        strs = q; str_cap = c;
    }
    memcpy(strs + str_len, s, n);
    str_len += n;
    return (uint32_t)(str_len - n);
}
static const char *str(uint32_t at) { return strs ? strs + at : ""; }

void image_clear(void) {
    changed = img_n > 0;
    img_n = place_n = 0;
    str_len = 0;
    pool_next = POOL_START;
    stopped = 0;
    str_add("");                                     /* смещение 0 — пустая строка */
}

void image_layout_begin(void) { place_n = 0; }
void image_stop(void) { stopped = 1; }

/* Картинка с этим адресом — номер; нет — новая. −1 — нет памяти. */
static int find_or_add(const char *url, const char *alt) {
    for (unsigned i = 0; i < img_n; i++) if (!strcmp(str(imgs[i].url), url)) return (int)i;
    if (!grow((void **)&imgs, &img_cap, img_n + 1, sizeof(img_t))) return -1;
    img_t *g = &imgs[img_n];
    memset(g, 0, sizeof(*g));
    g->url = str_add(url);
    g->alt = str_add(alt);
    return (int)img_n++;
}

/* Размер места: оба атрибута — они; один — второй по пропорции картинки (если она известна);
 * без атрибутов — размер картинки, пока неизвестен — заглушка 32×32. Шире места — уменьшить. */
static void box_size(const img_t *g, int aw, int ah, int max_w, int *w, int *h, int *sized) {
    int nw = g ? g->w : 0, nh = g ? g->h : 0;
    *sized = aw > 0 && ah > 0;
    if (aw > 0 && ah > 0) { *w = aw; *h = ah; }
    else if (aw > 0 && nw) { *w = aw; *h = (int)((long)nh * aw / nw); }
    else if (ah > 0 && nh) { *h = ah; *w = (int)((long)nw * ah / nh); }
    else if (nw) { *w = nw; *h = nh; }
    else if (aw > 0 || ah > 0) { *w = aw > 0 ? aw : ah; *h = ah > 0 ? ah : aw; }
    else { *w = *h = PLACEHOLDER; }
    if (max_w > 0 && *w > max_w) { *h = (int)((long)*h * max_w / *w); *w = max_w; }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    if (*h > 4000) *h = 4000;
}

int image_add(const char *url, const char *alt, int attr_w, int attr_h, int max_w, int link, int create,
              int *w, int *h) {
    int sized, gi = -1;
    if (create) gi = find_or_add(url, alt);
    else for (unsigned i = 0; i < img_n; i++) if (!strcmp(str(imgs[i].url), url)) gi = (int)i;
    box_size(gi >= 0 ? &imgs[gi] : NULL, attr_w, attr_h, max_w, w, h, &sized);
    if (!create || gi < 0 || !grow((void **)&places, &place_cap, place_n + 1, sizeof(place_t))) return -1;
    place_t *p = &places[place_n];
    memset(p, 0, sizeof(*p));
    p->img = gi; p->w = *w; p->h = *h; p->link = link; p->sized = (uint8_t)sized;
    p->aw = attr_w; p->ah = attr_h; p->max_w = max_w;
    return IMAGE_ID_BASE + (int)place_n++;
}

void image_place(int id, int x, int y, uint32_t bg) {
    unsigned k = (unsigned)(id - IMAGE_ID_BASE);
    if (k >= place_n) return;
    place_t *p = &places[k];
    p->x = x; p->y = y; p->bg = bg; p->placed = 1;
    page_widget(x, y, p->w, p->h, id, bg, p->link);
}

/* --- заглушка (дизайн Codex 08b): светлая плашка с рамкой, значок картинки, у крупной — надпись
 * «Loading image…» или «Image unavailable» и подпись (alt или имя файла) --- */
#define COL_PH_FILL  TSA_UI_COLOR_SOFT_RGB888
#define COL_PH_EDGE  TSA_UI_COLOR_LINE_RGB888
#define COL_PH_TEXT  TSA_UI_COLOR_MUTED_RGB888

void image_paint(int id, int x, int y, int w, int h, uint32_t bg) {
    unsigned k = (unsigned)(id - IMAGE_ID_BASE);
    if (k >= place_n) return;
    const img_t *g = &imgs[places[k].img];
    if (g->state == IMG_READY) return;                   /* картинка — в дисплей-листе поверх */
    int r = w >= 24 && h >= 24 ? 6 : 0;
    if (w >= 8 && h >= 8) {
        page_paint_round(x, y, w, h, r, COL_PH_EDGE, bg);
        page_paint_round(x + 1, y + 1, w - 2, h - 2, r ? r - 1 : 0, COL_PH_FILL, COL_PH_EDGE);
    } else {
        page_paint_round(x, y, w, h, 0, COL_PH_FILL, bg);
    }
    if (w < 28 || h < 28) return;
    int iy = h >= 96 ? y + 24 : y + (h - TSA_UI_ASSET_IMAGE_HEIGHT) / 2;
    page_paint_mask(x + (w - TSA_UI_ASSET_IMAGE_WIDTH) / 2, iy, TSA_UI_ASSET_IMAGE_WIDTH, TSA_UI_ASSET_IMAGE_HEIGHT,
                    ui_assets + TSA_UI_ASSET_IMAGE_OFFSET, TSA_UI_ASSET_IMAGE_STRIDE, COL_PH_TEXT, COL_PH_FILL);
    if (w < 120 || h < 96) return;
    uint16_t gl[96];
    const char *title = g->state == IMG_FAILED ? "Image unavailable" : "Loading image...";
    unsigned n = font_shape(title, (unsigned)strlen(title), gl, 96);
    /* надпись — самым крупным начертанием, которое влезает в плашку (у Codex — 20 px) */
    static const uint8_t faces[3] = {FACE_BODY, FACE_SMALL, FACE_CAPTION};
    unsigned face = FACE_COUNT;
    for (int k = 0; k < 3 && face == FACE_COUNT; k++)
        if ((int)n * font_faces[faces[k]].w <= w - 32) face = faces[k];
    if (face == FACE_COUNT) return;
    int tx = x + 16, ty = iy + TSA_UI_ASSET_IMAGE_HEIGHT + 10;
    page_paint_text(tx, ty, face, gl, n, COL_PH_TEXT, COL_PH_FILL, 0, x + 4, x + w - 4);
    if (h < 140) return;
    const char *cap = str(g->alt);
    if (!cap[0]) {                                       /* без подписи — имя файла из адреса */
        cap = str(g->url);
        const char *s = strrchr(cap, '/');
        if (s && s[1]) cap = s + 1;
    }
    n = font_shape(cap, (unsigned)strlen(cap), gl, 96);
    page_paint_text(tx, ty + font_faces[face].h + 4, FACE_CAPTION, gl, n, COL_PH_TEXT, COL_PH_FILL, 0, x + 4, x + w - 4);
}

/* Перерисовать места картинки g в странице (убрать заглушку). */
static void redraw(int gi) {
    for (unsigned k = 0; k < place_n; k++)
        if (places[k].img == gi && places[k].placed) page_redraw(places[k].y, places[k].h);
}

/* --- дисплей-лист --- */

void image_dl(ft_dl_t *d, int top) {
    int began = 0, drawn = 0;
    for (unsigned k = 0; k < place_n && drawn < DRAW_MAX; k++) {
        const place_t *p = &places[k];
        const img_t *g = &imgs[p->img];
        int sy = p->y - top;
        if (!p->placed || g->state != IMG_READY || sy >= TS_H || sy + p->h <= 0 || sy < -2000) continue;
        if (g->fmt == FT_L8) {                           /* серая: L8 — это прозрачность, под ней — чёрное */
            if (began) dl_put(d, dl_end());
            dl_put(d, dl_begin(FT_RECTS));
            dl_put(d, dl_color_rgb(0));
            dl_put(d, dl_line_width(16));
            dl_put(d, dl_vertex(p->x * 16, sy * 16));
            dl_put(d, dl_vertex((p->x + p->w - 1) * 16, (sy + p->h - 1) * 16));
            dl_put(d, dl_end());
            began = 0;
        }
        if (!began) {
            dl_put(d, dl_begin(FT_BITMAPS));
            dl_put(d, dl_color_a(255));
            began = 1;
        }
        dl_put(d, dl_color_rgb(0xFFFFFF));
        if (g->pal) dl_put(d, dl_palette_source(g->pal));
        /* картинка dw × dh в памяти — на место w × h: BITMAP_SIZE — размер места, а во сколько раз
         * растянуть — BITMAP_TRANSFORM_A/E (шаг по картинке на точку экрана, 8.8: 256 — 1:1) */
        dl_put(d, dl_source(g->addr));
        dl_put(d, (0x07u << 24) | ((uint32_t)g->fmt << 19) | ((g->stride & 0x3FFu) << 9) | (g->dh & 0x1FFu));
        dl_put(d, (0x28u << 24) | (((g->stride >> 10) & 3u) << 2) | ((g->dh >> 9) & 3u));
        dl_put(d, (0x08u << 24) | (((unsigned)p->w & 0x1FFu) << 9) | ((unsigned)p->h & 0x1FFu));
        dl_put(d, (0x29u << 24) | ((((unsigned)p->w >> 9) & 3u) << 2) | (((unsigned)p->h >> 9) & 3u));
        int scaled = p->w != g->dw || p->h != g->dh;
        if (scaled) {
            dl_put(d, (0x15u << 24) | ((uint32_t)((long)g->dw * 256 / p->w) & 0x1FFFFu));
            dl_put(d, (0x19u << 24) | ((uint32_t)((long)g->dh * 256 / p->h) & 0x1FFFFu));
        }
        dl_put(d, dl_vertex(p->x * 16, sy * 16));
        if (scaled) {                                    /* масштаб ручки — обратно 1:1 */
            dl_put(d, (0x15u << 24) | 256u);
            dl_put(d, (0x19u << 24) | 256u);
        }
        drawn++;
    }
    if (began) dl_put(d, dl_end());
}

/* --- загрузка --- */

const char *image_next(int top) {
    if (stopped) return NULL;
    int best = -1;
    long best_d = 0;
    for (unsigned k = 0; k < place_n; k++) {
        const place_t *p = &places[k];
        if (!p->placed || imgs[p->img].state != IMG_NEW) continue;
        /* расстояние от видимой части: видимые — 0, ниже — сколько ниже, выше — вдвое дальше */
        long d = p->y + p->h < top ? 2L * (top - p->y - p->h) : p->y > top + TS_H ? p->y - top - TS_H : 0;
        if (best < 0 || d < best_d) { best = (int)k; best_d = d; }
    }
    if (best < 0) return NULL;
    img_t *g = &imgs[places[best].img];
    g->state = IMG_LOADING;
    return str(g->url);
}

/* Место в RAM_G под bytes байт (по 32 — пакет SDRAM). 0 — нет места. */
static uint32_t pool_alloc(uint32_t bytes) {
    uint32_t at = (pool_next + 31u) & ~31u;
    if (at + bytes > POOL_END) return 0;
    pool_next = at + bytes;
    return at;
}

static uint32_t reg(uint32_t a) { uint32_t v = 0; ts_core->ft_read(a, &v); return v; }
static void reg_set(uint32_t a, uint32_t v) { ft_put(a, &v, 4); }

/* Сопроцессор в ошибке (REG_CMD_READ = 0xFFF) или занят чужим: сброс (как в PG FT81x, §5.6) */
static void copro_reset(void) {
    reg_set(REG_CPURESET, 1);
    ts_core->delay(5);
    reg_set(REG_CMD_READ, 0);
    reg_set(REG_CMD_WRITE, 0);
    reg_set(REG_CPURESET, 0);
}

static char copro_why[128];              /* причина последней ошибки сопроцессора — для журнала */
static int box_ok = 1;                   /* BL616 знает CMD_LOADIMAGE_BOX (прошивка 39+) */

/* Сопроцессор сбился (REG_CMD_READ = 0xFFF) — прочесть, почему, пока его не сбросили. BL616 39+
 * кладёт в начало RAM_CMD слово «IMG» + код, число и с байта 8 строку ASCII («too big: need
 * 5808360 bytes»); метка 0x04F2 — «неизвестная команда» (её оставляет и старая прошивка).
 * 1 — команда сопроцессору неизвестна. */
static int copro_report(void) {
    uint32_t w0 = reg(RAM_CMD);
    snprintf(copro_why, sizeof(copro_why), "not decoded");
    if (w0 == FAULT_UNKNOWN) { snprintf(copro_why, sizeof(copro_why), "unknown command"); return 1; }
    if ((w0 & 0xFFFFFFu) != IMG_REPORT) return 0;
    char s[121];
    for (unsigned i = 0; i < 120u; i += 4) {                 /* строка — словами, до нуля */
        uint32_t v = reg(RAM_CMD + 8u + i);
        memcpy(s + i, &v, 4);
        if (!(v & 0xFFu) || !(v & 0xFF00u) || !(v & 0xFF0000u) || !(v >> 24)) break;
    }
    s[120] = 0;
    if (s[0]) snprintf(copro_why, sizeof(copro_why), "%s", s);
    return 0;
}

/* Команда head[0..nh) (не больше 16 слов) и следом байты файла data — в кольцо RAM_CMD, сколько
 * влезает за раз (одно слово кольца всегда пустое: иначе полное кольцо не отличить от пустого).
 * Ждём, пока сопроцессор всё заберёт и выполнит. *ring — где в кольце легла команда (туда
 * сопроцессор пишет её выходные слова). 1 — выполнена; 0 — ошибка (картинка испорчена, не
 * поддерживается, не влезла; срок) — причина в copro_why; −1 — команда сопроцессору неизвестна. */
static int copro_send(const uint32_t *head, unsigned nh, const uint8_t *data, size_t len, uint32_t *ring) {
    static uint32_t buf[256];
    uint32_t wp = reg(REG_CMD_WRITE) & 4095u, rp = reg(REG_CMD_READ);
    if (rp == 0xFFFu || rp != wp) { copro_reset(); wp = 0; }
    if (ring) *ring = wp;
    size_t at = 0;
    int sent = 0;
    uint32_t last = ts_core->millis();
    while (!sent || at < len) {
        rp = reg(REG_CMD_READ);
        if (rp == 0xFFFu) { int unknown = copro_report(); copro_reset(); return unknown ? -1 : 0; }
        uint32_t room = (rp - wp - 4u) & 4095u;
        if (room < 64u) {                                /* меньше 16 слов — подождать */
            if ((uint32_t)(ts_core->millis() - last) > 10000u) {
                snprintf(copro_why, sizeof(copro_why), "coprocessor does not answer");
                copro_reset();
                return 0;
            }
            ts_core->delay(1);
            continue;
        }
        unsigned n = 0, cap = room / 4u > 256u ? 256u : room / 4u;
        if (!sent) {
            for (; n < nh; n++) buf[n] = head[n];
            sent = 1;
        }
        while (n < cap && at < len) {                     /* байты файла словами, хвост — нулями */
            uint32_t w = 0;
            size_t k = len - at < 4 ? len - at : 4;
            memcpy(&w, data + at, k);
            buf[n++] = w;
            at += k;
        }
        unsigned first = (4096u - wp) / 4u;
        if (first > n) first = n;
        if (!ft_put(RAM_CMD + wp, buf, first * 4u) ||
            (n > first && !ft_put(RAM_CMD, buf + first, (n - first) * 4u))) {
            snprintf(copro_why, sizeof(copro_why), "no link to the video chip");
            return 0;
        }
        wp = (wp + n * 4u) & 4095u;
        reg_set(REG_CMD_WRITE, wp);
        last = ts_core->millis();
    }
    for (;;) {                                           /* сопроцессор раскладывает остаток */
        rp = reg(REG_CMD_READ);
        if (rp == wp) return 1;
        if (rp == 0xFFFu) { int unknown = copro_report(); copro_reset(); return unknown ? -1 : 0; }
        if ((uint32_t)(ts_core->millis() - last) > 10000u) {
            snprintf(copro_why, sizeof(copro_why), "coprocessor does not answer");
            copro_reset();
            return 0;
        }
        ts_core->delay(1);
    }
}

/* CMD_IMAGEINFO: что вышло у последней картинки — источник w | h << 16, выход w | h << 16,
 * формат FT | прозрачность << 8 | шаг строки << 16, байтов палитры с ptr. Ответ сопроцессор
 * пишет на место четырёх выходных слов команды в кольце — оттуда и читаем. */
static int copro_info(uint32_t info[4]) {
    static const uint32_t head[5] = {CMD_IMAGEINFO, 0, 0, 0, 0};
    uint32_t pos;
    if (copro_send(head, 5, NULL, 0, &pos) != 1) return 0;
    for (unsigned i = 0; i < 4; i++) info[i] = reg(RAM_CMD + ((pos + 4u + 4u * i) & 4095u));
    return 1;
}

static void max_box(int gi, int *mw, int *mh);

/* JPEG или PNG «в рамку» (BL616 39+): рамка — самое большое место картинки на странице (если
 * размер задан в HTML), иначе ширина страницы; памяти — весь остаток RAM_G картинок (BL616
 * держит там и черновик: копию прогрессивного JPEG, строки PNG «через строку»). Не влезает —
 * BL616 сам уменьшит сильнее. Занято потом ровно столько, сколько вышло. −1 — BL616 не знает
 * команды (старая прошивка). */
static int copro_box(img_t *g, int gi, const uint8_t *body, size_t len, int *w, int *h, const char **why) {
    int mw, mh;
    max_box(gi, &mw, &mh);
    if (!mw) { mw = WIDE_MAX; mh = BOX_MAX; }
    if (mw > BOX_MAX) mw = BOX_MAX;
    if (mh > BOX_MAX) mh = BOX_MAX;
    uint32_t at = (pool_next + 31u) & ~31u;
    if (at + 64u > POOL_END) { *why = "no room in RAM_G"; return 0; }
    uint32_t head[5] = {CMD_LOADIMAGE_BOX, at, OPT_NODL, (uint32_t)mw | (uint32_t)mh << 16, POOL_END - at};
    int r = copro_send(head, 5, body, len, NULL);
    if (r <= 0) { *why = copro_why; return r; }
    uint32_t info[4];
    if (!copro_info(info)) { *why = "no image info"; return 0; }
    *w = (int)(info[0] & 0xFFFFu);
    *h = (int)(info[0] >> 16);
    uint32_t dw = info[1] & 0xFFFFu, dh = info[1] >> 16, stride = info[2] >> 16, pal = info[3];
    if (!dw || !dh || !stride || at + pal + stride * dh > POOL_END) { *why = "bad image info"; return 0; }
    g->fmt = (uint8_t)(info[2] & 0xFFu);
    g->alpha = (uint8_t)(info[2] >> 8 & 1u);
    g->pal = pal ? at : 0;
    g->addr = at + pal;
    g->stride = stride;
    g->dw = (uint16_t)dw;
    g->dh = (uint16_t)dh;
    pool_next = at + pal + stride * dh;
    return 1;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* JPEG: размер из заголовка кадра SOF; 1 — «baseline» (его понимает сопроцессор), 0 — иной. */
static int jpeg_info(const uint8_t *p, size_t n, int *w, int *h) {
    size_t i = 2;
    while (i + 9 < n) {
        if (p[i] != 0xFF) { i++; continue; }
        uint8_t m = p[i + 1];
        if (m == 0xFF) { i++; continue; }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
        size_t len = (size_t)(p[i + 2] << 8 | p[i + 3]);
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = p[i + 5] << 8 | p[i + 6];
            *w = p[i + 7] << 8 | p[i + 8];
            return m == 0xC0 || m == 0xC1;
        }
        i += 2 + len;
    }
    return 0;
}

/* PNG: размер, вид цвета; *npal — цветов палитры, *trns — есть tRNS. 1 — его понимает сопроцессор
 * (8 бит, без interlace, вид 0, 2, 3 или 6). */
static int png_info(const uint8_t *p, size_t n, int *w, int *h, int *ctype, int *npal, int *trns) {
    if (n < 33 || memcmp(p + 12, "IHDR", 4)) return 0;
    *w = (int)be32(p + 16);
    *h = (int)be32(p + 20);
    int depth = p[24];
    *ctype = p[25];
    int interlace = p[28];
    *npal = 0; *trns = 0;
    for (size_t i = 8; i + 8 <= n;) {
        uint32_t len = be32(p + i);
        if (!memcmp(p + i + 4, "PLTE", 4)) *npal = (int)(len / 3 > 256 ? 256 : len / 3);
        if (!memcmp(p + i + 4, "tRNS", 4)) *trns = 1;
        if (!memcmp(p + i + 4, "IDAT", 4)) break;
        i += 12 + len;
    }
    return depth == 8 && !interlace && (*ctype == 0 || *ctype == 2 || *ctype == 3 || *ctype == 6) &&
           *w > 0 && *h > 0 && *w < 2048 && *h < 2048;
}

/* Самое большое место картинки gi с размером из HTML: ESP-декодеру (GIF) крупнее не нужно. */
static void max_box(int gi, int *mw, int *mh) {
    *mw = *mh = 0;
    for (unsigned k = 0; k < place_n; k++) {
        const place_t *p = &places[k];
        if (p->img != gi) continue;
        if (!p->sized) { *mw = *mh = 0; return; }        /* есть место «по картинке» — нужен весь размер */
        if (p->w > *mw) *mw = p->w;
        if (p->h > *mh) *mh = p->h;
    }
}

/* --- память для libwebp (main/webp_alloc.h): PSRAM из таблицы прошивки --- */
void *tsafari_malloc(size_t n) { return ts_alloc(n ? n : 1); }
void *tsafari_calloc(size_t count, size_t size) {
    size_t n = count * size;
    if (size && n / size != count) return NULL;          /* переполнение — как у calloc */
    void *p = ts_alloc(n ? n : 1);
    if (p) memset(p, 0, n);
    return p;
}
void *tsafari_realloc(void *p, size_t n) { return ts_core->realloc(p, n); }
void tsafari_free(void *p) { if (p) ts_core->free(p); }

/* WebP: libwebp раскладывает сразу в наш буфер и сразу в формат видеочипа — RGB565 (без
 * прозрачности) или RGBA4444 (с ней; ниблы потом переставляются в ARGB4 видеочипа). Размер —
 * не больше места на странице (если оно задано в HTML) и ширины страницы, и чтобы влез в RAM_G. */
static int webp_load(img_t *g, int gi, const uint8_t *body, size_t len, int *w, int *h, const char **why) {
    WebPDecoderConfig cfg;
    if (!WebPInitDecoderConfig(&cfg) || WebPGetFeatures(body, len, &cfg.input) != VP8_STATUS_OK) {
        *why = "WebP damaged";
        return 0;
    }
    *w = cfg.input.width;
    *h = cfg.input.height;
    int mw, mh;
    max_box(gi, &mw, &mh);
    if (!mw) { mw = WIDE_MAX; mh = 4000; }
    int tw = *w, th = *h;
    if (tw > mw) { th = (int)((long)th * mw / tw); tw = mw; }
    if (th > mh) { tw = (int)((long)tw * mh / th); th = mh; }
    uint32_t room = POOL_END - ((pool_next + 31u) & ~31u);
    while ((uint32_t)tw * th * 2u > room && tw > 16 && th > 16) { tw = tw * 3 / 4; th = th * 3 / 4; }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (tw != *w || th != *h) {
        cfg.options.use_scaling = 1;
        cfg.options.scaled_width = tw;
        cfg.options.scaled_height = th;
    }
    int alpha = cfg.input.has_alpha;
    uint32_t bytes = ((uint32_t)tw * th * 2u + 3u) & ~3u;
    uint16_t *pix = ts_alloc(bytes);
    if (!pix) { *why = "no memory for WebP"; return 0; }
    cfg.output.colorspace = alpha ? MODE_RGBA_4444 : MODE_RGB_565;
    cfg.output.is_external_memory = 1;                   /* разбирать прямо в pix */
    cfg.output.u.RGBA.rgba = (uint8_t *)pix;
    cfg.output.u.RGBA.stride = tw * 2;
    cfg.output.u.RGBA.size = (size_t)tw * th * 2;
    if (WebPDecode(body, len, &cfg) != VP8_STATUS_OK) {
        WebPFreeDecBuffer(&cfg.output);
        ts_core->free(pix);
        *why = "WebP not decoded";
        return 0;
    }
    WebPFreeDecBuffer(&cfg.output);
    if (alpha)                                           /* RGBA4444 → ARGB4: альфа — в старший нибл */
        for (size_t i = 0; i < (size_t)tw * th; i++) pix[i] = (uint16_t)(pix[i] >> 4 | (pix[i] & 15u) << 12);
    uint32_t at = pool_alloc(bytes);
    int ok = at && ft_put(at, pix, bytes);
    ts_core->free(pix);
    if (!ok) { *why = "no room in RAM_G"; return 0; }
    g->fmt = alpha ? FT_ARGB4 : FT_RGB565;
    g->alpha = (uint8_t)alpha;
    g->addr = at;
    g->stride = (uint32_t)tw * 2u;
    g->dw = (uint16_t)tw;
    g->dh = (uint16_t)th;
    return 1;
}

/* JPEG или PNG командой CMD_LOADIMAGE, как у FT812 (BL616 старше 39): в полный размер, только
 * JPEG «baseline» и PNG по 8 бит без «через строку». */
static int copro_plain(img_t *g, int jpeg, const uint8_t *body, size_t len, int *w, int *h, const char **why) {
    uint32_t head[3] = {CMD_LOADIMAGE, 0, OPT_NODL};
    if (jpeg) {
        if (!jpeg_info(body, len, w, h)) { *why = "progressive JPEG"; return 0; }
        if (*w >= 2048 || *h >= 2048 || !(head[1] = pool_alloc((uint32_t)*w * *h * 2u))) { *why = "no room in RAM_G"; return 0; }
        if (copro_send(head, 3, body, len, NULL) != 1) { *why = "JPEG not decoded"; return 0; }
        g->fmt = FT_RGB565; g->addr = head[1]; g->stride = (uint32_t)*w * 2u;
        g->dw = (uint16_t)*w; g->dh = (uint16_t)*h;
        return 1;
    }
    int ct, npal, trns;
    if (!png_info(body, len, w, h, &ct, &npal, &trns)) { *why = "PNG kind not supported"; return 0; }
    uint32_t bpp = ct == 2 || ct == 6 ? 2u : 1u, pal = ct == 3 ? 2u * (uint32_t)npal : 0u;
    if (!(head[1] = pool_alloc(pal + (uint32_t)*w * *h * bpp))) { *why = "no room in RAM_G"; return 0; }
    if (copro_send(head, 3, body, len, NULL) != 1) { *why = "PNG not decoded"; return 0; }
    g->fmt = ct == 0 ? FT_L8 : ct == 2 ? FT_RGB565 : ct == 6 ? FT_ARGB4 : trns ? 15u : FT_PALETTED565;
    g->alpha = ct == 6 || (ct == 3 && trns);
    g->pal = pal ? head[1] : 0;
    g->addr = head[1] + pal;
    g->stride = (uint32_t)*w * bpp;
    g->dw = (uint16_t)*w; g->dh = (uint16_t)*h;
    return 1;
}

int image_loaded(const char *url, const uint8_t *body, size_t len) {
    int gi = -1;
    for (unsigned i = 0; i < img_n; i++) if (!strcmp(str(imgs[i].url), url)) { gi = (int)i; break; }
    if (gi < 0) return 0;
    img_t *g = &imgs[gi];
    int w = 0, h = 0, ok = 0;
    const char *why = "not an image";
    int jpeg = body && len >= 4 && body[0] == 0xFF && body[1] == 0xD8;
    if (jpeg || (body && len >= 8 && !memcmp(body, "\x89PNG", 4))) {           /* JPEG, PNG — BL616 */
        if (box_ok) {                                    /* BL616 39+: «в рамку» места на странице */
            ok = copro_box(g, gi, body, len, &w, &h, &why);
            if (ok < 0) {                                /* старая прошивка BL616 — как у FT812 */
                box_ok = 0;
                ts_log("tsafari: BL616 has no CMD_LOADIMAGE_BOX (firmware before 39): images as on FT812");
            }
        }
        if (!box_ok) ok = copro_plain(g, jpeg, body, len, &w, &h, &why);
    } else if (body && gif_is(body, len)) {                                    /* GIF — сам ESP32 */
        uint16_t *pix = NULL;
        int mw, mh, ow, oh;
        gif_size(body, len, &w, &h);
        max_box(gi, &mw, &mh);
        if (!mw) { mw = WIDE_MAX; mh = 4000; }
        if (!gif_decode(body, len, mw, mh, &pix, &ow, &oh)) why = "GIF not decoded";
        else {
            uint32_t bytes = ((uint32_t)ow * oh * 2u + 3u) & ~3u;
            uint32_t at = pool_alloc(bytes);
            if (!at || !ft_put(at, pix, bytes)) why = "no room in RAM_G";
            else {
                g->fmt = FT_ARGB1555; g->alpha = 1; g->addr = at; g->stride = (uint32_t)ow * 2u;
                g->dw = (uint16_t)ow; g->dh = (uint16_t)oh;
                ok = 1;
            }
            ts_core->free(pix);
        }
    } else if (body && len >= 12 && !memcmp(body, "RIFF", 4) && !memcmp(body + 8, "WEBP", 4)) {
        ok = webp_load(g, gi, body, len, &w, &h, &why);               /* WebP — libwebp на ESP32 */
    }
    if (w > 0 && h > 0) { g->w = (uint16_t)(w > 65535 ? 65535 : w); g->h = (uint16_t)(h > 65535 ? 65535 : h); }
    g->state = ok ? IMG_READY : IMG_FAILED;
    changed = 1;
    if (!ok) ts_log("tsafari: image %.120s: %s", url, body ? why : "not loaded");
    /* раскладка меняется, если место было «по картинке», а картинка оказалась другого размера */
    int relayout = 0;
    for (unsigned k = 0; k < place_n; k++) {
        const place_t *p = &places[k];
        if (p->img != gi || p->sized || !g->w) continue;
        int bw, bh, s;
        box_size(g, p->aw, p->ah, p->max_w, &bw, &bh, &s);
        if (bw != p->w || bh != p->h) relayout = 1;
    }
    if (!ok || g->alpha) redraw(gi);                    /* заглушку — прочь (у непрозрачной её закроет картинка) */
    return relayout;
}
