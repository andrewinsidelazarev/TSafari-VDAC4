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
#include "download.h"
#include "url.h"
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

static void run_stream(void *st);

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
    /* Сервер мог переадресовать (google.com → www.google.com), и прошивка сама сходила по новому
     * адресу. Адрес страницы — тот, откуда она пришла: от него считаются относительные ссылки
     * («/images/…» с google.com — 404, на плате 04.11 не встал логотип). Прошивка 0.1.119+ отдаёт
     * его как «:url»; старая — NULL, тогда адрес остаётся набранным. */
    h = ts_core->net_header(st, ":url");
    if (h && h[0] && strlen(h) < sizeof(res.url)) memcpy(res.url, h, strlen(h) + 1);
    ts_core->mutex_unlock(mutex);
    if (!res.image && fetch_is_file(res.content_type, res.disposition)) { run_stream(st); return; }
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

/* Файл, а не страница: не копить его в памяти, а сразу отдавать на карту Evo через кольцо
 * download.c (зачем — там). Кольцо полно — download_stream_put ждёт, пока Z80 запишет часть;
 * сервер тем временем держит соединение (TCP сам притормаживает передачу). */
static void run_stream(void *st) {
    static uint8_t chunk[4096];                     /* static — не в стеке задачи загрузки */
    if (!download_stream_begin(res.url, res.disposition, (uint32_t)res.total)) {
        snprintf(res.error, sizeof(res.error), "another download is in progress");
        ts_core->net_close(st);
        finish(FETCH_FAILED);
        return;
    }
    ts_core->mutex_lock(mutex);
    res.streamed = 1;
    ts_core->mutex_unlock(mutex);
    size_t total = 0;
    int ok = 1;
    uint32_t last = ts_core->millis();
    for (;;) {
        if (cancel_flag || !__atomic_load_n(&ts_running, __ATOMIC_ACQUIRE)) {
            ok = 0;
            snprintf(res.error, sizeof(res.error), "Download cancelled");
            break;
        }
        int n = ts_core->net_read(st, chunk, sizeof(chunk), 300);
        if (n < 0) break;                                   /* конец или обрыв */
        if (n == 0) {
            if ((uint32_t)(ts_core->millis() - last) > IDLE_MS) {
                ok = 0;
                snprintf(res.error, sizeof(res.error), "Server stopped sending");
                break;
            }
            continue;
        }
        if (!download_stream_put(chunk, (size_t)n, &cancel_flag)) {   /* отменили или карта сорвалась */
            ok = 0;
            snprintf(res.error, sizeof(res.error), "Download cancelled");
            break;
        }
        total += (size_t)n;
        got = total;
        last = ts_core->millis();
    }
    ts_core->net_close(st);
    if (ok && res.total && total < res.total) {
        ok = 0;
        snprintf(res.error, sizeof(res.error), "Connection broke off");
    }
    download_stream_end(ok, res.error);
    ts_log("tsafari: download %s: %u bytes from the network%s", res.url, (unsigned)total, ok ? "" : ", failed");
    finish(cancel_flag ? FETCH_CANCELLED : ok ? FETCH_DONE : FETCH_FAILED);
}

/* --- отправка формы POST ---------------------------------------------------------------------
 * net_open прошивки умеет только GET. POST собираем сами по сокету TCP (sock_open): строка
 * запроса, заголовки, пустая строка и тело «имя=значение&…». Ответ — заголовки и тело; если
 * сервер отвечает «смотри другой адрес» (303 и подобные — так почти всегда после POST), идём
 * туда обычным GET. По HTTPS — тот же запрос через сокет с шифрованием TLS (sock_open_tls,
 * ABI 1.12, прошивка ESP 0.1.117): сертификат сервера проверяет прошивка, договор TLS идёт в этой
 * задаче (её стека 6 КБ хватает, как и net_open). Со старой прошивкой POST по HTTPS — ошибка. */
static char *post_body;                  /* тело текущей отправки (забрано у want_body) */
static size_t post_len;
static int want_image;                   /* следующая загрузка — картинка */
static char *want_body;                  /* тело следующей отправки; NULL — следующая загрузка GET */
static size_t want_len;

static int sock_ok(void) {
    return ts_core->size >= offsetof(zm_core_t, sock_close) + sizeof(ts_core->sock_close) && ts_core->sock_open;
}
static int tls_ok(void) {
    return ts_core->size >= offsetof(zm_core_t, sock_open_tls) + sizeof(ts_core->sock_open_tls) && ts_core->sock_open_tls;
}

/* «http[s]://хост[:порт]/путь» → хост, порт (по умолчанию 80 или 443), путь; *tls = 1 у https.
 * 0 — не http и не https. */
