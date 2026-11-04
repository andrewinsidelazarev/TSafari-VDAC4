/* TSAFARI.ELF — главный файл браузера: запуск, главный цикл, история, обмен с Z80.
 * (Общая схема — в ts.h.)
 *
 * КАК ПРИЛОЖЕНИЕ ЗАПУСКАЕТСЯ. Прошивка ESP32 загружает TSAFARI.ELF в память и вызывает main
 * (в самом конце файла). main отдаёт прошивке таблицу zm_app_t: имя, версию и функции
 * start, stop, command. Дальше прошивка вызывает:
 *   start   — один раз, при запуске: здесь запускается задача браузера;
 *   command — каждый раз, когда программа Z80 прислала команду (0x73 или 0x75);
 *   stop    — при выходе: всё остановить.
 *
 * ОБМЕН С Z80. Программа Z80 (TSAFARI.SPG) переключает экран компьютера на FT812 (M_SEL = 1),
 * много раз в секунду шлёт команду 0x75 с мышью и клавишами (input.h) и спрашивает состояние
 * командой 0x73. Ответ: 1 — подготовка, 2 — работает, 3 — пользователь вышел (F12),
 * 255 — ошибка (тогда в ответе и текст ошибки, его покажет Z80). Так же устроен DOOM VDAC4.
 *
 * ЗАДАЧА БРАУЗЕРА — ядро процессора 0, приоритет 1. Обмен с Z80 идёт на ядре 1 с приоритетом 1;
 * если поставить туда задачу выше, Z80 перестанет получать ответы (так было у проигрывателя
 * 1.26). Задача браузера крутит цикл: ввод → действия → загрузка → прокрутка → кадр. */
#include <string.h>
#include <stdio.h>
#include "ts.h"
#include "ft.h"
#include "font.h"
#include "page.h"
#include "doc.h"
#include "ui.h"
#include "input.h"
#include "demo.h"
#include "fetch.h"
#include "download.h"
#include "charset.h"
#include "html.h"
#include "url.h"
#include "ui_design.h"

/* Состояние браузера — его спрашивает Z80 командой 0x73. */
enum { ST_START = 0, ST_LOADING = 1, ST_RUNNING = 2, ST_EXIT = 3, ST_ERROR = 255 };

const zm_core_t *ts_core;
volatile int ts_running;
static volatile int state = ST_START;
static volatile int task_done = 1;       /* задача браузера вышла */
static char error_text[64];              /* текст ошибки для Z80 */
static ft_dl_t *dl;                      /* дисплей-лист, который собирается каждый кадр */

#define WIFI_WAIT_MS 20000u              /* сколько ждать Wi-Fi до сообщения «не удалось» */
#define MOUSE_GAIN 2                     /* курсор движется вдвое быстрее мыши: экран большой */

void *ts_alloc(size_t bytes) { return ts_core->alloc(bytes, ZM_MEM_PSRAM); }

/* Ошибка, после которой браузер работать не может: записать текст и сообщить Z80. */
static void fail(const char *text) {
    strncpy(error_text, text, sizeof(error_text) - 1);
    ts_log("tsafari: %s", text);
    __atomic_store_n(&state, ST_ERROR, __ATOMIC_RELEASE);
}

/* Собрать и отправить кадр: очистить экран цветом фона, нарисовать страницу (если не открыто
 * окно Wi-Fi), поверх — интерфейс, в конце — DISPLAY («список окончен»). Переполненный лист
 * не отправляем: на экране остаётся прошлый кадр. */
static int frame(void) {
    dl->n = 0; dl->overflow = 0;
    /* под окном Wi-Fi — пустой фон бумаги, страницы ещё нет (DESIGN.md) */
    dl_put(dl, dl_clear_color(ui_wifi_open() ? TSA_UI_COLOR_PAPER_RGB888 : page_bg()));
    dl_put(dl, dl_clear());
    if (!ui_wifi_open()) page_dl(dl);
    ui_dl(dl);
    dl_put(dl, dl_display());
    if (dl->overflow) { ts_log("tsafari: display list overflow"); return 1; }
    return ft_dl_commit(dl->w, dl->n);
}

/* --- Wi-Fi: окно на старте; подключилось — окно сразу уходит, в том числе после ошибки --- */
static int wifi_ok, wifi_failed;         /* подключились; прошло WIFI_WAIT_MS без подключения */
static uint32_t wifi_since, wifi_poll_at; /* когда открыли окно; когда спрашивали прошивку */

