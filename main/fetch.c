/* Загрузка страницы в своей задаче (зачем — в fetch.h).
 *
 * ЗАДАЧА И ПРИОРИТЕТ. Задача загрузки работает на ядре процессора 0 с приоритетом 0 — ниже
 * всех (как онлайн-задача проигрывателя PLAYER). Договор о шифровании HTTPS (TLS) — это сотни
 * миллисекунд сплошного счёта; с низким приоритетом он идёт только тогда, когда задаче
 * браузера (приоритет 1) нечего делать, и курсор не дёргается. Соединение net_open живёт
 * только в той задаче, что его открыла, поэтому и читать его можно только отсюда.
 *
 * КАК ЗАДАЧИ ДОГОВАРИВАЮТСЯ. Задача браузера кладёт адрес в want и ставит request = 1, потом
 * «будит» задачу загрузки сигналом (signal_give). Задача загрузки спит в signal_wait, просыпается,
 * забирает адрес и грузит. Результат копится в res; когда загрузка кончилась, reported = 0
 * («итог ещё не забрали»), и fetch_poll отдаёт копию итога ровно один раз. Общие переменные
 * читаются и пишутся только под замком (mutex, см. input.c). volatile — значение меняет
 * другая задача, компилятор не должен держать его копию в регистре.
 *
 * ПАМЯТЬ ПОД СТРАНИЦУ растёт кусками по 64 КБ (realloc) до FETCH_LIMIT в PSRAM.
 * Ширина для серверов — 768 точек (решение пользователя): заголовок Viewport-Width. */
#include <stdio.h>
#include <string.h>
#include "fetch.h"
#include "ts.h"

#define STEP (64u * 1024u)                 /* на столько растёт память под страницу */
#define IDLE_MS 20000u                     /* без новых байтов дольше — обрыв */
/* Дополнительные строки запроса к серверу: какие языки мы предпочитаем (русский, потом
 * английский) и какой ширины наш экран. \r\n — конец строки в протоколе HTTP. */
#define EXTRA "Accept-Language: ru,en;q=0.8\r\nViewport-Width: 768\r\nSec-CH-Viewport-Width: 768\r\n"

static fetch_result_t res, out;          /* итог, который копится; копия для fetch_poll */
static void *wake, *mutex;               /* сигнал «проснись» и замок */
static volatile int request;             /* есть новый адрес в want */
static volatile int cancel_flag;         /* просят бросить текущую загрузку */
static volatile int worker_done = 1;     /* задача загрузки вышла (или не запускалась) */
static volatile int reported = 1;        /* итог уже забран через fetch_poll */
static volatile size_t got;              /* сколько байт принято — для fetch_progress */
static char want[sizeof(res.url)];       /* следующий адрес */
static size_t cap;                       /* сколько памяти выделено под body */

/* Есть ли в прошивке функции сети (проверка по размеру таблицы — см. ft_init в ft.c). */
/* Файл, а не страница: тип не текст и не HTML/XML (пустой тип — страница), или сервер прямо
 * сказал «attachment» — «сохранить как файл». */
int fetch_is_file(const char *ct, const char *disp) {
    if (disp && disp[0] && !strncmp(disp, "attachment", 10)) return 1;
    return ct[0] && strncmp(ct, "text/", 5) && !strstr(ct, "html") && !strstr(ct, "xml");
}

/* Число из начала строки (Content-Length); не число — 0. */
static size_t number(const char *s) {
    size_t v = 0;
    while (s && *s == ' ') s++;
    while (s && *s >= '0' && *s <= '9') v = v * 10 + (size_t)(*s++ - '0');
    return v;
}

static int net_ok(void) {
    return ts_core->size >= offsetof(zm_core_t, net_close) + sizeof(ts_core->net_close) && ts_core->net_open;
}

