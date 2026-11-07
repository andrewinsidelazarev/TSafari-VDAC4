/* Кэш картинок на карте Evo (что это — в imgcache.h, обмен с программой Z80 — в начале
 * launcher/imgcache.asm).
 *
 * ДВЕ ЗАДАЧИ, как у download.c: события и сроки — задача браузера (imgcache_tick), ответы на
 * команды программы Z80 — задача обмена прошивки (imgcache_command). Общие переменные операции
 * (op) — под замком mutex. Память освобождает только задача браузера.
 *
 * ОДНА ОПЕРАЦИЯ ЗА РАЗ. Программа Z80 делает одно дело: пока она читает или пишет файл, она ждёт
 * свои ответы, а чужие кадры пропускает — новое событие она бы потеряла. Поэтому операция кэша
 * одна (op), и событие загрузки E7 не уходит, пока она идёт (download.c), — и наоборот.
 *
 * УЧЁТ. Список файлов кэша (ents) — в PSRAM: свёртка адреса, размер и «время» (order). Из
 * списка с карты order — время файла FAT (дата << 16 | время), у новых — больше самого
 * большого. Кольцо: пока новый файл не влезает в предел, удаляется файл с самым малым order.
 *
 * ФАЙЛ КЭША: «TSIC», версия 1, вид (IMGCACHE_FILE — файл картинки, IMGCACHE_PIX — точки экрана со
 * своим заголовком от image.c), длина адреса LE16, длина содержимого LE32, адрес, содержимое.
 *
 * ЧТЕНИЕ ВАЖНЕЕ ЗАПИСИ. Картинка нужна сейчас, а запись — только следующим заходам: если кэш
 * занят записью, она прерывается (на следующий кусок 76 — ответ «хватит»; программа Z80 закроет
 * недописанный «.$$$», он удалится при следующей записи того же файла или при запуске), чтение
 * ждёт её конца (pend) и идёт первым, а прерванная запись встаёт в начало очереди. */
#include <stdio.h>
#include <string.h>
#include "imgcache.h"
#include "download.h"
#include "ts.h"
#ifdef TSAFARI_HOST
#include <stdlib.h>                      /* стенд ПК: getenv (журнал TSAFARI_IMGLOG) */
#endif

#define LIMIT_DEFAULT (100u * 1024u * 1024u)   /* 100 МБ — предел кэша (пользователь, 07.11) */
#define FILE_MAX    (8u * 1024u * 1024u)       /* больше — не кэшировать */
/* Копий в очереди на запись (вместе с той, что пишется) — не больше: PSRAM у браузера ~6 МБ, и
 * разбору большого WebP нужно много (стенд 07.11: с очередью до 4 МБ четыре WebP «no memory») */
#define QUEUE_MAX   (1536u * 1024u)
#define ENTRIES_MAX 20000u                     /* файлов в кэше — не больше (учёт — 16 байт на файл) */
#define JUNK_MAX    16                         /* недописанных «.$$$» за запуск удалить — столько */
#define PIECE       246                        /* кусок записи в ответе F6: кадр к Z80 ≤ 256 байт */
#define HEAD        12                         /* заголовок файла кэша без адреса */
#define URL_LONGEST 2000u
#define START_MS    10000u                     /* событие не принято — программа Z80 занята или старая */
#define STALL_MS    120000u                    /* замолчала посреди операции (большая карта — долго, download.c) */
#define TRIES       3                          /* столько раз послать событие, потом — кэша нет */

enum { OP_LIST = 1, OP_READ = 2, OP_WRITE = 3, OP_DELETE = 4 };
enum { ST_IDLE, ST_DUE, ST_SENT, ST_RUN, ST_OK, ST_FAILED };

