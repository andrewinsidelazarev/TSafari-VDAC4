/* Дерево документа (DOM) для скриптов страницы.
 *
 * html.c раскладывает страницу на лету, дерева не строит. Скриптам же (js.c) нужно дерево: они
 * ищут элементы, меняют классы и атрибуты, вставляют и убирают узлы. Здесь оно и есть: страница
 * (UTF-8) разбирается в узлы по упрощённым правилам HTML (какие теги закрываются сами, где
 * <head> и <body>), скрипты меняют дерево, и потом оно снова становится текстом HTML
 * (dom_serialize) — его раскладывает html.c, как обычную страницу.
 *
 * Узел — номер (dom_id): 0 — «нет узла», 1 — сам документ. Номера не меняются, пока дерево
 * живо (до dom_free или нового dom_parse); удалённый узел просто отцеплен. Строки (имена,
 * значения, текст) возвращаются указателями во внутренний буфер: они верны до следующего
 * изменения дерева — кто их хранит, копирует. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef uint32_t dom_id;
enum { DOM_ELEMENT = 1, DOM_TEXT = 3, DOM_COMMENT = 8, DOM_DOCUMENT = 9, DOM_DOCTYPE = 10, DOM_FRAGMENT = 11 };
#define DOM_DOC 1u                        /* номер узла документа */

/* Разобрать страницу (UTF-8) в новое дерево; прежнее — выбрасывается. 0 — нет памяти. */
int dom_parse(const char *html, size_t n);
void dom_free(void);
int dom_alive(void);                      /* дерево есть */
size_t dom_bytes(void);                   /* сколько памяти занято (замер) */
uint32_t dom_count(void);                 /* узлов всего (номера 1…dom_count()) */

/* Структура */
int dom_type(dom_id n);                   /* DOM_ELEMENT…; 0 — нет такого узла */
dom_id dom_parent(dom_id n);
dom_id dom_first(dom_id n);
dom_id dom_last(dom_id n);
dom_id dom_next(dom_id n);
dom_id dom_prev(dom_id n);
dom_id dom_html(void);                    /* <html>, <head>, <body> документа (0 — нет) */
dom_id dom_head(void);
dom_id dom_body(void);
int dom_contains(dom_id a, dom_id b);     /* b — сам a или внутри a */

/* Элемент: имя тега (строчными; внутри <svg> и <math> — как в тексте) */
const char *dom_tag(dom_id n);
/* Текст и комментарий: данные */
const char *dom_data(dom_id n, size_t *len);
int dom_set_data(dom_id n, const char *s, size_t len);

/* Атрибуты */
int dom_attr_count(dom_id n);
const char *dom_attr_name(dom_id n, int i);
const char *dom_attr_value(dom_id n, int i);
const char *dom_get_attr(dom_id n, const char *name);          /* NULL — нет такого */
int dom_set_attr(dom_id n, const char *name, const char *value);   /* 0 — нет памяти */
void dom_remove_attr(dom_id n, const char *name);

/* Новые узлы (ещё не в дереве) */
dom_id dom_create_element(const char *tag);
dom_id dom_create_text(const char *s, size_t len);
dom_id dom_create_comment(const char *s, size_t len);
dom_id dom_create_fragment(void);
dom_id dom_clone(dom_id n, int deep);

/* Вставить node в parent перед before (0 — в конец); узел сначала отцепляется оттуда, где был.
 * Фрагмент вставляет своих детей. 0 — нельзя (вставка в самого себя или в свой узел). */
int dom_insert(dom_id parent, dom_id node, dom_id before);
void dom_detach(dom_id node);

/* Разобрать кусок HTML и вставить его узлы в parent перед before (innerHTML, insertAdjacentHTML,
 * document.write). 0 — нет памяти. */
int dom_insert_html(dom_id parent, dom_id before, const char *html, size_t n);

/* Текст всех текстовых узлов внутри (textContent) — в out (≤ cap − 1 байт); длина полного */
size_t dom_text_content(dom_id n, char *out, size_t cap);
/* Дерево в HTML (UTF-8). outer — сам узел с тегами, иначе только содержимое. Буфер — ts_alloc,
 * освобождает вызвавший; NULL — нет памяти. page — для html.c: без <script> и <noscript>
 * (скрипты уже выполнены), <meta charset> — utf-8. */
char *dom_serialize(dom_id n, int outer, int page, size_t *len);

/* Объект скрипта, привязанный к узлу (js.c), — чтобы у одного узла был один объект */
void *dom_wrap(dom_id n);
void dom_set_wrap(dom_id n, void *w);
