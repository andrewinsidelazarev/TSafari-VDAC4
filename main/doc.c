/* Раскладка текста (что это — в doc.h).
 *
 * Как устроено. Пока абзац не кончился, его текст копится в массиве cells — номера глифов,
 * по одному на колонку (букву). Рядом, в cell_style, для каждой клетки записан номер её стиля
 * в маленькой таблице styles. В конце абзаца doc_flush режет cells на строки:
 *   - берёт столько клеток, сколько помещается в строку;
 *   - если абзац на этом не кончился, ищет назад последний пробел и режет по нему (так слово
 *     целиком уходит на следующую строку); слово длиннее строки рвётся прямо по краю;
 *   - пробелы в начале и конце строки отбрасывает.
 * Каждая строка уходит в page_text кусками одного стиля: строка «обычный текст и ссылка» —
 * это два элемента страницы.
 *
 * Промежутки. Между абзацами — пустое место (gap). Его не добавляют сразу, а запоминают в
 * pending и добавляют перед следующей строкой. Так два промежутка подряд (конец списка и
 * начало заголовка) не складываются: берётся больший. И в самом верху страницы промежутка нет. */
#include <string.h>
#include "page.h"
#include "doc.h"
#include "font.h"
#include "ts.h"

#define MAX_CELLS 4096                   /* клеток в абзаце; длиннее — режется на несколько */
#define MAX_STYLES 64                    /* разных стилей в одном абзаце */

static int y;                            /* куда ляжет следующая строка (точки от верха страницы) */
static int pending;                      /* промежуток, который добавится перед следующей строкой */
static int box_rect = -1, box_y;         /* открытый блок с фоном: номер его прямоугольника и верх */
static int indent;                       /* отступ слева, в колонках */
static uint32_t page_bg_rgb, box_bg;     /* фон страницы и фон открытого блока */
static uint16_t cells[MAX_CELLS];        /* глифы текущего абзаца */
static uint8_t cell_style[MAX_CELLS];    /* номер стиля каждой клетки */
static unsigned cell_n;
static doc_style_t styles[MAX_STYLES];   /* стили текущего абзаца */
static unsigned style_n;

void doc_begin(uint32_t bg) {
    page_begin(bg);
    page_bg_rgb = bg;
    y = PAGE_MARGIN; pending = 0;
    box_rect = -1; indent = 0;
    cell_n = 0; style_n = 0;
}

int doc_y(void) { return y + pending; }
int doc_empty(void) { return cell_n == 0; }
void doc_indent(int cols) { indent = cols < 0 ? 0 : cols > DOC_COLS / 2 ? DOC_COLS / 2 : cols; }
int doc_get_indent(void) { return indent; }

/* Фон под текстом: блока, если он открыт, иначе страницы (к нему считается сглаживание). */
static uint32_t current_bg(void) { return box_rect >= 0 ? box_bg : page_bg_rgb; }

/* Одна строка — клетки [from, to) — с левым краем x0. Сначала добавить отложенный промежуток,
 * потом отдать строку кусками одного стиля. */
static void emit_line(unsigned from, unsigned to, int x0) {
    if (y > PAGE_MARGIN || pending) { y += pending; pending = 0; }
    unsigned i = from;
    while (i < to) {
        unsigned j = i + 1;
        while (j < to && cell_style[j] == cell_style[i]) j++;
        const doc_style_t *s = &styles[cell_style[i]];
        page_text(x0 + (int)(i - from) * FONT_CELL_W, y, cells + i, j - i, s->fg, s->bg ? s->bg : current_bg(),
                  s->flags, s->link);
        i = j;
    }
    y += PAGE_LINE_H;
}

/* Разрезать абзац на строки (как — в начале файла). inset — в блоке с фоном текст отступает
 * на одну колонку от каждого края. */
void doc_flush(int gap) {
    int inset = box_rect >= 0 ? 1 : 0;
    unsigned cols = (unsigned)(DOC_COLS - 2 * inset - indent);
    uint16_t space = (uint16_t)font_lookup(' ');
    unsigned start = 0;
    while (start < cell_n) {
        while (start < cell_n && cells[start] == space) start++;    /* пробелы в начале строки */
        if (start >= cell_n) break;
        unsigned end = start + cols < cell_n ? start + cols : cell_n, cut = end;
        if (end < cell_n) {                                         /* перенос по последнему пробелу */
            unsigned k = end;
            while (k > start && cells[k] != space) k--;
            if (k > start) cut = k;
        }
        while (cut > start && cells[cut - 1] == space) cut--;       /* хвостовые пробелы */
        emit_line(start, cut > start ? cut : end, PAGE_MARGIN + (inset + indent) * FONT_CELL_W);
        start = cut > start ? cut : end;
    }
    cell_n = 0; style_n = 0;
    if (gap > pending) pending = gap;
}

/* Добавить кусок текста. Новый стиль заводится, только если он отличается от последнего.
 * Текст превращается в глифы порциями по 256 байт; граница порции сдвигается так, чтобы не
 * разрезать букву UTF-8 посередине (байты продолжения буквы имеют вид 10xxxxxx). */
void doc_text(const char *utf8, unsigned bytes, const doc_style_t *st) {
    if (!bytes) return;
    if (!style_n || memcmp(&styles[style_n - 1], st, sizeof(*st))) {
        if (style_n == MAX_STYLES) doc_flush(0);                    /* редкий абзац-радуга */
        styles[style_n++] = *st;
    }
    uint8_t si = (uint8_t)(style_n - 1);
    while (bytes) {
        if (cell_n >= MAX_CELLS - 8) { doc_flush(0); styles[style_n++] = *st; si = 0; }
        unsigned chunk = bytes > 256 ? 256 : bytes;
        while (chunk < bytes && ((uint8_t)utf8[chunk] & 0xC0) == 0x80) chunk++;   /* не рвать букву */
        unsigned got = font_shape(utf8, chunk, cells + cell_n, MAX_CELLS - cell_n);
        memset(cell_style + cell_n, si, got);
        cell_n += got;
        utf8 += chunk; bytes -= chunk;
    }
}

void doc_para(const char *const *texts, const doc_style_t *st, unsigned parts, int gap) {
    for (unsigned p = 0; p < parts; p++) doc_text(texts[p], (unsigned)strlen(texts[p]), &st[p]);
    doc_flush(gap);
}

/* Горизонтальная полоса: прямоугольник от отступа до правого поля. */
void doc_rule(uint32_t rgb, int h, int gap) {
    doc_flush(0);
    if (y > PAGE_MARGIN || pending) { y += pending; pending = 0; }
    page_rect(PAGE_MARGIN + indent * FONT_CELL_W, y, TS_W - 2 * PAGE_MARGIN - indent * FONT_CELL_W, h, rgb);
    y += h;
    pending = gap;
}

/* Блок с фоном: заводим прямоугольник фона нулевой высоты (высота ещё неизвестна) и
 * отступаем 6 точек сверху. В doc_box_end высота станет известна и допишется. */
void doc_box_begin(uint32_t bg) {
    doc_flush(0);
    if (y > PAGE_MARGIN || pending) { y += pending; pending = 0; }
    box_bg = bg;
    box_y = y;
    box_rect = page_rect(PAGE_MARGIN, y, TS_W - 2 * PAGE_MARGIN, 0, bg);
    y += FONT_CELL_W / 2;
}

void doc_box_end(int gap) {
    doc_flush(0);
    y += FONT_CELL_W / 2;
    page_rect_height(box_rect, y - box_y);
    box_rect = -1;
    pending = gap;
}
