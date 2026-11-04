/* Адреса страниц (URL).
 *
 * Как устроен адрес: https://vtrd.in/press/index.php?page=2#top
 *   https          — схема: каким способом ходить (http — открыто, https — с шифрованием);
 *   vtrd.in        — хост: имя сервера;
 *   /press/index.php — путь к странице на сервере; «каталог» — всё до последней «/»;
 *   ?page=2        — запрос: параметры для сервера;
 *   #top           — фрагмент: место внутри страницы, на сервер не посылается.
 *
 * Ссылки на страницах часто «относительные»: «next.html», «../img/a.png», «/news», «?page=3».
 * Их нужно превратить в полный адрес относительно адреса текущей страницы — это правила
 * стандарта RFC 3986 (раздел 5.2). */
#pragma once
#include <stddef.h>

#define URL_MAX 512                      /* как у net_open прошивки (HttpStream::url_) */

/* Что набрал пользователь → полный адрес в out. Без схемы — добавляется https://; несколько
 * слов или слово без точки — это поиск; «tsafari:…» — встроенные страницы. 0 — не адрес. */
int url_from_input(const char *typed, char *out, size_t cap);
/* Ссылку ref со страницы base превратить в полный адрес out. 0 — не получилось (схема не
 * http/https, например mailto:, или адрес не помещается в cap байт). */
int url_resolve(const char *base, const char *ref, char *out, size_t cap);
/* 1 — встроенная страница («tsafari:about» и т. п.): её не загружают из сети. */
int url_is_internal(const char *url);
