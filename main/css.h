/* css.h — упрощённый CSS браузера («алфавит CSS», 07.11): таблицы стилей, селекторы, значения.
 *
 * Как устроено (подробно — в css.c):
 * - Таблица стилей разбирается один раз в правила: «селектор { свойства }». Селектор — цепочка
 *   до четырёх простых селекторов («.menu > li a.active»): тег, классы, id, атрибуты [x=y];
 *   между ними — «потомок» (пробел) или «ребёнок» (>). Соседи (+, ~) и состояния мыши (:hover)
 *   не поддерживаются — такие правила пропускаются.
 * - Правила, которые не могут подойти к документу (их классов, id или тегов в нём нет), сразу
 *   выбрасываются: у больших сайтов в таблицах тысячи правил, а нужны десятки.
 * - Для каждого элемента css_match находит подходящие правила и отдаёт их свойства в порядке
 *   CSS: сначала менее «весомые» (специфичность), при равенстве — по порядку в таблицах.
 * - Внешние таблицы (<link rel=stylesheet>, @import) грузит app.c: адреса копит очередь
 *   css_link…, пришедший текст разбирается в правила, которые живут до новой страницы.
 * - Значения: длины (px, em, rem, pt, %, vw, vh… и calc/min/max/clamp), цвета (все имена CSS,
 *   #rgb(a), rgb(a), hsl(a), transparent, currentColor), переменные var(--x) с наследованием
 *   (стек css_vars…), условия @media для экрана 1024 × 768. */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Элемент для сравнения с селекторами: отпечатки (хэши) имени тега, id и классов. */
#define CSS_EL_CLS 16
typedef struct {
    uint32_t tag, id;                    /* 0 — нет */
    uint32_t cls[CSS_EL_CLS];
    uint8_t ncls;
} css_el_t;

/* Как прочитать атрибут элемента (для селекторов [x=y]): 1 — есть, значение в out. */
typedef int (*css_attr_fn)(void *ctx, const char *name, char *out, size_t cap);

/* Отпечаток строки: как есть (классы, id) и без учёта регистра латиницы (теги, атрибуты). */
uint32_t css_hash(const char *s, size_t n);
uint32_t css_hash_ci(const char *s, size_t n);
/* Заполнить el: тег, class="a b c", id (строки могут быть пустыми). */
void css_el_make(css_el_t *el, const char *tag, const char *cls, const char *id);

/* Имена документа (теги, классы, id) — чтобы не держать правила, которые к нему не подойдут.
 * Сброс — перед раскладкой страницы; потом html.c просматривает документ и добавляет имена. */
void css_names_reset(void);
void css_name_tag(const char *tag);
void css_name_classes(const char *cls);
void css_name_id(const char *id);

/* Таблицы стилей самого документа (<style>): сброс перед каждым документом (фреймом) и разбор.
 * base — адрес документа (для @import), part — номер фрейма (−1 — обычная страница). */
void css_doc_reset(void);
void css_doc_sheet(const char *s, size_t n, const char *base, int part);

/* Подходящие элементу el правила: их свойства (строки «color: red; …») — в out по порядку
 * применения (последнее главнее). anc — предки от ближайшего (родитель, дед, …, body, html).
 * Возвращает число строк (не больше max). Строки живут до следующего разбора таблицы стилей. */
int css_match(const css_el_t *el, css_attr_fn fn, void *ctx, const css_el_t *const *anc, int nanc, int part,
              const char **out, int max);

/* Внешние таблицы стилей. css_links_clear — новая страница: забыть адреса и их правила.
 * css_link — адрес из <link> или @import (повторы не добавляются).
 * css_link_next — следующий адрес, который пора загрузить (NULL — нечего); css_link_done — его
 * текст пришёл (b = NULL — не загрузился). css_links_wanted — сколько ещё не загружено. */
void css_links_clear(void);
void css_link(const char *url, int part);
const char *css_link_next(void);
void css_link_done(const char *url, const uint8_t *b, size_t n);
int css_links_wanted(void);
/* Таблицы стилей в памяти (PSRAM) на время работы браузера — сайт подключает одни и те же на всех
 * страницах (пользователь 07.11: «оба пункта» — память и карта). css_mem_get — текст таблицы url
 * (NULL — нет; указатель живёт до следующего css_mem_put или css_mem_trim); css_mem_put — запомнить
 * (буфер text забирает себе: освободит сам); css_mem_trim — всё отдать памяти (1 — что-то было). */
const uint8_t *css_mem_get(const char *url, size_t *len);
void css_mem_put(const char *url, uint8_t *text, size_t len);
int css_mem_trim(void);
/* «Стоп»: незагруженные таблицы больше не грузить. */
void css_links_stop(void);
/* Сколько правил в наборе: внешних таблиц (ext = 1) или <style> последнего документа (0). */
int css_rules(int ext);

/* Значение свойства prop в строке свойств (последнее, без «!important»), строчными, с
 * подставленными var(--x); 0 — свойства нет (или переменной нет и запасного значения нет).
 * css_important = 1 — только свойства с «!important» (второй проход, они главнее). */
extern int css_important;
int css_get(const char *decls, const char *prop, char *out, size_t cap);

/* Длина CSS в точках: 1 — да (*px), −1 — auto, 0 — не длина. Проценты — от base (base < 0 —
 * процентов посчитать не от чего: ответ 0). Экран — 1024 × 768 (vw, vh), em и rem — 16. */
int css_len(const char *v, int base, int *px);
/* Цвет: 1 — да, *rgb и *alpha (0…255; 0 — прозрачный). cur — текущий цвет текста (currentColor). */
int css_color(const char *v, uint32_t cur, uint32_t *rgb, int *alpha);
/* Условие @media (или атрибут media=) для экрана 1024 × 768: 1 — подходит. */
int css_media(const char *q, size_t n);

/* Переменные CSS (--x) с наследованием: свои у каждого уровня вложенности depth.
 * css_vars_cut — забыть всё на глубине depth и глубже; css_vars_take — объявления «--x: …»
 * из строки свойств — на глубину depth. */
void css_vars_cut(int depth);
void css_vars_take(const char *decls, int depth);