/* Загрузка кончилась: записать итог и время под замком. */
static void finish(int state) {
    ts_core->mutex_lock(mutex);
    res.state = state;
    res.took = ts_core->millis() - res.started;
    reported = 0;
    ts_core->mutex_unlock(mutex);
}

/* Загрузить один адрес: открыть соединение (до 15 с), узнать тип содержимого и читать тело
 * порциями: net_read ждёт данных до 300 мс и возвращает число байт, 0 — пока ничего,
 * меньше 0 — конец или обрыв. Между порциями проверяем отмену и остановку браузера. */
static void run_one(void) {
    int status = 0;
    void *st = ts_core->net_open(res.url, EXTRA, 15000, &status);
    res.status = status;
    if (!st) {
        if (status) snprintf(res.error, sizeof(res.error), "HTTP %d", status);
        else snprintf(res.error, sizeof(res.error), "no connection");
        finish(cancel_flag ? FETCH_CANCELLED : FETCH_FAILED);
        return;
    }
    /* Заголовки — под замком: их читает fetch_peek из задачи браузера. net_header прошивки
     * отдаёт значение в ОДНОМ общем буфере (HttpStream::value_), и следующий вызов затирает
     * прежнее значение. Поэтому каждое значение копируется сразу после своего вызова: раньше
     * три вызова шли подряд, тип страницы затирался её длиной («2168»), и любая страница
     * выглядела как файл (на плате 04.11 браузер «всё скачивал»). */
    ts_core->mutex_lock(mutex);
    const char *h = ts_core->net_header(st, "content-type");
    snprintf(res.content_type, sizeof(res.content_type), "%s", h ? h : "text/html");
    h = ts_core->net_header(st, "content-disposition");
    snprintf(res.disposition, sizeof(res.disposition), "%s", h ? h : "");
    h = ts_core->net_header(st, "content-length");
    res.total = number(h);
    ts_core->mutex_unlock(mutex);
    size_t limit = fetch_is_file(res.content_type, res.disposition) ? FETCH_FILE_LIMIT : FETCH_LIMIT;
    uint32_t last = ts_core->millis();
    for (;;) {
        if (cancel_flag || !__atomic_load_n(&ts_running, __ATOMIC_ACQUIRE)) break;
        if (res.length == cap) {
            if (cap >= limit) {
                snprintf(res.error, sizeof(res.error), "%s", limit == FETCH_LIMIT ? "page is larger than 2 MB" : "file is larger than 4 MB");
                break;
            }
            uint8_t *p = ts_core->realloc(res.body, cap + STEP);
            if (!p) { snprintf(res.error, sizeof(res.error), "no memory for the page"); break; }
            res.body = p;
            cap += STEP;
        }
        int n = ts_core->net_read(st, res.body + res.length, (uint32_t)(cap - res.length), 300);
        if (n < 0) break;                                   /* конец или обрыв */
        if (n > 0) { res.length += (size_t)n; got = res.length; last = ts_core->millis(); continue; }
        if ((uint32_t)(ts_core->millis() - last) > IDLE_MS) { snprintf(res.error, sizeof(res.error), "server stopped sending"); break; }
    }
    ts_core->net_close(st);
    if (cancel_flag) finish(FETCH_CANCELLED);
    else finish(res.error[0] && !res.length ? FETCH_FAILED : FETCH_DONE);
}

/* Тело задачи загрузки: ждать сигнала (не дольше 200 мс, чтобы заметить остановку браузера),
 * забрать новый адрес и загрузить. Выходит, когда ts_running станет 0. */
static void worker(void *arg) {
    (void)arg;
    while (__atomic_load_n(&ts_running, __ATOMIC_ACQUIRE)) {
        ts_core->signal_wait(wake, 200);
        ts_core->mutex_lock(mutex);
        int go = request;
        if (go) {
            request = 0; cancel_flag = 0;
            memcpy(res.url, want, sizeof(res.url));
            res.state = FETCH_BUSY; res.status = 0; res.length = 0; got = 0;
            res.error[0] = 0; res.content_type[0] = 0; res.disposition[0] = 0; res.total = 0;
            res.started = ts_core->millis();
        }
        ts_core->mutex_unlock(mutex);
        if (go) run_one();
    }
    __atomic_store_n(&worker_done, 1, __ATOMIC_RELEASE);
}