/* Спросить прошивку, подключён ли Wi-Fi: имя сети (ssid) и адрес (ip). */
static int wifi_connected(char *ssid, char *ip) {
    char mac[18];
    if (ts_core->size < offsetof(zm_core_t, wifi_info) + sizeof(ts_core->wifi_info) || !ts_core->wifi_info) return 0;
    return ts_core->wifi_info(ssid, ip, mac) == ZM_OK;
}

/* Раз в 250 мс, пока нет связи: подключились — закрыть окно; ждём дольше 20 с — показать
 * «не удалось» (прошивка ищет сеть дальше, и окно закроется, как только связь появится). */
static void wifi_step(uint32_t now) {
    if (wifi_ok || (uint32_t)(now - wifi_poll_at) < 250) return;
    wifi_poll_at = now;
    char ssid[33] = "", ip[16] = "";
    if (wifi_connected(ssid, ip)) {
        wifi_ok = 1;
        ui_wifi(UI_WIFI_OFF);
        ui_set_busy(0);
        ts_log("tsafari: Wi-Fi %s, %s", ssid, ip);
    } else if (ui_wifi_open() && !wifi_failed && (uint32_t)(now - wifi_since) > WIFI_WAIT_MS) {
        wifi_failed = 1;                        /* сеть ищется дальше; F12 — выход */
        ui_wifi(UI_WIFI_FAILED);
        ui_set_busy(0);
        ts_log("tsafari: no Wi-Fi after %u s", (unsigned)(WIFI_WAIT_MS / 1000));
    }
}

/* Не прокручивать выше начала и ниже конца страницы. */
static void clamp_target(int *t) {
    int limit = page_height() > TS_H ? page_height() - TS_H : 0;
    if (*t > limit) *t = limit;
    if (*t < 0) *t = 0;
}

/* --- адреса, история, загрузка --- */
/* История — массив из 32 записей: hist_n — сколько в ней, hist_at — где мы сейчас. «Назад» —
 * hist_at − 1, «вперёд» — hist_at + 1. Запись — адрес страницы, а если это набор фреймов, то
 * и адреса страниц в его фреймах на тот момент: строка «имя TAB адрес» по фрейму на строку,
 * в PSRAM (NULL — фреймы такие, какие даёт сама страница). Так «назад» возвращает и переходы
 * внутри фреймов. */
typedef struct {
    char url[URL_MAX];
    char *frames;
} hist_t;
#define HISTORY 32
static hist_t history[HISTORY];
static char current[URL_MAX];            /* адрес страницы на экране (или загружаемой) */
static char shown_url[URL_MAX];          /* адрес страницы, которая сейчас на экране */
static int nav_pushed;                   /* текущий переход добавил запись в историю */
static int net_file;                     /* сейчас из сети качается файл (по заголовкам), а не страница */
static int hist_n, hist_at;
static int loading;                      /* идёт загрузка */
static int internal_page;                /* на экране встроенная страница (ссылки — на её разделы) */
static int net_ready;                    /* сеть есть (fetch_init удался) */
static int target;                       /* куда хотим прокрутить; страница догоняет по 128 строк */

/* --- фреймы (что это — в html.h) ---
 * fr — фреймы страницы на экране (fr_n = 0 — обычная страница), nx — набор, который сейчас
 * загружается. У каждого фрейма своя копия ответа сервера (body): буфер загрузки (fetch.c)
 * занят уже следующим фреймом. Переход внутри фрейма копирует fr в nx (тела общие), меняет
 * адрес одного фрейма и грузит только его. Когда всё загружено, nx становится fr, а тела,
 * которых в новом наборе нет, освобождаются. */
typedef struct {
    char name[32];                       /* имя фрейма (его ищет target у ссылок) */
    char url[URL_MAX];
    uint8_t *body;                       /* копия ответа (PSRAM); NULL — ещё не загружен или ошибка */
    size_t length;
    char content_type[96];
    char error[96];                      /* не загрузился: почему */
    int y;                               /* где фрейм начинается на составной странице */
} frame_t;
static frame_t fr[HTML_FRAMES_MAX], nx[HTML_FRAMES_MAX];
static int fr_n, nx_n;
static int nx_loading;                   /* 1 — грузим фреймы nx (а не обычную страницу) */
static int nx_at;                        /* какой фрейм nx сейчас грузится */
static int nx_focus;                     /* куда прокрутить потом: фрейм; −1 — к началу; −2 — где был */
static int nx_expand;                    /* сколько фреймов уже оказались сами наборами фреймов */

