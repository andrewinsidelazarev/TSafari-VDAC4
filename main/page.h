/* Страница браузера: что на ней нарисовать и как она лежит в памяти видеочипа.
 *
 * Страница — список «элементов»: прямоугольников (фоны, линии) и строк текста. Их составляет
 * doc.c, а здесь они превращаются в точки. Координаты — в точках от левого верхнего угла
 * страницы; страница бывает много выше экрана, и показывается её кусок высотой 768 строк.
 *
 * Как точки лежат в видеочипе — «кольцо» (см. page.c): 896 строк по 1024 байта в RAM_G,
 * байт на точку — номер цвета в палитре (формат PALETTED565, ft.h). */
#pragma once
#include <stdint.h>
#include "ft.h"

#define PAGE_RING_ROWS 896u              /* 768 видимых + 128 строк запаса под прокрутку */
#define PAGE_ROW_BYTES 1024u             /* байт в строке кольца: точка = 1 байт */
#define PAGE_STEP      128               /* сдвиг за кадр: новые строки — только в невидимые */
#define PAGE_LINE_H    34                /* строка основного текста: ячейка 12×34 (дизайн) */
#define PAGE_ASCENT    26                /* базовая линия от верха строки */
#define PAGE_MARGIN    8                 /* поля страницы — экран по максимуму */

#define RAMG_RING 0x000000u              /* 1024 × 896 = 896 КБ */
#define RAMG_PAL  0x0E0000u              /* две палитры по 256 × RGB565 */

#define PAGE_UNDERLINE 1u                /* флаг строки текста: подчеркнуть (ссылки) */
#define PAGE_BOLD      2u                /* жирный: глиф ещё раз на точку правее (дизайн Codex) */
#define PAGE_STRIKE    4u                /* зачёркнутый (<s>, <del>, text-decoration: line-through) */

/* Начать новую страницу: цвет фона bg (0xRRGGBB), пустой список элементов, новая палитра. */
void page_begin(uint32_t bg);
/* Прямоугольник цвета rgb (страница, точки); номер — для page_rect_height (−1 — нет памяти). */
int page_rect(int x, int y, int w, int h, uint32_t rgb);
/* Высота прямоугольника, когда она стала известна (фон блока под абзацами). */
void page_rect_height(int index, int h);
/* Коробка CSS (doc_cssbox_begin): заливка fill, рамка border толщиной bw (0 — без рамки), углы
 * скруглены радиусом radius (0 — прямые); under — цвет под коробкой (к нему сглаживаются углы).
 * Высоту, как у page_rect, дописывают потом — page_rect_height. Номер элемента или −1. */
int page_box(int x, int y, int w, int h, uint32_t fill, uint32_t border, int bw, int radius, uint32_t under);
/* Рамка коробки index разной толщины по сторонам (верх, право, низ, лево; 0…15 точек; без
 * скругления): так рисуются border-top, border-bottom… */
void page_box_sides(int index, int t, int r, int b, int l);
/* Строка глифов одной строкой (без переноса) начертанием face (font.h); y — верх клетки
 * начертания (базовая линия — y + font_faces[face].baseline). bg — цвет фона под ней
 * (сглаживание считается к нему). flags — PAGE_UNDERLINE, PAGE_BOLD. link ≥ 0 — номер ссылки. */
void page_text(int x, int y, unsigned face, const uint16_t *glyphs, unsigned count, uint32_t fg, uint32_t bg,
               unsigned flags, int link);
/* Встроенный элемент (поле формы, кнопка, картинка) w×h с левым верхним углом (x, y) и номером
 * id; bg — цвет фона под ним; link ≥ 0 — он внутри ссылки (картинка-ссылка), иначе −1. Рисует его
 * не page.c: при каждой отрисовке строк, которые он задевает, зовётся painter(id, x, y, w, h, bg),
 * и тот рисует функциями page_paint_… (только из painter). */
int page_widget(int x, int y, int w, int h, int id, uint32_t bg, int link);
void page_set_painter(void (*paint)(int id, int x, int y, int w, int h, uint32_t bg));
/* Встроенный элемент под точкой экрана (x, y) — его id или −1. */
int page_widget_at(int x, int y);
/* Перерисовать строки страницы [y, y + h), если они есть в кольце (поле формы изменилось).
 * 0 — отказ LINK. */
int page_redraw(int y, int h);
/* Скруглённый прямоугольник (радиус r; w = h = 2r — круг) цвета fg на фоне bg, края сглажены. */
void page_paint_round(int x, int y, int w, int h, int r, uint32_t fg, uint32_t bg);
/* Маска L4 w×h (stride байт в строке) — иконка — цветом fg на фоне bg. */
void page_paint_mask(int x, int y, int w, int h, const uint8_t *l4, int stride, uint32_t fg, uint32_t bg);
/* Глифы начертания face (y — верх клетки); видно только между clip_left и clip_right. */
void page_paint_text(int x, int y, unsigned face, const uint16_t *g, unsigned n, uint32_t fg, uint32_t bg,
                     unsigned flags, int clip_left, int clip_right);
/* Высота страницы (по нижнему краю последнего элемента). */
int page_height(void);
/* Ссылка под точкой экрана (x, y при текущей прокрутке) или −1. */
int page_link_at(int x, int y);

/* Показ: верх экрана — строка top страницы. Шаг за раз — не больше PAGE_STEP, дальний прыжок —
 * перерисовка всего экрана. Строки дописываются в RAM_G. 0 — отказ LINK. */
int page_scroll_to(int top);
/* Текущий показанный верх (после page_scroll_to). */
int page_top(void);
/* Добавить в дисплей-лист команды, рисующие страницу: палитру и один-два куска кольца
 * (ручка 0; BEGIN BITMAPS и END — здесь же). */
void page_dl(ft_dl_t *d);
/* Цвет фона страницы (0xRRGGBB). */
uint32_t page_bg(void);
/* Сменить фон страницы, пока на ней ничего не нарисовано (<body bgcolor=…>). */
void page_set_bg(uint32_t bg);
