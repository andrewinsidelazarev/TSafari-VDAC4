/* Сохранение скачанного файла на карту Evo (что это — в download.h, как идёт обмен с
 * программой Z80 — в launcher/download.asm).
 *
 * ДВЕ ЗАДАЧИ. Событие программе Z80 шлёт и сроки проверяет задача браузера (download_tick),
 * а куски файла отдаёт задача обмена прошивки (download_command), когда программа Z80 их
 * просит. Общие переменные — под замком mutex (как в input.c). Освобождать память файла
 * задача обмена не берётся: она только ставит release, а освобождает задача браузера. */
#include <stdio.h>
#include <string.h>
#include "download.h"
#include "charset.h"
#include "font.h"
#include "ts.h"

#define PIECE 246                        /* данных в одном ответе F6: кадр к Z80 ≤ 256 байт */
#define START_MS 10000u                  /* программа Z80 не подтвердила событие — она старая */
/* Программа Z80 замолчала посреди файла. Срок большой: пока драйвер FAT32 пишет блок, Z80
 * ничего не шлёт, а на большой карте поиск свободного места по FAT идёт десятки секунд (на
 * плате 04.11 при 15 с — «Evo stopped answering», хотя запись шла). */
#define STALL_MS 120000u

static void *mutex;
static uint8_t *data;                    /* файл (PSRAM) */
static uint32_t size, sent;              /* размер; до какого места куски уже отданы */
static uint16_t seq;                     /* номер загрузки: ответ на прежнюю не спутать с новой */
static int state, cancel, event_due, release;
static int stalled;                      /* DL_FAILED из-за срока: поздний ответ Z80 ещё примем */
static uint32_t last;                    /* когда программа Z80 последний раз отозвалась */
static char card_name[DL_NAME_MAX + 1];  /* имя на карте, CP866 */
static download_info_t info;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

void download_init(void) {
    mutex = ts_core->mutex_create();
    state = DL_IDLE;
}

int download_busy(void) { return state == DL_WAIT || state == DL_SEND; }

/* Сменить состояние (под замком). Готово или ошибка — память файла больше не нужна. */
static void set_state(int s) {
    state = s;
    info.state = s;
    info.changed = ts_core->millis();
    if (s == DL_DONE || s == DL_FAILED) release = 1;
}

/* --- имя файла на карте ---------------------------------------------------------------- */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Подстрока key в s без учёта регистра латиницы; NULL — нет. */
static const char *find_ci(const char *s, const char *key) {
    size_t n = strlen(key);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && (s[i] | 32) == (key[i] | 32)) i++;
        if (i == n) return s;
    }
    return NULL;
}

/* Знак Unicode → байт CP866 — кодировки, в которой драйвер FAT32 пишет длинные имена. Русские
 * буквы в CP866 есть (А…п — 0x80…0xAF, р…я — 0xE0…0xEF, Ё и ё — 0xF0, 0xF1); остальных
 * нет — вместо них «_». */
static int to_cp866(uint32_t c) {
    if (c < 0x80) return (int)c;
    if (c >= 0x410 && c <= 0x43F) return (int)(c - 0x410 + 0x80);
    if (c >= 0x440 && c <= 0x44F) return (int)(c - 0x440 + 0xE0);
    if (c == 0x401) return 0xF0;
    if (c == 0x451) return 0xF1;
    return '_';
}

/* Срезать точки и пробелы в конце (драйвер срезал бы их сам, а поиск ищет имя как дано). */
static size_t trim(char *s, size_t n) {
    while (n && (s[n - 1] == '.' || s[n - 1] == ' ')) n--;
    s[n] = 0;
    return n;
}

/* Правила — как у ZiFi SPG (README, «Имя файла»):
 *   - имя — из Content-Disposition (filename*=UTF-8''… или filename=…), иначе последняя часть
 *     пути адреса (до «?» и «#»); «%D0%90» в адресе — байты UTF-8, их раскрываем;
 *   - знаки, которых нет в длинном имени FAT (управляющие и " * / : < > ? \ |), — «_»;
 *   - точки и пробелы в конце срезаются;
 *   - длиннее 48 — укорачивается основа, расширение до восьми букв остаётся;
 *   - окончание «.$$$» занято временными файлами — у имени точка перед ним становится «_»;
 *   - пустое имя — «download». */
