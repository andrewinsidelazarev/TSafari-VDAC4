/* Ввод: байты PS/2 и мышь Kempston из пакетов Z80 → нажатия и перемещения (устройство пакета
 * — в input.h).
 *
 * ДВЕ ЗАДАЧИ. На ESP32 одновременно идут несколько «задач» (потоков): пакеты принимает задача
 * обмена прошивки (на ядре процессора 1), а разбирает их задача браузера (на ядре 0). Они
 * работают в одно и то же время, и если обе полезут в одни данные сразу, данные испортятся.
 * Поэтому общие данные защищены «мьютексом» (замком): mutex_lock — взять замок (если он занят,
 * подождать), mutex_unlock — отдать. Пока замок у одной задачи, другая эти данные не трогает.
 *
 * КОЛЬЦЕВОЙ БУФЕР. Байты клавиатуры складываются в ring[256]: head — куда писать следующий,
 * tail — откуда читать. Индексы — байты (uint8_t), поэтому после 255 сами становятся 0, и
 * массив используется по кругу. head == tail — пусто; head + 1 == tail — полно (тогда байты
 * теряются, ставим lost, и модификаторы сбрасываются, чтобы Shift не «залип»).
 *
 * МЫШЬ. Перемещение — разность счётчиков (int8_t превращает разность 0…255 в −128…127: так
 * переход 255 → 0 даёт +1, а не −255). Пока мышь не «синхронизирована» (msync = 0) или
 * пакетов не было дольше 0,5 с, разность не считаем: иначе курсор прыгнул бы. Перемещения
 * копятся (acc_…), пока задача браузера их не заберёт. */
#include <string.h>
#include "input.h"
#include "ts.h"

/* общие данные двух задач — только под замком */
static void *mutex;
static uint8_t ring[256];                /* очередь байтов клавиатуры */
static uint8_t head, tail, lost;
static uint8_t mx, my, mwheel, mbuttons; /* прошлые значения счётчиков мыши и кнопки */
static int msync;                        /* 1 — прошлые значения есть, можно считать разность */
static int acc_dx, acc_dy, acc_wheel;    /* накопленные перемещения */
static uint32_t last_packet;             /* время прошлого пакета */

/* разбор (только задача браузера) */
static int e0, f0, skip;                 /* был префикс E0 / F0; сколько байтов Pause пропустить */
static uint8_t mods_l, mods_r;           /* зажатые модификаторы: левые и правые клавиши отдельно */
static int russian;                      /* 1 — русская раскладка */

void input_init(void) {
    mutex = ts_core->mutex_create();
    head = tail = lost = 0; msync = 0; acc_dx = acc_dy = acc_wheel = 0;
    e0 = f0 = skip = 0; mods_l = mods_r = 0; russian = 0;
}

/* FF FF FF в байтах мыши — мыши нет (или AVR её перезапускает). Y Kempston растёт вверх,
 * а на экране вниз — поэтому знак меняется. Колесо — 4-битный счётчик: разность 0…15
 * превращаем в −8…7 тем же приёмом, что и int8_t. AVR прибавляет к счётчику колеса 4-й байт
 * пакета мыши PS/2 (IntelliMouse) как есть, а там поворот от себя — отрицательное число
 * (так его читает и Linux, psmouse). У нас «от себя» — плюс, поэтому знак меняется. */
void input_packet(const uint8_t *p, unsigned n) {
    if (n < 4 || !mutex) return;
    ts_core->mutex_lock(mutex);
    uint32_t now = ts_core->millis();
    int stale = (uint32_t)(now - last_packet) > 500;
    last_packet = now;
    if (p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF) {
        msync = 0; mbuttons = 0;                    /* мыши нет или AVR её переинициализирует */
    } else {
        uint8_t wheel = p[1] >> 4;
        if (msync && !stale) {
            acc_dx += (int8_t)(uint8_t)(p[2] - mx);
            acc_dy -= (int8_t)(uint8_t)(p[3] - my);   /* Y Kempston растёт вверх */
            int w = (int)((wheel - mwheel) & 15);
            acc_wheel -= w >= 8 ? w - 16 : w;
        }
        mx = p[2]; my = p[3]; mwheel = wheel; msync = 1;
        mbuttons = (uint8_t)(~p[1] & 7);
    }
    for (unsigned i = 4; i < n; i++) {
        if ((uint8_t)(head + 1) == tail) { lost = 1; break; }
        ring[head++] = p[i];
    }
    ts_core->mutex_unlock(mutex);
}

/* Забрать накопленное и обнулить. Кнопки «отпускаются», если пакетов нет 0,5 с (программа Z80
 * остановилась) — иначе зажатая кнопка осталась бы зажатой навсегда. */
