/* nanosvg (third_party/nanosvg) — сама библиотека: разбор SVG в контуры и их отрисовка со
 * сглаживанием. Здесь — её реализация и то, чего ей не хватает у модуля ESP32.
 *
 * Прошивка ESP32 даёт модулям узкую libc (Firmware/src/zm_symbols.cpp): ссылку на функцию, которой
 * там нет, загрузчик не свяжет, и модуль не загрузится. nanosvg зовёт ещё strtoll и strtol (числа),
 * sscanf (только для цветов «#rrggbb», «#rgb» и «rgb(r, g, b)») и acosf (дуги) — они свои, ниже, и
 * подставляются вместо библиотечных макросами. Функции файлов нужны ей только в nsvgParseFromFile,
 * которой браузер не пользуется: файлов у него нет, они заменены «ничего не открылось».
 *
 * Память — функции браузера (webp_alloc.h: malloc, calloc, realloc, free → PSRAM из таблицы
 * прошивки, как у libwebp). Чужой код собирается без предупреждений (-w), поэтому свой — в svg.c. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "webp_alloc.h"

/* Целое из строки (основание 10 — других nanosvg не просит): знак, цифры; *end — за последней. */
static long long svg_strtoll(const char *s, char **end, int base) {
    const char *p = s;
    long long v = 0;
    int neg = 0, digits = 0;
    (void)base;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    while (*p >= '0' && *p <= '9') {
        if (v < 100000000000000000LL) v = v * 10 + (*p - '0');   /* дальше — не точнее float */
        p++;
        digits++;
    }
    if (end) *end = (char *)(digits ? p : s);
    return neg ? -v : v;
}

/* Маленький sscanf: обычные знаки шаблона — как есть, пробел — любые пробелы (и ни одного), %x и
 * %u — шестнадцатеричное и десятичное число в unsigned int (перед числом пробелы пропускаются), %2x —
 * не больше двух цифр. Ответ — сколько чисел прочитано, как у sscanf. */
static int svg_sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    int n = 0;
    va_start(ap, fmt);
    while (*fmt) {
        if (*fmt == ' ') {
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) break;
            s++;
            fmt++;
            continue;
        }
        fmt++;
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + *fmt++ - '0';
        char conv = *fmt++;
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        unsigned v = 0;
        int digits = 0;
        for (;;) {
            int d;
            if (width && digits >= width) break;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (conv == 'x' && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (conv == 'x' && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            v = v * (conv == 'x' ? 16u : 10u) + (unsigned)d;
            s++;
            digits++;
        }
        if (!digits || (conv != 'x' && conv != 'u')) break;
        *va_arg(ap, unsigned *) = v;
        n++;
    }
    va_end(ap);
    return n;
}

/* arccos через arctg (atan2f и sqrtf прошивка даёт). */
static float svg_acosf(float x) {
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return atan2f(sqrtf(1.0f - x * x), x);
}

#define strtoll svg_strtoll
#define strtol(s, end, base) ((long)svg_strtoll((s), (end), (base)))
#define sscanf svg_sscanf
#define acosf svg_acosf
#define fopen(name, mode) ((FILE *)NULL)
#define fclose(f) 0
#define fseek(f, off, whence) (-1)
#define ftell(f) 0L
#define fread(p, size, n, f) ((size_t)0)

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
