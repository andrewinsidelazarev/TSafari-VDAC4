/* Интерфейс поверх страницы (что он делает — в ui.h).
 *
 * АНИМАЦИЯ. У выезда кнопок и строки адреса есть «время анимации»: nav_t и addr_t — сколько
 * миллисекунд анимация уже прошла (0 — спрятано, NAV_MS или ADDR_MS — выехало полностью).
 * Codex заранее посчитал таблицы: для каждой миллисекунды — насколько сдвинуто
 * (tsa_ui_nav_offset_px) и насколько прозрачен глобус. Движение в таблице плавное: быстро в
 * начале и мягко в конце. Каждый кадр ui_tick двигает время к цели (открыто или закрыто), а
 * рисование берёт сдвиг из таблицы по текущему времени.
 *
 * ПРОЗРАЧНОСТЬ (альфа): 0 — не видно, 255 — непрозрачно; глобус в покое — 112, полупрозрачный.
 *
 * НОЖНИЦЫ (scissor) — прямоугольник экрана, вне которого видеочип ничего не рисует. Строка
 * адреса выезжает «из-под глобуса»: ножницы обрезают её слева, пока она едет.
 *
 * changed — «картинка изменилась»: тогда app.c соберёт и отправит новый дисплей-лист. Если
 * ничего не менялось, лист не отправляется, и связь ESP32 с видеочипом не занята зря. */
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "ui_data.h"
#include "ui_design.h"
#include "font.h"
#include "ts.h"

#define NAV_MS    TSA_UI_NAV_ANIMATION_MS        /* длина анимации кнопок, мс */
#define ADDR_MS   TSA_UI_ADDRESS_ANIMATION_MS    /* длина анимации строки адреса, мс */
#define CY        TSA_UI_NAV_CENTER_Y            /* середина кнопок по высоте */
#define ADDR_COLS TSA_UI_ADDRESS_VISIBLE_COLUMNS /* букв адреса видно сразу */
#define ADDR_STRIDE TSA_UI_ADDRESS_L4_STRIDE     /* байт в строке картинки текста адреса */
#define ADDR_MAX  1024                           /* самый длинный адрес, символов */
#define DL_TEXT_MAX 192                          /* строка индикатора скачивания, байт UTF-8 */
#define DL_Y      TSA_UI_DOWNLOAD_Y              /* верх индикатора (строки состояния ссылки пока нет) */

/* Строки текста окна Wi-Fi. Их рисуем мы сами нашим шрифтом в «маски» — картинки L4
 * (4 бита на точку, только форма букв, без цвета; цвет даст видеочип). Лежат в памяти
 * видеочипа сразу за картинками Codex. _Static_assert — проверка при сборке: если адреса
 * когда-нибудь наедут друг на друга, программа просто не соберётся. */
#define RAMG_WIFI   0x0E6000u
#define WIFI_COLS   34
#define WIFI_STRIDE (WIFI_COLS * FONT_CELL_W / 2)
#define WIFI_BYTES  (WIFI_STRIDE * FONT_CELL_H)
enum { WL_SUBTITLE, WL_STATE, WL_HINT, WL_EXIT, WL_COUNT };
_Static_assert(RAMG_WIFI >= TSA_UI_RAM_G_BASE + TSA_UI_STATIC_BYTES, "тексты Wi-Fi — после статического пакета");
_Static_assert(RAMG_WIFI + WL_COUNT * WIFI_BYTES <= TSA_UI_ASSET_INPUT_EN_RAM_G, "тексты Wi-Fi — до меток языка");

/* Куда показывает мышь: 0…3 — кнопки назад, вперёд, стоп, обновить; дальше глобус,
 * «перейти» и настройки в строке адреса, индикатор скачивания и его крестик; H_NONE —
 * никуда (на страницу). */
enum { H_NONE = -1, H_GLOBE = 4, H_GO = 5, H_SETTINGS = 6, H_DL = 7, H_DL_X = 8 };

static int mx = TS_W / 2, my = TS_H / 2;  /* курсор мыши; на старте — в середине экрана */
static int nav_t, nav_open;              /* время анимации кнопок и куда она идёт (1 — выезжать) */
static int addr_t, addr_open;            /* то же для строки адреса */
static int addr_pending;                 /* строку адреса просили открыть: ждём, пока выедут кнопки */
static uint32_t last;                    /* время прошлого ui_tick */
static uint32_t hot_since;               /* с какого времени курсор в горячем углу (0 — не там) */
static uint32_t leave_at;                /* когда курсор ушёл от кнопок (для задержки скрытия) */
static uint32_t frame_at, scroll_at;     /* когда сменился кадр глобуса; когда была прокрутка */
static int busy, frame;                  /* идёт загрузка; номер кадра вращения глобуса */
static int can_back, can_fwd;            /* есть ли история назад и вперёд */
static int hover = H_NONE, held_btn = H_NONE;  /* над чем курсор; на чём зажата кнопка мыши */
static int wifi;                         /* состояние окна Wi-Fi (UI_WIFI_…) */
static unsigned wifi_len[WL_COUNT];      /* длина каждой строки окна Wi-Fi в буквах (для центровки) */
static int s_top, s_height, scroll_shown; /* прокрутка страницы, её высота, видна ли полоса */
/* Адрес хранится кодами Unicode — по одному числу на букву: так проще вставлять и стирать
 * буквы в середине (в UTF-8 буквы разной длины). saved — адрес до правки, для Esc. */
static uint32_t url[ADDR_MAX], saved[ADDR_MAX];
static unsigned url_n, saved_n;          /* длины адресов в буквах */
static unsigned caret, view;             /* каретка (перед какой буквой) и первая видимая буква */
static int select_all;                  /* адрес выделен целиком: набор его заменяет */
static uint32_t lang_until;             /* до этого времени метка языка видна везде (0 — нет) */
static int text_dirty = 1, changed = 1;  /* перерисовать текст адреса; картинка изменилась */
static uint8_t *mask_buf;                /* место, где рисуются маски текста перед отправкой */
static int link_hover;                   /* курсор над ссылкой: заливка курсора — цветом темы */

/* Индикатор скачивания (дизайн Codex, design/DESIGN.md, п. 16). */
static int dl_kind = -1;                 /* TSA_UI_DOWNLOAD_…; −1 — не виден */
static uint32_t dl_done, dl_total;       /* байты для полосы */
static uint32_t dl_since, dl_gone = 1;   /* когда началось состояние; какое состояние уже убрано */
static uint32_t dl_hide_at;              /* когда уйти (готово и ошибки) */
static uint32_t dl_text_at;              /* когда перерисовывали текст (ход — не чаще 4 раз в с) */
static int dl_text_dirty;
static char dl_title[DL_TEXT_MAX], dl_line[DL_TEXT_MAX];
static uint32_t dl_hover_since;          /* с какого времени курсор над плашкой (0 — не над ней) */
static unsigned dl_scroll, dl_name_n;    /* сдвиг длинного имени в знаках; сколько знаков в имени */
static void render_download(void);
static char url_utf8[ADDR_MAX * 4 + 1];  /* адрес в UTF-8 для ui_url (до 4 байт на букву) */

/* Сдвиг кнопок (насколько они ещё за левым краем) и положение глобуса по таблицам дизайна. */
static int nav_off(void) { return tsa_ui_nav_offset_px[nav_t]; }
static int globe_x(void) { return TSA_UI_GLOBE_OPEN_CENTER_X - nav_off(); }
static int addr_off(void) { return tsa_ui_address_offset_px[addr_t]; }
/* Точка (x, y) внутри круга с центром (cx, cy) и радиусом r — по теореме Пифагора, без корня. */
static int in_circle(int cx, int cy, int r, int x, int y) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }

