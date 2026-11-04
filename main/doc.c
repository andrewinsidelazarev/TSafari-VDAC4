/* Раскладка текста (что это — в doc.h).
 *
 * КАК УСТРОЕНО. Пока абзац не кончился, его текст копится в массиве cells — номера глифов, по
 * одному на букву. Рядом, в cell_style, для каждой клетки записан номер её стиля в маленькой
 * таблице styles. Встроенный блок — особая клетка DOC_INLINE, а cell_style у неё — номер блока в
 * таблице inl (размер и id). В конце абзаца doc_flush режет cells на строки:
 *   - берёт клетки, пока сумма их ширин помещается в ширину рамки;
 *   - если абзац на этом не кончился, ищет назад последнее место переноса — пробел или край
 *     встроенного блока — и режет по нему (так слово целиком уходит на следующую строку); слово
 *     длиннее строки рвётся прямо по краю;
 *   - пробелы в начале и конце строки отбрасывает.
 * Каждая строка уходит в page_text кусками одного стиля: строка «обычный текст и ссылка» — это
 * два элемента страницы.
 *
 * ВЫСОТА СТРОКИ. У каждого начертания (font.h) клетка высотой h с базовой линией baseline. Над
 * базовой линией в строке нужно место max(baseline), под ней — max(h − baseline); встроенный
 * блок стоит низом на базовой линии и требует над ней свою высоту. Сумма — высота строки; у
 * строки основного текста она 34 точки, как в дизайне.
 *
 * ПРОМЕЖУТКИ. Между абзацами — пустое место (gap). Его не добавляют сразу, а запоминают в
 * pending и добавляют перед следующей строкой. Так два промежутка подряд (конец списка и начало
 * заголовка) не складываются: берётся больший. И в самом верху рамки промежутка нет.
 *
 * РАМКИ. Строки идут в текущую рамку (стек fs): у страницы — вся ширина с полями, у ячейки
 * таблицы — её столбец. У рамки свои y, промежуток, отступ, выравнивание и блок с фоном. */
#include <string.h>
#include "page.h"
#include "doc.h"
#include "font.h"
#include "ts.h"

#define MAX_CELLS 4096                   /* клеток в абзаце; длиннее — режется на несколько */
#define MAX_STYLES 64                    /* разных стилей в одном абзаце */
#define MAX_INLINE 64                    /* встроенных блоков в одном абзаце */
#define MAX_FRAMES 24                    /* вложенность рамок (таблица в таблице…) */
#define DOC_INLINE 0xFFFEu               /* клетка — встроенный блок */
#define BAR_W 3                          /* черта цитаты слева */

typedef struct {
    int x, w;                            /* левый край и ширина рамки */
    int y, pending, top;                 /* куда ляжет строка; промежуток перед ней; верх рамки */
    int indent, align;                   /* отступ слева (точки) и выравнивание строк */
    int hang;                            /* первая строка абзаца — на столько левее */
    uint32_t bg;                         /* фон рамки (под сглаживание букв) */
    int box_rect, box_bar, box_y;        /* открытый блок с фоном: прямоугольники и верх */
    int pad_x, pad_y;                    /* поля блока */
    uint32_t box_bg;
} frame_t;

static frame_t fs[MAX_FRAMES];
static int fn;                           /* рамок в стеке; текущая — fs[fn − 1] */
#define F (&fs[fn - 1])

static struct { int min, max; } ms[MAX_FRAMES];
static int mn;                           /* > 0 — идёт измерение, ms[mn − 1] — текущее */

static uint16_t cells[MAX_CELLS];        /* глифы текущего абзаца (DOC_INLINE — блок) */
static uint8_t cell_style[MAX_CELLS];    /* номер стиля клетки (у блока — номер в inl) */
static unsigned cell_n;
static doc_style_t styles[MAX_STYLES];   /* стили текущего абзаца */
static unsigned style_n;
static struct { int w, h, desc, id; } inl[MAX_INLINE];
static unsigned inl_n;
static void (*inline_place)(int id, int x, int y);
static uint16_t space_glyph;

