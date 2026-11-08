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
#include "svg.h"
#include "imgcache.h"
#include "css.h"
#include <math.h>
#include "src/webp/decode.h"
extern int VDAC4WebPRingOff;             /* libwebp (правка VDAC4): 1 — точки VP8L без кольца строк */
#include "page.h"
#include "font.h"
#include "ui_data.h"
#include "ui_design.h"
#include "ts.h"
#ifdef TSAFARI_HOST
#include <stdlib.h>                      /* стенд ПК: getenv для журнала TSAFARI_IMGLOG */
#endif

#define POOL_START 0x114000u             /* RAM_G картинок: после масок раскрытых списков форм */
#define POOL_END_2MB 0x200000u           /* конец пула: RAM_G 2 МБ */
#define POOL_END_3MB 0x300000u           /* RAM_G 3 МБ (ft_ramg_3mb): пул ~1,9 МБ */
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
/* Одной картинке — не больше 354 КБ (3/8 пула 2 МБ: две большие видимые встают вместе, и четверть
 * остаётся значкам, 07.11, zx-evolution.com). С пулом 3 МБ предел тот же: больше точек — больше
 * PSRAM разбору и дольше запись на карту, а на экране 1024×768 разница мала. */
#define IMG_CAP        ((POOL_END_2MB - POOL_START) / 8u * 3u)
#define ROOM_MIN       4096u             /* меньше места не стоит и пробовать */
#define KEEP_MAX       (1024u * 1024u)   /* файлов картинок в PSRAM — не больше 1 МБ на страницу */
#define PIX_MAX        (1024u * 1024u)   /* разобранных ESP32 точек в PSRAM — не больше 1 МБ: точки
                                          * WebP ESP32 считает секундами, а в RAM_G кладёт за доли
                                          * секунды. Картинки живут в RAM_G (пул 3 МБ), PSRAM нужна
                                          * разбору и загрузке (2 МБ кэша — «not loaded», 07.11) */
#define PREFETCH       TS_H              /* заранее грузится картинка не дальше экрана от видимой части */
#define ABANDON        (2 * TS_H)        /* картинка, что идёт по сети, ушла дальше — бросить её */

/* Состояния картинки. PARKED — была разложена (или пришла, а места не было) и убрана из RAM_G:
 * ушла далеко от видимой части, а место понадобилось ближним. Вернётся, когда снова подойдёт. */
enum { IMG_NEW, IMG_LOADING, IMG_READY, IMG_FAILED, IMG_PARKED };

