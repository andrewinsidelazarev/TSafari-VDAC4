/* Видеочип FT812 как видеокарта браузера.
 *
 * ЧТО ТАКОЕ FT812. Это видеочип (его изображает ПЛИС Tang в режиме Slot1). В нём три области
 * памяти, к которым ESP32 обращается по адресу, как к массиву байтов:
 *   - RAM_G (с адреса 0, 2 МБ) — «графическая память»: сюда кладут картинки — точки страницы,
 *     иконки, шрифты. Сама по себе она на экране не видна.
 *   - RAM_DL (с адреса 0x300000, 8 КБ) — «дисплей-лист»: список команд рисования, 32-битных
 *     слов. Например: «выбрать цвет», «взять картинку из RAM_G по такому-то адресу», «нарисовать
 *     её в точке (x, y)», «нарисовать круг». Видеочип выполняет этот список заново для каждого
 *     кадра, 59 раз в секунду. Чтобы картинка на экране изменилась, пишут новый список.
 *   - регистры (с 0x302000) — настройки и состояние: номер кадра, команда «смени список» и т. д.
 *
 * СМЕНА СПИСКА БЕЗ МЕЛЬКАНИЯ. Пока видеочип рисует по одному списку, мы пишем новый, а потом
 * записываем 2 в регистр REG_DLSWAP: «возьми новый список в конце этого кадра». Видеочип сам
 * сбрасывает регистр в 0, когда взял его. Писать следующий список можно только после этого.
 *
 * Хост — тот, кто управляет видеочипом. Раньше это был Z80, в браузере — ESP32 (функции
 * ft_open, ft_write, ft_read прошивки; так же устроен DOOM VDAC4). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define FT_RAM_G      0x000000u          /* начало графической памяти */
#define FT_RAM_DL     0x300000u          /* начало дисплей-листа */
#define FT_REG_FRAMES 0x302004u          /* регистр: номер кадра */
#define FT_REG_DLSWAP 0x302054u          /* регистр: смена дисплей-листа */
#define FT_DL_WORDS   2048u              /* столько слов помещается в RAM_DL (8 КБ) */

/* Переключить Tang в ядро FT812 (ПЛИС перезагружается в другую прошивку, до timeout_ms) и
 * стать его хозяином. Возвращает ZM_OK или код ошибки из zm_app.h. */
int ft_init(uint32_t timeout_ms);
/* RAM_G 3 МБ (on = 1) или 2 МБ (on = 0): регистр LINK FT_CFG, бит 0 (ядро FT812 1.5+; прочие биты —
 * как в FT_STAT). 1 — режим такой, как просили (проверено чтением FT_STAT, бит 27). */
int ft_ramg_3mb(int on);
/* Записать байты в память видеочипа (адрес и длина кратны 4). 0 — ошибка связи или остановка. */
int ft_put(uint32_t address, const void *data, size_t bytes);
/* Записать новый дисплей-лист и заказать его показ с конца текущего кадра; перед этим ждёт,
 * пока видеочип возьмёт прошлый. 0 — ошибка. */
int ft_dl_commit(const uint32_t *dl, unsigned words);
/* Дождаться, пока видеочип возьмёт последний заказанный дисплей-лист: после этого на экране
 * именно он. 0 — ошибка или остановка. */
int ft_swap_wait(void);
/* Номер кадра видеочипа (растёт 59 раз в секунду), 0 — не прочитался. */
uint32_t ft_frames(void);