void doc_begin(uint32_t bg) {
    page_begin(bg);
    memset(fs, 0, sizeof(fs));
    fn = 1;
    F->x = PAGE_MARGIN; F->w = DOC_WIDTH;
    F->y = F->top = PAGE_MARGIN;
    F->box_rect = F->box_bar = -1;
    F->bg = bg;
    mn = 0;
    cell_n = 0; style_n = 0; inl_n = 0;
    space_glyph = (uint16_t)font_lookup(' ');
}

void doc_set_inline_cb(void (*place)(int id, int x, int y)) { inline_place = place; }
int doc_y(void) { return F->y + F->pending; }
int doc_empty(void) { return cell_n == 0; }
int doc_measuring(void) { return mn > 0; }
void doc_indent(int px) { F->indent = px < 0 ? 0 : px > F->w / 2 ? F->w / 2 : px; }
int doc_get_indent(void) { return F->indent; }
void doc_align(int a) { F->align = a; }
void doc_hang(int px) { F->hang = px; }
void doc_set_bg(uint32_t bg) { page_set_bg(bg); fs[0].bg = bg; }
int doc_get_align(void) { return F->align; }
int doc_x(void) { return F->x + F->indent + F->pad_x; }
int doc_width(void) { int w = F->w - F->indent - 2 * F->pad_x; return w > 1 ? w : 1; }

/* Фон под текстом: блока, если он открыт, иначе рамки (к нему считается сглаживание). */
static uint32_t current_bg(void) { return F->box_bg ? F->box_bg : F->bg; }
uint32_t doc_bg(void) { return current_bg(); }

/* Ширина клетки: у буквы — шаг её начертания, у блока — его ширина. */
static int cell_w(unsigned i) {
    return cells[i] == DOC_INLINE ? inl[cell_style[i]].w : font_faces[styles[cell_style[i]].face].w;
}

/* Добавить отложенный промежуток перед новой строкой (кроме самого верха рамки). */
static void take_pending(void) {
    if (F->y > F->top || F->pending) { F->y += F->pending; F->pending = 0; }
}

/* Одна строка — клетки [from, to): высота по самому высокому начертанию, буквы на общей базовой
 * линии, выравнивание внутри ширины avail с левым краем left. */
static void emit_line(unsigned from, unsigned to, int left, int avail) {
    int lw = 0, asc = 0, desc = 0;
    for (unsigned i = from; i < to; i++) {
        lw += cell_w(i);
        if (cells[i] == DOC_INLINE) {
            int up = inl[cell_style[i]].h - inl[cell_style[i]].desc;
            if (up > asc) asc = up;
            if (inl[cell_style[i]].desc > desc) desc = inl[cell_style[i]].desc;
        } else {
            const font_face_t *f = &font_faces[styles[cell_style[i]].face];
            if (f->baseline > asc) asc = f->baseline;
            if (f->h - f->baseline > desc) desc = f->h - f->baseline;
        }
    }
    take_pending();
    int x = left;
    if (F->align == DOC_CENTER && lw < avail) x += (avail - lw) / 2;
    else if (F->align == DOC_RIGHT && lw < avail) x += avail - lw;
    unsigned i = from;
    while (i < to) {
        if (cells[i] == DOC_INLINE) {
            if (inline_place)
                inline_place(inl[cell_style[i]].id, x, F->y + asc - (inl[cell_style[i]].h - inl[cell_style[i]].desc));
            x += inl[cell_style[i]].w;
            i++;
            continue;
        }
        unsigned j = i + 1;
        while (j < to && cells[j] != DOC_INLINE && cell_style[j] == cell_style[i]) j++;
        const doc_style_t *s = &styles[cell_style[i]];
        const font_face_t *f = &font_faces[s->face];
        if (s->bg) page_rect(x, F->y, (int)(j - i) * f->w, asc + desc, s->bg);   /* свой фон под текстом */
        page_text(x, F->y + asc - f->baseline, s->face, cells + i, j - i, s->fg, s->bg ? s->bg : current_bg(),
                  s->flags, s->link);
        x += (int)(j - i) * f->w;
        i = j;
    }
    F->y += asc + desc;
}

