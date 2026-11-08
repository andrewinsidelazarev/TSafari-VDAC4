/* JavaScript страницы: движок QuickJS (third_party/quickjs) над деревом документа dom.c.
 *
 * Как браузер выполняет скрипты страницы:
 *   1. js_begin — страница (UTF-8) разбирается в дерево, собирается список <script> по порядку;
 *   2. внешние скрипты (<script src>) загружает app.c, как таблицы стилей: js_script_next даёт
 *      адрес, js_script_body отдаёт пришедшее;
 *   3. js_run — скрипты выполняются по порядку, потом события DOMContentLoaded и load, потом
 *      таймеры (setTimeout…) в пределах бюджета времени;
 *   4. js_result — дерево снова в HTML (без <script> и <noscript>): его раскладывает html.c;
 *   5. js_end — всё освобождается (память движка — своими кусками, без остатка).
 * Скриптам видны дерево (document, элементы, события), window, location, navigator, таймеры,
 * localStorage (на время страницы), getComputedStyle (по правилам css.c), matchMedia. Сеть
 * (fetch, XMLHttpRequest) — пока отвечает ошибкой. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Разобрать страницу, собрать скрипты. 1 — собраны; 0 — скриптов нет: выполнять нечего;
 * −1 — дереву страницы не хватило памяти. */
int js_begin(const char *html, size_t n, const char *url);
/* Адрес следующего внешнего скрипта, который надо загрузить (NULL — больше нет). */
const char *js_script_next(void);
/* Пришёл внешний скрипт (n = 0 — не загрузился). Байты копируются. */
void js_script_body(const char *url, const uint8_t *b, size_t n);
/* То же, но буфер b (ts_alloc, после данных — нулевой байт, как у fetch_take_body) отдаётся скриптам
 * без копии: большой скрипт (747 КБ у zxart.ee) не лежит в памяти дважды. */
void js_script_take(const char *url, uint8_t *b, size_t n);
/* Сколько внешних скриптов ещё не пришло. */
int js_scripts_wanted(void);
/* Выполнить всё (budget_ms — на всё вместе). 1 — дерево изменено скриптами; 0 — нет. */
int js_run(uint32_t budget_ms);
/* Дерево в HTML (ts_alloc; освобождает вызвавший). */
char *js_result(size_t *len);
/* Строка журнала скриптов (console.log и ошибки) — по одной; NULL — больше нет. */
const char *js_log_next(void);
void js_end(void);
/* То же js_run — в фоновой задаче (на стенде ПК — сразу): 1 — запущено. js_finished — задача
 * кончилась (итог можно брать: js_changed, js_result); js_abort — попросить остановиться (скрипт
 * прерывается при ближайшей проверке QuickJS). js_end — только после js_finished. */
int js_start(uint32_t budget_ms);
int js_finished(void);
void js_abort(void);
int js_changed(void);
int js_out_of_memory(void);                /* скриптам не хватило памяти (последний js_run) */
/* Скрипт попросил перейти на другой адрес (location.href = …, replace…): адрес, иначе NULL;
 * *replace — location.replace (не новая запись истории). Действует до js_end. */
const char *js_nav(int *replace);
extern uint32_t js_mem_limit;              /* потолок кучи движка, байт */
/* Замер: память движка (байт занято, наибольшее), дерево, время разбора и выполнения. */
void js_stats(size_t *heap, size_t *heap_peak, size_t *dom, uint32_t *ms_run, int *scripts_ok, int *scripts_failed);
