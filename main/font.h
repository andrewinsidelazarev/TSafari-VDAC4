/* Шрифт браузера: Noto Sans Mono из просмотрщика текстов TXTVIEW2 (WC Improved).
 *
 * СЛОВА, КОТОРЫЕ ВСТРЕТЯТСЯ НИЖЕ
 *   Unicode   — общий номер для каждой буквы всех языков: «A» = 0x41, «Я» = 0x42F, «€» = 0x20AC.
 *               Этот номер называют «кодом» символа.
 *   UTF-8     — способ записать коды Unicode байтами: латиница — 1 байт, кириллица — 2, большинство
 *               остальных — 3, редкие знаки — 4. Почти все страницы в интернете в UTF-8.
 *   глиф      — картинка одной буквы. Глифов больше, чем кодов: например, «é» может быть
 *               готовым глифом, хотя в тексте записано двумя кодами «e» + «◌́».
 *   атлас     — все глифы подряд в одном массиве. Каждый глиф — клетка 12×34 точки; точка —
 *               4 бита (16 уровней: 0 — фон, 15 — полностью цвет буквы, между ними — края
 *               сглаживания). В строке клетки 12 точек = 6 байт, левая точка — старшие 4 бита.
 *   базовая линия — линия, на которой «стоят» буквы (у «р» и «у» хвосты уходят ниже неё):
 *               26 точек от верха клетки.
 *
 * Атлас строит tools/gen_font.py из файла шрифта и кладёт в программу сжатым (zlib, ~97 КБ);
 * при запуске font_init распаковывает его в PSRAM (~570 КБ). */
#pragma once
#include <stdint.h>
#include "font_gen.h"

#define FONT_GLYPH_BYTES (FONT_CELL_W / 2 * FONT_CELL_H)   /* 6 байт строки × 34 строки */
#define FONT_NONE 0xFFFFu                                  /* «такого глифа нет» */

/* НАЧЕРТАНИЯ (размеры шрифта) — по дизайну Codex: основной текст 20 px и ещё пять. У каждого
 * своя клетка: ширина (она же шаг — шрифт моноширинный), высота и базовая линия. Номера глифов
 * у всех одни и те же (основного атласа); в дополнительных начертаниях есть только частые знаки
 * (font_sub_glyphs), остальные рисуются основным. */
enum { FACE_BODY, FACE_H1, FACE_H2, FACE_H3, FACE_SMALL, FACE_CAPTION, FACE_COUNT };
typedef struct {
    uint8_t w, h, baseline, stride;      /* клетка, базовая линия, байт в строке точек */
    const uint8_t *zlib;                 /* сжатый атлас (у основного — NULL: он свой) */
    uint32_t zlib_bytes, bytes;
} font_face_t;
extern const font_face_t font_faces[FACE_COUNT];
extern const uint16_t font_sub_glyphs[FONT_SUB_GLYPHS];

/* Таблицы из font_data.c (тоже от gen_font.py):
 *   font_codes   — коды Unicode, у которых есть глиф, по возрастанию; номер в таблице = номер глифа;
 *   font_compose — тройки «глиф буквы, код знака, готовый глиф» (е + ◌̈ = ё), по возрастанию;
 *   font_fast    — для частых кодов (меньше 0x600) номер глифа сразу, без поиска;
 *   font_l4_zlib — сжатый атлас. */
extern const uint16_t font_codes[FONT_CODES];
extern const uint16_t font_compose[FONT_COMPOSE * 3];
extern const uint16_t font_fast[FONT_FAST_LIMIT];
extern const uint8_t font_l4_zlib[FONT_L4_ZLIB_BYTES];

/* Распаковать атлас в PSRAM (один раз за запуск). 0 — нет памяти или атлас испорчен. */
int font_init(void);
/* Точки глифа: FONT_CELL_H строк по 6 байт, левая точка — старшая тетрада (4 бита). */
const uint8_t *font_glyph(unsigned glyph);
/* Точки глифа в начертании face (font_faces[face].h строк по stride байт); FACE_BODY — как
 * font_glyph. Знака в этом начертании нет (или нет памяти) — NULL: рисовать основным. */
const uint8_t *font_glyph_face(unsigned face, unsigned glyph);
/* Номер глифа для кода Unicode; нет в шрифте — FONT_REPLACEMENT (знак «�»). */
unsigned font_lookup(uint32_t code);
/* Готовая буква «глиф + надстрочный знак» (например, е + ◌̈ = ё) или FONT_NONE. */
unsigned font_combine(unsigned base, uint32_t mark);
/* Прочитать один символ UTF-8 с *p (не дальше end) и сдвинуть *p за него. Неправильные байты
 * не роняют программу: вместо них возвращается U+FFFD («�»), и пропускается один байт. */
uint32_t utf8_next(const char **p, const char *end);
/* Превратить строку UTF-8 в номера глифов (не больше max). Надстрочные знаки 0300–036F
 * сливаются с предыдущей буквой, если готовая буква есть. Возвращает число глифов. */
unsigned font_shape(const char *text, unsigned bytes, uint16_t *glyphs, unsigned max);