/* Файл кэша: свёртка адреса (из неё — имя), размер, «время» для кольца (меньше — старше). */
typedef struct { uint8_t key[5]; uint32_t size, order; } entry_t;
/* Картинка в очереди на запись: заголовок с адресом (head) и файл картинки (body) — свой буфер
 * кэша, без копии, если картинка его не хранит (imgcache_give). len — весь файл кэша. */
typedef struct wjob {
    struct wjob *next;
    uint8_t key[5];
    int again;
    uint8_t *head, *body;
    uint32_t head_len, len;
} wjob_t;

static void wjob_free(wjob_t *w) {
    if (w->head) ts_core->free(w->head);
    if (w->body) ts_core->free(w->body);
    ts_core->free(w);
}

static void *mutex;
static entry_t *ents;
static unsigned ent_n, ent_cap;
static uint32_t total;                   /* сумма размеров файлов кэша */
static uint32_t top_order;               /* самое новое «время» */
static uint32_t limit = LIMIT_DEFAULT;
static int ready;                        /* список с карты получен */
static int off;                          /* кэша нет: программа Z80 его не знает или карта отказала */
static wjob_t *queue;                    /* очередь записи (только задача браузера) */
static uint32_t queued;
static char junk[JUNK_MAX][20];          /* недописанные файлы прошлых запусков — удалить */
static unsigned junk_n;
static uint16_t seq_last;
static unsigned reads, writes;
static char *read_url;                   /* адрес читаемой картинки — сверить с заголовком файла */

/* Текущая операция. */
static struct {
    int kind, st, tries, cancel;
    uint16_t seq;
    uint8_t key[5];
    char name[20];                       /* имя файла на карте */
    uint8_t *data;                       /* запись: файл картинки (после head); чтение: принятое */
    uint8_t *head;                       /* запись: заголовок с адресом — начало файла */
    uint32_t head_len;
    uint32_t size, at;                   /* размер файла; запись — до куда отдано, чтение — сколько принято */
    uint32_t last;                       /* когда программа Z80 отзывалась */
    uint32_t began;                      /* когда операция встала в очередь — скорость карты в журнал */
    uint32_t piece_at, gap;              /* чтение: когда пришёл прошлый кусок, самый долгий промежуток */
    unsigned pieces;                     /* чтение: сколько кусков пришло */
    uint8_t code;                        /* код отказа карты */
    uint32_t order;                      /* запись: «время» нового файла */
    wjob_t *job;                         /* запись: из очереди */
    int requeue;                         /* запись прервана ради чтения — снова в очередь */
} op;

/* Чтение, которое ждёт конца текущей операции (её прервали ради него). */
static struct {
    int on;
    uint8_t key[5];
    char name[20];
    uint32_t size;
    uint8_t *buf;
} pend;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

/* --- имена ---
 * Свёртка адреса — 40 бит: FNV-1a (32 бита) и ещё 8 бит от второй свёртки с другим началом.
 * Без 64-битной арифметики: её помощники из libgcc модулю ESP32 никто не даст. */
static void url_key(const char *url, uint8_t key[5]) {
    uint32_t h = 2166136261u, g = 0x9E3779B9u;
    for (const uint8_t *s = (const uint8_t *)url; *s; s++) {
        h = (h ^ *s) * 16777619u;
        g = (g ^ *s) * 0x01000193u + 0x7F4A7C15u;
    }
    wr32(key, h);
    key[4] = (uint8_t)(g >> 24 ^ g >> 8);
}