void input_mouse(mouse_state_t *m) {
    ts_core->mutex_lock(mutex);
    m->dx = acc_dx; m->dy = acc_dy; m->wheel = acc_wheel;
    m->buttons = (uint32_t)(ts_core->millis() - last_packet) > 500 ? 0 : mbuttons;
    acc_dx = acc_dy = acc_wheel = 0;
    ts_core->mutex_unlock(mutex);
}

int input_russian(void) { return russian; }

/* Таблицы раскладки: номер элемента = скан-код клавиши (набор 2 PS/2).
 * Запись [0x1C] = {'a', 'A'} задаёт элемент массива с номером 0x1C; остальные — нули
 * («клавиша букву не даёт»). Латиница: [0] — без Shift, [1] — с Shift. */
static const char latin[0x80][2] = {
    [0x1C] = {'a', 'A'}, [0x32] = {'b', 'B'}, [0x21] = {'c', 'C'}, [0x23] = {'d', 'D'}, [0x24] = {'e', 'E'},
    [0x2B] = {'f', 'F'}, [0x34] = {'g', 'G'}, [0x33] = {'h', 'H'}, [0x43] = {'i', 'I'}, [0x3B] = {'j', 'J'},
    [0x42] = {'k', 'K'}, [0x4B] = {'l', 'L'}, [0x3A] = {'m', 'M'}, [0x31] = {'n', 'N'}, [0x44] = {'o', 'O'},
    [0x4D] = {'p', 'P'}, [0x15] = {'q', 'Q'}, [0x2D] = {'r', 'R'}, [0x1B] = {'s', 'S'}, [0x2C] = {'t', 'T'},
    [0x3C] = {'u', 'U'}, [0x2A] = {'v', 'V'}, [0x1D] = {'w', 'W'}, [0x22] = {'x', 'X'}, [0x35] = {'y', 'Y'},
    [0x1A] = {'z', 'Z'}, [0x45] = {'0', ')'}, [0x16] = {'1', '!'}, [0x1E] = {'2', '@'}, [0x26] = {'3', '#'},
    [0x25] = {'4', '$'}, [0x2E] = {'5', '%'}, [0x36] = {'6', '^'}, [0x3D] = {'7', '&'}, [0x3E] = {'8', '*'},
    [0x46] = {'9', '('}, [0x0E] = {'`', '~'}, [0x4E] = {'-', '_'}, [0x55] = {'=', '+'}, [0x5D] = {'\\', '|'},
    [0x54] = {'[', '{'}, [0x5B] = {']', '}'}, [0x4C] = {';', ':'}, [0x52] = {'\'', '"'}, [0x41] = {',', '<'},
    [0x49] = {'.', '>'}, [0x4A] = {'/', '?'}, [0x29] = {' ', ' '},
};
/* Русская раскладка ЙЦУКЕН на тех же клавишах: строчная буква (заглавная — −0x20, ё/Ё — особо) */
static const uint16_t cyr[0x80] = {
    [0x15] = 0x439, [0x1D] = 0x446, [0x24] = 0x443, [0x2D] = 0x43A, [0x2C] = 0x435, [0x35] = 0x43D,
    [0x3C] = 0x433, [0x43] = 0x448, [0x44] = 0x449, [0x4D] = 0x437, [0x54] = 0x445, [0x5B] = 0x44A,
    [0x1C] = 0x444, [0x1B] = 0x44B, [0x23] = 0x432, [0x2B] = 0x430, [0x34] = 0x43F, [0x33] = 0x440,
    [0x3B] = 0x43E, [0x42] = 0x43B, [0x4B] = 0x434, [0x4C] = 0x436, [0x52] = 0x44D, [0x1A] = 0x44F,
    [0x22] = 0x447, [0x21] = 0x441, [0x2A] = 0x43C, [0x32] = 0x438, [0x31] = 0x442, [0x3A] = 0x44C,
    [0x41] = 0x431, [0x49] = 0x44E, [0x0E] = 0x451,
};
/* Управляющие клавиши без префикса E0 (F1–F12, Esc, Backspace, Tab, Enter). */
static const uint8_t fkeys[0x84] = {
    [0x05] = KEY_F1, [0x06] = KEY_F2, [0x04] = KEY_F3, [0x0C] = KEY_F4, [0x03] = KEY_F5, [0x0B] = KEY_F6,
    [0x83] = KEY_F7, [0x0A] = KEY_F8, [0x01] = KEY_F9, [0x09] = KEY_F10, [0x78] = KEY_F11, [0x07] = KEY_F12,
    [0x76] = KEY_ESC, [0x66] = KEY_BACKSPACE, [0x0D] = KEY_TAB, [0x5A] = KEY_ENTER,
};
/* Клавиши с префиксом E0: стрелки, PgUp/PgDn, Home/End, Insert/Delete, Win, Enter цифрового блока. */
static const uint8_t ekeys[0x80] = {
    [0x75] = KEY_UP, [0x72] = KEY_DOWN, [0x6B] = KEY_LEFT, [0x74] = KEY_RIGHT, [0x7D] = KEY_PGUP,
    [0x7A] = KEY_PGDN, [0x6C] = KEY_HOME, [0x69] = KEY_END, [0x70] = KEY_INSERT, [0x71] = KEY_DELETE,
    [0x1F] = KEY_WIN, [0x27] = KEY_WIN, [0x5A] = KEY_ENTER,
};