void download_name(const char *url, const char *disp, char *out, size_t cap) {
    const char *src = "";
    size_t len = 0;
    const char *f;
    if (disp && (f = find_ci(disp, "filename*="))) {
        f += 10;
        const char *q = strstr(f, "''");             /* UTF-8''имя */
        if (q) f = q + 2;
        src = f;
        len = strcspn(f, "; ");
    } else if (disp && (f = find_ci(disp, "filename="))) {
        f += 9;
        if (*f == '"') { f++; len = strcspn(f, "\""); } else len = strcspn(f, "; ");
        src = f;
    } else {
        const char *p = strstr(url, "://");
        p = p ? p + 3 : url;
        size_t end = strcspn(p, "?#");
        for (size_t i = 0; i < end; i++)
            if (p[i] == '/') { src = p + i + 1; len = end - i - 1; }
    }
    char raw[256];
    size_t n = 0;
    for (size_t i = 0; i < len && n < sizeof(raw) - 1; i++) {
        if (src[i] == '%' && i + 2 < len && hexval(src[i + 1]) >= 0 && hexval(src[i + 2]) >= 0) {
            raw[n++] = (char)(hexval(src[i + 1]) * 16 + hexval(src[i + 2]));
            i += 2;
        } else {
            raw[n++] = src[i];
        }
    }
    char nm[256];
    size_t m = 0;
    const char *p = raw, *e = raw + n;
    while (p < e && m < sizeof(nm) - 1) {
        int b = to_cp866(utf8_next(&p, e));
        if (b < 0x20 || b == 0x7F || (b < 0x80 && strchr("\"*/:<>?\\|", b))) b = '_';
        nm[m++] = (char)b;
    }
    m = trim(nm, m);
    if (m > DL_NAME_MAX) {
        size_t dot = 0;                              /* точка расширения: 2…9 знаков с точкой */
        for (size_t k = m - 1; k + 9 >= m && k > 0; k--)
            if (nm[k] == '.') { dot = k; break; }
        if (dot) {
            size_t ext = m - dot, base = DL_NAME_MAX - ext;
            memmove(nm + base, nm + dot, ext);
            m = base + ext;
        } else {
            m = DL_NAME_MAX;
        }
        m = trim(nm, m);
    }
    if (m >= 4 && !memcmp(nm + m - 4, ".$$$", 4)) nm[m - 4] = '_';
    if (!m) m = (size_t)snprintf(nm, sizeof(nm), "download");
    nm[m] = 0;
    snprintf(out, cap, "%s", nm);
}

/* --- задача браузера -------------------------------------------------------------------- */

int download_start(uint8_t *body, size_t length, const char *url, const char *disposition) {
    uint8_t *old = NULL;
    ts_core->mutex_lock(mutex);
    int busy = download_busy();
    if (!busy) {
        old = data;                                  /* прошлый файл, ещё не освобождённый */
        release = 0;
        data = body;
        stalled = 0;
        size = (uint32_t)length;
        sent = 0;
        seq++;
        cancel = 0;
        event_due = 1;
        last = ts_core->millis();
        download_name(url, disposition, card_name, sizeof(card_name));
        memset(&info, 0, sizeof(info));
        info.name[charset_to_utf8(CS_CP866, (const uint8_t *)card_name, strlen(card_name), info.name)] = 0;
        info.size = size;
        set_state(DL_WAIT);
    }
    ts_core->mutex_unlock(mutex);
    if (old) ts_core->free(old);
    if (busy) ts_core->free(body);
    else ts_log("tsafari: download %s (%u bytes)", card_name, (unsigned)length);
    return !busy;
}

void download_note(const char *url, const char *disposition, const char *error, int cancelled) {
    char cp[DL_NAME_MAX + 1];
    download_name(url, disposition, cp, sizeof(cp));
    ts_core->mutex_lock(mutex);
    if (!download_busy()) {
        memset(&info, 0, sizeof(info));
        info.name[charset_to_utf8(CS_CP866, (const uint8_t *)cp, strlen(cp), info.name)] = 0;
        snprintf(info.error, sizeof(info.error), "%s", error);
        info.cancelled = cancelled;
        set_state(DL_FAILED);
        release = 0;
    }
    ts_core->mutex_unlock(mutex);
}

void download_cancel(void) {
    ts_core->mutex_lock(mutex);
    if (download_busy()) cancel = 1;
    ts_core->mutex_unlock(mutex);
}

/* Событие E7 [номер][размер][имя, 0] — не ждать места в очереди (wait 0): не ушло — уйдёт
 * на следующем проходе. Сроки: программа Z80 не начала брать куски за START_MS (старая
 * TSAFARI.SPG этого события не знает) или замолчала посреди файла на STALL_MS. */