static int split_url(const char *url, char *host, size_t cap, uint16_t *port, const char **path, int *tls) {
    *tls = !strncmp(url, "https://", 8);
    if (!*tls && strncmp(url, "http://", 7)) return 0;
    const char *h = url + (*tls ? 8 : 7), *e = h;
    while (*e && *e != '/' && *e != ':' && *e != '?' && *e != '#') e++;
    size_t hl = (size_t)(e - h);
    if (!hl || hl >= cap) return 0;
    memcpy(host, h, hl);
    host[hl] = 0;
    *port = *tls ? 443 : 80;
    if (*e == ':') {
        unsigned p = 0;
        for (e++; *e >= '0' && *e <= '9'; e++) p = p * 10 + (unsigned)(*e - '0');
        if (p && p < 65536) *port = (uint16_t)p;
    }
    *path = *e == '/' ? e : "/";
    return 1;
}

/* Значение заголовка name (без учёта регистра) из блока заголовков h (n байт) — в out. */
static int header_value(const char *h, size_t n, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl < n; i++) {
        if (i && h[i - 1] != '\n') continue;
        size_t k = 0;
        while (k < nl && (h[i + k] | 0x20) == (name[k] | 0x20)) k++;
        if (k < nl || h[i + nl] != ':') continue;
        size_t a = i + nl + 1, b;
        while (a < n && h[a] == ' ') a++;
        for (b = a; b < n && h[b] != '\r' && h[b] != '\n'; b++) {}
        size_t len = b - a < cap - 1 ? b - a : cap - 1;
        memcpy(out, h + a, len);
        out[len] = 0;
        return 1;
    }
    return 0;
}

/* Тело кусками «chunked» (длина шестнадцатеричным числом, строка, кусок, … до длины 0) —
 * склеить на месте. Возвращает новую длину. */
static size_t unchunk(uint8_t *b, size_t n) {
    size_t i = 0, o = 0;
    while (i < n) {
        size_t len = 0;
        int digits = 0;
        for (; i < n; i++) {
            int c = b[i], d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
            if (d < 0) break;
            len = len * 16 + (size_t)d;
            digits++;
        }
        while (i < n && b[i] != '\n') i++;                  /* до конца строки длины */
        i++;
        if (!digits || !len || i >= n) break;
        if (len > n - i) len = n - i;
        memmove(b + o, b + i, len);
        o += len;
        i += len + 2;                                       /* кусок и его «\r\n» */
    }
    return o;
}

static void run_one(void);

