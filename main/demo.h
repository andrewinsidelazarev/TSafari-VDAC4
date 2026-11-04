/* Встроенная страница «tsafari:about» (без сети): проверка шрифта, прокрутки и ссылок. */
#pragma once

#define DEMO_SECTIONS 4                  /* разделов на странице */

/* Разложить страницу через doc.c. */
void demo_page(void);
/* Верх раздела (номер ссылки на странице) для прокрутки, −1 — нет. */
int demo_section_y(int section);