void download_tick(uint32_t now) {
    uint8_t ev[6 + DL_NAME_MAX + 1];
    uint16_t len = 0;
    uint8_t *drop = NULL;
    ts_core->mutex_lock(mutex);
    if (event_due && state == DL_WAIT) {
        wr16(ev, seq);
        wr32(ev + 2, size);
        size_t k = strlen(card_name) + 1;
        memcpy(ev + 6, card_name, k);
        len = (uint16_t)(6 + k);
    }
    if (state == DL_WAIT && (uint32_t)(now - last) > START_MS) {
        snprintf(info.error, sizeof(info.error), "TSAFARI.SPG did not take the file (old version?)");
        set_state(DL_FAILED);
    } else if (state == DL_SEND && (uint32_t)(now - last) > STALL_MS) {
        snprintf(info.error, sizeof(info.error), "Evo stopped answering");
        set_state(DL_FAILED);
        stalled = 1;                             /* файл не освобождать: Z80 может ожить */
        release = 0;
    }
    if (release) { drop = data; data = NULL; release = 0; }
    ts_core->mutex_unlock(mutex);
    if (len && ts_core->event(0xE7, ev, len, 0) == ZM_OK) {
        ts_core->mutex_lock(mutex);
        event_due = 0;
        last = now;
        ts_core->mutex_unlock(mutex);
    }
    if (drop) ts_core->free(drop);
}

void download_info(download_info_t *out) {
    ts_core->mutex_lock(mutex);
    *out = info;
    out->sent = sent;
    ts_core->mutex_unlock(mutex);
}

/* --- задача обмена ----------------------------------------------------------------------- */

/* 76 [номер][смещение] → F6 [номер][1][смещение][число][данные] — кусок с этого места (не
 * дальше уже отданного: программа Z80 может переспросить кусок, если кадр не дошёл).
 * Чужой номер, отмена, смещение за концом — F6 [номер][0]… (отказ).
 * 77 [номер][итог][код][ГГГГ-ММ-ДД] → F7 [1] — чем кончилось. */
int download_command(uint8_t cmd, const uint8_t *p, uint16_t n) {
    if (cmd == 0x76) {
        uint8_t r[9 + PIECE];
        uint16_t len = 9;
        memset(r, 0, 9);
        if (n >= 2) memcpy(r, p, 2);
        ts_core->mutex_lock(mutex);
        uint32_t at = n == 6 ? rd32(p + 2) : 0;
        if (n == 6 && rd16(p) == seq && (download_busy() || stalled) && !cancel && data && at < size && at <= sent) {
            if (stalled) { stalled = 0; set_state(DL_SEND); info.error[0] = 0; }   /* ожила */
            uint32_t count = size - at;
            if (count > PIECE) count = PIECE;
            r[2] = 1;
            wr32(r + 3, at);
            wr16(r + 7, count);
            memcpy(r + 9, data + at, count);
            len = (uint16_t)(9 + count);
            if (state == DL_WAIT) set_state(DL_SEND);
            if (at + count > sent) sent = at + count;
            last = ts_core->millis();
        }
        ts_core->mutex_unlock(mutex);
        ts_core->reply(0xF6, r, len);
        return ZM_OK;
    }
    if (cmd == 0x77) {
        uint8_t ok = 0;
        ts_core->mutex_lock(mutex);
        if (n >= 4 && rd16(p) == seq && p[2] == 3 && state == DL_WAIT) {
            set_state(DL_SEND);                  /* «принято, начинаю»: дальше — долгий срок */
            last = ts_core->millis();
            ok = 1;
        } else if (n >= 4 && rd16(p) == seq && p[2] != 3 && (download_busy() || stalled)) {
            stalled = 0;
            char date[11] = "";
            if (n >= 14) { memcpy(date, p + 4, 10); date[10] = 0; }
            if (p[2] == 0) {
                snprintf(info.path, sizeof(info.path), "\\zifi\\downloads\\%s\\%s", date, info.name);
                set_state(DL_DONE);
            } else {
                /* коды драйвера FAT32: #10 и FILEX #22 — на карте нет места */
                if (p[2] == 1 && (p[3] == 0x10 || p[3] == 0x22)) snprintf(info.error, sizeof(info.error), "No space on Evo SD");
                else if (p[2] == 1) snprintf(info.error, sizeof(info.error), "SD write failed (code %02X)", p[3]);
                else if (cancel) { snprintf(info.error, sizeof(info.error), "Download cancelled"); info.cancelled = 1; }
                else snprintf(info.error, sizeof(info.error), "Transfer to Evo broke off");
                set_state(DL_FAILED);
            }
            ok = 1;
        }
        ts_core->mutex_unlock(mutex);
        ts_core->reply(0xF7, &ok, 1);
        if (ok && p[2] != 3) ts_log("tsafari: download %s: %s", card_name, p[2] ? "failed" : "saved");
        return ZM_OK;
    }
    return ZM_ERROR;
}