static const char B32[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";

/* Пять бит свёртки с бита b (0 — младший бит key[0]). */
static unsigned key_bits(const uint8_t key[5], unsigned b) {
    unsigned i = b / 8, w = key[i] | (i + 1 < 5 ? key[i + 1] << 8 : 0);
    return (w >> (b % 8)) & 31u;
}

/* Имя файла: 8 знаков свёртки (старшие биты — первыми) и «.IMG». */
static void key_name(const uint8_t key[5], char *out) {
    for (int j = 0; j < 8; j++) out[j] = B32[key_bits(key, (unsigned)(5 * (7 - j)))];
    memcpy(out + 8, ".IMG", 5);
}

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

/* «XXXXXXXX.IMG» (регистр не важен) → свёртка. 1 — имя наше. */
static int name_key(const char *s, size_t n, uint8_t key[5]) {
    if (n != 12 || s[8] != '.' || upper(s[9]) != 'I' || upper(s[10]) != 'M' || upper(s[11]) != 'G') return 0;
    memset(key, 0, 5);
    for (int j = 0; j < 8; j++) {
        const char *d = strchr(B32, upper(s[j]));
        if (!d || !*d) return 0;
        unsigned v = (unsigned)(d - B32), b = (unsigned)(5 * (7 - j));
        for (unsigned k = 0; k < 5; k++)
            if (v >> k & 1u) key[(b + k) / 8] |= (uint8_t)(1u << ((b + k) % 8));
    }
    return 1;
}

/* «XXXXXXXX.IMG.$$$» — недописанный файл (запись оборвалась): удалить. */
static int name_junk(const char *s, size_t n) {
    uint8_t k[5];
    return n == 16 && !memcmp(s + 12, ".$$$", 4) && name_key(s, 12, k);
}

/* --- учёт --- */

static int find_key(const uint8_t key[5]) {
    for (unsigned i = 0; i < ent_n; i++)
        if (!memcmp(ents[i].key, key, 5)) return (int)i;
    return -1;
}

static void drop_entry(int i) {
    total -= ents[i].size;
    ents[i] = ents[--ent_n];
}

/* Файл — в учёт (такой же — заменить). 0 — учёт полон или нет памяти. */
static int add_entry(const uint8_t key[5], uint32_t size, uint32_t order) {
    int i = find_key(key);
    if (i >= 0) drop_entry(i);
    if (ent_n == ent_cap) {
        if (ent_cap >= ENTRIES_MAX) return 0;
        unsigned cap = ent_cap ? ent_cap * 2 : 256;
        if (cap > ENTRIES_MAX) cap = ENTRIES_MAX;
        entry_t *e = ts_core->realloc(ents, cap * sizeof(entry_t));
        if (!e) return 0;
        ents = e;
        ent_cap = cap;
    }
    memcpy(ents[ent_n].key, key, 5);
    ents[ent_n].size = size;
    ents[ent_n].order = order;
    ent_n++;
    total += size;
    if (order > top_order) top_order = order;
    return 1;
}

/* Самый старый файл, кроме key (его как раз пишем заново). −1 — нет. */
static int oldest(const uint8_t key[5]) {
    int best = -1;
    for (unsigned i = 0; i < ent_n; i++)
        if (memcmp(ents[i].key, key, 5) && (best < 0 || ents[i].order < ents[best].order)) best = (int)i;
    return best;
}

/* --- операции --- */

/* Новая операция (под замком): событие уйдёт на ближайшем проходе imgcache_tick. */
static void op_begin(int kind, const char *name, uint32_t size) {
    seq_last = (uint16_t)((seq_last + 1) & 0x7FFFu);
    op.kind = kind;
    op.seq = (uint16_t)(IMGCACHE_SEQ | seq_last);
    snprintf(op.name, sizeof(op.name), "%s", name);
    op.size = size;
    op.at = 0;
    op.tries = 0;
    op.cancel = 0;
    op.code = 0;
    op.requeue = 0;
    op.pieces = 0;
    op.began = ts_core->millis();
    op.st = ST_DUE;
}

int imgcache_busy(void) {
    int st = __atomic_load_n(&op.st, __ATOMIC_ACQUIRE);
    return st == ST_SENT || st == ST_RUN;
}

void imgcache_init(void) {
    if (!mutex) mutex = ts_core->mutex_create();
    ready = off = 0;
    ent_n = 0;
    total = top_order = 0;
    junk_n = 0;
    ts_core->mutex_lock(mutex);
    op_begin(OP_LIST, "", 0);
    ts_core->mutex_unlock(mutex);
}

/* Операция кончилась (задача браузера, op.st — ST_OK или ST_FAILED): учесть итог. Чтение
 * сюда не попадает — его забирает imgcache_read_poll (кроме брошенного). */
static void op_finish(void) {
    int ok = op.st == ST_OK;
    if (op.kind == OP_LIST) {
        if (ok) {
            ready = 1;
            ts_log("tsafari: image cache: %u files, %u KB", ent_n, (unsigned)(total / 1024u));
        } else {
            off = 1;
            ts_log("tsafari: image cache: no Cache\\Images on Evo SD (code %02X)", op.code);
        }
    } else if (op.kind == OP_WRITE) {
        if (ok) {
            add_entry(op.key, op.size, op.order);
            writes++;
            /* скорость записи через программу Z80 на плате ещё не мерили — в журнал; только крупные:
             * журнал пишется во флеш, строка на каждый значок останавливала бы кэши ESP32 */
            if (op.size >= 65536u) ts_log("tsafari: image cache: %s written, %u KB in %u ms", op.name, (unsigned)(op.size / 1024u),
                   (unsigned)(ts_core->millis() - op.began));
        } else if (op.code == 0x10 || op.code == 0x22) {     /* на карте нет места (download.c) */
            off = 1;
            ts_log("tsafari: image cache: no space on Evo SD, cache off");
        }
        if (op.job && !ok && op.requeue && op.job->body) {   /* прервана ради чтения — в начало очереди */
#ifdef TSAFARI_HOST
            if (getenv("TSAFARI_IMGLOG")) printf("    CACHE write %s stopped at %u of %u for a read\n", op.name,
                                                 (unsigned)op.at, (unsigned)op.size);
#endif
            op.job->next = queue;
            queue = op.job;
            op.job = NULL;
        }
        if (op.job) {
            queued -= op.job->len;
            wjob_free(op.job);                                /* body NULL — отдан памяти (imgcache_trim) */
            op.job = NULL;
        }
        op.data = op.head = NULL;
    }
    if (op.data) ts_core->free(op.data);                      /* брошенное чтение */
    op.data = NULL;
    op.kind = 0;
    __atomic_store_n(&op.st, ST_IDLE, __ATOMIC_RELEASE);
}

/* Следующее дело, когда операций нет: недописанные файлы — прочь, потом запись из очереди (если
 * она не влезает в предел — сначала удаление самого старого). */
static void op_next(void) {
    char name[20];
    if (junk_n) {
        op_begin(OP_DELETE, junk[--junk_n], 0);
        return;
    }
    while (queue) {
        wjob_t *w = queue;
        int i = find_key(w->key);
        if (i >= 0 && !w->again) {                            /* уже в кэше — не писать */
            queue = w->next;
            queued -= w->len;
            wjob_free(w);
            continue;
        }
        if (w->len > limit) {                                 /* больше всего кэша — не писать */
            queue = w->next;
            queued -= w->len;
            wjob_free(w);
            continue;
        }
        uint32_t mine = i >= 0 ? ents[i].size : 0;            /* старый файл заменится новым */
        if (total - mine + w->len > limit || ent_n >= ENTRIES_MAX) {
            int k = oldest(w->key);
            if (k >= 0) {                                     /* кольцо: самый старый — прочь */
                key_name(ents[k].key, name);
                drop_entry(k);
                op_begin(OP_DELETE, name, 0);
                return;
            }
        }
        queue = w->next;
        key_name(w->key, name);
        op_begin(OP_WRITE, name, w->len);
        memcpy(op.key, w->key, 5);
        op.head = w->head;
        op.head_len = w->head_len;
        op.data = w->body;
        op.job = w;
        op.order = ++top_order;
        return;
    }
}

void imgcache_tick(uint32_t now) {
    if (!mutex) return;
    ts_core->mutex_lock(mutex);
    if (op.st == ST_SENT && (uint32_t)(now - op.last) > START_MS) {
        if (++op.tries >= TRIES) {                            /* программа Z80 кэша не знает */
            if (!off) ts_log("tsafari: image cache: TSAFARI.SPG does not answer (old version?), cache off");
            off = 1;
            op.st = ST_FAILED;
        } else {
            op.st = ST_DUE;                                   /* послать ещё раз */
        }
    } else if (op.st == ST_RUN && (uint32_t)(now - op.last) > STALL_MS) {
        op.st = ST_FAILED;
    }
    int done = (op.st == ST_OK || op.st == ST_FAILED) && (op.kind != OP_READ || op.cancel);
    if (done) op_finish();
    if (op.st == ST_IDLE && pend.on && !off) {                /* чтение ждало конца прежней операции */
        op_begin(OP_READ, pend.name, pend.size);
        memcpy(op.key, pend.key, 5);
        op.data = pend.buf;
        pend.buf = NULL;
        pend.on = 0;
    }
    if (op.st == ST_IDLE && ready && !off) op_next();
    else if (op.st == ST_IDLE && off) {
        while (queue) {                                       /* кэша нет — очередь не нужна */
            wjob_t *w = queue;
            queue = w->next;
            wjob_free(w);
        }
        queued = 0;
    }
    /* E8 [операция][номер LE16][размер LE32][имя, 0] — не во время загрузки (download.c) */
    uint8_t ev[8 + sizeof(op.name)];
    uint16_t len = 0;
    if (op.st == ST_DUE && !download_busy()) {
        ev[0] = (uint8_t)op.kind;
        wr16(ev + 1, op.seq);
        wr32(ev + 3, op.size);
        size_t k = strlen(op.name) + 1;
        memcpy(ev + 7, op.name, k);
        len = (uint16_t)(7 + k);
    }
    ts_core->mutex_unlock(mutex);
    if (len && ts_core->event(0xE8, ev, len, 0) == ZM_OK) {
        ts_core->mutex_lock(mutex);
        if (op.st == ST_DUE) {
            op.last = now;
            __atomic_store_n(&op.st, ST_SENT, __ATOMIC_RELEASE);
        }
        ts_core->mutex_unlock(mutex);
    }
}

/* --- чтение --- */

int imgcache_read_start(const char *url) {
    if (!mutex || !ready || off || pend.on || download_busy()) return 0;
    uint8_t key[5];
    url_key(url, key);
    int i = find_key(key);
    if (i < 0) return 0;
    size_t ul = strlen(url);
    uint8_t *buf = ts_alloc(ents[i].size ? ents[i].size : 1);
    char *u = ts_alloc(ul + 1);
    if (!buf || !u) {
        if (buf) ts_core->free(buf);
        if (u) ts_core->free(u);
        return 0;
    }
    memcpy(u, url, ul + 1);
    if (read_url) ts_core->free(read_url);
    read_url = u;
    char name[20];
    key_name(key, name);
    ts_core->mutex_lock(mutex);
    if (op.st == ST_DUE && op.kind == OP_WRITE && op.job) {  /* запись ещё не начата — назад в очередь */
        op.job->next = queue;
        queue = op.job;
        op.job = NULL;
        op.data = op.head = NULL;
        op.kind = 0;
        op.st = ST_IDLE;
    }
    if (op.st == ST_IDLE) {
        op_begin(OP_READ, name, ents[i].size);
        memcpy(op.key, key, 5);
        op.data = buf;
    } else {                                                  /* занят: чтение — следом, запись — прервать */
        pend.on = 1;
        memcpy(pend.key, key, 5);
        memcpy(pend.name, name, sizeof(pend.name));
        pend.size = ents[i].size;
        pend.buf = buf;
        if (op.kind == OP_WRITE && !op.cancel) {
            op.cancel = 1;                                    /* на следующий кусок 76 — «хватит» */
            op.requeue = 1;
        }
    }
    ts_core->mutex_unlock(mutex);
    return 1;
}

int imgcache_read_poll(uint8_t **body, size_t *len, int *kind) {
    if (!mutex) return -1;
    if (pend.on) {                                            /* ждёт конца прерванной записи */
        if (!off) return 0;
        ts_core->free(pend.buf);
        pend.buf = NULL;
        pend.on = 0;
        return -1;
    }
    ts_core->mutex_lock(mutex);
    if (op.kind != OP_READ || op.cancel) { ts_core->mutex_unlock(mutex); return -1; }
    if (op.st != ST_OK && op.st != ST_FAILED) { ts_core->mutex_unlock(mutex); return 0; }
    int ok = op.st == ST_OK && op.at == op.size;
    uint8_t *d = op.data;
    uint32_t n = op.size, ul = 0, bl = 0;
    uint8_t code = op.code;
    uint32_t took = ts_core->millis() - op.began;
    unsigned pieces = op.pieces, gap = (unsigned)op.gap;
    op.data = NULL;
    op.kind = 0;
    __atomic_store_n(&op.st, ST_IDLE, __ATOMIC_RELEASE);
    ts_core->mutex_unlock(mutex);
    if (ok && n >= HEAD && !memcmp(d, "TSIC", 4) && d[4] == 1 && d[5] <= IMGCACHE_PIX) {
        ul = rd16(d + 6);
        bl = rd32(d + 8);
    }
    if (ok && ul && HEAD + ul + bl == n && read_url && strlen(read_url) == ul && !memcmp(d + HEAD, read_url, ul)) {
        *kind = d[5];
        memmove(d, d + HEAD + ul, bl);
        *body = d;
        *len = bl;
        reads++;
        /* код доклада «сделано» — сколько раз Z80 ждал места в очереди ESP (imgcache.asm) */
        if (n >= 65536u)
            ts_log("tsafari: image cache: %s read, %u KB in %u ms (%u pieces, gap max %u ms, Z80 waits %u)", op.name,
                   (unsigned)(n / 1024u), (unsigned)took, pieces, gap, (unsigned)code);
        return 1;
    }
    /* Не вышло: файл — прочь из учёта (картинка придёт из сети и запишется заново, заменив его) */
    int i = find_key(op.key);
    if (i >= 0) drop_entry(i);
    if (!ok) ts_log("tsafari: image cache: %s not read (code %02X)", op.name, code);
    else ts_log("tsafari: image cache: %s is another image or damaged", op.name);
    if (d) ts_core->free(d);
    return -1;
}

void imgcache_read_cancel(void) {
    if (!mutex) return;
    if (pend.on) {                                            /* ещё не начато — просто забыть */
        ts_core->free(pend.buf);
        pend.buf = NULL;
        pend.on = 0;
        return;
    }
    ts_core->mutex_lock(mutex);
    uint8_t *drop = NULL;
    if (op.kind == OP_READ) {
        if (op.st == ST_DUE || op.st == ST_OK || op.st == ST_FAILED) {   /* программа Z80 не занята */
            drop = op.data;
            op.data = NULL;
            op.kind = 0;
            __atomic_store_n(&op.st, ST_IDLE, __ATOMIC_RELEASE);
        } else {
            op.cancel = 1;                    /* ответом F9 [0] — остановить; итог примет tick */
        }
    }
    ts_core->mutex_unlock(mutex);
    if (drop) ts_core->free(drop);
}

/* --- запись --- */

/* В очередь: kind, pre_len байт pre (в заголовок — сразу за адресом) и body (свой буфер — его кэш
 * освободит сам). 0 — не взят (body не тронут). */
static int queue_body(const char *url, int kind, const void *pre, size_t pre_len, uint8_t *body, size_t len,
                      int again) {
    if (!mutex || off || !len || len > FILE_MAX || pre_len > 64) return 0;
    size_t ul = strlen(url);
    if (!ul || ul > URL_LONGEST) return 0;
    uint32_t n = (uint32_t)(HEAD + ul + pre_len + len);
    if (queued + n > QUEUE_MAX) return 0;                     /* очередь полна — в другой раз */
    uint8_t key[5];
    url_key(url, key);
    if (ready && !again && find_key(key) >= 0) return 0;
    for (wjob_t *w = queue; w; w = w->next)
        if (!memcmp(w->key, key, 5)) return 0;
    wjob_t *w = ts_alloc(sizeof(wjob_t));
    uint8_t *h = ts_alloc(HEAD + ul + pre_len);
    if (!w || !h) {
        if (w) ts_core->free(w);
        if (h) ts_core->free(h);
        return 0;
    }
    memcpy(h, "TSIC", 4);
    h[4] = 1;
    h[5] = (uint8_t)kind;
    wr16(h + 6, (uint32_t)ul);
    wr32(h + 8, (uint32_t)(pre_len + len));
    memcpy(h + HEAD, url, ul);
    if (pre_len) memcpy(h + HEAD + ul, pre, pre_len);
    memset(w, 0, sizeof(*w));
    memcpy(w->key, key, 5);
    w->again = again;
    w->head = h;
    w->head_len = (uint32_t)(HEAD + ul + pre_len);
    w->body = body;
    w->len = n;
    wjob_t **tail = &queue;
    while (*tail) tail = &(*tail)->next;
    *tail = w;
    queued += n;
    return 1;
}

void imgcache_store(const char *url, int kind, const void *pre, size_t pre_len, const uint8_t *body, size_t len,
                    int again) {
    if (!mutex || off || !len || len > FILE_MAX || queued + len > QUEUE_MAX) return;
    uint8_t *copy = ts_alloc(len);
    if (!copy) return;
    memcpy(copy, body, len);
    if (!queue_body(url, kind, pre, pre_len, copy, len, again)) ts_core->free(copy);
}

void imgcache_give(const char *url, int kind, const void *pre, size_t pre_len, uint8_t *body, size_t len, int again) {
    if (!queue_body(url, kind, pre, pre_len, body, len, again) && body) ts_core->free(body);
}

int imgcache_trim(void) {
    int freed = 0;
    while (queue) {
        wjob_t *w = queue;
        queue = w->next;
        queued -= w->len;
        wjob_free(w);
        freed = 1;
    }
    if (!mutex) return freed;
    uint8_t *drop = NULL;
    ts_core->mutex_lock(mutex);
    if (op.kind == OP_WRITE && op.job && op.data) {
        drop = op.data;                       /* куски больше не отдаются: Z80 получит отказ */
        op.data = NULL;
        op.job->body = NULL;
        op.cancel = 1;
    }
    ts_core->mutex_unlock(mutex);
    if (drop) {
        ts_core->free(drop);
        freed = 1;
    }
    return freed;
}

/* --- задача обмена --- */

/* Записи списка [размер LE32][дата LE16][время LE16][признак][имя, 0]… — в учёт (под замком). */
static void list_part(const uint8_t *q, const uint8_t *end) {
    while (end - q >= 10) {
        const char *name = (const char *)q + 9;
        size_t k = 0;
        while (q + 9 + k < end && name[k]) k++;
        if (q + 9 + k >= end) break;                         /* нет конца имени */
        uint8_t key[5];
        if (name_key(name, k, key)) add_entry(key, rd32(q), (uint32_t)rd16(q + 4) << 16 | rd16(q + 6));
        else if (name_junk(name, k) && junk_n < JUNK_MAX) memcpy(junk[junk_n++], name, k + 1);
        q += 9 + k + 1;
    }
}

/* 77 [номер][итог][код][дата] — чем кончилось (итог 3 — «принято, начинаю») → F7 [1].
 * 76 [номер][смещение] — кусок записываемого файла → F6 (как у загрузок, download.c).
 * 78 [номер][записи…] — кусок списка → F8 [номер][1 дальше / 0 хватит].
 * 79 [номер][смещение LE32][данные] — кусок читаемого файла → F9 [номер][1 / 0]. */
int imgcache_command(uint8_t cmd, const uint8_t *p, uint16_t n) {
    uint16_t s = n >= 2 ? rd16(p) : 0;
    uint32_t now = ts_core->millis();
    if (!mutex) return ZM_ERROR;
    ts_core->mutex_lock(mutex);
    int mine = (op.st == ST_SENT || op.st == ST_RUN) && s == op.seq;
    if (cmd == 0x77) {
        uint8_t ok = 0;
        if (mine && n >= 4) {
            if (p[2] == 3) {
                op.st = ST_RUN;
            } else {
                op.code = p[3];
                __atomic_store_n(&op.st, p[2] == 0 ? ST_OK : ST_FAILED, __ATOMIC_RELEASE);
            }
            op.last = now;
            ok = 1;
        }
        ts_core->mutex_unlock(mutex);
        ts_core->reply(0xF7, &ok, 1);
        return ZM_OK;
    }
    if (cmd == 0x76) {
        uint8_t r[9 + PIECE];
        uint16_t len = 9;
        memset(r, 0, 9);
        wr16(r, s);
        uint32_t at = n == 6 ? rd32(p + 2) : 0;
        if (mine && n == 6 && op.kind == OP_WRITE && !op.cancel && op.data && op.head && at < op.size && at <= op.at) {
            uint32_t count = op.size - at;
            if (count > PIECE) count = PIECE;
            r[2] = 1;
            wr32(r + 3, at);
            wr16(r + 7, count);
            for (uint32_t i = 0; i < count; i++)             /* файл кэша = заголовок + картинка */
                r[9 + i] = at + i < op.head_len ? op.head[at + i] : op.data[at + i - op.head_len];
            len = (uint16_t)(9 + count);
            if (at + count > op.at) op.at = at + count;
            op.st = ST_RUN;
            op.last = now;
        }
        ts_core->mutex_unlock(mutex);
        ts_core->reply(0xF6, r, len);
        return ZM_OK;
    }
    if (cmd == 0x78 || cmd == 0x79) {
        uint8_t r[3];
        wr16(r, s);
        r[2] = 0;
        if (cmd == 0x78 && mine && op.kind == OP_LIST) {
            list_part(p + 2, p + n);
            r[2] = 1;
        } else if (cmd == 0x79 && mine && op.kind == OP_READ && !op.cancel && n >= 6 && op.data) {
            uint32_t at = rd32(p + 2), k = (uint32_t)n - 6u;
            if (at == op.at && at + k <= op.size) {
                memcpy(op.data + at, p + 6, k);
                op.at += k;
                r[2] = 1;
                /* замер для журнала: сколько кусков и самый долгий промежуток между ними */
                if (!op.pieces) op.gap = 0;
                else if (now - op.piece_at > op.gap) op.gap = now - op.piece_at;
                op.piece_at = now;
                op.pieces++;
            }
        }
        if (r[2]) {
            op.st = ST_RUN;
            op.last = now;
        }
        ts_core->mutex_unlock(mutex);
        ts_core->reply(cmd == 0x78 ? 0xF8 : 0xF9, r, 3);
        return ZM_OK;
    }
    ts_core->mutex_unlock(mutex);
    return ZM_ERROR;
}

#ifdef TSAFARI_HOST
void imgcache_host_stats(unsigned *files, uint32_t *bytes, unsigned *r, unsigned *w) {
    *files = ent_n;
    *bytes = total;
    *r = reads;
    *w = writes;
}
void imgcache_host_limit(uint32_t bytes) { limit = bytes; }
uint32_t imgcache_host_queued(void) { return queued; }
#endif