/* Подготовка: место под маски текста, всё в исходное состояние, картинки Codex — в видеочип. */
int ui_init(void) {
    size_t mask_bytes = (size_t)(ADDR_STRIDE > WIFI_STRIDE ? ADDR_STRIDE : WIFI_STRIDE) * FONT_CELL_H;
    if (mask_bytes < TSA_UI_BUFFER_DOWNLOAD_TEXT_BYTES) mask_bytes = TSA_UI_BUFFER_DOWNLOAD_TEXT_BYTES;
    mask_buf = ts_alloc(mask_bytes);
    if (!mask_buf) return 0;
    nav_t = nav_open = addr_t = addr_open = addr_pending = 0;
    hot_since = leave_at = 0;
    hover = held_btn = H_NONE; busy = 0; frame = 0; wifi = UI_WIFI_OFF; scroll_shown = 0;
    changed = text_dirty = 1;
    /* Пакет картинок — в RAM_G по отрезкам плана: между ними лежат текстовые буферы. */
    for (unsigned i = 0; i < UI_PLAN_RANGES; i++)
        if (!ft_put(ui_plan[i].ram_g, ui_assets + ui_plan[i].offset, ui_plan[i].bytes)) return 0;
    return 1;
}

void ui_set_busy(int on) { if (busy != on) { busy = on; changed = 1; if (!on) frame = 0; } }
void ui_set_history(int b, int f) { if (b != can_back || f != can_fwd) { can_back = b; can_fwd = f; changed = 1; } }
int ui_cursor_x(void) { return mx; }
int ui_cursor_y(void) { return my; }
int ui_wifi_open(void) { return wifi != UI_WIFI_OFF; }

/* Полоса прокрутки показывается при прокрутке и прячется сама через время из дизайна. */
void ui_set_scroll(int top, int height, int moved) {
    if (top != s_top || height != s_height) changed = 1;
    s_top = top; s_height = height;
    if (moved) { scroll_shown = 1; scroll_at = ts_core->millis(); changed = 1; }
}

/* Нарисовать строку UTF-8 нашим шрифтом в маску L4 шириной cols букв и отправить её в
 * память видеочипа по адресу address. Глифы просто копируются строка за строкой: шрифт
 * уже хранится в формате L4. Возвращает число букв. */
static unsigned text_mask(uint32_t address, const char *utf8, unsigned cols) {
    uint16_t g[64];
    unsigned stride = cols * FONT_CELL_W / 2;
    if (cols > 64) cols = 64;
    unsigned n = font_shape(utf8, (unsigned)strlen(utf8), g, cols);
    memset(mask_buf, 0, (size_t)stride * FONT_CELL_H);
    for (unsigned c = 0; c < n; c++) {
        const uint8_t *cell = font_glyph(g[c]);
        for (int row = 0; row < FONT_CELL_H; row++)
            memcpy(mask_buf + row * stride + c * (FONT_CELL_W / 2), cell + row * (FONT_CELL_W / 2), FONT_CELL_W / 2);
    }
    ft_put(address, mask_buf, (size_t)stride * FONT_CELL_H);
    return n;
}

/* Открыть или закрыть окно Wi-Fi. Тексты окна рисуются в маски один раз — при смене
 * состояния, а не каждый кадр. */
void ui_wifi(int state) {
    if (state == wifi) return;
    wifi = state;
    changed = 1;
    if (state == UI_WIFI_OFF) return;
    static const char *const lines[2][WL_COUNT] = {
        {TSA_UI_WIFI_SUBTITLE_TEXT, "Connecting to Wi-Fi...", "Please wait.", "F12: Exit browser"},
        {TSA_UI_WIFI_SUBTITLE_TEXT, "Unable to connect.", "Retrying automatically.", "F12: Exit browser"},
    };
    for (unsigned i = 0; i < WL_COUNT; i++)
        wifi_len[i] = text_mask(RAMG_WIFI + i * WIFI_BYTES, lines[state == UI_WIFI_FAILED][i], WIFI_COLS);
}

/* UTF-8 → коды Unicode, каретка — в конец. */
void ui_set_url(const char *utf8) {
    const char *p = utf8, *end = utf8 + strlen(utf8);
    url_n = 0;
    while (p < end && url_n < ADDR_MAX) url[url_n++] = utf8_next(&p, end);
    caret = url_n; view = 0; text_dirty = changed = 1;
}

/* Коды Unicode → UTF-8 (обратно к font.c, utf8_next): старшие биты кода — в первый байт с
 * меткой длины (110…, 1110…, 11110…), остальные — по 6 бит в байты 10xxxxxx. */