static void run_post(void) {
    /* static — не в стеке задачи загрузки: после «смотри другой адрес» отсюда идёт run_one, а в
     * нём — договор о шифровании HTTPS, которому нужен почти весь стек (6 КБ) */
    static char head[640 + sizeof(res.url)], host[128], port_s[8], v[sizeof(res.url)], next[sizeof(res.url)];
    uint16_t port;
    const char *path;
    int tls;
    if (!sock_ok() || !split_url(res.url, host, sizeof(host), &port, &path, &tls)) {
        snprintf(res.error, sizeof(res.error), "cannot send this form");
        finish(FETCH_FAILED);
        return;
    }
    if (tls && !tls_ok()) {
        snprintf(res.error, sizeof(res.error), "POST over HTTPS needs ESP firmware 0.1.117 or newer");
        finish(FETCH_FAILED);
        return;
    }
    int s = tls ? ts_core->sock_open_tls(host, port, 15000) : ts_core->sock_open(0, host, port, 15000);
    if (s < 0) {
        snprintf(res.error, sizeof(res.error), "%s", tls ? "no connection or bad certificate" : "no connection");
        finish(cancel_flag ? FETCH_CANCELLED : FETCH_FAILED);
        return;
    }
    port_s[0] = 0;
    if (port != (tls ? 443 : 80)) snprintf(port_s, sizeof(port_s), ":%u", (unsigned)port);
    int hn = snprintf(head, sizeof(head),
                      "POST %s HTTP/1.0\r\nHost: %s%s\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: %u\r\n" EXTRA "Connection: close\r\n\r\n",
                      path, host, port_s, (unsigned)post_len);
    int ok = hn > 0 && hn < (int)sizeof(head) && ts_core->sock_send(s, head, (uint32_t)hn) == ZM_OK &&
             (!post_len || ts_core->sock_send(s, post_body, (uint32_t)post_len) == ZM_OK);
    uint32_t last = ts_core->millis();
    while (ok) {                                            /* ответ целиком: заголовки и тело */
        if (cancel_flag || !__atomic_load_n(&ts_running, __ATOMIC_ACQUIRE)) break;
        if (res.length == cap) {
            if (cap >= FETCH_LIMIT) { snprintf(res.error, sizeof(res.error), "page is larger than 2 MB"); break; }
            uint8_t *p = ts_core->realloc(res.body, cap + STEP);
            if (!p) { snprintf(res.error, sizeof(res.error), "no memory for the page"); break; }
            res.body = p;
            cap += STEP;
        }
        int n = ts_core->sock_recv(s, res.body + res.length, (uint32_t)(cap - res.length), 300);
        if (n < 0) break;
        if (n > 0) { res.length += (size_t)n; got = res.length; last = ts_core->millis(); continue; }
        if ((uint32_t)(ts_core->millis() - last) > IDLE_MS) { snprintf(res.error, sizeof(res.error), "server stopped sending"); break; }
    }
    ts_core->sock_close(s);
    if (cancel_flag) { finish(FETCH_CANCELLED); return; }
    if (!ok) { snprintf(res.error, sizeof(res.error), "cannot send the form"); finish(FETCH_FAILED); return; }
    /* заголовки ответа: «HTTP/1.1 200 OK», строки «Имя: значение», пустая строка */
    size_t hend = 0;
    for (size_t i = 0; i + 3 < res.length; i++)
        if (res.body[i] == '\r' && res.body[i + 1] == '\n' && res.body[i + 2] == '\r' && res.body[i + 3] == '\n') { hend = i + 4; break; }
    if (!hend || res.length < 12 || strncmp((const char *)res.body, "HTTP/", 5)) {
        snprintf(res.error, sizeof(res.error), "bad answer from the server");
        finish(FETCH_FAILED);
        return;
    }
    const char *h = (const char *)res.body;
    const char *sp = memchr(h, ' ', hend);
    int status = sp ? (int)number(sp + 1) : 0;
    res.status = status;
    if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
        header_value(h, hend, "location", v, sizeof(v))) {
        if (url_resolve(res.url, v, next, sizeof(next))) {   /* ответ — на другом адресе: GET туда */
            ts_core->mutex_lock(mutex);
            memcpy(res.url, next, sizeof(res.url));
            res.length = 0; got = 0;
            ts_core->mutex_unlock(mutex);
            run_one();
            return;
        }
    }
    ts_core->mutex_lock(mutex);
    snprintf(res.content_type, sizeof(res.content_type), "%.95s", header_value(h, hend, "content-type", v, sizeof(v)) ? v : "text/html");
    snprintf(res.disposition, sizeof(res.disposition), "%.127s", header_value(h, hend, "content-disposition", v, sizeof(v)) ? v : "");
    ts_core->mutex_unlock(mutex);
    int chunked = header_value(h, hend, "transfer-encoding", v, sizeof(v)) && strstr(v, "chunked");
    memmove(res.body, res.body + hend, res.length - hend);
    res.length -= hend;
    if (chunked) res.length = unchunk(res.body, res.length);
    got = res.length;
    if (status < 200 || status >= 300) {
        snprintf(res.error, sizeof(res.error), "HTTP %d", status);
        finish(FETCH_FAILED);
        return;
    }
    finish(FETCH_DONE);
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
            if (post_body) ts_core->free(post_body);       /* тело отправки — забрать себе */
            post_body = want_body; post_len = want_len;
            want_body = NULL; want_len = 0;
            request = 0; cancel_flag = 0;
            memcpy(res.url, want, sizeof(res.url));
            res.state = FETCH_BUSY; res.status = 0; res.length = 0; got = 0; res.streamed = 0;
            res.image = want_image;
            res.error[0] = 0; res.content_type[0] = 0; res.disposition[0] = 0; res.total = 0;
            res.started = ts_core->millis();
        }
        ts_core->mutex_unlock(mutex);
        if (go && post_body) run_post();
        else if (go) run_one();
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

/* Новый запрос: адрес и тело (NULL — GET) кладутся под замком вместе, потом задача загрузки
 * будится — так она не возьмёт адрес без тела. */
static void start_request(const char *url, char *body, size_t len, int image) {
    ts_core->mutex_lock(mutex);
    snprintf(want, sizeof(want), "%s", url);
    want_image = image;
    if (want_body) ts_core->free(want_body);
    want_body = body; want_len = len;
    if (res.state == FETCH_BUSY) cancel_flag = 1;           /* прежняя — прочь */
    request = 1;
    reported = 1;
    ts_core->mutex_unlock(mutex);
#ifdef TSAFARI_HOST
    request = 0; cancel_flag = 0;
    memcpy(res.url, want, sizeof(res.url));
    res.state = FETCH_BUSY; res.status = 0; res.length = 0; got = 0; res.streamed = 0;
    res.image = want_image;
    res.error[0] = 0; res.content_type[0] = 0; res.disposition[0] = 0; res.total = 0;
    res.started = ts_core->millis();
    if (post_body) ts_core->free(post_body);
    post_body = want_body; post_len = want_len;
    want_body = NULL; want_len = 0;
    if (post_body) run_post(); else run_one();
#else
    ts_core->signal_give(wake);
#endif
}

void fetch_start(const char *url) { start_request(url, NULL, 0, 0); }
void fetch_start_image(const char *url) { start_request(url, NULL, 0, 1); }

void fetch_post(const char *url, const char *body, size_t len) {
    char *copy = ts_alloc(len + 1);
    if (!copy) return;
    memcpy(copy, body, len);
    copy[len] = 0;
    start_request(url, copy, len, 0);
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