static uint8_t mods(void) { return mods_l | mods_r; }

/* Разобрать один байт клавиатуры: 1 и *ev — нажатие, 0 — байт ничего не дал (префикс,
 * отпускание, модификатор). Клавиша Pause присылает 8 байтов подряд — их пропускаем.
 * Левый Shift — код 12, правый — 59; правые Ctrl и Alt — те же коды с E0. Нажатие буквы:
 * в русской раскладке — кириллица (заглавная на 0x20 меньше строчной, у ё/Ё свои коды),
 * иначе латиница. С Ctrl или Alt кириллица не выдаётся — это сочетания клавиш. */
static int decode(uint8_t b, key_event_t *ev) {
    if (skip) { skip--; return 0; }
    if (b == 0xE1) { skip = 7; return 0; }               /* Pause: E1 14 77 E1 F0 14 F0 77 */
    if (b == 0xFF || b == 0x00) { mods_l = mods_r = 0; e0 = f0 = 0; return 0; }   /* переполнение */
    if (b == 0xE0) { e0 = 1; return 0; }
    if (b == 0xF0) { f0 = 1; return 0; }
    int ext = e0, up = f0;
    e0 = f0 = 0;
    uint8_t bit = 0;
    if (!ext && (b == 0x12 || b == 0x59)) bit = MOD_SHIFT;
    if (b == 0x14) bit = MOD_CTRL;
    if (b == 0x11) bit = MOD_ALT;
    if (bit) {
        uint8_t *m = (ext || b == 0x59) ? &mods_r : &mods_l;
        uint8_t before = mods();
        if (up) *m &= (uint8_t)~bit; else *m |= bit;
        /* Ctrl+Shift — переключить раскладку (как в WC): второй из пары нажат при первом */
        if (!up && (before & (MOD_CTRL | MOD_SHIFT)) && (mods() & (MOD_CTRL | MOD_SHIFT)) == (MOD_CTRL | MOD_SHIFT) &&
            (before & (MOD_CTRL | MOD_SHIFT)) != (MOD_CTRL | MOD_SHIFT))
            russian ^= 1;
        return 0;
    }
    if (up) return 0;
    memset(ev, 0, sizeof(*ev));
    ev->mods = mods();
    if (ext) {
        if (b < 0x80 && ekeys[b]) { ev->key = ekeys[b]; return 1; }
        return 0;
    }
    if (b < 0x84 && fkeys[b]) { ev->key = fkeys[b]; return 1; }
    if (b >= 0x80) return 0;
    int shift = (ev->mods & MOD_SHIFT) != 0;
    if (russian && cyr[b] && !(ev->mods & (MOD_CTRL | MOD_ALT))) {
        uint32_t c = cyr[b];
        if (shift) c = c == 0x451 ? 0x401 : c - 0x20;
        ev->key = KEY_CHAR; ev->ch = c;
        return 1;
    }
    if (russian && b == 0x4A && !shift) { ev->key = KEY_CHAR; ev->ch = '.'; return 1; }
    if (latin[b][0]) { ev->key = KEY_CHAR; ev->ch = (uint8_t)latin[b][shift]; return 1; }
    return 0;
}

/* Брать байты из очереди под замком, разбирать — без замка (чтобы не держать задачу обмена). */
int input_key(key_event_t *ev) {
    for (;;) {
        uint8_t b;
        ts_core->mutex_lock(mutex);
        if (lost) { lost = 0; ts_core->mutex_unlock(mutex); mods_l = mods_r = 0; continue; }
        if (tail == head) { ts_core->mutex_unlock(mutex); return 0; }
        b = ring[tail++];
        ts_core->mutex_unlock(mutex);
        if (decode(b, ev)) return 1;
    }
}