typedef struct {
    uint32_t url, alt;                   /* строки в strs */
    uint8_t state, fmt, alpha;           /* IMG_…; формат FT (ft.h); есть прозрачность */
    uint16_t w, h;                       /* настоящий размер картинки (0 — ещё неизвестен) */
    uint16_t dw, dh;                     /* размер того, что лежит в RAM_G (бывает уменьшен) */
    uint32_t addr, pal, stride;          /* точки, палитра (0 — нет), байт в строке */
    uint32_t base, bytes;                /* занятый кусок пула RAM_G (палитра + точки); 0 — не занят */
    uint8_t *body;                       /* файл картинки в PSRAM — разложить снова без сети; NULL — нет */
    uint32_t body_len;
    uint16_t *pix;                       /* разобранные ESP32 точки в PSRAM — снова в RAM_G без разбора */
    uint32_t pix_bytes;
    uint32_t wait;                       /* места не хватило при epoch = wait: не пробовать, пока не сдвинется */
    uint8_t cache;                       /* файл пришёл из сети: 1 — в кэш на карте, 2 — в кэш заново */
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
static int stopped;                      /* «стоп»: больше не грузить */
static int changed;                      /* готовых картинок стало больше — нужен новый лист */
static uint32_t kept;                    /* сколько байт файлов картинок держим в PSRAM */
static uint32_t pix_kept;                /* сколько байт разобранных точек держим в PSRAM */
static uint32_t page_gen = 1;            /* номер страницы: итог фонового разбора прошлой — в мусор */
static int cur_top;                      /* прокрутка при последнем image_next */
static int cur_gi = -1;                  /* картинка, которую раскладываем: вытесняются только дальние */
static uint32_t pool_end = POOL_END_2MB; /* конец пула: 2 или 3 МБ RAM_G (image_pool) */
static uint32_t epoch = 1;               /* растёт, когда в пуле освобождается место или сдвинулась
                                          * прокрутка: картинкам, ждущим места, — пора пробовать снова */
static int epoch_top;                    /* прокрутка, при которой epoch рос последний раз */

int image_changed(void) { int c = changed; changed = 0; return c; }

void image_pool(int three_mb) { pool_end = three_mb ? POOL_END_3MB : POOL_END_2MB; }

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
    for (unsigned i = 0; i < img_n; i++) {
        if (imgs[i].body) ts_core->free(imgs[i].body);
        if (imgs[i].pix) ts_core->free(imgs[i].pix);
    }
    kept = pix_kept = 0;
    page_gen++;                                      /* идущий фоновый разбор — уже не нашей страницы */
    epoch++;
    img_n = place_n = 0;
    str_len = 0;
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

/* --- память видеочипа: пул картинок — кэш вокруг видимой части ---
 *
 * Весь пул — ~1,9 МБ (RAM_G 3 МБ, ft_ramg_3mb; со старым ядром — ~940 КБ), а на длинной странице
 * картинок бывает на несколько мегабайт (руководство
 * zx-evolution.com, 07.11: 27 картинок, после четырёх места не осталось). Поэтому в RAM_G лежат
 * картинки видимой части и около неё; ушедшая далеко убирается (PARKED), когда место нужно
 * ближней, а её файл остаётся в PSRAM — когда она снова подойдёт, она раскладывается заново без
 * сети. Видимые картинки не убираются никогда. */

static void max_box(int gi, int *mw, int *mh);

/* Как далеко место p от видимой части при прокрутке top: видно — 0, ниже — на сколько точек
 * ниже, выше — вдвое дальше (страницу чаще листают вниз). */
static long place_dist(const place_t *p, int top) {
    return p->y + p->h < top ? 2L * (top - p->y - p->h) : p->y > top + TS_H ? (long)(p->y - top - TS_H) : 0;
}

/* Картинка gi — по самому близкому из её мест. Ни одного места на странице — очень далеко. */
static long img_dist(int gi, int top) {
    long best = 0x7FFFFFFFL;
    for (unsigned k = 0; k < place_n; k++) {
        if (places[k].img != gi || !places[k].placed) continue;
        long d = place_dist(&places[k], top);
        if (d < best) best = d;
    }
    return best;
}

/* Расстояния всех картинок сразу — одним проходом по местам (img_dist для каждой — проход по
 * всем местам: на странице с сотнями значков выбор «самой дальней» в цикле стоил бы сотни
 * миллионов шагов, и браузер вставал бы). Массив — вызывающего (освободить); NULL — нет памяти,
 * тогда — img_dist по одной (dist_of). */
static long *img_dists(int top) {
    long *d = img_n ? ts_alloc(img_n * sizeof(long)) : NULL;
    if (!d) return NULL;
    for (unsigned i = 0; i < img_n; i++) d[i] = 0x7FFFFFFFL;
    for (unsigned k = 0; k < place_n; k++) {
        if (!places[k].placed || (unsigned)places[k].img >= img_n) continue;
        long v = place_dist(&places[k], top);
        if (v < d[places[k].img]) d[places[k].img] = v;
    }
    return d;
}
static long dist_of(const long *d, int gi, int top) { return d ? d[gi] : img_dist(gi, top); }

/* Свободные промежутки пула — между кусками картинок, что лежат в RAM_G (начало — по 32 байта,
 * пакет SDRAM). need > 0 — первый промежуток не короче need (ответ — его длина, 0 — такого нет);
 * need = 0 — самый большой. *at — его начало. */
static uint32_t pool_gap(uint32_t need, uint32_t *at) {
    uint32_t cur = POOL_START, best = 0, best_at = POOL_START;
    for (;;) {
        uint32_t next = pool_end, next_end = pool_end;  /* ближайший занятый кусок за cur */
        for (unsigned i = 0; i < img_n; i++) {
            const img_t *g = &imgs[i];
            if (g->bytes && g->base >= cur && g->base < next) { next = g->base; next_end = g->base + g->bytes; }
        }
        uint32_t a = (cur + 31u) & ~31u, len = next > a ? next - a : 0;
        if (need && len >= need) { *at = a; return len; }
        if (len > best) { best = len; best_at = a; }
        if (next >= pool_end) break;
        cur = next_end;
    }
    *at = best_at;
    return need ? 0 : best;
}

#ifdef TSAFARI_HOST
#define IMGLOG(...) do { if (getenv("TSAFARI_IMGLOG")) printf(__VA_ARGS__); } while (0)
#else
#define IMGLOG(...) do { } while (0)
#endif

/* Убрать картинку gi из RAM_G (её место нужно ближним): на её местах — снова заглушка. */
static void park(int gi) {
    img_t *g = &imgs[gi];
    IMGLOG("    PARK %s (for %s, top %d)\n", str(g->url), cur_gi >= 0 ? str(imgs[cur_gi].url) : "-", cur_top);
    g->state = IMG_PARKED;
    g->base = g->bytes = g->addr = g->pal = 0;
    g->wait = 0;
    epoch++;
    changed = 1;
    redraw(gi);
}

/* need байт подряд в пуле. Не хватает — убрать из RAM_G картинки дальше от видимой части, чем
 * та, что раскладывается (cur_gi), начиная с самой дальней; видимые не трогаются. Ответ — длина
 * найденного промежутка (≥ need — получилось; меньше — самый большой, какой есть), *at — начало. */
static uint32_t pool_make(uint32_t need, uint32_t *at) {
    uint32_t len = pool_gap(need, at);
    if (len) return len;
    long *dd = img_dists(cur_top);
    long mine = cur_gi >= 0 ? dist_of(dd, cur_gi, cur_top) : 0;
    for (;;) {
        int far = -1;
        long far_d = mine;
        for (unsigned i = 0; i < img_n; i++) {
            if ((int)i == cur_gi || !imgs[i].bytes) continue;
            long d = dist_of(dd, (int)i, cur_top);
            if (d > far_d) { far = (int)i; far_d = d; }
        }
        if (far < 0) { len = pool_gap(0, at); break; }
        park(far);
        if ((len = pool_gap(need, at)) != 0) break;
    }
    if (dd) ts_core->free(dd);
    return len;
}

/* Место в RAM_G под bytes байт (с уборкой дальних картинок). 0 — места нет. */
static uint32_t pool_alloc(uint32_t bytes) {
    uint32_t at;
    return pool_make(bytes, &at) >= bytes ? at : 0;
}

/* Сколько RAM_G, наверное, займёт картинка gi: по самому большому месту на странице (у RGB565 и
 * ARGB4 — 2 байта на точку), не больше 3/8 пула. Размер неизвестен (нет в HTML, ещё не пришла) —
 * четверть от этого предела. */
static uint32_t est_bytes(int gi) {
    const img_t *g = &imgs[gi];
    int mw, mh;
    max_box(gi, &mw, &mh);
    if (!mw && g->w && g->h) {
        mw = g->w < WIDE_MAX ? g->w : WIDE_MAX;
        mh = (int)((long)g->h * mw / g->w);
    }
    if (!mw) return IMG_CAP / 4u;
    uint32_t b = (uint32_t)mw * (uint32_t)(mh < 4000 ? mh : 4000) * 2u;
    return b < IMG_CAP ? b : IMG_CAP;
}

/* --- загрузка --- */

static int load(int gi, uint8_t *body, size_t len);
static int put_pix(img_t *g, int gi, const uint16_t *pix, uint32_t bytes);
static int job_busy(void);

/* Ближайшая к видимой части картинка, которой нет в RAM_G (новая или убранная). Убранная, чей
 * файл остался в PSRAM, раскладывается тут же, без сети (ответ — NULL); новой и убранной без
 * файла — адрес для загрузки. Картинка не видна, а места ей не будет даже после уборки дальних —
 * ждёт, пока страницу не прокрутят или место не освободится (g->wait). */
/* Прокрутка сейчас. Решение «что убрать из RAM_G» идёт по ней: со старой (пока картинка шла по сети
 * секунду-другую, страницу листали) видимую картинку можно было бы счесть далёкой и убрать.
 * Поэтому браузер сообщает её на каждом шаге, а не только в image_next. Сдвиг больше четверти
 * экрана — картинкам, ждущим места, пора пробовать снова. */
void image_view(int top) {
    if (top - epoch_top > TS_H / 4 || epoch_top - top > TS_H / 4) { epoch++; epoch_top = top; }
    cur_top = top;
}

const char *image_next(int top) {
    image_view(top);
    if (stopped) return NULL;
    int best = -1;
    long best_d = 0;
    for (unsigned k = 0; k < place_n; k++) {
        const place_t *p = &places[k];
        const img_t *g = &imgs[p->img];
        if (!p->placed || (g->state != IMG_NEW && g->state != IMG_PARKED) || g->wait == epoch) continue;
        long d = place_dist(p, top);
        /* Дальше экрана — не грузить заранее: картинки идут по одной, и далёкая WebP на мегабайт
         * (сеть и разбор — десятки секунд) держала бы видимые (07.11: заглавная JPEG
         * zx-evolution.com ждала WebP галереи, которую до того и не видели) */
        if (d > PREFETCH) continue;
        if (best < 0 || d < best_d) { best = p->img; best_d = d; }
    }
    if (best < 0) return NULL;
    img_t *g = &imgs[best];
    if (best_d > 0) {                                /* не видна: только если место будет */
        uint32_t at;
        int farther = 0;
        long *dd = img_dists(top);
        for (unsigned i = 0; i < img_n && !farther; i++)
            farther = imgs[i].bytes && dist_of(dd, (int)i, top) > best_d;
        if (dd) ts_core->free(dd);
        if (!farther && !pool_gap(est_bytes(best), &at)) { g->wait = epoch; return NULL; }
    }
    if (g->state == IMG_PARKED && g->pix) {          /* точки в PSRAM — сразу в RAM_G, без разбора */
        if (put_pix(g, best, g->pix, g->pix_bytes)) {
            g->state = IMG_READY;
            changed = 1;
            if (g->alpha) redraw(best);
        } else {
            g->wait = epoch;
        }
        return NULL;
    }
    if (job_busy()) return NULL;                     /* фоновая задача считает — подождать её */
    if (g->state == IMG_PARKED && g->body) {         /* файл есть — разложить снова, без сети */
        uint8_t *b = g->body;
        size_t n = g->body_len;
        g->body = NULL;                              /* файл — разбору (он вернёт его картинке) */
        g->body_len = 0;
        kept -= (uint32_t)n;
        load(best, b, n);
        return NULL;
    }
    g->state = IMG_LOADING;
    IMGLOG("    NEXT %s (top %d, %ld px away)\n", str(g->url), top, best_d);
    return str(g->url);
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
static int copro_code;                   /* её код из отчёта BL616 (4 — не влезла в room), 0 — нет */
static int box_ok = 1;                   /* BL616 знает CMD_LOADIMAGE_BOX (прошивка 39+) */

/* Сопроцессор сбился (REG_CMD_READ = 0xFFF) — прочесть, почему, пока его не сбросили. BL616 39+
 * кладёт в начало RAM_CMD слово «IMG» + код, число и с байта 8 строку ASCII («too big: need
 * 5808360 bytes»); метка 0x04F2 — «неизвестная команда» (её оставляет и старая прошивка).
 * 1 — команда сопроцессору неизвестна. */
static int copro_report(void) {
    uint32_t w0 = reg(RAM_CMD);
    snprintf(copro_why, sizeof(copro_why), "not decoded");
    copro_code = 0;
    if (w0 == FAULT_UNKNOWN) { snprintf(copro_why, sizeof(copro_why), "unknown command"); return 1; }
    if ((w0 & 0xFFFFFFu) != IMG_REPORT) return 0;
    copro_code = (int)(w0 >> 24);
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
    copro_code = 0;
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

/* JPEG или PNG «в рамку» (BL616 39+): рамка — самое большое место картинки на странице (если
 * размер задан в HTML), иначе ширина страницы; памяти — свободный промежуток пула (с уборкой
 * дальних картинок), но не больше 3/8 пула. Там BL616 держит и черновик: копию прогрессивного
 * JPEG, строки PNG «через строку». Не влезает — BL616 сам уменьшит сильнее. Занято потом ровно
 * столько, сколько вышло. 1 — готово; 0 — не разбирается; −1 — нет места (подождать);
 * −2 — BL616 не знает команды (старая прошивка). */
static int copro_box(img_t *g, int gi, const uint8_t *body, size_t len, int *w, int *h, const char **why) {
    int mw, mh;
    max_box(gi, &mw, &mh);
    if (!mw) { mw = WIDE_MAX; mh = BOX_MAX; }
    if (mw > BOX_MAX) mw = BOX_MAX;
    if (mh > BOX_MAX) mh = BOX_MAX;
    /* Выходу — не больше 3/8 пула, а копия файла (прогрессивный JPEG, WebP) — сверх того:
     * черновик BL616 временный, после разбора место снова свободно. */
    uint32_t cap = IMG_CAP + (uint32_t)len;
    uint32_t need = est_bytes(gi) + (uint32_t)len;
    uint32_t at, room = pool_make(need < cap ? need : cap, &at);
    if (room > cap) room = cap;
    if (room < ROOM_MIN) { *why = "no room in RAM_G"; return -1; }
    uint32_t head[5] = {CMD_LOADIMAGE_BOX, at, OPT_NODL, (uint32_t)mw | (uint32_t)mh << 16, room};
    int r = copro_send(head, 5, body, len, NULL);
    if (r < 0) return -2;
    /* не влезла в room (4) — подождать места; «велика» для памяти самого BL616 (3) при урезанном
     * room — тоже: с полным местом он выбирает другой путь (07.11: прогрессивный JPEG 1024×1024
     * рядом с большим логотипом) */
    if (!r) { *why = copro_why; return copro_code == 4 || (copro_code == 3 && room < cap) ? -1 : 0; }
    uint32_t info[4];
    if (!copro_info(info)) { *why = "no image info"; return 0; }
    *w = (int)(info[0] & 0xFFFFu);
    *h = (int)(info[0] >> 16);
    uint32_t dw = info[1] & 0xFFFFu, dh = info[1] >> 16, stride = info[2] >> 16, pal = info[3];
    if (!dw || !dh || !stride || pal + stride * dh > room) { *why = "bad image info"; return 0; }
    g->fmt = (uint8_t)(info[2] & 0xFFu);
    g->alpha = (uint8_t)(info[2] >> 8 & 1u);
    g->pal = pal ? at : 0;
    g->addr = at + pal;
    g->stride = stride;
    g->dw = (uint16_t)dw;
    g->dh = (uint16_t)dh;
    g->base = at;
    g->bytes = pal + stride * dh;
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

/* --- фоновый разбор: GIF, WebP и SVG считает задача ESP32 на ядре 1 ---
 *
 * Большой WebP ESP32 разбирает секунды (слайдер zx-evolution.com 2560×2560 — ~10 с). Если считать
 * его в задаче браузера, экран и ответы Evo стоят, а сторожевой таймер ESP-IDF через 5 с без отдыха
 * ядра перезапускает ESP (на плате 07.11: «Task watchdog», задача zm-app, внутри libwebp). Поэтому
 * GIF, WebP и SVG разбирает своя задача: приоритет 0 (как у простаивающей задачи FreeRTOS — та
 * получает своё время, и сторожевой таймер доволен), ядро 1 (там почти всё время простой; ответы
 * Evo — loopTask с приоритетом 1 — её вытесняют). Она раскладывает файл в точки видеочипа в PSRAM;
 * в RAM_G их кладёт сама задача браузера (image_poll) — доли секунды. Задание одно: пока оно не
 * кончилось, новые картинки не грузятся (image_next ждёт).
 *
 * Разобранные точки картинки остаются в PSRAM (пока их всех не больше PIX_MAX): убранная из RAM_G
 * картинка при прокрутке обратно встаёт сразу, без нового разбора. */

enum { JOB_FREE, JOB_QUEUED, JOB_RUNNING, JOB_DONE };
enum { KIND_GIF, KIND_WEBP, KIND_SVG };

typedef struct {
    int state;                           /* JOB_… — меняется атомарно (две задачи) */
    int gi;                              /* какая картинка */
    uint32_t page;                       /* какой страницы (image_clear меняет page_gen) */
    int kind, mw, mh;                    /* что разбирать; рамка — место на странице (0 — по картинке) */
    uint8_t *body;                       /* файл — у задания свой */
    size_t len;
    char bl616[128];                     /* почему не смог BL616 (WebP), пусто — не пробовал */
    /* итог — пишет фоновая задача */
    int ok, w, h, dw, dh;                /* разобрано; свой размер; размер точек */
    uint8_t fmt, alpha;
    uint16_t *pix;                       /* точки видеочипа (PSRAM), bytes — с дополнением до слова */
    uint32_t bytes;
    char why[160];                       /* почему не вышло */
    int oom;                             /* не хватило памяти (кэши освободить — и ещё раз) */
    int retried;                         /* сколько шагов освобождения памяти пройдено (free_for_job) */
#ifdef TSAFARI_HOST
    uint32_t ready_at;                   /* стенд «как на плате»: итог — не раньше этого времени */
#endif
} job_t;

static job_t job;
static void *job_signal;                 /* «есть задание» для фоновой задачи */
static volatile int worker_done = 1;     /* фоновая задача вышла (или не запускалась) */

/* Рамка точек: картинка w × h в рамку mw × mh (0 — не шире страницы), не больше 3/8 пула. */
static void fit_box(int w, int h, int mw, int mh, int *tw, int *th) {
    if (!mw) { mw = WIDE_MAX; mh = 4000; }
    *tw = w;
    *th = h;
    if (*tw > mw) { *th = (int)((long)*th * mw / *tw); *tw = mw; }
    if (*th > mh) { *tw = (int)((long)*tw * mh / *th); *th = mh; }
    while ((uint32_t)*tw * (uint32_t)*th * 2u > IMG_CAP && *tw > 16 && *th > 16) { *tw = *tw * 7 / 8; *th = *th * 7 / 8; }
    if (*tw < 1) *tw = 1;
    if (*th < 1) *th = 1;
}

/* WebP: libwebp раскладывает сразу в наш буфер и сразу в формат видеочипа — RGB565 (без
 * прозрачности) или RGBA4444 (с ней; ниблы потом переставляются в ARGB4 видеочипа), уменьшая до
 * рамки прямо при разборе. */
static void webp_pix(job_t *j) {
    WebPDecoderConfig cfg;
    if (!WebPInitDecoderConfig(&cfg) || WebPGetFeatures(j->body, j->len, &cfg.input) != VP8_STATUS_OK) {
        snprintf(j->why, sizeof(j->why), "WebP damaged");
        return;
    }
    j->w = cfg.input.width;
    j->h = cfg.input.height;
    int tw, th;
    fit_box(j->w, j->h, j->mw, j->mh, &tw, &th);
    if (tw != j->w || th != j->h) {
        cfg.options.use_scaling = 1;
        cfg.options.scaled_width = tw;
        cfg.options.scaled_height = th;
    }
    int alpha = cfg.input.has_alpha;
    uint32_t bytes = ((uint32_t)tw * th * 2u + 3u) & ~3u;
    uint16_t *pix = ts_alloc(bytes);
    if (!pix) { snprintf(j->why, sizeof(j->why), "no memory for WebP"); j->oom = 1; return; }
    cfg.output.colorspace = alpha ? MODE_RGBA_4444 : MODE_RGB_565;
    cfg.output.is_external_memory = 1;                   /* разбирать прямо в pix */
    cfg.output.u.RGBA.rgba = (uint8_t *)pix;
    cfg.output.u.RGBA.stride = tw * 2;
    cfg.output.u.RGBA.size = (size_t)tw * th * 2;
    VP8StatusCode st = WebPDecode(j->body, j->len, &cfg);
    if (st == VP8_STATUS_UNSUPPORTED_FEATURE) {
        /* Точки VP8L libwebp держит кольцом строк (правка VDAC4, third_party/libwebp/README.md),
         * а ссылка назад ушла дальше кольца — ещё раз со всей картинкой в памяти, как у самой
         * libwebp: вдруг памяти хватит (анимацию libwebp не разбирает — откажет и так). */
        WebPFreeDecBuffer(&cfg.output);
        VDAC4WebPRingOff = 1;
        st = WebPDecode(j->body, j->len, &cfg);
        VDAC4WebPRingOff = 0;
    }
    WebPFreeDecBuffer(&cfg.output);
    if (st != VP8_STATUS_OK) {
        ts_core->free(pix);
        j->oom = st == VP8_STATUS_OUT_OF_MEMORY;
        snprintf(j->why, sizeof(j->why), "WebP not decoded (%s)", st == VP8_STATUS_OUT_OF_MEMORY ? "no memory" :
                 st == VP8_STATUS_UNSUPPORTED_FEATURE ? "unsupported" : "damaged");
        return;
    }
    if (alpha)                                           /* RGBA4444 → ARGB4: альфа — в старший нибл */
        for (size_t i = 0; i < (size_t)tw * th; i++) pix[i] = (uint16_t)(pix[i] >> 4 | (pix[i] & 15u) << 12);
    j->pix = pix;
    j->bytes = bytes;
    j->dw = tw;
    j->dh = th;
    j->fmt = alpha ? FT_ARGB4 : FT_RGB565;
    j->alpha = (uint8_t)alpha;
    j->ok = 1;
}

/* SVG — nanosvg (svg.c). Векторную картинку незачем растягивать видеочипом: она рисуется сразу в
 * размер места на странице (размер из HTML; внутри места — с пропорциями картинки и по середине,
 * как у браузеров), без размера в HTML — в свой (width, height или viewBox), не шире страницы. Не
 * больше 3/8 пула. Рисуется полосами по SVG_BAND строк: RGBA только на полосу. Выход — ARGB4 (у SVG
 * почти всегда прозрачный фон). */
#define SVG_BAND 32
static void svg_pix(job_t *j) {
    svg_t *s = svg_open(j->body, j->len);
    if (!s) { snprintf(j->why, sizeof(j->why), "SVG not decoded"); return; }
    float sw, sh;
    svg_size(s, &sw, &sh);
    j->w = (int)ceilf(sw);
    j->h = (int)ceilf(sh);
    int tw, th;
    float scale, tx = 0.0f, ty = 0.0f;
    if (j->mw) {                                         /* место задано: вписать в него */
        tw = j->mw;
        th = j->mh;
        scale = (float)tw / sw < (float)th / sh ? (float)tw / sw : (float)th / sh;
        tx = ((float)tw - sw * scale) / 2.0f;
        ty = ((float)th - sh * scale) / 2.0f;
    } else {                                             /* свой размер, не шире страницы */
        tw = j->w < WIDE_MAX ? j->w : WIDE_MAX;
        scale = (float)tw / sw;
        th = (int)(sh * scale + 0.5f);
    }
    if (tw > BOX_MAX) tw = BOX_MAX;
    if (th > BOX_MAX) th = BOX_MAX;
    while ((uint32_t)tw * th * 2u > IMG_CAP && tw > 8 && th > 8) {   /* не больше 3/8 пула */
        tw = tw * 7 / 8;
        th = th * 7 / 8;
        scale = scale * 7.0f / 8.0f;
        tx = tx * 7.0f / 8.0f;
        ty = ty * 7.0f / 8.0f;
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    uint32_t bytes = ((uint32_t)tw * th * 2u + 3u) & ~3u;
    uint16_t *pix = ts_alloc(bytes);
    uint8_t *rgba = ts_alloc((size_t)tw * SVG_BAND * 4u);
    int ok = pix && rgba;
    for (int y0 = 0; ok && y0 < th; y0 += SVG_BAND) {
        int rows = th - y0 < SVG_BAND ? th - y0 : SVG_BAND;
        size_t n = (size_t)tw * (size_t)rows;
        uint16_t *px = pix + (size_t)y0 * (size_t)tw;
        ok = svg_render(s, scale, tx, ty, tw, y0, rows, rgba);
        for (size_t i = 0; ok && i < n; i++) {           /* RGBA 8888 → ARGB4 видеочипа */
            const uint8_t *c = rgba + i * 4u;
            px[i] = (uint16_t)((c[3] >> 4) << 12 | (c[0] >> 4) << 8 | (c[1] >> 4) << 4 | c[2] >> 4);
        }
    }
    svg_close(s);
    if (!ok) {
        snprintf(j->why, sizeof(j->why), pix && rgba ? "SVG not drawn" : "no memory for SVG");
        j->oom = !pix || !rgba;
        if (pix) ts_core->free(pix);
        if (rgba) ts_core->free(rgba);
        return;
    }
    ts_core->free(rgba);
    j->pix = pix;
    j->bytes = bytes;
    j->dw = tw;
    j->dh = th;
    j->fmt = FT_ARGB4;
    j->alpha = 1;
    j->ok = 1;
}

/* GIF — свой декодер ESP32 (gif.c, первый кадр) в ARGB1555: не больше места на странице и не
 * больше 3/8 пула. */
static void gif_pix(job_t *j) {
    uint16_t *pix = NULL;
    int mw = j->mw, mh = j->mh, ow, oh;
    gif_size(j->body, j->len, &j->w, &j->h);
    if (!mw) { mw = WIDE_MAX; mh = 4000; }
    if (j->w > 0 && mw > j->w) mw = j->w;                /* больше самой картинки рамка не нужна */
    if (j->h > 0 && mh > j->h) mh = j->h;
    while ((uint32_t)mw * mh * 2u > IMG_CAP && mw > 16 && mh > 16) { mw = mw * 7 / 8; mh = mh * 7 / 8; }
    if (!gif_decode(j->body, j->len, mw, mh, &pix, &ow, &oh)) { snprintf(j->why, sizeof(j->why), "GIF not decoded"); return; }
    j->pix = pix;
    j->bytes = ((uint32_t)ow * oh * 2u + 3u) & ~3u;
    j->dw = ow;
    j->dh = oh;
    j->fmt = FT_ARGB1555;
    j->alpha = 1;
    j->ok = 1;
}

static void run_job(job_t *j) {
    if (j->kind == KIND_WEBP) webp_pix(j);
    else if (j->kind == KIND_SVG) svg_pix(j);
    else gif_pix(j);
}

/* Фоновая задача: ждать задание (не дольше 200 мс — заметить остановку браузера), разобрать. */
static void image_worker(void *arg) {
    (void)arg;
    while (__atomic_load_n(&ts_running, __ATOMIC_ACQUIRE)) {
        ts_core->signal_wait(job_signal, 200);
        if (__atomic_load_n(&job.state, __ATOMIC_ACQUIRE) != JOB_QUEUED) continue;
        __atomic_store_n(&job.state, JOB_RUNNING, __ATOMIC_RELEASE);
        run_job(&job);
        __atomic_store_n(&job.state, JOB_DONE, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&worker_done, 1, __ATOMIC_RELEASE);
}

/* task_start(функция, аргумент, стек 8 КБ, приоритет 0, ядро 1): стек — libwebp ~2,5 КБ, nanosvg
 * ~4 КБ (замер -fstack-usage 07.11). На стенде ПК задачи нет: задание считается сразу. */
int image_init(void) {
#ifdef TSAFARI_HOST
    return 1;
#else
    job_signal = ts_core->signal_create();
    if (!job_signal) return 0;
    worker_done = 0;
    if (ts_core->task_start(image_worker, NULL, 8192, 0, 1) != ZM_OK) { worker_done = 1; return 0; }
    return 1;
#endif
}

int image_stopped(void) { return __atomic_load_n(&worker_done, __ATOMIC_ACQUIRE); }

static int job_busy(void) { return __atomic_load_n(&job.state, __ATOMIC_ACQUIRE) != JOB_FREE; }

/* Задание — фоновой задаче (на стенде ПК задачи нет: считается сразу). */
static void start_job(void) {
#ifdef TSAFARI_HOST
    run_job(&job);
    /* TSAFARI_SLOW=1: разбор «длится» 1 с + 1 с на каждые 100 КБ файла (на плате WebP 2560×2560 — ~10 с) */
    job.ready_at = getenv("TSAFARI_SLOW") ? ts_core->millis() + 1000u + (uint32_t)job.len / 100u : 0;
    __atomic_store_n(&job.state, JOB_DONE, __ATOMIC_RELEASE);
#else
    __atomic_store_n(&job.state, JOB_QUEUED, __ATOMIC_RELEASE);
    ts_core->signal_give(job_signal);
#endif
}

/* Отдать файл картинки gi фоновой задаче: файл (body) задание забирает себе. 2 — отдано;
 * −3 — задача занята (картинка возьмётся, как только она освободится; файл остаётся у вызвавшего). */
static int queue_job(int gi, int kind, uint8_t *body, size_t len, const char *bl616_why) {
    if (__atomic_load_n(&job.state, __ATOMIC_ACQUIRE) != JOB_FREE) return -3;
    memset(&job, 0, sizeof(job));
    job.gi = gi;
    job.page = page_gen;
    job.kind = kind;
    max_box(gi, &job.mw, &job.mh);
    job.body = body;
    job.len = len;
    if (bl616_why) snprintf(job.bl616, sizeof(job.bl616), "%s", bl616_why);
    start_job();
    return 2;
}

/* Разбору не хватило памяти: кэши — точки и файлы других картинок — освободить (они только
 * ускоряют возврат картинок). all = 0 — только тех, что сейчас не видны (видимые в RAM_G, и их
 * кэш понадобится, лишь когда они уйдут и вернутся); 1 — всех. 1 — что-то освободилось. */
static int drop_caches(int keep_gi, int all) {
    int freed = 0;
    long *dd = all ? NULL : img_dists(cur_top);
    for (unsigned i = 0; i < img_n; i++) {
        img_t *g = &imgs[i];
        if (!all && !dist_of(dd, (int)i, cur_top)) continue;
        if (g->pix) { ts_core->free(g->pix); pix_kept -= g->pix_bytes; g->pix = NULL; g->pix_bytes = 0; freed = 1; }
        if (g->body && (int)i != keep_gi) { ts_core->free(g->body); kept -= g->body_len; g->body = NULL; g->body_len = 0; freed = 1; }
    }
    if (dd) ts_core->free(dd);
    return freed;
}

/* Разбору не хватило памяти: освободить следующее по очереди — 0: очередь записи на карту
 * (imgcache.c: её файлы ждут в PSRAM, а нужны только следующим заходам), 1: кэши картинок, которых
 * не видно, 2: все кэши. Нечего на этом шаге — следующий. 1 — освободилось, разбирать снова. */
static int free_for_job(job_t *j) {
    while (j->retried < 3) {
        int step = j->retried++;
        /* шаг 0 — очередь записи на карту и таблицы стилей в памяти (css.h): они вернутся с карты */
        if (step == 0 ? (imgcache_trim() | css_mem_trim()) : drop_caches(j->gi, step == 2)) return 1;
    }
    return 0;
}

/* Раскладка меняется, если место было «по картинке», а картинка оказалась другого размера. */
static int relayout_needed(int gi) {
    const img_t *g = &imgs[gi];
    for (unsigned k = 0; k < place_n; k++) {
        const place_t *p = &places[k];
        if (p->img != gi || p->sized || !g->w) continue;
        int bw, bh, s;
        box_size(g, p->aw, p->ah, p->max_w, &bw, &bh, &s);
        if (bw != p->w || bh != p->h) return 1;
    }
    return 0;
}

/* Точки картинки (из фоновой задачи или из кэша PSRAM) — в RAM_G. 1 — легли; 0 — места нет. */
static int put_pix(img_t *g, int gi, const uint16_t *pix, uint32_t bytes) {
    cur_gi = gi;
    uint32_t at = pool_alloc(bytes);
    cur_gi = -1;
    if (!at || !ft_put(at, pix, bytes)) return 0;
    g->addr = at;
    g->pal = 0;
    g->stride = (uint32_t)g->dw * 2u;
    g->base = at;
    g->bytes = bytes;
    return 1;
}

/* Оставить точки картинки gi в PSRAM (кэш до PIX_MAX). Не влезают — убрать из кэша точки картинок,
 * что дальше от видимой части, чем она (самые дальние — первыми): раньше точки только что
 * разобранной картинки просто выбрасывались, если кэш был полон, — и WebP, которой не нашлось места
 * в RAM_G, при прокрутке к ней снова шла по сети и снова разбиралась десяток секунд, по кругу
 * (07.11: «картинки перезагружаются при скролле верх-вниз»). 1 — оставлены (теперь — картинки);
 * 0 — не влезают, буфер не тронут: вызывающий освободит его или отдаст кэшу на карте. */
static int keep_pix(img_t *g, int gi, uint16_t *pix, uint32_t bytes) {
    long *dd = !g->pix && pix_kept + bytes > PIX_MAX ? img_dists(cur_top) : NULL;
    long mine = dist_of(dd, gi, cur_top);
    while (!g->pix && pix_kept + bytes > PIX_MAX) {
        int far = -1;
        long far_d = mine;
        for (unsigned i = 0; i < img_n; i++) {
            if ((int)i == gi || !imgs[i].pix) continue;
            long d = dist_of(dd, (int)i, cur_top);
            if (d > far_d) { far = (int)i; far_d = d; }
        }
        if (far < 0) break;
        ts_core->free(imgs[far].pix);
        pix_kept -= imgs[far].pix_bytes;
        imgs[far].pix = NULL;
        imgs[far].pix_bytes = 0;
    }
    if (dd) ts_core->free(dd);
    if (g->pix || pix_kept + bytes > PIX_MAX) return 0;
    g->pix = pix;
    g->pix_bytes = bytes;
    pix_kept += bytes;
    return 1;
}

/* --- точки экрана в кэше на карте (imgcache.h, IMGCACHE_PIX) ---
 * Заголовок PIX_HEAD байт (числа младшим байтом вперёд): свой размер картинки w, h (по нему
 * раскладка страницы), размер точек dw, dh, формат FT, прозрачность, рамка разбора mw, mh (место на
 * странице, 0 — по картинке: файл с другой рамкой этому месту не подходит), 0, 0; за ним — точки,
 * dw × dh по 2 байта, с дополнением до слова. */
#define PIX_HEAD 16
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static unsigned get16(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }

static void pix_head(uint8_t *p, const img_t *g, int mw, int mh) {
    put16(p, g->w);
    put16(p + 2, g->h);
    put16(p + 4, g->dw);
    put16(p + 6, g->dh);
    p[8] = g->fmt;
    p[9] = g->alpha;
    put16(p + 10, (unsigned)mw);
    put16(p + 12, (unsigned)mh);
    p[14] = p[15] = 0;
}

/* Точки картинки gi готовы (разобраны или прочитаны с карты; dw, dh, fmt, alpha — уже в g): в
 * RAM_G (места нет — ждать), в кэш PSRAM; если картинка пришла из сети (g->cache) — и в кэш на
 * карте: копией, если остались в PSRAM, иначе буфер отдаётся кэшу. Буфер pix — больше не
 * вызывающего. mw, mh — рамка разбора (в заголовок файла кэша). */
static void pix_ready(img_t *g, int gi, uint16_t *pix, uint32_t bytes, int mw, int mh) {
    if (put_pix(g, gi, pix, bytes)) {
        g->state = IMG_READY;
    } else {                                             /* места нет — подождать, точки — в кэше */
        g->state = IMG_PARKED;
        g->wait = epoch;
    }
    int keep = keep_pix(g, gi, pix, bytes);
    if (g->cache) {
        uint8_t pre[PIX_HEAD];
        pix_head(pre, g, mw, mh);
        if (keep) imgcache_store(str(g->url), IMGCACHE_PIX, pre, PIX_HEAD, (const uint8_t *)pix, bytes, g->cache != 1);
        else imgcache_give(str(g->url), IMGCACHE_PIX, pre, PIX_HEAD, (uint8_t *)pix, bytes, g->cache != 1);
        g->cache = 0;
    } else if (!keep) {
        ts_core->free(pix);
    }
    changed = 1;
}

int image_poll(void) {
    if (__atomic_load_n(&job.state, __ATOMIC_ACQUIRE) != JOB_DONE) return 0;
#ifdef TSAFARI_HOST
    if (job.ready_at && (int32_t)(ts_core->millis() - job.ready_at) < 0) return 0;
#endif
    job_t *j = &job;
    int relayout = 0;
    /* Не хватило памяти: освободить по шагам (free_for_job) — и ещё раз */
    if (!j->ok && j->oom && j->page == page_gen && free_for_job(j)) {
        ts_log("tsafari: image %.120s: no memory, caches dropped (step %d), decoding again", str(imgs[j->gi].url),
               j->retried);
        j->oom = 0;
        j->why[0] = 0;
        start_job();
        return 0;
    }
    IMGLOG("    POLL %s: ok %d, page %s\n", (unsigned)j->gi < img_n ? str(imgs[j->gi].url) : "-", j->ok,
           j->page == page_gen ? "same" : "other");
    if (j->page == page_gen && (unsigned)j->gi < img_n) {
        int gi = j->gi;
        img_t *g = &imgs[gi];
        if (j->w > 0 && j->h > 0) { g->w = (uint16_t)(j->w > 65535 ? 65535 : j->w); g->h = (uint16_t)(j->h > 65535 ? 65535 : j->h); }
        int cache = g->cache, file_to_card = 0;
        if (j->ok) {
            g->fmt = j->fmt;
            g->alpha = j->alpha;
            g->dw = (uint16_t)j->dw;
            g->dh = (uint16_t)j->dh;
            /* В кэш на карте: WebP — точками экрана (разбор — секунды, с карты встанет сразу), GIF и
             * SVG — тем, что меньше: SVG рисуется быстро, а точки логотипа в 3,7 КБ — 300 КБ, их
             * запись и чтение через Z80 дольше рисования (стенд 07.11) */
            int as_pix = j->kind == KIND_WEBP || j->bytes <= j->len;
            if (cache && !as_pix) {
                g->cache = 0;
                file_to_card = cache == 1 || cache == 2;     /* 3 — файл и так с карты */
            }
            pix_ready(g, gi, j->pix, j->bytes, j->mw, j->mh);
            j->pix = NULL;
        } else {
            g->state = IMG_FAILED;
            if (j->bl616[0]) ts_log("tsafari: image %.120s: %s; BL616: %s", str(g->url), j->why, j->bl616);
            else ts_log("tsafari: image %.120s: %s", str(g->url), j->why);
        }
        /* Файл держать себе стоит, только если точки картинки не остались в PSRAM: с ними файл не
         * нужен, а память нужна разбору (WebP 2560×2560 — файл 1,2 МБ). Мелкий (SVG) — пусть будет. */
        int own = !g->body && (!g->pix || j->len <= 16384u) && kept + j->len <= KEEP_MAX;
        g->cache = 0;                                    /* не разобралась — в кэш нечего */
        if (file_to_card && j->body) {                   /* в кэш на карте файлом */
            if (own) {
                imgcache_store(str(g->url), IMGCACHE_FILE, NULL, 0, j->body, j->len, cache == 2);   /* копией */
            } else {
                imgcache_give(str(g->url), IMGCACHE_FILE, NULL, 0, j->body, j->len, cache == 2);    /* без копии */
                j->body = NULL;
            }
        }
        if (j->body && own) {                            /* файл — себе: разобрать снова без сети */
            g->body = j->body;
            g->body_len = (uint32_t)j->len;
            kept += (uint32_t)j->len;
            j->body = NULL;
        }
        changed = 1;
        if (g->state != IMG_READY || g->alpha) redraw(gi);   /* заглушку — прочь */
        relayout = relayout_needed(gi);
    }
    if (j->pix) ts_core->free(j->pix);
    if (j->body) ts_core->free(j->body);
    j->pix = NULL;
    j->body = NULL;
    __atomic_store_n(&job.state, JOB_FREE, __ATOMIC_RELEASE);
    return relayout;
}

/* JPEG или PNG командой CMD_LOADIMAGE, как у FT812 (BL616 старше 39): в полный размер, только
 * JPEG «baseline» и PNG по 8 бит без «через строку». Больше 3/8 пула — не разложить вовсе;
 * −1 — сейчас места нет (подождать). */
static int copro_plain(img_t *g, int jpeg, const uint8_t *body, size_t len, int *w, int *h, const char **why) {
    uint32_t head[3] = {CMD_LOADIMAGE, 0, OPT_NODL}, bytes;
    if (jpeg) {
        if (!jpeg_info(body, len, w, h)) { *why = "progressive JPEG"; return 0; }
        bytes = (uint32_t)*w * (uint32_t)*h * 2u;
        if (*w >= 2048 || *h >= 2048 || bytes > IMG_CAP) { *why = "too big for RAM_G"; return 0; }
        if (!(head[1] = pool_alloc(bytes))) { *why = "no room in RAM_G"; return -1; }
        if (copro_send(head, 3, body, len, NULL) != 1) { *why = "JPEG not decoded"; return 0; }
        g->fmt = FT_RGB565; g->addr = head[1]; g->stride = (uint32_t)*w * 2u;
        g->dw = (uint16_t)*w; g->dh = (uint16_t)*h;
        g->base = head[1]; g->bytes = bytes;
        return 1;
    }
    int ct, npal, trns;
    if (!png_info(body, len, w, h, &ct, &npal, &trns)) { *why = "PNG kind not supported"; return 0; }
    uint32_t bpp = ct == 2 || ct == 6 ? 2u : 1u, pal = ct == 3 ? 2u * (uint32_t)npal : 0u;
    bytes = pal + (uint32_t)*w * (uint32_t)*h * bpp;
    if (bytes > IMG_CAP) { *why = "too big for RAM_G"; return 0; }
    if (!(head[1] = pool_alloc(bytes))) { *why = "no room in RAM_G"; return -1; }
    if (copro_send(head, 3, body, len, NULL) != 1) { *why = "PNG not decoded"; return 0; }
    g->fmt = ct == 0 ? FT_L8 : ct == 2 ? FT_RGB565 : ct == 6 ? FT_ARGB4 : trns ? 15u : FT_PALETTED565;
    g->alpha = ct == 6 || (ct == 3 && trns);
    g->pal = pal ? head[1] : 0;
    g->addr = head[1] + pal;
    g->stride = (uint32_t)*w * bpp;
    g->dw = (uint16_t)*w; g->dh = (uint16_t)*h;
    g->base = head[1]; g->bytes = bytes;
    return 1;
}

/* Разобрать файл картинки по его первым байтам. 1 — лежит в RAM_G; 0 — не разбирается;
 * −1 — места в RAM_G сейчас нет (картинка подождёт, файл — в PSRAM); 2 — отдан фоновой задаче;
 * −3 — фоновая задача занята (картинка подождёт её).
 *
 * WebP — BL616, если влезет (с прошивки 40 он их разбирает сам, ESP32 свободен для сети), иначе
 * ESP32 своей libwebp. Памяти у BL616 мало (~290 КБ, VDAC4/Tang Nano 20k/BL616/docs/images.md):
 * прозрачность ему стоит 2 байта на точку всей картинки, без потерь — 4 (до ~240×240), а файл
 * больше 48 КБ он кладёт в RAM_G рядом с выходом. Такие — сразу ESP32: его libwebp держит
 * прозрачность окном строк, а точки VP8L — кольцом (правка VDAC4, third_party/libwebp/README.md),
 * и слайдер zx-evolution.com 2560×2560 ей по силам (07.11). Отказ BL616 по WebP — тоже ESP32;
 * анимацию libwebp ESP32 не разбирает — она только BL616 (первый кадр). */
static int webp_for_bl616(const uint8_t *body, size_t len) {
    WebPBitstreamFeatures f;
    if (WebPGetFeatures(body, len, &f) != VP8_STATUS_OK) return 0;
    if (f.has_animation) return 1;
    uint32_t px = (uint32_t)f.width * (uint32_t)f.height;
    if (len > 256u * 1024u) return 0;                    /* файл с выходом не влезет в room */
    if (f.format == 2) return px <= 50000u;              /* без потерь: 4 байта на точку */
    if (f.has_alpha) return px <= 100000u;               /* прозрачность: 2 байта на точку */
    return 1;
}

static int decode(img_t *g, int gi, uint8_t *body, size_t len, int *w, int *h, const char **why) {
    *why = "not an image";
    int jpeg = len >= 4 && body[0] == 0xFF && body[1] == 0xD8;
    int webp = len >= 12 && !memcmp(body, "RIFF", 4) && !memcmp(body + 8, "WEBP", 4);
    const char *bl616_why = NULL;                        /* почему WebP не разобрал BL616 */
    int to_bl616 = !webp || webp_for_bl616(body, len);
    if (to_bl616 && (jpeg || webp || (len >= 8 && !memcmp(body, "\x89PNG", 4)))) {   /* JPEG, PNG, WebP — BL616 */
        if (box_ok) {                                    /* BL616 39+: «в рамку» места на странице */
            int r = copro_box(g, gi, body, len, w, h, why);
            if (r != -2 && !(webp && r <= 0)) return r;
            if (r == -2) {                               /* старая прошивка BL616 — как у FT812 */
                box_ok = 0;
                ts_log("tsafari: BL616 has no CMD_LOADIMAGE_BOX (firmware before 39): images as on FT812");
            } else {
                bl616_why = *why;
            }
        }
        if (!webp) return copro_plain(g, jpeg, body, len, w, h, why);
    }
    /* GIF, SVG, WebP — фоновая задача ESP32 */
    return gif_is(body, len) ? queue_job(gi, KIND_GIF, body, len, NULL) :
           svg_is(body, len) ? queue_job(gi, KIND_SVG, body, len, NULL) :
           webp ? queue_job(gi, KIND_WEBP, body, len, bl616_why) : 0;
}

/* Разложить файл картинки gi в RAM_G (body = NULL — не загрузилась). Файл (body) — свой: он
 * остаётся в PSRAM (пока их всех не больше KEEP_MAX), чтобы картинку, убранную из RAM_G, разложить
 * снова без сети, иначе освобождается; места сейчас нет — картинка ждёт (PARKED); GIF, WebP и SVG
 * уходят фоновой задаче вместе с файлом — их итог примет image_poll. 1 — раскладка страницы
 * меняется (см. image_loaded). */
static int load(int gi, uint8_t *body, size_t len) {
    img_t *g = &imgs[gi];
    int w = 0, h = 0;
    const char *why = "not loaded";
    cur_gi = gi;
    int r = body ? decode(g, gi, body, len, &w, &h, &why) : 0;
    cur_gi = -1;
    IMGLOG("    LOAD %s: %u bytes, r %d\n", str(g->url), (unsigned)len, r);
    if (r == 2) {                                        /* считает фоновая задача */
        g->state = IMG_LOADING;
        return 0;
    }
    if (w > 0 && h > 0) { g->w = (uint16_t)(w > 65535 ? 65535 : w); g->h = (uint16_t)(h > 65535 ? 65535 : h); }
    if (body && r > 0 && g->cache) {                     /* разложил BL616 — в кэш на карте файлом */
        if (g->cache == 3) {
            /* файл и так с карты */
        } else if (!g->body && kept + len <= KEEP_MAX) {
            imgcache_store(str(g->url), IMGCACHE_FILE, NULL, 0, body, len, g->cache == 2);   /* копией: и себе */
        } else {
            imgcache_give(str(g->url), IMGCACHE_FILE, NULL, 0, body, len, g->cache == 2);    /* без копии */
            body = NULL;
        }
        g->cache = 0;
    }
    if (body && r && !g->body && kept + len <= KEEP_MAX) {   /* файл — картинке */
        g->body = body;
        g->body_len = (uint32_t)len;
        kept += (uint32_t)len;
    } else if (body) {
        ts_core->free(body);
    }
    if (r > 0) g->state = IMG_READY;
    else if (r == -3) g->state = IMG_PARKED;            /* фоновая задача занята — сразу после неё */
    else if (r < 0) { g->state = IMG_PARKED; g->wait = epoch; }   /* места нет — до прокрутки */
    else g->state = IMG_FAILED;
    changed = 1;
    if (!r) ts_log("tsafari: image %.120s: %s", str(g->url), why);
    if (r <= 0 || g->alpha) redraw(gi);                 /* заглушку — прочь (у непрозрачной её закроет картинка) */
    return relayout_needed(gi);
}

int image_loaded(const char *url, uint8_t *body, size_t len, int cache) {
    for (unsigned i = 0; i < img_n; i++)
        if (!strcmp(str(imgs[i].url), url)) {
            imgs[i].cache = (uint8_t)(body ? cache : 0);
            return load((int)i, body, len);
        }
    if (body) ts_core->free(body);
    return 0;
}

int image_cached(const char *url, uint8_t *blob, size_t len) {
    int gi = -1;
    for (unsigned i = 0; i < img_n && gi < 0; i++)
        if (!strcmp(str(imgs[i].url), url)) gi = (int)i;
    if (gi < 0) {                                        /* страница уже другая */
        if (blob) ts_core->free(blob);
        return 0;
    }
    img_t *g = &imgs[gi];
    int mw, mh;
    max_box(gi, &mw, &mh);
    unsigned dw = 0, dh = 0, fmt = 0;
    size_t bytes = 0;
    if (blob && len > PIX_HEAD) {
        dw = get16(blob + 4);
        dh = get16(blob + 6);
        fmt = blob[8];
        bytes = len - PIX_HEAD;
    }
    if (!dw || !dh || bytes != (((size_t)dw * dh * 2u + 3u) & ~(size_t)3u) ||
        (fmt != FT_RGB565 && fmt != FT_ARGB4 && fmt != FT_ARGB1555) ||
        get16(blob + 10) != (unsigned)mw || get16(blob + 12) != (unsigned)mh) {
        IMGLOG("    CARD %s: pixels do not fit this place (%dx%d)\n", url, mw, mh);
        if (blob) ts_core->free(blob);
        return -1;
    }
    g->w = (uint16_t)get16(blob);
    g->h = (uint16_t)get16(blob + 2);
    g->dw = (uint16_t)dw;
    g->dh = (uint16_t)dh;
    g->fmt = (uint8_t)fmt;
    g->alpha = blob[9];
    g->cache = 0;
    memmove(blob, blob + PIX_HEAD, bytes);               /* точки — в начало буфера */
    IMGLOG("    CARD %s: pixels %ux%u, no decoding\n", url, dw, dh);
    pix_ready(g, gi, (uint16_t *)blob, (uint32_t)bytes, mw, mh);
    if (g->state != IMG_READY || g->alpha) redraw(gi);  /* заглушку — прочь */
    return relayout_needed(gi);
}

int image_far(const char *url, int top) {
    for (unsigned i = 0; i < img_n; i++)
        if (!strcmp(str(imgs[i].url), url)) return img_dist((int)i, top) > ABANDON;
    return 0;
}

void image_requeue(const char *url) {
    for (unsigned i = 0; i < img_n; i++)
        if (!strcmp(str(imgs[i].url), url) && imgs[i].state == IMG_LOADING) {
            imgs[i].state = IMG_NEW;
            imgs[i].cache = 0;
            IMGLOG("    DROP %s (top %d)\n", url, cur_top);
        }
}

#ifdef TSAFARI_HOST
uint32_t image_host_kept(void) { return pix_kept + kept; }

void image_host_stats(int top, int *seen, int *ready, int *failed, uint32_t *used) {
    *seen = *ready = *failed = 0;
    *used = 0;
    if (getenv("TSAFARI_PLACES"))                        /* все места: где, какого размера, что с картинкой */
        for (unsigned k = 0; k < place_n; k++)
            printf("    place %u: y %d..%d w %d, %s state %d, RAM_G %u, pix %u, body %u, dist %ld\n", k, places[k].y,
                   places[k].y + places[k].h, places[k].w, str(imgs[places[k].img].url), imgs[places[k].img].state,
                   (unsigned)imgs[places[k].img].bytes, (unsigned)imgs[places[k].img].pix_bytes,
                   (unsigned)imgs[places[k].img].body_len, place_dist(&places[k], top));
    for (unsigned k = 0; k < place_n; k++) {
        const place_t *p = &places[k];
        if (!p->placed || place_dist(p, top)) continue;
        (*seen)++;
        if (imgs[p->img].state == IMG_READY) (*ready)++;
        else if (imgs[p->img].state == IMG_FAILED) (*failed)++;
        else printf("    not ready: %s state %d wait %d pix %d body %d job %d\n", str(imgs[p->img].url),
                    imgs[p->img].state, imgs[p->img].wait == epoch, imgs[p->img].pix != NULL,
                    imgs[p->img].body != NULL, job.state);
    }
    for (unsigned i = 0; i < img_n; i++) *used += imgs[i].bytes;
}

int image_host_size(const char *tail, int *w, int *h) {
    size_t tl = strlen(tail);
    for (unsigned k = 0; k < place_n; k++) {
        const char *u = str(imgs[places[k].img].url);
        size_t ul = strlen(u);
        if (ul >= tl && !strcmp(u + ul - tl, tail)) { *w = places[k].w; *h = places[k].h; return 1; }
    }
    return 0;
}

int image_host_rect(const char *tail, int *x, int *y, int *w, int *h) {
    size_t tl = strlen(tail);
    for (unsigned k = 0; k < place_n; k++) {
        const char *u = str(imgs[places[k].img].url);
        size_t ul = strlen(u);
        if (ul >= tl && !strcmp(u + ul - tl, tail)) {
            *x = places[k].x; *y = places[k].y; *w = places[k].w; *h = places[k].h;
            return 1;
        }
    }
    return 0;
}
#endif