/* Строки без учёта регистра латиницы (имена фреймов: «Down» и «down» — одно и то же). */
static int same_ci(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* Освободить тела фреймов a, которых нет среди keep (у fr и nx тела бывают общие). */
static void free_bodies(frame_t *a, int an, const frame_t *keep, int kn) {
    for (int i = 0; i < an; i++) {
        if (!a[i].body) continue;
        int shared = 0;
        for (int k = 0; k < kn && !shared; k++) shared = keep[k].body == a[i].body;
        if (!shared) ts_core->free(a[i].body);
        a[i].body = NULL;
    }
}
/* Бросить недогруженный набор nx. */
static void frames_abort(void) {
    if (nx_loading) free_bodies(nx, nx_n, fr, fr_n);
    nx_n = 0;
    nx_loading = 0;
}
/* На экране будет обычная страница: фреймы больше не нужны. */
static void frames_clear(void) {
    frames_abort();
    free_bodies(fr, fr_n, NULL, 0);
    fr_n = 0;
}
/* Адреса фреймов для записи истории (строка в PSRAM; NULL — нет памяти). */
static char *frames_state(const frame_t *f, int n) {
    size_t len = 1;
    for (int i = 0; i < n; i++) len += strlen(f[i].name) + strlen(f[i].url) + 2;
    char *s = ts_alloc(len);
    if (!s) return NULL;
    size_t o = 0;
    s[0] = 0;
    for (int i = 0; i < n; i++) o += (size_t)snprintf(s + o, len - o, "%s\t%s\n", f[i].name, f[i].url);
    return s;
}

static void history_drop(int i) {
    if (history[i].frames) ts_core->free(history[i].frames);
    history[i].frames = NULL;
}
static void history_ui(void) { ui_set_history(hist_at > 0, hist_at + 1 < hist_n); }

/* Добавить запись в историю после текущей (frames — адреса фреймов из frames_state или NULL;
 * запись забирает строку себе). Всё, что было «впереди», выбрасывается — как в любом
 * браузере. Переполнилась — старейшая запись уходит. Тот же адрес без фреймов второй раз
 * подряд не добавляется. 1 — запись добавлена. */
static int history_push(const char *url, char *frames) {
    if (!frames && hist_n && !history[hist_at].frames && !strcmp(history[hist_at].url, url)) return 0;
    for (int i = hist_n ? hist_at + 1 : 0; i < hist_n; i++) history_drop(i);   /* «вперёд» — прочь */
    hist_n = hist_n ? hist_at + 1 : 0;
    if (hist_n == HISTORY) {
        history_drop(0);
        memmove(history, history + 1, sizeof(history[0]) * (HISTORY - 1));
        hist_n--;
    }
    snprintf(history[hist_n].url, URL_MAX, "%s", url);
    history[hist_n].frames = frames;
    hist_at = hist_n++;
    return 1;
}

/* Новая страница на экране: верх, кольцо целиком. */
static int show_new_page(void) {
    snprintf(shown_url, sizeof(shown_url), "%s", current);
    target = 0;
    return page_scroll_to(0);
}

/* Переход не состоялся (по ссылке пришёл файл, а не страница): вернуть адрес страницы на
 * экране в строку адреса и убрать из истории запись, которую добавил этот переход. */
static void undo_nav(void) {
    if (nav_pushed && hist_n) {
        history_drop(hist_at);
        hist_n = hist_at;
        hist_at = hist_n ? hist_n - 1 : 0;
    }
    nav_pushed = 0;
    snprintf(current, sizeof(current), "%s", shown_url);
    ui_set_url(current);
    history_ui();
}

/* По ссылке пришёл файл: память с ним — модулю сохранения на карту Evo (download.c), а на
 * экране остаётся прежняя страница. Пустой файл тоже сохраняется (памяти у него нет). */
static int save_file(const fetch_result_t *r) {
    uint8_t *body = fetch_take_body();
    if (r->error[0]) {                           /* не докачался (больше 4 МБ, обрыв) — не писать огрызок */
        if (body) ts_core->free(body);
        download_note(r->url, r->disposition, r->error, 0);
    } else if (body || !r->length) {
        download_start(body, r->length, r->url, r->disposition);
    }
    loading = 0;
    ui_set_busy(0);
    undo_nav();
    return 1;
}

/* Перейти по адресу: записать его в строку адреса и историю (push = 1; при «назад», «вперёд»
 * и «обновить» — 0), встроенную страницу показать сразу, остальные — начать загружать. Пока
 * грузится, на экране остаётся прежняя страница, а глобус вращается. */
static int open_url(const char *url, int push) {
    frames_abort();
    snprintf(current, sizeof(current), "%s", url);
    ui_set_url(url);
    nav_pushed = push && history_push(url, NULL);
    history_ui();
    if (url_is_internal(url) || !net_ready) {
        if (loading) fetch_cancel();
        loading = 0;
        ui_set_busy(0);
        frames_clear();
        if (url_is_internal(url)) { demo_page(); internal_page = 1; }
        else { html_error_page(url, "Network is not available", "This ESP firmware has no net_open (ABI 1.2+)."); internal_page = 0; }
        return show_new_page();
    }
    loading = 1;
    ui_set_busy(1);
    fetch_start(url);
    return 1;
}


/* Набор nx: загрузить следующий ещё не загруженный фрейм. Все загружены — nx становится fr,
 * страницы фреймов собираются сверху вниз в одну, и она показывается: с начала, с фрейма,
 * куда вела ссылка (если он начинается ниже середины экрана), или там же, где была
 * («обновить»). 0 — не удалось отправить страницу в видеочип. */
static int frames_next(void) {
    for (nx_at = 0; nx_at < nx_n; nx_at++)
        if (!nx[nx_at].body && !nx[nx_at].error[0]) {
            loading = 1;
            ui_set_busy(1);
            fetch_start(nx[nx_at].url);
            return 1;
        }
    int was_top = page_top();
    free_bodies(fr, fr_n, nx, nx_n);
    memcpy(fr, nx, sizeof(nx[0]) * (size_t)nx_n);
    fr_n = nx_n;
    nx_n = 0;
    nx_loading = 0;
    loading = 0;
    ui_set_busy(0);
    internal_page = 0;
    snprintf(shown_url, sizeof(shown_url), "%s", current);
    uint32_t t0 = ts_core->millis();
    html_begin();
    for (int i = 0; i < fr_n; i++) {
        fr[i].y = doc_y();
        if (fr[i].body) html_part(fr[i].body, fr[i].length, fr[i].content_type, fr[i].url, i);
        else html_part_note(fr[i].url, fr[i].error, i);
    }
    html_end();
    ts_log("tsafari: %s: %d frames, layout %u ms, %d px", current, fr_n, (unsigned)(ts_core->millis() - t0), page_height());
    target = 0;
    if (nx_focus == -2) target = was_top;
    else if (nx_focus >= 0 && nx_focus < fr_n && fr[nx_focus].y > TS_H / 2) target = fr[nx_focus].y - PAGE_MARGIN;
    clamp_target(&target);
    return page_scroll_to(target);
}

/* Начать загрузку набора nx (у фреймов, которые надо загрузить, body = NULL, error пуст). */
static int frames_load(int focus) {
    nx_loading = 1;
    nx_focus = focus;
    nx_expand = 0;
    return frames_next();
}

/* Фрейм nx[nx_at] загрузился (или нет). Его страница сама оказалась набором фреймов —
 * его место занимают её фреймы (не больше 3 раз за загрузку: страница может ссылаться
 * сама на себя). */
static int frame_done(int st, const fetch_result_t *r) {
    frame_t *f = &nx[nx_at];
    if (st == FETCH_DONE && fetch_is_file(r->content_type, r->disposition)) {
        frames_abort();                          /* файл — на карту, фреймы остаются как были */
        return save_file(r);
    }
    if (st == FETCH_FAILED) {
        snprintf(f->error, sizeof(f->error), "%s", r->error[0] ? r->error : "cannot open");
    } else {
        html_frame_t sub[HTML_FRAMES_MAX];
        int k = nx_expand < 3 ? html_frames(r->body, r->length, r->content_type, r->url, sub, HTML_FRAMES_MAX - nx_n + 1) : 0;
        if (k > 0) {
            nx_expand++;
            memmove(&nx[nx_at + k], &nx[nx_at + 1], sizeof(nx[0]) * (size_t)(nx_n - nx_at - 1));
            for (int i = 0; i < k; i++) {
                frame_t *c = &nx[nx_at + i];
                memset(c, 0, sizeof(*c));
                snprintf(c->name, sizeof(c->name), "%s", sub[i].name);
                snprintf(c->url, sizeof(c->url), "%s", sub[i].url);
            }
            nx_n += k - 1;
            return frames_next();
        }
        uint8_t *copy = ts_alloc(r->length ? r->length : 1);
        if (!copy) {
            snprintf(f->error, sizeof(f->error), "no memory for the page");
        } else {
            memcpy(copy, r->body, r->length);
            f->body = copy;
            f->length = r->length;
            snprintf(f->content_type, sizeof(f->content_type), "%s", r->content_type);
        }
        ts_log("tsafari: frame %s: %u bytes in %u ms", r->url, (unsigned)r->length, (unsigned)r->took);
    }
    return frames_next();
}

/* Куда открыть ссылку на странице из фреймов: номер фрейма или −1 — во всё окно.
 * Без target (или _self, _parent) — в тот же фрейм, где ссылка; имя фрейма — в него;
 * _top, _blank и незнакомое имя — во всё окно (второго окна у нас нет). */
static int link_frame(int id) {
    if (!fr_n) return -1;
    const char *t = html_link_target(id);
    int part = html_link_part(id);
    if (!t[0] || same_ci(t, "_self") || same_ci(t, "_parent")) return part < fr_n ? part : -1;
    for (int i = 0; i < fr_n; i++)
        if (fr[i].name[0] && same_ci(fr[i].name, t)) return i;
    return -1;
}

/* Щелчок по ссылке: во всё окно — как набранный адрес; во фрейм — загрузить новую страницу
 * только в этот фрейм, остальные фреймы остаются как есть. */
static int follow_link(int id) {
    const char *url = html_link(id);
    if (!url) return 1;
    int f = link_frame(id);
    if (f < 0) return open_url(url, 1);
    frames_abort();
    if (loading) fetch_cancel();
    memcpy(nx, fr, sizeof(fr[0]) * (size_t)fr_n);
    nx_n = fr_n;
    snprintf(nx[f].url, sizeof(nx[f].url), "%s", url);
    nx[f].body = NULL;
    nx[f].length = 0;
    nx[f].error[0] = 0;
    nav_pushed = history_push(current, frames_state(nx, nx_n));
    history_ui();
    return frames_load(f);
}

/* Открыть запись истории номер at («назад», «вперёд»). Запись с фреймами — сразу их адреса,
 * страницу самого набора заново не грузим. */
static int open_history(int at) {
    hist_at = at;
    nav_pushed = 0;
    const hist_t *h = &history[at];
    if (!h->frames) return open_url(h->url, 0);
    frames_abort();
    snprintf(current, sizeof(current), "%s", h->url);
    ui_set_url(h->url);
    history_ui();
    nx_n = 0;
    for (const char *p = h->frames; *p && nx_n < HTML_FRAMES_MAX;) {
        const char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
        if (!tab || !nl || tab > nl) break;
        frame_t *f = &nx[nx_n++];
        memset(f, 0, sizeof(*f));
        snprintf(f->name, sizeof(f->name), "%.*s", (int)(tab - p), p);
        snprintf(f->url, sizeof(f->url), "%.*s", (int)(nl - tab - 1), tab + 1);
        p = nl + 1;
    }
    return frames_load(-1);
}

/* «Обновить»: у набора фреймов — заново все фреймы с их нынешними адресами, прокрутка
 * остаётся; обычная страница — заново по адресу. */
static int reload(void) {
    nav_pushed = 0;
    if (!fr_n) return open_url(current, 0);
    frames_abort();
    if (loading) fetch_cancel();
    memcpy(nx, fr, sizeof(fr[0]) * (size_t)fr_n);
    nx_n = fr_n;
    for (int i = 0; i < nx_n; i++) { nx[i].body = NULL; nx[i].length = 0; nx[i].error[0] = 0; }
    return frames_load(-2);
}

/* Загрузка кончилась? Тогда разобрать страницу (или показать ошибку) и вывести её с начала.
 * Страница оказалась набором фреймов — начать загрузку фреймов. 0 — не удалось отправить
 * страницу в видеочип. Время загрузки и раскладки — в журнал. */
static int fetch_step(void) {
    fetch_result_t *r = NULL;
    int st = fetch_poll(&r);
    if (st == FETCH_BUSY || st == FETCH_IDLE || !r) return 1;
    loading = 0;
    ui_set_busy(0);
    int was_file = net_file;
    net_file = 0;
    if (was_file && st != FETCH_DONE) {          /* файл не докачался: сказать в индикаторе */
        download_note(r->url, r->disposition, st == FETCH_CANCELLED ? "Download cancelled" : "Network error",
                      st == FETCH_CANCELLED);
        frames_abort();
        undo_nav();
        return 1;
    }
    if (st == FETCH_CANCELLED) { frames_abort(); return 1; }   /* стоп — прежняя страница остаётся */
    if (nx_loading) return frame_done(st, r);
    if (st == FETCH_DONE && fetch_is_file(r->content_type, r->disposition)) return save_file(r);
    if (st == FETCH_DONE) {
        html_frame_t sub[HTML_FRAMES_MAX];
        int k = html_frames(r->body, r->length, r->content_type, r->url, sub, HTML_FRAMES_MAX);
        if (k > 0) {
            ts_log("tsafari: %s: frameset of %d frames", r->url, k);
            nx_n = k;
            for (int i = 0; i < k; i++) {
                memset(&nx[i], 0, sizeof(nx[i]));
                snprintf(nx[i].name, sizeof(nx[i].name), "%s", sub[i].name);
                snprintf(nx[i].url, sizeof(nx[i].url), "%s", sub[i].url);
            }
            return frames_load(-1);
        }
    }
    internal_page = 0;
    frames_clear();
    if (st == FETCH_FAILED) {
        html_error_page(r->url, "Cannot open the page", r->error);
    } else {
        uint32_t t0 = ts_core->millis();
        html_render(r->body, r->length, r->content_type, r->url);
        ts_log("tsafari: %s: %u bytes in %u ms, layout %u ms, %d px", r->url, (unsigned)r->length,
               (unsigned)r->took, (unsigned)(ts_core->millis() - t0), page_height());
    }
    return show_new_page();
}

/* --- главный цикл --- */

/* Индикатор скачивания: пока файл идёт из сети — его ход (по заголовкам видно, что это файл,
 * а не страница); потом — запись на карту, готово или ошибка (download.c). */
static void download_ui(void) {
    static fetch_result_t pk;                    /* static: в нём адрес на 512 байт */
    download_info_t di;
    if (loading && fetch_peek(&pk) && fetch_is_file(pk.content_type, pk.disposition)) {
        char cp[DL_NAME_MAX + 1], name[DL_NAME_MAX * 2 + 1];
        download_name(pk.url, pk.disposition, cp, sizeof(cp));
        name[charset_to_utf8(CS_CP866, (const uint8_t *)cp, strlen(cp), name)] = 0;
        net_file = 1;
        ui_download(pk.total ? TSA_UI_DOWNLOAD_KNOWN : TSA_UI_DOWNLOAD_UNKNOWN, name, NULL,
                    (uint32_t)pk.length, (uint32_t)pk.total, pk.started);
        return;
    }
    download_info(&di);
    int k = di.state == DL_WAIT || di.state == DL_SEND ? TSA_UI_DOWNLOAD_WRITING
          : di.state == DL_DONE ? TSA_UI_DOWNLOAD_DONE
          : di.state == DL_FAILED ? (di.cancelled ? TSA_UI_DOWNLOAD_CANCELLED : TSA_UI_DOWNLOAD_ERROR) : -1;
    ui_download(k, di.name, k == TSA_UI_DOWNLOAD_DONE ? di.path : k == TSA_UI_DOWNLOAD_ERROR ? di.error : NULL,
                k == TSA_UI_DOWNLOAD_WRITING ? di.sent : di.size, di.size, di.changed);
}

static int cx = TS_W / 2, cy = TS_H / 2;  /* курсор мыши */
static uint8_t held;                     /* какие кнопки мыши были зажаты в прошлый раз */

/* Подготовка: шрифт, FT812, картинки интерфейса, сеть, первая страница, окно Wi-Fi.
 * 0 — ошибка (state = ST_ERROR, текст — для Z80). */
static int browser_init(void) {
    __atomic_store_n(&state, ST_LOADING, __ATOMIC_RELEASE);
    if (!font_init()) { fail("FONT ATLAS: NO MEMORY"); return 0; }
    if (!(dl = ts_alloc(sizeof(ft_dl_t)))) { fail("DISPLAY LIST: NO MEMORY"); return 0; }
    int r = ft_init(15000);
    if (r != ZM_OK) {
        char m[48];
        snprintf(m, sizeof(m), "FT812 OPEN FAILED: CODE %d", r);
        fail(m);
        return 0;
    }
    if (!ui_init()) { fail("UI ASSETS UPLOAD FAILED"); return 0; }
    net_ready = fetch_init();
    download_init();
    if (!net_ready) ts_log("tsafari: no net_open in this firmware: pages cannot be loaded");
    hist_n = hist_at = 0; loading = 0;
    if (!open_url("tsafari:about", 1)) { fail("PAGE UPLOAD FAILED"); return 0; }
    uint32_t now = ts_core->millis();
    wifi_ok = wifi_failed = 0; wifi_since = now; wifi_poll_at = now - 1000;
    ui_set_busy(1);
    ui_wifi(UI_WIFI_CONNECTING);
    cx = TS_W / 2; cy = TS_H / 2; target = 0; held = 0;
    __atomic_store_n(&state, ST_RUNNING, __ATOMIC_RELEASE);
    ts_log("tsafari " TS_VERSION ": running, page %d px", page_height());
    return 1;
}

/* Один проход цикла: ввод → действия → прокрутка → кадр. 1 — кадр отправлен, 0 — менять
 * нечего, −1 — конец (выход по F12 или ошибка). */
static int browser_step(uint32_t now) {
    wifi_step(now);
    download_tick(now);
    mouse_state_t m;
    input_mouse(&m);
    cx += m.dx * MOUSE_GAIN; cy += m.dy * MOUSE_GAIN;
    if (cx < 0) cx = 0;
    if (cx > TS_W - 1) cx = TS_W - 1;
    if (cy < 0) cy = 0;
    if (cy > TS_H - 1) cy = TS_H - 1;
    /* «нажата сейчас» = зажата теперь и не была зажата в прошлый раз */
    uint8_t pressed = (uint8_t)(m.buttons & ~held);
    held = m.buttons;
    int act = ui_mouse(cx, cy, pressed, held);
    /* курсор над ссылкой страницы (не над кнопками и плашками) — заливка цветом темы */
    ui_set_link_hover(!ui_wifi_open() && !ui_pointer_on_ui() && page_link_at(cx, cy) >= 0);
    if (act == UI_CLICK) {
        int link = page_link_at(cx, cy);
        if (link >= 0) {
            if (internal_page) target = demo_section_y(link);
            else if (!follow_link(link)) { fail("PAGE UPLOAD FAILED"); return -1; }
        }
    }
    /* колесо — по 3 строки за щелчок; клавиши, которые не взял интерфейс, — прокрутка и
     * переходы (список клавиш — на странице tsafari:about) */
    if (m.wheel && !ui_wifi_open()) target -= m.wheel * 3 * PAGE_LINE_H;
    key_event_t ev;
    while (input_key(&ev)) {
        int used;
        int a = ui_key(&ev, &used);
        if (a != UI_NONE) act = a;
        if (used) continue;
        switch (ev.key) {
        case KEY_UP: target -= PAGE_LINE_H; break;
        case KEY_DOWN: target += PAGE_LINE_H; break;
        case KEY_PGUP: target -= TS_H - 2 * PAGE_LINE_H; break;
        case KEY_PGDN: target += TS_H - 2 * PAGE_LINE_H; break;
        case KEY_HOME: target = 0; break;
        case KEY_END: target = page_height(); break;
        case KEY_CHAR: if (ev.ch == ' ') target += (ev.mods & MOD_SHIFT ? -1 : 1) * (TS_H - 2 * PAGE_LINE_H); break;
        case KEY_F5: act = UI_RELOAD; break;
        case KEY_ESC: act = UI_STOP; break;
        case KEY_BACKSPACE: act = UI_BACK; break;
        case KEY_LEFT: if (ev.mods & MOD_ALT) act = UI_BACK; break;
        case KEY_RIGHT: if (ev.mods & MOD_ALT) act = UI_FORWARD; break;
        case KEY_F12: __atomic_store_n(&state, ST_EXIT, __ATOMIC_RELEASE); return -1;
        default: break;
        }
    }
    /* действие пользователя (кнопкой или клавишей) */
    int ok = 1;
    if (act == UI_GO) {
        char url[URL_MAX];
        if (url_from_input(ui_url(), url, sizeof(url))) ok = open_url(url, 1);
        else ui_set_url(current);
    } else if (act == UI_RELOAD) {
        ok = reload();
    } else if (act == UI_STOP) {
        if (loading) fetch_cancel();
        else if (download_busy()) download_cancel();   /* Esc при записи файла — отмена */
    } else if (act == UI_DL_CANCEL) {                  /* крестик индикатора */
        if (loading && net_file) fetch_cancel();
        else download_cancel();
    } else if (act == UI_BACK && hist_at > 0) {
        ok = open_history(hist_at - 1);
    } else if (act == UI_FORWARD && hist_at + 1 < hist_n) {
        ok = open_history(hist_at + 1);
    }
    if (ok) ok = fetch_step();
    download_ui();
    if (!ok) { fail("PAGE UPLOAD FAILED"); return -1; }
    /* прокрутка к target (не больше 128 строк за кадр), потом кадр — только если что-то
     * изменилось: так связь с видеочипом не занята без нужды */
    clamp_target(&target);
    int moved = 0;
    if (target != page_top()) {
        if (!page_scroll_to(target)) { fail("PAGE UPLOAD FAILED"); return -1; }
        moved = 1;
    }
    ui_set_scroll(page_top(), page_height(), moved);
    if (!ui_tick(now) && !moved) return 0;
    if (!frame()) { if (ts_running) fail("FT812 LINK FAILED"); return -1; }
    return 1;
}

/* Тело задачи браузера. Если менять нечего, спим 4 мс — отдаём процессор задаче загрузки. */
static void browser_task(void *arg) {
    (void)arg;
    if (browser_init())
        while (__atomic_load_n(&ts_running, __ATOMIC_ACQUIRE) && state == ST_RUNNING) {
            int r = browser_step(ts_core->millis());
            if (r < 0) break;
            if (r == 0) ts_core->delay(4);
        }
    if (state == ST_EXIT) ts_log("tsafari: exit by F12");
    __atomic_store_n(&task_done, 1, __ATOMIC_RELEASE);
}

/* --- обмен с программой Z80 ---
 * Вызывается в задаче обмена прошивки (не в задаче браузера!), поэтому должна отвечать сразу:
 * Z80 ждёт ответа. На каждую команду — ответ reply (код ответа, байты). */
static int command(uint8_t cmd, const uint8_t *p, uint16_t n) {
    if (cmd == 0x76 || cmd == 0x77) return download_command(cmd, p, n);   /* загрузки на карту */
    if (cmd == 0x75) {                 /* [джойстик, кнопки мыши, X, Y, байты PS/2…] */
        uint8_t ok = n >= 4;
        if (ok) input_packet(p, n);
        ts_core->reply(0xF5, &ok, 1);
        return ZM_OK;
    }
    if (cmd == 0x73) {                 /* состояние: [state, 1, 0, 0, 0, 0, текст ошибки] */
        uint8_t status[6 + sizeof(error_text)] = {(uint8_t)__atomic_load_n(&state, __ATOMIC_ACQUIRE), 1};
        unsigned length = 6;
        if (status[0] == ST_ERROR) {
            size_t k = strlen(error_text) + 1;
            memcpy(status + 6, error_text, k);
            length += (unsigned)k;
        }
        ts_core->reply(0xF3, status, (uint16_t)length);
        return ZM_OK;
    }
    return ZM_ERROR;
}

/* Запуск: проверить, что прошивка подходит (версия таблицы и функции FT812 есть), и
 * запустить задачу браузера: стек 8 КБ, приоритет 1, ядро 0. */
static int start(const zm_core_t *c) {
    ts_core = c;
    state = ST_START;
    error_text[0] = 0;
    if (c->abi_major != 1 || c->size < offsetof(zm_core_t, ft_close) + sizeof(c->ft_close) || !c->ft_open ||
        !c->ft_write || !c->ft_read)
        return ZM_ERROR;
    input_init();
    ts_running = 1;
    task_done = 0;
    int r = c->task_start(browser_task, NULL, 8192, 1, 0);
    if (r != ZM_OK) {
        task_done = 1; ts_running = 0;
        c->log("tsafari: start failed: browser task (%d)", r);
        return r;
    }
    return ZM_OK;
}

/* Остановка: ts_running = 0 — все циклы выходят; ждём обе задачи (до 17 с), иначе прошивка
 * выгрузит код, который ещё выполняется. */
static void stop(void) {
    __atomic_store_n(&ts_running, 0, __ATOMIC_RELEASE);
    /* ft_open при первом переходе в Slot1 ждёт до 15 с: дать задачам выйти до снятия объектов */
    for (unsigned i = 0; i < 1700 && !(__atomic_load_n(&task_done, __ATOMIC_ACQUIRE) && fetch_stopped()); i++)
        ts_core->delay(10);
}

#ifdef TSAFARI_HOST
/* Стенд на ПК (tests/host.c): тот же код без задач ESP — шаги цикла по одному. */
int tsafari_host_init(const zm_core_t *c) { ts_core = c; input_init(); ts_running = 1; return browser_init(); }
int tsafari_host_step(uint32_t now) { return browser_step(now); }
int tsafari_host_command(uint8_t cmd, const uint8_t *p, uint16_t n) { return command(cmd, p, n); }
int tsafari_host_state(void) { return state; }
#endif

/* Точка входа ELF: прошивка передаёт в argv[2] место, куда положить адрес таблицы
 * приложения, и проверяет возвращённую версию ABI («договора» между прошивкой и приложением). */
int main(int argc, char **argv) {
    static const zm_app_t app = {
        .size = sizeof(zm_app_t), .abi_major = ZM_APP_ABI_MAJOR, .abi_minor = ZM_APP_ABI_MINOR,
        .name = "tsafari", .version = TS_VERSION, .command_first = 0x73, .command_last = 0x77,
        .start = start, .stop = stop, .command = command, .poll = NULL,
    };
    if (argc != 3) return 0;
    *(const zm_app_t **)argv[2] = &app;
    return ZM_APP_ABI_MAJOR;
}