/* Измерение абзаца: самое длинное неразрывное и вся длина без переносов (+ отступ и поля). */
static void measure_para(void) {
    int total = 0, word = 0, best = 0;
    for (unsigned i = 0; i < cell_n; i++) {
        int w = cell_w(i);
        if (cells[i] == space_glyph) { word = 0; total += w; continue; }
        if (cells[i] == DOC_INLINE) word = 0;
        word += w;
        total += w;
        if (word > best) best = word;
        if (cells[i] == DOC_INLINE) word = 0;
    }
    int extra = F->indent + 2 * F->pad_x;
    if (best + extra > ms[mn - 1].min) ms[mn - 1].min = best + extra;
    if (total + extra > ms[mn - 1].max) ms[mn - 1].max = total + extra;
}

/* Разрезать абзац на строки (как — в начале файла). */
void doc_flush(int gap) {
    if (mn) {
        if (cell_n) measure_para();
    } else {
        int left = doc_x(), avail = doc_width();
        unsigned start = 0;
        while (start < cell_n) {
            while (start < cell_n && cells[start] == space_glyph) start++;    /* пробелы в начале строки */
            if (start >= cell_n) break;
            if (F->hang) { left -= F->hang; avail += F->hang; }                /* висячая первая строка */
            int x = 0;
            unsigned i = start, brk = start;                                 /* brk — последнее место переноса */
            while (i < cell_n) {
                int w = cell_w(i);
                if (x + w > avail && i > start) break;
                if (cells[i] == space_glyph || cells[i] == DOC_INLINE) brk = i;
                if (i > start && cells[i - 1] == DOC_INLINE) brk = i;        /* перенос после блока */
                x += w;
                i++;
            }
            unsigned cut = i;
            if (i < cell_n) {                                                /* перенос по последнему месту */
                if (cells[i] == space_glyph || cells[i] == DOC_INLINE) cut = i;
                else if (brk > start) cut = brk;
            }
            unsigned end = cut;
            while (end > start && cells[end - 1] == space_glyph) end--;      /* хвостовые пробелы */
            if (end == start) end = cut > start ? cut : start + 1;
            emit_line(start, end, left, avail);
            if (F->hang) { left += F->hang; avail -= F->hang; F->hang = 0; }
            start = cut > start ? cut : start + 1;
        }
    }
    F->hang = 0;
    cell_n = 0; style_n = 0; inl_n = 0;
    if (gap > F->pending) F->pending = gap;
}

/* Новый стиль заводится, только если он отличается от последнего. Мест в таблице нет —
 * абзац выводится, и таблица начинается заново. */
static uint8_t style_index(const doc_style_t *st) {
    if (!style_n || memcmp(&styles[style_n - 1], st, sizeof(*st))) {
        if (style_n == MAX_STYLES) doc_flush(0);                    /* редкий абзац-радуга */
        styles[style_n++] = *st;
    }
    return (uint8_t)(style_n - 1);
}

/* Добавить кусок текста. Текст превращается в глифы порциями по 256 байт; граница порции
 * сдвигается так, чтобы не разрезать букву UTF-8 посередине (байты продолжения буквы имеют вид
 * 10xxxxxx). */
void doc_text(const char *utf8, unsigned bytes, const doc_style_t *st) {
    if (!bytes) return;
    uint8_t si = style_index(st);
    while (bytes) {
        if (cell_n >= MAX_CELLS - 8) { doc_flush(0); si = style_index(st); }
        unsigned chunk = bytes > 256 ? 256 : bytes;
        while (chunk < bytes && ((uint8_t)utf8[chunk] & 0xC0) == 0x80) chunk++;   /* не рвать букву */
        unsigned got = font_shape(utf8, chunk, cells + cell_n, MAX_CELLS - cell_n);
        memset(cell_style + cell_n, si, got);
        cell_n += got;
        utf8 += chunk; bytes -= chunk;
    }
}

