/* Память для libwebp (third_party/libwebp): сборка подключает этот файл раньше всех к каждому
 * файлу libwebp (ключ -include), и вызовы malloc, calloc, realloc, free в нём становятся вызовами
 * функций браузера (image.c) — те берут память PSRAM у прошивки ESP32 через таблицу zm_core_t.
 * Своей кучи у приложения ELF нет, а malloc прошивка приложениям не даёт. */
#pragma once
#include <stddef.h>
/* stdlib.h — раньше подмены: её объявления (picolibc ссылается на имя free в атрибутах malloc)
 * остаются как есть, подменяются только вызовы в файлах libwebp. */
#include <stdlib.h>

void *tsafari_malloc(size_t n);
void *tsafari_calloc(size_t count, size_t size);
void *tsafari_realloc(void *p, size_t n);
void tsafari_free(void *p);

#define malloc(n) tsafari_malloc(n)
#define calloc(c, s) tsafari_calloc(c, s)
#define realloc(p, n) tsafari_realloc(p, n)
#define free(p) tsafari_free(p)
