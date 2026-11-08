/* Разбор HTML — языка, на котором написаны веб-страницы.
 *
 * HTML — это текст с «тегами» в угловых скобках: <h1>Заголовок</h1>, <p>абзац</p>,
 * <a href="адрес">ссылка</a>. Тег говорит, чем является кусок текста. Браузер читает страницу
 * слева направо и по тегам решает: здесь новый абзац, здесь крупный заголовок, здесь ссылка.
 *
 * Сейчас (этап 1) понимаем: текст, заголовки, абзацы, списки, цитаты, <pre> (текст как есть),
 * таблицы со столбцами, ссылки, подписи картинок, формы (поля, кнопки, флажки, списки — form.h),
 * фреймы; из CSS — цвета, фон, жирность, подчёркивание, размер, выравнивание, display: none
 * (в style="…" и в таблицах стилей <style>). Программы JS не выполняются.
 *
 * ФРЕЙМЫ. Старые сайты (например, vtrd.in) делят окно на части — «фреймы», и в каждой своя
 * страница со своим адресом. Сама главная страница тогда — только список фреймов:
 *   <frameset rows="20,*"> <frame src="menu.htm" name="Top"> <frame src="news.php" name="Down">
 * Ссылка с target="Down" открывает свою страницу не во всём окне, а только во фрейме «Down».
 * Экран у нас один столбец, поэтому фреймы идут сверху вниз через черту: меню, под ним —
 * содержимое. Такую составную страницу собирают так: html_begin, затем html_part на каждый
 * фрейм, html_end (это делает app.c, он же загружает страницы фреймов). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "url.h"

#define HTML_FRAMES_MAX 8                /* больше фреймов на одной странице не бывает у нас */

/* Один фрейм: имя (для target у ссылок) и адрес его страницы. */
typedef struct {
    char name[32];
    char url[URL_MAX];
} html_frame_t;

/* Разобрать обычную страницу и разложить её через doc.c. body (len байт) — что прислал сервер,
 * content_type — заголовок ответа (из него — кодировка), url — адрес страницы (от него
 * считаются относительные ссылки, url.h). */
void html_render(const uint8_t *body, size_t len, const char *content_type, const char *url);
/* Страница — набор фреймов? Тогда записать их в out (не больше max) и вернуть их число;
 * обычная страница — 0. Ничего не рисует. */
int html_frames(const uint8_t *body, size_t len, const char *content_type, const char *url,
                html_frame_t *out, int max);
/* Составная страница из фреймов: начать, добавить страницу каждого фрейма (part — номер
 * фрейма; перед всеми, кроме первого, — черта), закончить. */
void html_begin(void);
void html_part(const uint8_t *body, size_t len, const char *content_type, const char *url, int part);
/* Вместо страницы фрейма — строка «не открылась» (what — что случилось). */
void html_part_note(const char *url, const char *what, int part);
void html_end(void);
/* Полный адрес ссылки номер id (номер даёт page_link_at при щелчке) или NULL. */
const char *html_link(int id);
/* Куда открывать ссылку — значение target («Down», «_top»…; пусто — не указано). */
const char *html_link_target(int id);
/* Номер фрейма, в котором стоит ссылка; −1 — обычная страница. */
int html_link_part(int id);
/* Как узнать, что ссылка посещённая (цвет ссылки — фиолетовый, по дизайну Codex). */
void html_set_visited(int (*cb)(const char *url));
/* Картинки data: страницы (адрес — сами байты картинки): у image.c и app.c вместо адреса — ключ
 * «data:…». html_data_copy — копия байтов по ключу (PSRAM, теперь — вызывающего; NULL — нет);
 * html_data_clear — новая страница: хранилище — прочь. */
uint8_t *html_data_copy(const char *key, size_t *len);
void html_data_clear(void);
/* Заголовок страницы (<title>), UTF-8; пусто — нет. */
const char *html_title(void);
/* Вместо страницы — страница ошибки: какой адрес не открылся (url), что случилось (what) и
 * подробности (detail). */
void html_error_page(const char *url, const char *what, const char *detail);
