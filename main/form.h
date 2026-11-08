/* Формы: поля ввода, кнопки, флажки, переключатели и списки на странице.
 *
 * ЧТО ТАКОЕ ФОРМА. На странице бывает «анкета»: поле для поиска и кнопка «Найти», поля логина,
 * флажок «запомнить меня», список «выберите город». В HTML это тег <form> с элементами внутри:
 *   <form action="/search" method="get">
 *     <input name="q" value="ZX">  <input type="submit" value="Найти">
 *   </form>
 * Когда нажимают кнопку (или Enter в поле), браузер собирает пары «имя=значение» всех
 * элементов формы (q=ZX) и отправляет их на адрес action: способом GET — дописав к адресу
 * после «?» (/search?q=ZX), способом POST — отдельным «телом» запроса.
 *
 * КАК ЭТО УСТРОЕНО У НАС.
 *   - html.c находит теги и зовёт form_open / form_add / form_option; размер элемента
 *     (form_size) раскладка ставит в строку как встроенный блок (doc_inline);
 *   - когда строка легла на страницу, form_place запоминает, где элемент, и ставит на
 *     странице встроенный элемент (page_widget);
 *   - рисует элемент form_paint — прямо в точки страницы, как текст (вид — дизайн Codex 08c:
 *     поле, кнопка, флажок, переключатель, список);
 *   - щелчки и клавиши app.c отдаёт сюда (form_click, form_key, form_mouse); набранный текст
 *     меняет только строки поля (page_redraw), а каретку и раскрытый список видеочип рисует
 *     поверх страницы (form_dl);
 *   - отправка: form_click / form_key возвращают FORM_SUBMIT и заполняют form_submit_t —
 *     адрес (GET) или адрес и тело (POST); переходит по нему app.c. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ft.h"
#include "input.h"
#include "url.h"

/* Виды элементов. */
enum {
    FC_TEXT,        /* строка текста (<input>, type=text, search, email…) */
    FC_PASSWORD,    /* пароль: вместо букв точки */
    FC_TEXTAREA,    /* многострочный текст (<textarea>) */
    FC_CHECKBOX,    /* флажок: включён или нет */
    FC_RADIO,       /* переключатель: из группы с одним именем включён один */
    FC_SELECT,      /* список выбора (<select>) */
    FC_SUBMIT,      /* кнопка «отправить» */
    FC_RESET,       /* кнопка «вернуть как было» */
    FC_BUTTON,      /* кнопка для программы JS — у нас ничего не делает */
    FC_IMAGE,       /* картинка-кнопка «отправить» (пока — кнопка с подписью alt) */
    FC_HIDDEN,      /* скрытое значение: не видно, но отправляется */
    FC_FILE,        /* выбор файла — не умеем, видна неактивная кнопка */
};

/* Новый элемент — то, что html.c прочитал в теге. Строки — UTF-8; NULL — нет. */
typedef struct {
    int type;                    /* FC_… */
    const char *name;            /* имя для отправки */
    const char *value;           /* значение: текст поля, подпись кнопки, значение флажка */
    const char *placeholder;     /* серая подсказка в пустом поле */
    const char *label;           /* подпись кнопки, если она не value: текст <button>, alt картинки */
    int checked, disabled, readonly;
    int size;                    /* ширина поля в буквах (атрибут size); 0 — 20 */
    int maxlength;               /* больше букв не набрать; 0 — без предела */
    int rows, cols;              /* textarea: строк и букв в строке; select: самая длинная подпись */
    int width;                   /* ширина из CSS в точках; 0 — по size или cols */
    int height;                  /* высота из CSS в точках; до 24 — «маленькое» поле мелким шрифтом */
    uint32_t fill, ink;          /* фон и цвет текста из CSS (0xRRGGBB); 0 — по дизайну */
    int no_border;               /* CSS border: 0 / none — без рамки */
    int no_fill;                 /* CSS background: transparent / none — без своего фона (видна страница) */
    int empty_label;             /* <input type=submit value=""> — подпись пустая (не «Submit») */
} form_new_t;

/* Что отправить (итог FORM_SUBMIT). */
typedef struct {
    char url[URL_MAX];           /* куда: у GET — уже с «?имя=значение…» */
    char *body;                  /* POST: тело (ts_alloc; освобождает забравший), NULL — это GET */
    size_t body_len;
    char target[32];             /* target формы (во фрейм) */
    int part;                    /* номер фрейма, где стоит форма; −1 — обычная страница */
} form_submit_t;

enum { FORM_NONE, FORM_USED, FORM_SUBMIT };   /* ответы form_click и form_key */

/* Новая страница: забыть все формы и элементы прежней. */
void form_clear(void);
/* <form>: адрес action (полный), способ (post = 1 — POST), target, номер фрейма, кодировка
 * страницы (charset.h — в ней отправляются буквы). Следующие элементы — этой формы. */
void form_open(const char *action, int post, const char *target, int part, int cs);
/* </form>: следующие элементы — без формы. */
void form_close(void);
/* Размер элемента в строке: блок w×h, из них desc точек ниже базовой линии (поля — так, чтобы
 * текст в поле стоял на одной линии с текстом рядом). max_w — ширина места. Нужен и при
 * измерении таблиц, когда элементы ещё не заводятся. */
void form_size(const form_new_t *n, int max_w, int *w, int *h, int *desc);
/* Завести элемент; его номер (для doc_inline и form_place) или −1 (нет памяти). */
int form_add(const form_new_t *n, int max_w);
/* Пункт списка (после form_add с FC_SELECT): подпись, значение (NULL — как подпись), выбран ли,
 * недоступен ли. */
void form_option(int id, const char *label, const char *value, int selected, int disabled);
/* Строка с элементом id легла на страницу: левый верхний угол блока (x, y), фон bg. */
void form_place(int id, int x, int y, uint32_t bg);
/* Нарисовать элемент id (зовёт page.c при отрисовке строк). */
void form_paint(int id, int x, int y, int w, int h, uint32_t bg);

/* Щелчок по странице в точке экрана (x, y): по элементу — FORM_USED или FORM_SUBMIT (*out —
 * что отправить); мимо — FORM_NONE (поле теряет фокус, щелчок разбирают ссылки). */
int form_click(int x, int y, form_submit_t *out);
/* Мышь, пока раскрыт список: подсветка пункта, выбор щелчком, колесо листает список. 1 — мышь
 * занята списком (страница щелчок и колесо не получает); 0 — список закрыт. */
int form_mouse(int x, int y, int pressed, int wheel);
/* Клавиша: поле в фокусе её взяло — FORM_USED (Enter — FORM_SUBMIT); FORM_NONE — не взяло. */
int form_key(const key_event_t *ev, form_submit_t *out);
/* Курсор над элементом, по которому можно щёлкнуть (кнопка, флажок, список, поле). */
int form_over(int x, int y);
/* Каретка поля в фокусе и раскрытый список — поверх страницы (top — прокрутка страницы). */
void form_dl(ft_dl_t *d, int top);
/* 1 — каретка или список изменились, нужен новый дисплей-лист (флаг снимается). */
int form_changed(void);
/* Та же страница раскладывается заново (пришла картинка нового размера): запомнить набранное,
 * флажки, выбранные пункты и поле в фокусе — и вернуть их новым элементам с теми же номерами. */
void form_save(void);
void form_restore(void);
/* Номер элемента для встроенного блока: у полей форм — меньше этого. */
#define FORM_ID_LIMIT 0x40000000