const char *ui_url(void) {
    char *o = url_utf8;
    for (unsigned i = 0; i < url_n; i++) {
        uint32_t c = url[i];
        if (c < 0x80) *o++ = (char)c;
        else if (c < 0x800) { *o++ = (char)(0xC0 | c >> 6); *o++ = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { *o++ = (char)(0xE0 | c >> 12); *o++ = (char)(0x80 | ((c >> 6) & 63)); *o++ = (char)(0x80 | (c & 63)); }
        else {
            *o++ = (char)(0xF0 | c >> 18); *o++ = (char)(0x80 | ((c >> 12) & 63));
            *o++ = (char)(0x80 | ((c >> 6) & 63)); *o++ = (char)(0x80 | (c & 63));
        }
    }
    *o = 0;
    return url_utf8;
}

/* Открыть адрес: навигация скрыта — сначала её выезд, потом адрес (DESIGN.md). */
static void address_open(void) {
    memcpy(saved, url, url_n * sizeof(url[0])); saved_n = url_n;
    caret = url_n;
    select_all = url_n != 0;
    if (nav_t == NAV_MS) addr_open = 1; else { nav_open = 1; addr_pending = 1; }
    text_dirty = changed = 1;
}
/* Закрыть адрес; cancel — вернуть прежний URL. */
static void address_close(int cancel) {
    if (cancel) { memcpy(url, saved, saved_n * sizeof(url[0])); url_n = saved_n; caret = url_n; }
    addr_open = addr_pending = 0;
    select_all = 0;
    text_dirty = changed = 1;
}
/* Глобус и Win: открыть строку адреса, если закрыта; закрыть без перехода, если открыта. */
static void address_toggle(void) { if (addr_open || addr_pending) address_close(1); else address_open(); }

/* На что показывает мышь (H_…). Кнопки ловят щелчок только выехав полностью — чтобы не нажать
 * кнопку, которая ещё едет. */
static int dl_visible(void) { return dl_kind >= 0 && dl_gone != dl_since; }
static int dl_terminal(void) { return dl_kind >= TSA_UI_DOWNLOAD_DONE; }
/* Крестик: отмена — только пока файл идёт из сети; при записи на карту его нет (дизайн). */
static int dl_has_x(void) { return dl_kind != TSA_UI_DOWNLOAD_WRITING; }

static int hit_test(int x, int y) {
    if (dl_visible() && x >= TSA_UI_DOWNLOAD_X && x < TSA_UI_DOWNLOAD_X + TSA_UI_DOWNLOAD_WIDTH &&
        y >= DL_Y && y < DL_Y + TSA_UI_DOWNLOAD_HEIGHT) {
        int ax = x - TSA_UI_DOWNLOAD_ACTION_HIT_X, ay = y - DL_Y - TSA_UI_DOWNLOAD_ACTION_HIT_Y_OFFSET;
        return dl_has_x() && ax >= 0 && ax < TSA_UI_DOWNLOAD_ACTION_HIT_WIDTH && ay >= 0 &&
               ay < TSA_UI_DOWNLOAD_ACTION_HIT_HEIGHT ? H_DL_X : H_DL;
    }
    if (in_circle(globe_x(), TSA_UI_GLOBE_CENTER_Y, TSA_UI_GLOBE_RADIUS, x, y)) return H_GLOBE;
    if (nav_t == NAV_MS)
        for (int i = 0; i < TSA_UI_NAV_BUTTON_COUNT; i++)
            if (in_circle(tsa_ui_nav_centers_x[i], CY, TSA_UI_NAV_BUTTON_HIT_RADIUS, x, y)) return i;
    if (addr_t == ADDR_MS) {
        if (in_circle(TSA_UI_GO_CENTER_X, TSA_UI_GO_CENTER_Y, TSA_UI_NAV_BUTTON_HIT_RADIUS, x, y)) return H_GO;
        if (in_circle(TSA_UI_SETTINGS_CENTER_X, TSA_UI_SETTINGS_CENTER_Y, TSA_UI_SETTINGS_HIT_RADIUS, x, y)) return H_SETTINGS;
    }
    return H_NONE;
}

/* Мышь. Бит 0 в pressed и held — левая кнопка. Порядок:
 *   1) запомнить курсор; при окне Wi-Fi больше ничего не делать;
 *   2) горячий угол: засечь, с какого времени курсор в нём (кнопки выедут в ui_tick);
 *   3) курсор у правого края — показать полосу прокрутки;
 *   4) щелчок по кнопке — её действие; щелчок по странице — UI_CLICK (по ссылке или нет,
 *      решит app.c). Щелчок вне открытой строки адреса закрывает её. */
int ui_mouse(int x, int y, uint8_t pressed, uint8_t held) {
    if (x != mx || y != my) changed = 1;
    mx = x; my = y;
    if (wifi) return UI_NONE;                                   /* окно Wi-Fi: только курсор */
    uint32_t now = ts_core->millis();
    if (x < TSA_UI_HOT_X + TSA_UI_HOT_WIDTH && y < TSA_UI_HOT_Y + TSA_UI_HOT_HEIGHT) {
        if (!hot_since) hot_since = now | 1;
    } else hot_since = 0;
    if (x >= TSA_UI_SCROLL_HOVER_X && s_height > TS_H && !scroll_shown) { scroll_shown = 1; changed = 1; }
    if (x >= TSA_UI_SCROLL_HOVER_X) scroll_at = now;
    int h = hit_test(x, y);
    if (h != hover) {
        if ((h == H_DL_X) != (hover == H_DL_X)) dl_text_dirty = 1;   /* «Cancel download» под крестиком */
        hover = h;
        changed = 1;
    }
    if (!(held & 1) && held_btn != H_NONE) { held_btn = H_NONE; changed = 1; }
    if (!(pressed & 1)) return UI_NONE;
    held_btn = h; changed = 1;
    /* серые (недоступные) кнопки не срабатывают: назад без истории, стоп без загрузки */
    switch (h) {
    case H_GLOBE: address_toggle(); return UI_NONE;
    case 0: return can_back ? UI_BACK : UI_NONE;
    case 1: return can_fwd ? UI_FORWARD : UI_NONE;
    case 2: return busy ? UI_STOP : UI_NONE;
    case 3: return busy ? UI_NONE : UI_RELOAD;
    case H_GO: address_close(0); return UI_GO;
    case H_SETTINGS: address_close(1); return UI_SETTINGS;
    /* крестик: пока качается — отмена; готово или ошибка — убрать (и щелчок по плашке тоже) */
    case H_DL_X: if (!dl_terminal()) return UI_DL_CANCEL; dl_gone = dl_since; return UI_NONE;
    case H_DL: if (dl_terminal()) dl_gone = dl_since; return UI_NONE;
    default: break;
    }
    if (addr_open && y < TSA_UI_ADDRESS_Y + TSA_UI_ADDRESS_HEIGHT && x >= TSA_UI_ADDRESS_X) return UI_NONE;
    if (addr_open || addr_pending) address_close(1);           /* клик вне редактора — странице */
    return UI_CLICK;
}

/* Клавиатура. Пока строка адреса закрыта, интерфейс берёт только Win — остальное уходит
 * странице (прокрутка, назад…). Открыта — работает как маленький текстовый редактор: буквы
 * вставляются перед кареткой (memmove сдвигает хвост на одну букву вправо), Backspace стирает
 * букву слева, Delete — справа, стрелки, Home и End двигают каретку, Enter — перейти,
 * Esc — закрыть и вернуть прежний адрес. */
int ui_key(const key_event_t *ev, int *used) {
    *used = 1;
    if (wifi) { *used = ev->key != KEY_F12; return UI_NONE; }   /* F12 — выход — доходит */
    if (ev->key == KEY_WIN) { address_toggle(); return UI_NONE; }
    if (!addr_open && !addr_pending) { *used = 0; return UI_NONE; }
    text_dirty = changed = 1;
    if (select_all && (ev->key == KEY_CHAR || ev->key == KEY_BACKSPACE || ev->key == KEY_DELETE)) {
        url_n = 0; caret = 0; view = 0;                         /* выделенное заменяется */
        select_all = 0;
        if (ev->key != KEY_CHAR) return UI_NONE;
    }
    if (ev->key == KEY_LEFT || ev->key == KEY_RIGHT || ev->key == KEY_HOME || ev->key == KEY_END) select_all = 0;
    switch (ev->key) {
    case KEY_CHAR:
        if (url_n < ADDR_MAX) {
            memmove(url + caret + 1, url + caret, (url_n - caret) * sizeof(url[0]));
            url[caret++] = ev->ch; url_n++;
        }
        return UI_NONE;
    case KEY_BACKSPACE:
        if (caret) { memmove(url + caret - 1, url + caret, (url_n - caret) * sizeof(url[0])); caret--; url_n--; }
        return UI_NONE;
    case KEY_DELETE:
        if (caret < url_n) { memmove(url + caret, url + caret + 1, (url_n - caret - 1) * sizeof(url[0])); url_n--; }
        return UI_NONE;
    case KEY_LEFT: if (caret) caret--; return UI_NONE;
    case KEY_RIGHT: if (caret < url_n) caret++; return UI_NONE;
    case KEY_HOME: caret = 0; return UI_NONE;
    case KEY_END: caret = url_n; return UI_NONE;
    case KEY_ESC: address_close(1); return UI_NONE;
    case KEY_ENTER: address_close(0); return UI_GO;
    case KEY_F12: *used = 0; return UI_NONE;
    default: text_dirty = 0; return UI_NONE;
    }
}

/* Текст адреса — маска L4 648×34 (54 буквы). Адрес бывает длиннее: видно окно букв
 * view…view+54, и оно сдвигается так, чтобы каретка всегда была видна. */
static void render_address(void) {
    if (caret < view) view = caret;
    if (caret >= view + ADDR_COLS) view = caret - ADDR_COLS + 1;
    memset(mask_buf, 0, (size_t)ADDR_STRIDE * FONT_CELL_H);
    for (unsigned c = 0; c < ADDR_COLS && view + c < url_n; c++) {
        const uint8_t *cell = font_glyph(font_lookup(url[view + c]));
        for (int row = 0; row < FONT_CELL_H; row++)
            memcpy(mask_buf + row * ADDR_STRIDE + c * (FONT_CELL_W / 2), cell + row * (FONT_CELL_W / 2), FONT_CELL_W / 2);
    }
    ft_put(TSA_UI_ADDRESS_RAM_G_BASE, mask_buf, (size_t)ADDR_STRIDE * FONT_CELL_H);
    text_dirty = 0;
}

/* Сдвинуть время анимации *t на dt мс к цели target, не проскочив её. 1 — сдвинулось. */
static int step(int *t, int target, int dt) {
    int old = *t;
    if (*t < target) { *t += dt; if (*t > target) *t = target; }
    else if (*t > target) { *t -= dt; if (*t < target) *t = target; }
    return *t != old;
}

/* Раз в кадр: сколько прошло с прошлого раза (не больше 100 мс — после долгой паузы анимация
 * не перепрыгивает в конец), открыть или закрыть кнопки, подвинуть анимации, повернуть
 * глобус, спрятать полосу прокрутки, перерисовать текст адреса. */
int ui_tick(uint32_t now) {
    int dt = last ? (int)(now - last) : 0;
    if (dt > 100) dt = 100;
    last = now;
    int c = changed;
    if (!wifi && hot_since && (uint32_t)(now - hot_since) >= TSA_UI_HOT_DWELL_MS && !nav_open) { nav_open = 1; c = 1; }
    /* кнопки уезжают через 600 мс после ухода курсора из области панели и глобуса */
    int near = my < TSA_UI_NAV_Y + TSA_UI_NAV_HEIGHT + 4 && mx < globe_x() + TSA_UI_GLOBE_RADIUS + 4;
    if (nav_open && !addr_open && !addr_pending && !addr_t && held_btn == H_NONE && !near) {
        if (!leave_at) leave_at = now | 1;
        else if ((uint32_t)(now - leave_at) >= TSA_UI_NAV_HIDE_DELAY_MS) { nav_open = 0; leave_at = 0; }
    } else leave_at = 0;
    /* открытие: навигация → адрес; закрытие: адрес → навигация */
    if (addr_pending && nav_t == NAV_MS) { addr_pending = 0; addr_open = 1; }
    c |= step(&addr_t, addr_open ? ADDR_MS : 0, dt);
    if (nav_open || addr_t == 0) c |= step(&nav_t, nav_open ? NAV_MS : 0, dt);
    if (busy && (uint32_t)(now - frame_at) >= TSA_UI_GLOBE_FRAME_MS) {
        frame = (frame + 1) % TSA_UI_GLOBE_FRAME_COUNT; frame_at = now; c = 1;
    }
    if (scroll_shown && mx < TSA_UI_SCROLL_HOVER_X && (uint32_t)(now - scroll_at) >= TSA_UI_SCROLL_HIDE_DELAY_MS) {
        scroll_shown = 0; c = 1;
    }
    /* Ctrl+Shift меняет раскладку без нажатия «буквы»: метку языка перерисовать сразу и держать
     * её видимой 1 секунду, даже если каретка не в конце строки (решение пользователя). */
    static int last_russian = -1;
    if (input_russian() != last_russian) {
        if (last_russian >= 0) lang_until = (now + 1000u) | 1u;     /* первый проход — без показа */
        last_russian = input_russian();
        if (addr_t) c = 1;
    }
    if (lang_until && (int32_t)(now - lang_until) >= 0) { lang_until = 0; if (addr_t) c = 1; }
    if (text_dirty && (addr_open || addr_pending || addr_t)) render_address();
    /* индикатор: готово и ошибки уходят сами; пока курсор над плашкой — ждут */
    if (dl_visible() && dl_terminal()) {
        if (hover == H_DL || hover == H_DL_X) { if ((int32_t)(dl_hide_at - now) < 1000) dl_hide_at = now + 1000; }
        else if ((int32_t)(now - dl_hide_at) >= 0) { dl_gone = dl_since; c = 1; }
    }
    if (dl_visible() && (dl_kind == TSA_UI_DOWNLOAD_UNKNOWN || (dl_kind == TSA_UI_DOWNLOAD_WRITING && !dl_done)))
        c = 1;                                                        /* бегущий отрезок */
    /* курсор над плашкой: через 800 мс длинное имя едет на знак каждые 160 мс до конца */
    if (dl_visible() && (hover == H_DL || hover == H_DL_X)) {
        if (!dl_hover_since) dl_hover_since = now | 1;
        uint32_t t = now - dl_hover_since;
        unsigned want = t < TSA_UI_DOWNLOAD_TEXT_HOVER_DELAY_MS ? 0
                      : (t - TSA_UI_DOWNLOAD_TEXT_HOVER_DELAY_MS) / TSA_UI_DOWNLOAD_TEXT_SCROLL_STEP_MS + 1;
        if (want != dl_scroll && dl_name_n) { dl_scroll = want; dl_text_dirty = 1; }
    } else if (dl_hover_since) {
        dl_hover_since = 0;
        if (dl_scroll) { dl_scroll = 0; dl_text_dirty = 1; }
    }
    if (dl_visible() && dl_text_dirty && (dl_terminal() || (uint32_t)(now - dl_text_at) >= 250)) {
        render_download();
        dl_text_at = now;
        c = 1;
    }
    changed = 0;
    return c;
}

void ui_set_link_hover(int on) { if (on != link_hover) { link_hover = on; changed = 1; } }
int ui_lang_flash(void) { return lang_until != 0; }
int ui_pointer_on_ui(void) {
    return hover != H_NONE || (addr_t && my < TSA_UI_ADDRESS_Y + TSA_UI_ADDRESS_HEIGHT && mx >= TSA_UI_ADDRESS_X);
}

/* Строку UTF-8 — глифами в маску mask_buf (stride байт в строке), с колонки col строки row, не
 * больше cols знаков. Возвращает число глифов строки целиком (для сокращения). */
static unsigned put_text(unsigned stride, unsigned col, unsigned row, const uint16_t *g, unsigned n, unsigned cols) {
    for (unsigned c = 0; c < n && c < cols; c++) {
        const uint8_t *cell = font_glyph(g[c]);
        for (int r = 0; r < FONT_CELL_H; r++)
            memcpy(mask_buf + (size_t)(row * FONT_CELL_H + r) * stride + (col + c) * (FONT_CELL_W / 2),
                   cell + r * (FONT_CELL_W / 2), FONT_CELL_W / 2);
    }
    return n;
}

/* Одна строка индикатора с колонки col строки row, не длиннее cols знаков. Имя файла (name),
 * если не помещается, сокращается в середине: начало, «...» и последние 11 знаков (там
 * расширение), а пока курсор над плашкой — показывается окном со сдвигом dl_scroll. Прочие
 * длинные строки просто обрезаются. */
static void put_line(unsigned col, unsigned row, const char *s, unsigned cols, int name) {
    uint16_t g[256], e[4];
    unsigned stride = TSA_UI_BUFFER_DOWNLOAD_TEXT_STRIDE;
    unsigned n = font_shape(s, (unsigned)strlen(s), g, 256);
    if (name) dl_name_n = n > cols ? n : 0;
    if (n <= cols || !name) { put_text(stride, col, row, g, n, cols); return; }
    if (dl_scroll) {
        unsigned off = dl_scroll > n - cols ? n - cols : dl_scroll;     /* на конце остановиться */
        put_text(stride, col, row, g + off, cols, cols);
        return;
    }
    unsigned tail = TSA_UI_DOWNLOAD_FILENAME_SUFFIX_CHARS, head = cols - TSA_UI_DOWNLOAD_ELLIPSIS_CHARS - tail;
    unsigned dots = font_shape("...", 3, e, 4);
    put_text(stride, col, row, g, head, head);
    put_text(stride, col + head, row, e, dots, dots);
    put_text(stride, col + head + dots, row, g + n - tail, tail, tail);
}

/* Текст индикатора — маска 480×68 из двух строк: первая — правее значка (с колонки 3, до 34
 * знаков), вторая — с колонки 0 (до 40). Обычно первая — имя файла, вторая — ход или ошибка;
 * под крестиком вторая — «Cancel download». Готово: «Saved to Evo SD» и полный путь; путь
 * длиннее строки — первая строка каталог \zifi\downloads\ГГГГ-ММ-ДД\, вторая — имя. */
static void render_download(void) {
    unsigned title_col = TSA_UI_DOWNLOAD_TITLE_OFFSET_X / FONT_CELL_W;
    memset(mask_buf, 0, TSA_UI_BUFFER_DOWNLOAD_TEXT_BYTES);
    dl_name_n = 0;
    uint16_t g[256];
    if (dl_kind == TSA_UI_DOWNLOAD_DONE &&
        font_shape(dl_line, (unsigned)strlen(dl_line), g, 256) > TSA_UI_DOWNLOAD_TEXT_COLUMNS) {
        char dir[DL_TEXT_MAX];
        const char *slash = strrchr(dl_line, '\\');
        size_t k = slash ? (size_t)(slash - dl_line) + 1 : 0;
        snprintf(dir, sizeof(dir), "%.*s", (int)k, dl_line);
        put_line(title_col, 0, dir, TSA_UI_DOWNLOAD_TITLE_COLUMNS, 0);
        put_line(0, 1, dl_line + k, TSA_UI_DOWNLOAD_TEXT_COLUMNS, 1);
    } else {
        put_line(title_col, 0, dl_title, TSA_UI_DOWNLOAD_TITLE_COLUMNS, dl_kind != TSA_UI_DOWNLOAD_DONE);
        put_line(0, 1, hover == H_DL_X && dl_has_x() && !dl_terminal() ? "Cancel download" : dl_line,
                 TSA_UI_DOWNLOAD_TEXT_COLUMNS, 0);
    }
    ft_put(TSA_UI_BUFFER_DOWNLOAD_TEXT_RAM_G, mask_buf, TSA_UI_BUFFER_DOWNLOAD_TEXT_BYTES);
    dl_text_dirty = 0;
}

/* Размер для людей: «900 B», «48 KiB», «1.50 MiB» (как в превью Codex). */
static void human_size(char *o, size_t cap, uint32_t v) {
    if (v < 1024) snprintf(o, cap, "%u B", (unsigned)v);
    else if (v < 1024u * 1024u) snprintf(o, cap, "%u KiB", (unsigned)(v / 1024));
    else snprintf(o, cap, "%u.%02u MiB", (unsigned)(v / 1048576u), (unsigned)(v % 1048576u * 100 / 1048576u));
}

void ui_download(int kind, const char *title, const char *line, uint32_t done, uint32_t total, uint32_t since) {
    char t[DL_TEXT_MAX], l[DL_TEXT_MAX], a[24], b[24];
    if (kind < 0) {
        if (dl_kind >= 0) { dl_kind = -1; changed = 1; }
        return;
    }
    snprintf(t, sizeof(t), "%s", kind == TSA_UI_DOWNLOAD_DONE ? "Saved to Evo SD" : title ? title : "");
    if (line) snprintf(l, sizeof(l), "%s", line);
    else if (kind == TSA_UI_DOWNLOAD_KNOWN) { human_size(a, sizeof(a), done); human_size(b, sizeof(b), total); snprintf(l, sizeof(l), "%s / %s", a, b); }
    else if (kind == TSA_UI_DOWNLOAD_UNKNOWN) { human_size(a, sizeof(a), done); snprintf(l, sizeof(l), "%s received", a); }
    else if (kind == TSA_UI_DOWNLOAD_WRITING) snprintf(l, sizeof(l), "%s", done ? "Writing to Evo SD..." : "Preparing Evo SD...");
    else if (kind == TSA_UI_DOWNLOAD_CANCELLED) snprintf(l, sizeof(l), "Download cancelled");
    else l[0] = 0;
    if (kind != dl_kind || since != dl_since) {
        dl_hide_at = since + (kind == TSA_UI_DOWNLOAD_DONE ? TSA_UI_DOWNLOAD_DONE_HIDE_MS :
                              kind == TSA_UI_DOWNLOAD_CANCELLED ? TSA_UI_DOWNLOAD_CANCELLED_HIDE_MS : TSA_UI_DOWNLOAD_ERROR_HIDE_MS);
        dl_text_dirty = 1;
        changed = 1;
    }
    if (strcmp(t, dl_title) || strcmp(l, dl_line)) {
        snprintf(dl_title, sizeof(dl_title), "%s", t);
        snprintf(dl_line, sizeof(dl_line), "%s", l);
        dl_text_dirty = 1;
    }
    if (done != dl_done || total != dl_total) changed = 1;
    dl_kind = kind; dl_since = since; dl_done = done; dl_total = total;
}

/* --- рисование: команды дисплей-листа ---
 * Координаты для видеочипа — в 1/16 точки, поэтому везде · 16 (ft.h, dl_vertex). */

/* Скруглённый прямоугольник: видеочип рисует RECTS с толщиной линии r как прямоугольник со
 * скруглёнными углами радиуса r, «раздутый» на r во все стороны, — поэтому углы сдвинуты на r
 * внутрь. Вызывать между BEGIN RECTS и END. */
static void rrect(ft_dl_t *d, int x, int y, int w, int h, int r, uint32_t rgb) {
    dl_put(d, dl_color_rgb(rgb));
    dl_put(d, dl_line_width((unsigned)r * 16));
    dl_put(d, dl_vertex((x + r) * 16, (y + r) * 16));
    dl_put(d, dl_vertex((x + w - r) * 16, (y + h - r) * 16));
}
/* Прямоугольник с почти прямыми углами (толщина линии 1 точка) — разделители, каретка. */
static void bar(ft_dl_t *d, int x, int y, int w, int h, uint32_t rgb) {
    dl_put(d, dl_color_rgb(rgb));
    dl_put(d, dl_line_width(16));
    dl_put(d, dl_vertex(x * 16, y * 16));
    dl_put(d, dl_vertex((x + w) * 16, (y + h) * 16));
}
/* Картинка L4 (маска) цветом rgb в точке (x, y): видеочип красит форму маски этим цветом,
 * а 16 уровней маски дают плавные края. Вызывать между BEGIN BITMAPS и END. */
static void l4(ft_dl_t *d, uint32_t address, unsigned stride, unsigned w, unsigned h, int x, int y, uint32_t rgb) {
    dl_put(d, dl_color_rgb(rgb));
    dl_bitmap(d, address, FT_L4, stride, w, h);
    dl_put(d, dl_vertex(x * 16, y * 16));
}
static void icon(ft_dl_t *d, uint32_t address, int x, int y, uint32_t rgb) {
    l4(d, address, TSA_UI_ICON_WIDTH / 2, TSA_UI_ICON_WIDTH, TSA_UI_ICON_HEIGHT, x, y, rgb);
}
/* Глобус — несколько кадров вращения подряд в памяти; frame выбирает кадр. */
static void globe_frame(ft_dl_t *d, int x, int y, uint32_t rgb) {
    l4(d, TSA_UI_ASSET_GLOBE_00_RAM_G + (uint32_t)frame * (TSA_UI_GLOBE_BITMAP_WIDTH * TSA_UI_GLOBE_BITMAP_HEIGHT / 2),
       TSA_UI_GLOBE_BITMAP_WIDTH / 2, TSA_UI_GLOBE_BITMAP_WIDTH, TSA_UI_GLOBE_BITMAP_HEIGHT, x, y, rgb);
}
/* Строка окна Wi-Fi по центру окна: левый край = центр − половина длины строки. */
static void centered(ft_dl_t *d, unsigned line, int y, uint32_t rgb) {
    l4(d, RAMG_WIFI + line * WIFI_BYTES, WIFI_STRIDE, WIFI_COLS * FONT_CELL_W, FONT_CELL_H,
       TSA_UI_WIFI_CENTER_X - (int)wifi_len[line] * FONT_CELL_W / 2, y, rgb);
}

/* Окно Wi-Fi: рамка и фон (два скруглённых прямоугольника, второй на точку меньше), кружок
 * под значком, заголовок, подпись «Web browser for VDAC4…», вращающийся глобус или знак
 * предупреждения, строки состояния и подсказки. */
static void draw_wifi(ft_dl_t *d) {
    dl_put(d, dl_begin(FT_RECTS));
    dl_put(d, dl_color_a(255));
    rrect(d, TSA_UI_WIFI_X, TSA_UI_WIFI_Y, TSA_UI_WIFI_WIDTH, TSA_UI_WIFI_HEIGHT, TSA_UI_WIFI_RADIUS, TSA_UI_WIFI_BORDER_RGB888);
    rrect(d, TSA_UI_WIFI_X + 1, TSA_UI_WIFI_Y + 1, TSA_UI_WIFI_WIDTH - 2, TSA_UI_WIFI_HEIGHT - 2, TSA_UI_WIFI_RADIUS - 1,
          TSA_UI_WIFI_BG_RGB888);
    dl_put(d, dl_end());
    dl_put(d, dl_begin(FT_POINTS));
    dl_put(d, dl_color_rgb(TSA_UI_WIFI_INDICATOR_BG_RGB888));
    dl_put(d, dl_point_size(TSA_UI_WIFI_INDICATOR_RADIUS * 16));
    dl_put(d, dl_vertex(TSA_UI_WIFI_INDICATOR_CENTER_X * 16, TSA_UI_WIFI_INDICATOR_CENTER_Y * 16));
    dl_put(d, dl_end());
    dl_put(d, dl_begin(FT_BITMAPS));
    l4(d, TSA_UI_ASSET_WIFI_TITLE_RAM_G, TSA_UI_ASSET_WIFI_TITLE_STRIDE, TSA_UI_ASSET_WIFI_TITLE_WIDTH,
       TSA_UI_ASSET_WIFI_TITLE_HEIGHT, TSA_UI_WIFI_TITLE_X, TSA_UI_WIFI_TITLE_Y, TSA_UI_WIFI_TITLE_RGB888);
    l4(d, RAMG_WIFI, WIFI_STRIDE, WIFI_COLS * FONT_CELL_W, FONT_CELL_H, TSA_UI_WIFI_SUBTITLE_X, TSA_UI_WIFI_SUBTITLE_Y,
       TSA_UI_WIFI_SUBTITLE_RGB888);
    if (wifi == UI_WIFI_FAILED)
        icon(d, TSA_UI_ASSET_WARNING_RAM_G, TSA_UI_WIFI_WARNING_X, TSA_UI_WIFI_WARNING_Y, TSA_UI_WIFI_FAILED_RGB888);
    else
        globe_frame(d, TSA_UI_WIFI_GLOBE_X, TSA_UI_WIFI_GLOBE_Y, TSA_UI_WIFI_CONNECTING_RGB888);
    centered(d, WL_STATE, TSA_UI_WIFI_STATE_Y, TSA_UI_WIFI_STATE_RGB888);
    centered(d, WL_HINT, TSA_UI_WIFI_HINT_Y, TSA_UI_WIFI_HINT_RGB888);
    centered(d, WL_EXIT, TSA_UI_WIFI_HINT_Y + TSA_UI_WIFI_HINT_LINE_STEP, TSA_UI_WIFI_HINT_RGB888);
    dl_put(d, dl_end());
}

/* Панель кнопок: подложка, кружок под кнопкой, над которой мышь (темнее, если зажата), и
 * четыре иконки. Серый цвет — кнопка недоступна; зажатая иконка смещается на точку вниз-вправо. */
static void draw_nav(ft_dl_t *d) {
    int off = nav_off();
    dl_put(d, dl_begin(FT_RECTS));
    dl_put(d, dl_color_a(255));
    rrect(d, TSA_UI_NAV_X - off, TSA_UI_NAV_Y, TSA_UI_NAV_WIDTH, TSA_UI_NAV_HEIGHT, TSA_UI_NAV_RADIUS, TSA_UI_BUTTON_BG_RGB888);
    dl_put(d, dl_end());
    if (hover >= 0 && hover < TSA_UI_NAV_BUTTON_COUNT && nav_t == NAV_MS) {
        dl_put(d, dl_begin(FT_POINTS));
        dl_put(d, dl_color_rgb(held_btn == hover ? TSA_UI_BUTTON_PRESSED_BG_RGB888 : TSA_UI_BUTTON_HOVER_BG_RGB888));
        dl_put(d, dl_point_size(TSA_UI_NAV_HOVER_RADIUS * 16));
        dl_put(d, dl_vertex(tsa_ui_nav_centers_x[hover] * 16, CY * 16));
        dl_put(d, dl_end());
    }
    static const uint32_t assets[4] = {TSA_UI_ASSET_BACK_RAM_G, TSA_UI_ASSET_FORWARD_RAM_G, TSA_UI_ASSET_STOP_RAM_G,
                                       TSA_UI_ASSET_RELOAD_RAM_G};
    dl_put(d, dl_begin(FT_BITMAPS));
    for (int i = 0; i < TSA_UI_NAV_BUTTON_COUNT; i++) {
        int on = i == 0 ? can_back : i == 1 ? can_fwd : i == 2 ? busy : !busy;
        uint32_t rgb = !on ? TSA_UI_BUTTON_DISABLED_RGB888 : i == 2 ? TSA_UI_STOP_ACTIVE_RGB888 : TSA_UI_BUTTON_NORMAL_RGB888;
        int push = held_btn == i && hover == i ? TSA_UI_PRESSED_ICON_OFFSET : 0;
        icon(d, assets[i], tsa_ui_nav_centers_x[i] - off - TSA_UI_ICON_HALF_WIDTH + push, CY - TSA_UI_ICON_HEIGHT / 2 + push, rgb);
    }
    dl_put(d, dl_end());
}

/* Строка адреса: рамка, фон, разделители, выделение, каретка, кнопки «перейти» и настроек,
 * текст адреса и метка языка. Всё сдвинуто на off — так строка выезжает; ножницы обрезают её
 * у глобуса. В конце ножницы снова на весь экран. */
static void draw_address(ft_dl_t *d) {
    int off = addr_off();
    dl_put(d, dl_scissor_xy(TSA_UI_ADDRESS_CLIP_X, TSA_UI_ADDRESS_CLIP_Y));
    dl_put(d, dl_scissor_size(TSA_UI_ADDRESS_CLIP_WIDTH, TSA_UI_ADDRESS_CLIP_HEIGHT));
    dl_put(d, dl_begin(FT_RECTS));
    dl_put(d, dl_color_a(255));
    rrect(d, TSA_UI_ADDRESS_X + off, TSA_UI_ADDRESS_Y, TSA_UI_ADDRESS_WIDTH, TSA_UI_ADDRESS_HEIGHT, TSA_UI_ADDRESS_RADIUS,
          TSA_UI_ADDRESS_BORDER_RGB888);
    rrect(d, TSA_UI_ADDRESS_X + off + 1, TSA_UI_ADDRESS_Y + 1, TSA_UI_ADDRESS_WIDTH - 2, TSA_UI_ADDRESS_HEIGHT - 2,
          TSA_UI_ADDRESS_RADIUS - 1, TSA_UI_ADDRESS_BG_RGB888);
    bar(d, TSA_UI_ADDRESS_SEPARATOR_X + off, TSA_UI_ADDRESS_SEPARATOR_Y0, 0,
        TSA_UI_ADDRESS_SEPARATOR_Y1 - TSA_UI_ADDRESS_SEPARATOR_Y0, TSA_UI_ADDRESS_SEPARATOR_RGB888);
    bar(d, TSA_UI_SETTINGS_SEPARATOR_X + off, TSA_UI_SETTINGS_SEPARATOR_Y0, 0,
        TSA_UI_SETTINGS_SEPARATOR_Y1 - TSA_UI_SETTINGS_SEPARATOR_Y0, TSA_UI_ADDRESS_SEPARATOR_RGB888);
    if (select_all && url_n) {                                 /* выделение всего адреса */
        int w = (int)(url_n - view < ADDR_COLS ? url_n - view : ADDR_COLS) * FONT_CELL_W;
        rrect(d, TSA_UI_ADDRESS_TEXT_X + off, TSA_UI_SELECTION_Y, w, TSA_UI_SELECTION_HEIGHT, TSA_UI_SELECTION_RADIUS,
              TSA_UI_SELECTION_BG_RGB888);
    }
    if (addr_t == ADDR_MS) {                                    /* каретка */
        int cx = TSA_UI_ADDRESS_TEXT_X + (int)(caret - view) * FONT_CELL_W;
        dl_put(d, dl_color_rgb(TSA_UI_CARET_RGB888));
        dl_put(d, dl_line_width(16));
        dl_put(d, dl_vertex(cx * 16, TSA_UI_CARET_Y * 16));
        dl_put(d, dl_vertex((cx + TSA_UI_CARET_WIDTH - 1) * 16, (TSA_UI_CARET_Y + TSA_UI_CARET_HEIGHT - 1) * 16));
    }
    dl_put(d, dl_end());
    if (hover == H_SETTINGS) {
        dl_put(d, dl_begin(FT_POINTS));
        dl_put(d, dl_color_rgb(TSA_UI_BUTTON_HOVER_BG_RGB888));
        dl_put(d, dl_point_size(TSA_UI_SETTINGS_HOVER_RADIUS * 16));
        dl_put(d, dl_vertex((TSA_UI_SETTINGS_CENTER_X + off) * 16, TSA_UI_SETTINGS_CENTER_Y * 16));
        dl_put(d, dl_end());
    }
    dl_put(d, dl_begin(FT_BITMAPS));
    icon(d, TSA_UI_ASSET_GO_RAM_G, TSA_UI_GO_X + off, TSA_UI_GO_Y, TSA_UI_CARET_RGB888);
    icon(d, TSA_UI_ASSET_SETTINGS_RAM_G, TSA_UI_SETTINGS_X + off, TSA_UI_SETTINGS_Y, TSA_UI_SUGGESTION_ICON_RGB888);
    int tx = TSA_UI_ADDRESS_TEXT_X + off, x0 = tx > TSA_UI_ADDRESS_CLIP_X ? tx : TSA_UI_ADDRESS_CLIP_X;
    if (x0 < TSA_UI_ADDRESS_TEXT_X + TSA_UI_ADDRESS_TEXT_WIDTH + off) {
        dl_put(d, dl_scissor_xy((unsigned)x0, TSA_UI_ADDRESS_TEXT_CLIP_Y));
        dl_put(d, dl_scissor_size((unsigned)(tx + TSA_UI_ADDRESS_TEXT_WIDTH - x0), TSA_UI_ADDRESS_TEXT_CLIP_HEIGHT));
        l4(d, TSA_UI_ADDRESS_RAM_G_BASE, ADDR_STRIDE, TSA_UI_ADDRESS_TEXT_WIDTH, FONT_CELL_H, tx, TSA_UI_ADDRESS_TEXT_Y,
           TSA_UI_ADDRESS_TEXT_RGB888);
    }
    /* Метка языка ввода (EN или RU, дизайн Codex) — сразу справа от каретки. Видна всегда, когда
     * каретка в конце адреса; в середине строки — только 1 секунду после смены языка (иначе
     * закрывала бы буквы). */
    int flag_x = TSA_UI_ADDRESS_TEXT_X + off + (int)(caret - view) * FONT_CELL_W + TSA_UI_INPUT_LANG_OFFSET_FROM_CARET_X;
    if (addr_t == ADDR_MS && (caret == url_n || lang_until) && flag_x + TSA_UI_INPUT_LANG_WIDTH <= TSA_UI_INPUT_LANG_MAX_RIGHT) {
        dl_put(d, dl_scissor_xy(TSA_UI_INPUT_LANG_CLIP_X, TSA_UI_INPUT_LANG_CLIP_Y));
        dl_put(d, dl_scissor_size(TSA_UI_INPUT_LANG_CLIP_WIDTH, TSA_UI_INPUT_LANG_CLIP_HEIGHT));
        l4(d, input_russian() ? TSA_UI_ASSET_INPUT_RU_RAM_G : TSA_UI_ASSET_INPUT_EN_RAM_G, TSA_UI_ASSET_INPUT_EN_STRIDE,
           TSA_UI_INPUT_LANG_WIDTH, TSA_UI_INPUT_LANG_HEIGHT, flag_x, TSA_UI_INPUT_LANG_Y, TSA_UI_INPUT_LANG_RGB888);
    }
    dl_put(d, dl_end());
    dl_put(d, dl_scissor_xy(0, 0));
    dl_put(d, dl_scissor_size(2048, 2048));
}

/* Глобус: полупрозрачный круг (прозрачность — по таблице анимации; под мышью — плотнее) и
 * текущий кадр вращения поверх него. */
static void draw_globe(ft_dl_t *d) {
    int gx = globe_x();
    int a = addr_t ? TSA_UI_GLOBE_OPEN_ALPHA : tsa_ui_nav_globe_alpha[nav_t];
    if (!nav_t && !addr_t && hover == H_GLOBE) a = TSA_UI_GLOBE_HOVER_ALPHA;
    dl_put(d, dl_begin(FT_POINTS));
    dl_put(d, dl_color_rgb(TSA_UI_GLOBE_BG_RGB888));
    dl_put(d, dl_color_a((uint8_t)a));
    dl_put(d, dl_point_size(TSA_UI_GLOBE_RADIUS * 16));
    dl_put(d, dl_vertex(gx * 16, TSA_UI_GLOBE_CENTER_Y * 16));
    dl_put(d, dl_end());
    dl_put(d, dl_begin(FT_BITMAPS));
    globe_frame(d, gx + TSA_UI_GLOBE_BITMAP_OFFSET_X, TSA_UI_GLOBE_CENTER_Y + TSA_UI_GLOBE_BITMAP_OFFSET_Y, TSA_UI_GLOBE_RGB888);
    dl_put(d, dl_end());
}

/* Полоса прокрутки: дорожка и ползунок. Ползунок во столько раз короче дорожки, во сколько
 * страница выше экрана (но не короче минимума), и стоит на той же доле пути, что и экран на
 * странице. int64_t — чтобы произведение больших чисел не переполнилось. */
static void draw_scroll(ft_dl_t *d) {
    int track = TSA_UI_SCROLL_TRACK_HEIGHT;
    int h = (int)((int64_t)track * TS_H / s_height);
    if (h < TSA_UI_SCROLL_MIN_THUMB_HEIGHT) h = TSA_UI_SCROLL_MIN_THUMB_HEIGHT;
    int y = TSA_UI_SCROLL_TRACK_Y + (int)((int64_t)(track - h) * s_top / (s_height - TS_H));
    dl_put(d, dl_begin(FT_RECTS));
    dl_put(d, dl_color_a(255));
    rrect(d, TSA_UI_SCROLL_TRACK_X, TSA_UI_SCROLL_TRACK_Y, TSA_UI_SCROLL_TRACK_WIDTH, track, TSA_UI_SCROLL_TRACK_RADIUS,
          TSA_UI_SCROLL_TRACK_RGB888);
    rrect(d, TSA_UI_SCROLL_TRACK_X, y, TSA_UI_SCROLL_THUMB_WIDTH, h, TSA_UI_SCROLL_THUMB_RADIUS, TSA_UI_SCROLL_THUMB_RGB888);
    dl_put(d, dl_end());
}

/* Порядок рисования важен: что позже, то сверху. Курсор — последним, поверх всего; его
 * остриё — в точке (1, 1) картинки, поэтому картинка сдвинута на (−1, −1). Ручка 1 —
 * своя, отдельно от страницы (ручка 0). */
/* Индикатор: плашка, полоса хода (дорожка и заливка), кружок под крестиком, значок, крестик и
 * маска текста (две строки). Цвет значка и полосы — по состоянию: идёт — мятный, запись на
 * карту — янтарный, ошибка — янтарный знак, готово — мятная галочка и полная полоса. */
static void draw_download(ft_dl_t *d, uint32_t now) {
    int y = DL_Y, k = dl_kind, hx = hover == H_DL_X;
    int bx = TSA_UI_DOWNLOAD_BAR_X, by = y + TSA_UI_DOWNLOAD_BAR_Y_OFFSET, bw = TSA_UI_DOWNLOAD_BAR_WIDTH;
    int fx = 0, fw = 0;
    uint32_t fill = k == TSA_UI_DOWNLOAD_WRITING ? TSA_UI_DOWNLOAD_WRITING_RGB888 : TSA_UI_DOWNLOAD_FILL_RGB888;
    /* Бегающий отрезок — «готовлюсь», заполняющаяся полоса — ход (пользователь 04.11: «бегающая
     * туда-сюда полоса — это подготовка к чему-либо», «нужен прогресс-бар»). */
    int preparing = k == TSA_UI_DOWNLOAD_UNKNOWN || (k == TSA_UI_DOWNLOAD_WRITING && !dl_done);
    if (!preparing && (k == TSA_UI_DOWNLOAD_KNOWN || k == TSA_UI_DOWNLOAD_WRITING)) {
        /* полоса заполняется по доле файла. При записи на карту доля — сколько программа Z80
         * уже забрала (пишет она блоками по 16 КБ следом); до подтверждения «записано» — не
         * больше 99 %: полная полоса только у «готово» */
        uint32_t done = dl_done > dl_total ? dl_total : dl_done;
        fw = dl_total ? (int)((uint64_t)bw * done / dl_total) : 0;
        if (k == TSA_UI_DOWNLOAD_WRITING && fw > bw * 99 / 100) fw = bw * 99 / 100;
    } else if (preparing) {
        /* длина неизвестна или программа Z80 ещё готовит карту (дата, каталоги, временный
         * файл): отрезок бегает туда и обратно, процента не выдумываем */
        uint32_t t = now % TSA_UI_DOWNLOAD_INDETERMINATE_CYCLE_MS;
        if (t >= TSA_UI_DOWNLOAD_INDETERMINATE_HALF_MS) t = TSA_UI_DOWNLOAD_INDETERMINATE_CYCLE_MS - t;
        fx = (int)(t * TSA_UI_DOWNLOAD_INDETERMINATE_TRAVEL / TSA_UI_DOWNLOAD_INDETERMINATE_HALF_MS);
        fw = TSA_UI_DOWNLOAD_INDETERMINATE_WIDTH;
    } else if (k == TSA_UI_DOWNLOAD_DONE) {
        fw = bw;
    }
    if (fw && fw < TSA_UI_DOWNLOAD_BAR_HEIGHT) fw = TSA_UI_DOWNLOAD_BAR_HEIGHT;   /* скруглённому концу нужно место */
    dl_put(d, dl_begin(FT_RECTS));
    dl_put(d, dl_color_a(TSA_UI_DOWNLOAD_ALPHA));
    rrect(d, TSA_UI_DOWNLOAD_X, y, TSA_UI_DOWNLOAD_WIDTH, TSA_UI_DOWNLOAD_HEIGHT, TSA_UI_DOWNLOAD_RADIUS,
          hover == H_DL || hx ? TSA_UI_DOWNLOAD_HOVER_BG_RGB888 : TSA_UI_DOWNLOAD_BG_RGB888);
    rrect(d, bx, by, bw, TSA_UI_DOWNLOAD_BAR_HEIGHT, TSA_UI_DOWNLOAD_BAR_RADIUS, TSA_UI_DOWNLOAD_TRACK_RGB888);
    if (fw) rrect(d, bx + fx, by, fw, TSA_UI_DOWNLOAD_BAR_HEIGHT, TSA_UI_DOWNLOAD_BAR_RADIUS, fill);
    dl_put(d, dl_end());
    if (hx) {
        dl_put(d, dl_begin(FT_POINTS));
        dl_put(d, dl_color_rgb(TSA_UI_DOWNLOAD_ACTION_HOVER_BG_RGB888));
        dl_put(d, dl_point_size(TSA_UI_DOWNLOAD_ACTION_HOVER_RADIUS * 16));
        dl_put(d, dl_vertex(TSA_UI_DOWNLOAD_ACTION_CENTER_X * 16, (y + TSA_UI_DOWNLOAD_ACTION_CENTER_Y_OFFSET) * 16));
        dl_put(d, dl_end());
    }
    uint32_t icon_at = k == TSA_UI_DOWNLOAD_DONE ? TSA_UI_ASSET_CHECK_RAM_G :
                       k >= TSA_UI_DOWNLOAD_ERROR ? TSA_UI_ASSET_WARNING_RAM_G : TSA_UI_ASSET_DOWNLOAD_RAM_G;
    uint32_t icon_rgb = k == TSA_UI_DOWNLOAD_DONE ? TSA_UI_DOWNLOAD_DONE_RGB888 :
                        k >= TSA_UI_DOWNLOAD_ERROR ? TSA_UI_DOWNLOAD_ERROR_ICON_RGB888 :
                        k == TSA_UI_DOWNLOAD_WRITING ? TSA_UI_DOWNLOAD_WRITING_RGB888 : TSA_UI_DOWNLOAD_ICON_RGB888;
    dl_put(d, dl_begin(FT_BITMAPS));
    icon(d, icon_at, TSA_UI_DOWNLOAD_ICON_X, y + TSA_UI_DOWNLOAD_ICON_Y_OFFSET, icon_rgb);
    if (dl_has_x())
        icon(d, TSA_UI_ASSET_CLOSE_RAM_G, TSA_UI_DOWNLOAD_ACTION_X, y + TSA_UI_DOWNLOAD_ACTION_Y_OFFSET,
             hx ? TSA_UI_DOWNLOAD_ACTION_HOVER_RGB888 : TSA_UI_DOWNLOAD_ACTION_RGB888);
    l4(d, TSA_UI_BUFFER_DOWNLOAD_TEXT_RAM_G, TSA_UI_BUFFER_DOWNLOAD_TEXT_STRIDE, TSA_UI_BUFFER_DOWNLOAD_TEXT_WIDTH,
       TSA_UI_BUFFER_DOWNLOAD_TEXT_HEIGHT, TSA_UI_DOWNLOAD_TEXT_X, y + TSA_UI_DOWNLOAD_TEXT_Y_OFFSET, TSA_UI_DOWNLOAD_TEXT_RGB888);
    dl_put(d, dl_end());
}

void ui_dl(ft_dl_t *d) {
    dl_put(d, dl_handle(1));
    if (wifi) {
        draw_wifi(d);
    } else {
        if (dl_visible()) draw_download(d, ts_core->millis());
        if (scroll_shown && s_height > TS_H) draw_scroll(d);
        if (nav_t) draw_nav(d);
        if (addr_t) draw_address(d);
        draw_globe(d);                                          /* глобус — поверх поля адреса */
    }
    /* курсор — последним, острие в (1, 1). Над ссылкой — цвет темы: видеочип умножает цвет
     * точек картинки на COLOR_RGB, белая заливка становится мятной, тёмный контур остаётся. */
    dl_put(d, dl_begin(FT_BITMAPS));
    dl_put(d, dl_color_rgb(link_hover ? TSA_UI_CURSOR_LINK_RGB888 : 0xFFFFFF));
    dl_put(d, dl_color_a(255));
    dl_bitmap(d, TSA_UI_ASSET_CURSOR_ZUMA_RAM_G, FT_ARGB4, TSA_UI_CURSOR_WIDTH * 2u, TSA_UI_CURSOR_WIDTH, TSA_UI_CURSOR_HEIGHT);
    dl_put(d, dl_vertex((mx - TSA_UI_CURSOR_HOTSPOT_X) * 16, (my - TSA_UI_CURSOR_HOTSPOT_Y) * 16));
    dl_put(d, dl_end());
}
