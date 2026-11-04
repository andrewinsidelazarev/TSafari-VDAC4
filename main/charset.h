/* Кодировки страниц.
 *
 * Кодировка — договорённость, какой байт какую букву значит. Сейчас почти всё в UTF-8 (font.h),
 * но старые русские сайты пишут однобайтовыми кодировками: байт 0xC0 в Windows-1251 — «А»,
 * в KOI8-R — «ю», в CP866 — «└». Первые 128 кодов (латиница, цифры, знаки) у них общие,
 * различаются только байты 0x80…0xFF. Для каждой кодировки есть таблица из 128 кодов Unicode
 * для этих байтов (charset_data.c, от tools/gen_charsets.py). Внутри браузер везде работает
 * с UTF-8, поэтому страница сразу переводится в UTF-8. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { CS_UTF8, CS_CP1251, CS_KOI8R, CS_CP866, CS_CP1252 };   /* известные кодировки */

extern const uint16_t charset_cp1251[128], charset_koi8r[128], charset_cp866[128], charset_cp1252[128];

/* Кодировка по имени («windows-1251», «koi8-r», …); незнакомая — −1. */
int charset_id(const char *name, size_t len);
/* Кодировка страницы: Content-Type, BOM, <meta> в первых 4 КБ, иначе — проверка UTF-8
 * (много ошибок — Windows-1251: русские сайты без объявления). */
int charset_detect(const char *content_type, const uint8_t *body, size_t len);
/* Перевести n байт в UTF-8. Буква в UTF-8 занимает до 3 байт, поэтому out должен вмещать
 * 3 × n байт. Возвращает длину результата. */
size_t charset_to_utf8(int cs, const uint8_t *in, size_t n, char *out);
/* Байт однобайтовой кодировки cs для буквы с кодом Unicode c; −1 — такой буквы в ней нет
 * (и для UTF-8 — там буква занимает несколько байт). */
int charset_from_unicode(int cs, uint32_t c);
