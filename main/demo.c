/* Встроенная страница «tsafari:about»: проверка шрифта, палитры, прокрутки и ссылок без сети.
 * Ссылки ведут к разделам этой же страницы (номер ссылки = номер раздела). Это и простой
 * пример, как пользоваться раскладкой doc.c: стили, абзацы из кусков, блок с фоном, полосы.
 * Цвета — из дизайна Codex (ui_design.h). */
#include "page.h"
#include "doc.h"
#include "font.h"
#include "demo.h"
#include "ts.h"
#include "ui_design.h"

#define BG      TSA_UI_COLOR_PAPER_RGB888
#define TEXT    TSA_UI_COLOR_INK_RGB888
#define DIM     TSA_UI_COLOR_MUTED_RGB888
#define HEAD    TSA_UI_COLOR_INK_RGB888
#define LINK    TSA_UI_COLOR_LINK_RGB888
#define VISITED TSA_UI_COLOR_VISITED_RGB888
#define BOX     TSA_UI_COLOR_SOFT_RGB888
#define RULE    TSA_UI_COLOR_LINE_RGB888

static int section_y[DEMO_SECTIONS];     /* где начинается каждый раздел (для перехода по ссылке) */

/* Стили: {цвет букв, фон (0 — как у страницы), флаги, номер ссылки (−1 — не ссылка)}. */
static const doc_style_t S_TEXT = {TEXT, 0, 0, -1}, S_DIM = {DIM, 0, 0, -1}, S_HEAD = {HEAD, 0, 0, -1, FACE_H3};

/* Абзац обычного текста; после него — 10 точек промежутка. */
static void text(const char *t) { doc_para(&t, &S_TEXT, 1, 10); }
/* Заголовок раздела: запомнить, где он, и вывести подчёркнутым. */
static void head(int section, const char *t) {
    section_y[section] = doc_y();
    doc_para(&t, &S_HEAD, 1, 8);
}

/* Строки на разных языках — проверить, что все буквы и надстрочные знаки есть в шрифте. */
static const char *const languages[] = {
    "English     The quick brown fox jumps over the lazy dog.",
    "Русский     Съешь же ещё этих мягких французских булок, да выпей чаю.",
    "Українська  Ґанок, їжак, єдність, Україна.",
    "Deutsch     Grüße aus Köln. Ä Ö Ü ä ö ü ß.",
    "Polski      Zażółć gęślą jaźń.",
    "Čeština     Příliš žluťoučký kůň úpěl ďábelské ódy.",
    "Tiếng Việt  Trường, Nguyễn, cộng hòa.",
    "Ударение    сло́во, моло́ко, за́мок и замо́к.",
};

void demo_page(void) {
    doc_begin(BG);
    {
        const char *t[] = {"TSafari", " — веб-браузер для VDAC4: ZX Evolution, Tang Nano 20K, ESP32-S3."};
        const doc_style_t s[] = {{HEAD, 0, 0, -1, FACE_H1}, S_TEXT};
        doc_para(t, s, 2, 6);
    }
    {
        const char *t[] = {"Этап 0, без сети. Разделы: ", "шрифт", ", ", "прокрутка", ", ", "кнопки",
                           ", ", "клавиши", "."};
        const doc_style_t s[] = {S_DIM, {LINK, 0, PAGE_UNDERLINE, 0}, S_DIM, {LINK, 0, PAGE_UNDERLINE, 1}, S_DIM,
                                 {VISITED, 0, PAGE_UNDERLINE, 2}, S_DIM, {LINK, 0, PAGE_UNDERLINE, 3}, S_DIM};
        doc_para(t, s, 9, 14);
    }
    doc_rule(RULE, 1, 14);

    head(0, "1. Шрифт");
    text("Noto Sans Mono из просмотрщика текстов TXTVIEW2 (WC Improved): 2278 кодов Unicode, 2862 глифа, "
         "ячейка 12×34, кегль 20 точек. Сглаживание — 16 уровней через палитру страницы.");
    doc_box_begin(BOX, 12, 6, 0);
    for (unsigned i = 0; i < sizeof(languages) / sizeof(languages[0]); i++) {
        const char *t = languages[i];
        doc_para(&t, &S_TEXT, 1, 0);
    }
    doc_box_end(14);

    head(1, "2. Прокрутка");
    text("Страница лежит в RAM_G кольцом строк 1024×896, байт на точку. Колесо мыши, стрелки, PgUp/PgDn, "
         "пробел, Home и End двигают её; за кадр — не больше 128 строк, новые строки ложатся в невидимую "
         "часть кольца.");
    for (int i = 1; i <= 24; i++) {
        static char line[160];
        const char *t = line;
        int n = 0;
        const char *a = "Строка прокрутки ";
        while (*a) line[n++] = *a++;
        line[n++] = (char)('0' + i / 10); line[n++] = (char)('0' + i % 10);
        a = ": The board keeps the classic Z80 and adds HDMI, network and sound.";
        while (*a) line[n++] = *a++;
        line[n] = 0;
        doc_para(&t, i % 3 ? &S_TEXT : &S_DIM, 1, 0);
    }
    doc_rule(RULE, 1, 14);

    head(2, "3. Кнопки");
    text("В покое на экране только полупрозрачный глобус в левом верхнем углу. Курсор в самом углу — "
         "кнопки назад, вперёд, стоп и обновить выезжают из-за края. Глобус или клавиша Win — строка "
         "адреса выезжает из-под глобуса.");

    head(3, "4. Клавиши");
    text("Стрелки и колесо — прокрутка; PgUp, PgDn, пробел — на экран; Home, End — к началу и концу; "
         "Ctrl+← и Ctrl+→ (или Alt+← и Alt+→) — назад и вперёд по страницам, Backspace — назад; Win — строка адреса; "
         "в ней Ctrl+Shift — русская или латинская раскладка, Enter — перейти, Esc — закрыть. "
         "F5 — обновить. F12 — выход.");
    doc_rule(RULE, 1, 14);
    {
        const char *t = "TSafari " TS_VERSION ", этап 0.";
        doc_para(&t, &S_DIM, 1, PAGE_MARGIN);
    }
}

/* Минус поле страницы — чтобы заголовок встал у самого верха экрана. */
int demo_section_y(int section) {
    return section >= 0 && section < DEMO_SECTIONS ? section_y[section] - PAGE_MARGIN : -1;
}
