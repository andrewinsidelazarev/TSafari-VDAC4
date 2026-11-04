/* Загрузка страницы из интернета.
 *
 * Загрузка долгая: соединиться, договориться о шифровании (HTTPS), дождаться ответа сервера —
 * это от десятых долей секунды до десятков секунд. Если бы браузер ждал её сам, курсор и
 * кнопки замирали бы. Поэтому загрузка идёт в отдельной «задаче» (своём потоке), а браузер
 * каждый кадр спрашивает «как дела?» через fetch_poll и тем временем рисует и слушает мышь.
 *
 * Саму сеть делает прошивка ESP32: net_open посылает запрос GET по HTTP или HTTPS и сама
 * проходит переадресации (ответы «страница переехала туда-то»). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define FETCH_LIMIT (2u * 1024u * 1024u)        /* тело страницы — не больше 2 МБ */
#define FETCH_FILE_LIMIT (4u * 1024u * 1024u)   /* файл для карты Evo (не страница) — до 4 МБ */

/* Состояния: ничего не делаем; идёт загрузка; загружено; не вышло; отменено. */
enum { FETCH_IDLE, FETCH_BUSY, FETCH_DONE, FETCH_FAILED, FETCH_CANCELLED };

typedef struct {
    int state;                 /* FETCH_* */
    int status;                /* код ответа HTTP: 200 — хорошо, 404 — нет такой страницы… (0 — не соединилось) */
    char url[512];             /* какой адрес загружали */
    char content_type[96];     /* что прислали: «text/html; charset=utf-8», «image/png»… */
    char disposition[128];     /* Content-Disposition: «attachment; filename=…» — это файл */
    size_t total;              /* длина по заголовку Content-Length (0 — сервер не сказал) */
    char error[96];            /* текст ошибки, если была */
    uint8_t *body;             /* PSRAM, до FETCH_LIMIT; живёт до следующей загрузки */
    size_t length;             /* сколько байт в body */
    uint32_t started, took;    /* когда начали и сколько длилось, мс */
    int streamed;              /* это был файл: он ушёл потоком в download.c (body пуст) */
    int image;                 /* загружали картинку (fetch_start_image): тело — в body, как у страницы */
} fetch_result_t;

/* Запустить задачу загрузки. 0 — прошивка без сети или задача не запустилась. */
int fetch_init(void);
/* Начать загрузку url (прежняя — отменяется). */
void fetch_start(const char *url);
/* Отправить форму способом POST: тело body (len байт, копируется) на адрес url; ответ — как у
 * загрузки страницы (fetch_poll). POST идёт через сокеты прошивки: http — обычный, https — с TLS
 * (прошивка ESP 0.1.117+, ABI 1.12; со старой — ошибка с объяснением). */
void fetch_post(const char *url, const char *body, size_t len);
/* Загрузить картинку: тело целиком в body, даже если по типу это «файл» (не на карту Evo). */
void fetch_start_image(const char *url);
/* Остановить текущую загрузку (кнопка «стоп», Esc). */
void fetch_cancel(void);
/* Состояние; FETCH_DONE / FAILED / CANCELLED — один раз, дальше FETCH_IDLE. *r — копия итога. */
int fetch_poll(fetch_result_t **r);
/* Принято байтов текущей загрузки (для хода). */
size_t fetch_progress(void);
/* Идёт загрузка, и заголовки ответа уже пришли: копия адреса, типа, Content-Disposition и
 * длины в *r (state = FETCH_BUSY). 0 — нет. По ним видно, что качается файл, а не страница. */
int fetch_peek(fetch_result_t *r);
/* Забрать память тела последней загрузки себе (её освобождает забравший; следующая загрузка
 * возьмёт новую). Можно только после того, как fetch_poll отдал итог. NULL — нечего брать. */
uint8_t *fetch_take_body(void);
/* Файл, а не страница: тип не текстовый или сервер сказал «attachment». */
int fetch_is_file(const char *content_type, const char *disposition);
/* Задача загрузки вышла (после ts_running = 0) — для stop(). */
int fetch_stopped(void);