/* task_start(функция, аргумент, стек 6144 байт, приоритет 0, ядро 0). Стек 6 КБ — как у
 * задач PLAYER с HTTPS: договор о шифровании расходует много стека. */
int fetch_init(void) {
    if (!net_ok()) return 0;
#ifdef TSAFARI_HOST
    /* стенд на ПК: загрузка сразу в вызывающем потоке (tests/host.c — сеть из файлов) */
    mutex = ts_core->mutex_create();
    memset(&res, 0, sizeof(res));
    cap = 0;
    return 1;
#endif
    wake = ts_core->signal_create();
    mutex = ts_core->mutex_create();
    if (!wake || !mutex) return 0;
    memset(&res, 0, sizeof(res));
    cap = 0;
    worker_done = 0;
    if (ts_core->task_start(worker, NULL, 6144, 0, 0) != ZM_OK) { worker_done = 1; return 0; }
    return 1;
}

int fetch_stopped(void) { return __atomic_load_n(&worker_done, __ATOMIC_ACQUIRE); }

void fetch_start(const char *url) {
    ts_core->mutex_lock(mutex);
    snprintf(want, sizeof(want), "%s", url);
    if (res.state == FETCH_BUSY) cancel_flag = 1;           /* прежняя — прочь */
    request = 1;
    reported = 1;
    ts_core->mutex_unlock(mutex);
#ifdef TSAFARI_HOST
    request = 0; cancel_flag = 0;
    memcpy(res.url, want, sizeof(res.url));
    res.state = FETCH_BUSY; res.status = 0; res.length = 0; got = 0;
    res.error[0] = 0; res.content_type[0] = 0; res.disposition[0] = 0; res.total = 0;
    res.started = ts_core->millis();
    run_one();
#else
    ts_core->signal_give(wake);
#endif
}

void fetch_cancel(void) {
    ts_core->mutex_lock(mutex);
    request = 0;
    if (res.state == FETCH_BUSY) cancel_flag = 1;
    ts_core->mutex_unlock(mutex);
}

/* Пока есть новый, ещё не взятый адрес — «идёт загрузка». Итог отдаётся копией (out), чтобы
 * задача загрузки могла сразу начать следующую загрузку, не портя то, что читает браузер. */
int fetch_poll(fetch_result_t **r) {
    int st;
    ts_core->mutex_lock(mutex);
    st = request ? FETCH_BUSY : res.state;
    if (!request && !reported && (st == FETCH_DONE || st == FETCH_FAILED || st == FETCH_CANCELLED)) {
        reported = 1;
        out = res;
        res.state = FETCH_IDLE;
        if (r) *r = &out;
    } else if (st != FETCH_BUSY) {
        st = FETCH_IDLE;
    }
    ts_core->mutex_unlock(mutex);
    return st;
}

size_t fetch_progress(void) { return got; }

int fetch_peek(fetch_result_t *r) {
    int ok = 0;
    ts_core->mutex_lock(mutex);
    if (!request && res.state == FETCH_BUSY && res.content_type[0]) {
        *r = res;
        r->length = got;
        ok = 1;
    }
    ts_core->mutex_unlock(mutex);
    return ok;
}

uint8_t *fetch_take_body(void) {
    uint8_t *b = NULL;
    ts_core->mutex_lock(mutex);
    if (!request && res.state != FETCH_BUSY) {
        b = res.body;
        res.body = NULL;
        out.body = NULL;
        cap = 0;
    }
    ts_core->mutex_unlock(mutex);
    return b;
}
