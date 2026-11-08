/* Стартовая страница «tsafari:start»: поиск DuckDuckGo (пользователь 08.11: «Стартовой страницей в
 * браузере нужно сделать html.duckduckgo.com… написать сверху окна ввода Search with DuckDuckGo, а
 * снизу окна поиска — кнопку Help, которая будет вести на страницу помощи»).
 *
 * Своя страница, а не загруженная с DuckDuckGo и переделанная: переделывать чужую страницу при
 * загрузке — поправка под один сайт (так браузер не делает). Здесь — обычный HTML с формой: запрос
 * уходит GET на https://html.duckduckgo.com/html/ (версия DuckDuckGo без JavaScript, та же, что у
 * поиска из строки адреса, url.c), ответ — страница DuckDuckGo. Открывается сразу, без сети.
 * «Help» — форма на «tsafari:about» (страница помощи, demo.c): кнопки рисует form.c, обе одинаковые.
 * Цвета — из дизайна Codex (ui_design.h, те же значения). */
#include <string.h>
#include "start.h"
#include "html.h"

static const char start_html[] =
    "<!DOCTYPE html>"
    "<html><head><meta charset=\"utf-8\"><title>TSafari</title>"
    "<style>"
    "body { background: #F7F3E7; color: #213439; margin: 0; }"
    ".wrap { width: 680px; margin: 190px auto 0; text-align: center; }"
    "h1 { font-size: 28px; font-weight: normal; color: #213439; margin: 0 0 22px; }"
    ".q { width: 520px; }"
    ".help { margin: 30px 0 0; }"
    "</style></head>"
    "<body><div class=\"wrap\">"
    "<h1>Search with DuckDuckGo</h1>"
    "<form action=\"https://html.duckduckgo.com/html/\" method=\"get\">"
    "<input class=\"q\" type=\"text\" name=\"q\" placeholder=\"Search the web\"> "
    "<button type=\"submit\">Search</button>"
    "</form>"
    "<form class=\"help\" action=\"tsafari:about\" method=\"get\"><button type=\"submit\">Help</button></form>"
    "</div></body></html>";

void start_page(void) {
    html_render((const uint8_t *)start_html, sizeof(start_html) - 1, "text/html; charset=utf-8", START_URL);
}
