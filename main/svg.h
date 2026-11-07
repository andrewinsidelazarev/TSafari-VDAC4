/* SVG — векторные картинки на странице (<img src="….svg">). Разбирает и рисует их сам ESP32
 * библиотекой nanosvg (third_party/nanosvg): контуры, фигуры, заливка, обводка, градиенты,
 * CSS-классы; <use> подставляется до разбора; текста внутри SVG nanosvg не знает. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct svg svg_t;

/* Похоже на SVG: текст, первый тег — <?xml, <!--, <!DOCTYPE или <svg, и <svg — в начале файла
 * раньше, чем <html (страница ошибки вместо картинки — не SVG). */
int svg_is(const uint8_t *body, size_t len);
/* Разобрать SVG (body не меняется). NULL — испорчен, пуст или не хватило памяти. */
svg_t *svg_open(const uint8_t *body, size_t len);
/* Собственный размер картинки в точках: width и height SVG; без них (только viewBox) — вписанный
 * в 300 × 150, как у браузеров без CSS. */
void svg_size(const svg_t *s, float *w, float *h);
/* Нарисовать строки y0…y0 + rows − 1 картинки шириной w (масштаб scale, сдвиг tx, ty в точках
 * выхода) в rgba — по 4 байта на точку, без предумножения на прозрачность. 0 — не вышло. */
int svg_render(svg_t *s, float scale, float tx, float ty, int w, int y0, int rows, uint8_t *rgba);
void svg_close(svg_t *s);