/* --- команды дисплей-листа ---
 * Каждая команда — одно 32-битное слово: старший байт — код команды, остальные биты — её
 * параметры. Функции dl_… ниже только собирают такое слово из параметров.
 *
 * Форматы картинок (как хранятся точки в RAM_G):
 *   L4          — 4 бита на точку, 16 уровней яркости; цвет даёт команда COLOR_RGB. Так хранятся
 *                 буквы и одноцветные иконки: точка — «насколько закрашено» (сглаживание краёв).
 *   ARGB4       — 16 бит на точку: прозрачность и красный, зелёный, синий по 4 бита (курсор).
 *   RGB565      — 16 бит на точку: 5 бит красного, 6 зелёного, 5 синего (фотографии).
 *   PALETTED565 — 1 байт на точку: номер цвета в палитре из 256 цветов RGB565 (страница).
 * Примитивы (что рисовать между BEGIN и END): BITMAPS — картинки, POINTS — круги, LINES — линии,
 * RECTS — прямоугольники (с LINE_WIDTH — со скруглёнными углами). */
enum { FT_ARGB1555 = 0, FT_L4 = 2, FT_L8 = 3, FT_ARGB4 = 6, FT_RGB565 = 7, FT_PALETTED565 = 14 };
enum { FT_BITMAPS = 1, FT_POINTS = 2, FT_LINES = 3, FT_RECTS = 9 };

static inline uint32_t dl_display(void) { return 0; }
static inline uint32_t dl_clear_color(uint32_t rgb) { return (0x02u << 24) | (rgb & 0xFFFFFFu); }
static inline uint32_t dl_clear(void) { return (0x26u << 24) | 7u; }
static inline uint32_t dl_color_rgb(uint32_t rgb) { return (0x04u << 24) | (rgb & 0xFFFFFFu); }
static inline uint32_t dl_color_a(uint8_t a) { return (0x10u << 24) | a; }
static inline uint32_t dl_begin(unsigned prim) { return (0x1Fu << 24) | prim; }
static inline uint32_t dl_end(void) { return 0x21u << 24; }
static inline uint32_t dl_handle(unsigned h) { return (0x05u << 24) | h; }
static inline uint32_t dl_source(uint32_t a) { return (0x01u << 24) | (a & 0x3FFFFFu); }
static inline uint32_t dl_palette_source(uint32_t a) { return (0x2Au << 24) | (a & 0x3FFFFFu); }
static inline uint32_t dl_point_size(unsigned r16) { return (0x0Du << 24) | (r16 & 0x1FFFu); }
static inline uint32_t dl_line_width(unsigned w16) { return (0x0Eu << 24) | (w16 & 0xFFFu); }
static inline uint32_t dl_scissor_xy(unsigned x, unsigned y) { return (0x1Bu << 24) | ((x & 0x7FFu) << 11) | (y & 0x7FFu); }
static inline uint32_t dl_scissor_size(unsigned w, unsigned h) { return (0x1Cu << 24) | ((w & 0xFFFu) << 12) | (h & 0xFFFu); }
/* Вершина (точка на экране) в шестнадцатых долях точки: x16 = x · 16. Так видеочип умеет ставить
 * фигуры точнее одной точки (сглаженные круги и линии). Битмап рисуется от вершины вправо-вниз. */
static inline uint32_t dl_vertex(int x16, int y16) {
    return 0x40000000u | (((uint32_t)x16 & 0x7FFFu) << 15) | ((uint32_t)y16 & 0x7FFFu);
}

/* Дисплей-лист, который собирается в памяти ESP32 перед отправкой в видеочип: массив слов и их
 * число. В RAM_DL помещается 2048 слов; если команд больше — помечаем overflow и такой список
 * не отправляем (картинка останется прежней), чтобы не обрезать его посередине. */
typedef struct {
    uint32_t w[FT_DL_WORDS];
    unsigned n;
    int overflow;
} ft_dl_t;

/* Добавить одну команду в конец списка. */
static inline void dl_put(ft_dl_t *d, uint32_t word) {
    if (d->n < FT_DL_WORDS) d->w[d->n++] = word; else d->overflow = 1;
}
/* Описать картинку для текущей ручки: адрес, формат, байт в строке, ширина, высота (ft.c). */
void dl_bitmap(ft_dl_t *d, uint32_t address, unsigned format, unsigned stride, unsigned w, unsigned h);