void doc_inline(int w, int h, int desc, int id) {
    if (cell_n >= MAX_CELLS - 8 || inl_n == MAX_INLINE) doc_flush(0);
    inl[inl_n].w = w; inl[inl_n].h = h; inl[inl_n].desc = desc; inl[inl_n].id = id;
    cells[cell_n] = DOC_INLINE;
    cell_style[cell_n++] = (uint8_t)inl_n++;
}

void doc_para(const char *const *texts, const doc_style_t *st, unsigned parts, int gap) {
    for (unsigned p = 0; p < parts; p++) doc_text(texts[p], (unsigned)strlen(texts[p]), &st[p]);
    doc_flush(gap);
}

/* Горизонтальная полоса: прямоугольник от отступа до правого края рамки. */
void doc_rule(uint32_t rgb, int h, int gap) {
    doc_flush(0);
    if (mn) return;
    take_pending();
    page_rect(F->x + F->indent, F->y, F->w - F->indent, h, rgb);
    F->y += h;
    F->pending = gap;
}

/* Блок с фоном: заводим прямоугольник фона (и черту) нулевой высоты — высота ещё неизвестна —
 * и отступаем pad_y сверху. В doc_box_end высота станет известна и допишется. Блок в блоке не
 * открывается: внутренний просто становится частью внешнего. */
int doc_box_begin(uint32_t bg, int pad_x, int pad_y, uint32_t bar) {
    doc_flush(0);
    if (F->box_rect >= 0 || F->pad_x) return 0;
    F->pad_x = pad_x;
    F->pad_y = pad_y;
    F->box_bg = bg;
    if (mn) return 1;
    take_pending();
    F->box_y = F->y;
    F->box_rect = page_rect(F->x + F->indent, F->y, F->w - F->indent, 0, bg);
    F->box_bar = bar ? page_rect(F->x + F->indent, F->y, BAR_W, 0, bar) : -1;
    F->y += pad_y;
    return 1;
}

void doc_box_end(int gap) {
    doc_flush(0);
    if (!mn && F->box_rect >= 0) {
        F->y += F->pad_y;
        page_rect_height(F->box_rect, F->y - F->box_y);
        if (F->box_bar >= 0) page_rect_height(F->box_bar, F->y - F->box_y);
    }
    F->box_rect = F->box_bar = -1;
    F->pad_x = F->pad_y = 0;
    F->box_bg = 0;
    if (gap > F->pending) F->pending = gap;
}

void doc_skip_to(int y) {
    doc_flush(0);
    if (y > F->y) F->y = y;
    F->pending = 0;
}

void doc_frame_push(int x, int w, int y, uint32_t bg) {
    doc_flush(0);
    if (fn == MAX_FRAMES) return;
    uint32_t parent_bg = current_bg();
    fn++;
    memset(F, 0, sizeof(*F));
    F->x = x; F->w = w > 1 ? w : 1;
    F->y = F->top = y;
    F->bg = bg ? bg : parent_bg;
    F->box_rect = F->box_bar = -1;
}

int doc_frame_pop(void) {
    doc_flush(0);
    if (F->box_rect >= 0) doc_box_end(0);
    int bottom = F->y;
    if (fn > 1) fn--;
    return bottom;
}

void doc_measure_push(void) {
    doc_flush(0);
    if (mn == MAX_FRAMES) return;
    ms[mn].min = ms[mn].max = 0;
    mn++;
}

void doc_measure_pop(int *min_w, int *max_w) {
    doc_flush(0);
    if (!mn) { *min_w = *max_w = 0; return; }
    mn--;
    *min_w = ms[mn].min;
    *max_w = ms[mn].max;
}

void doc_measure_add(int min_w, int max_w) {
    if (!mn) return;
    int extra = F->indent + 2 * F->pad_x;
    if (min_w + extra > ms[mn - 1].min) ms[mn - 1].min = min_w + extra;
    if (max_w + extra > ms[mn - 1].max) ms[mn - 1].max = max_w + extra;
}
