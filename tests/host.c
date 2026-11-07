/* Стенд TSafari на ПК: код TSAFARI.ELF (main, все .c) с поддельным ядром ESP.
 *
 * FT812 — массив памяти (RAM_G, RAM_DL, регистры), смена листа проходит сразу. Время — шаги
 * сценария по 10 мс. Ввод — пакеты 75, как у программы Z80: мышь Kempston (кнопки, счётчики X/Y
 * по модулю 256, колесо в битах 7–4) и байты PS/2 набора 2. В точках сценария снимки: RAM_G
 * (2 МБ) и последний отправленный лист → каталог снимка; рисует их стенд ядра Slot1
 * (tests/render_host.py, fpga/vdac2/tb/vsys).
 *
 * Сборка и запуск — tests/run_host.ps1. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zm_app.h"
#include "download.h"
#include "imgcache.h"
#include "ft_copro.h"
#include <io.h>                          /* _findfirst — список файлов «карты» кэша картинок */
#include <time.h>

int tsafari_host_init(const zm_core_t *c);
int tsafari_host_step(uint32_t now);
int tsafari_host_command(uint8_t cmd, const uint8_t *p, uint16_t n);
int tsafari_host_state(void);
void download_name(const char *url, const char *disposition, char *out, size_t cap);

#define RAM_DL 0x300000u
#define REG_DLSWAP 0x302054u
#define REG_FRAMES 0x302004u

static uint8_t *ft;                     /* 4 МБ адресов FT812 */
static uint32_t dl_words, frames, now_ms;
static int wifi_up;
static const char *out_dir;

/* --- поддельное ядро ---
 * Память — с пределом, как PSRAM платы: приложению на ESP32-S3 после старта свободно ~5,8 МБ
 * (журнал 04.11), всего у него — ~6 МБ вместе с тем, что взято при старте. Без предела стенд
 * разбирал WebP 2560×2560, которому libwebp просит 39 МБ, а на плате он «Image unavailable». */
#define PSRAM_APP (6u * 1024u * 1024u)
uint32_t image_host_kept(void);                  /* image.c: точки и файлы картинок в PSRAM */
typedef struct { size_t n; size_t pad; } mem_t;
static size_t mem_used;
static void *c_alloc(size_t n, uint32_t caps) {
    (void)caps;
    if (!n) n = 1;
    if (mem_used + n > PSRAM_APP) {
        if (getenv("TSAFARI_MEMLOG")) {                  /* кто держит память, когда её не хватило */
            printf("    MEM no %u KB: used %u KB, image caches %u KB, card queue %u KB\n", (unsigned)(n / 1024u),
                   (unsigned)(mem_used / 1024u), image_host_kept() / 1024u, imgcache_host_queued() / 1024u);
        }
        return NULL;
    }
    mem_t *m = calloc(1, sizeof(mem_t) + n);
    if (!m) return NULL;
    m->n = n;
    mem_used += n;
    return m + 1;
}
static void c_free(void *p) {
    if (!p) return;
    mem_t *m = (mem_t *)p - 1;
    mem_used -= m->n;
    free(m);
}
static void *c_realloc(void *p, size_t n) {
    if (!p) return c_alloc(n, 0);
    mem_t *m = (mem_t *)p - 1;
    if (!n) n = 1;
    if (mem_used - m->n + n > PSRAM_APP) return NULL;
    mem_t *q = realloc(m, sizeof(mem_t) + n);
    if (!q) return NULL;
    mem_used = mem_used - q->n + n;
    q->n = n;
    return q + 1;
}
static void c_log(const char *f, ...) { va_list a; va_start(a, f); printf("  log: "); vprintf(f, a); printf("\n"); va_end(a); }
static uint32_t c_millis(void) { return now_ms; }
static void c_delay(uint32_t ms) { now_ms += ms; }
static void *c_mutex(void) { static int m; return &m; }
static void c_lock(void *m) { (void)m; }
/* Ответ на команду программы Z80 — запомнить: его читает поддельная программа Z80 загрузок.
 * Кадр к Z80 — не больше 256 байт (больше на плате теряется), событие — не больше 63. */
static uint8_t reply_buf[256], reply_code, ev_buf[64], ev8_buf[64];
static uint16_t reply_len;
static int ev_pending, ev8_pending;
static int c_reply(uint8_t r, const void *d, uint16_t n) {
    if (n > 256) { printf("reply %02X: %u bytes > 256\n", r, n); exit(3); }
    reply_code = r; reply_len = n; memcpy(reply_buf, d, n);
    return ZM_OK;
}
static int c_event(uint8_t code, const void *d, uint16_t n, uint32_t w) {
    (void)w;
    if (n > 63) { printf("event %02X: %u bytes > 63\n", code, n); exit(3); }
    if (code == 0xE7) { memcpy(ev_buf, d, n); ev_pending = 1; }
    if (code == 0xE8) { memcpy(ev8_buf, d, n); ev8_pending = 1; }   /* кэш картинок (imgcache.c) */
    return ZM_OK;
}
/* --- сопроцессор FT812: тот же код, что в прошивке BL616 (tests/copro, README там) ---
 * Шина — прямо в массив памяти видеочипа. Браузер записал REG_CMD_WRITE (или снял сброс) —
 * сопроцессор сразу выполняет всё, что есть в кольце RAM_CMD. */
static ftc_t copro;
static int copro_ready;
static uint32_t peek32(uint32_t a) { uint32_t v; memcpy(&v, ft + (a & 0x3FFFFFu), 4); return v; }
static void cp_rd(void *ctx, uint32_t a, uint32_t *w, uint32_t n) {
    (void)ctx;
    for (uint32_t i = 0; i < n; i++) w[i] = peek32(a + 4 * i);
}
static void cp_wr(void *ctx, uint32_t a, const uint32_t *w, uint32_t n) {
    (void)ctx;
    for (uint32_t i = 0; i < n; i++) memcpy(ft + ((a + 4 * i) & 0x3FFFFFu), &w[i], 4);
}
static void cp_status(void *ctx, ftc_status_t *st) {
    (void)ctx;
    st->cmd_write = peek32(FTC_REG_CMD_WRITE);
    st->cmd_read = peek32(FTC_REG_CMD_READ);
    st->cmd_dl = peek32(FTC_REG_CMD_DL);
    st->dlswap = 0;
    st->cpureset = peek32(FTC_REG_CPURESET);
    st->resets = 0;
}
static void cp_irq(void *ctx, uint32_t f) { (void)ctx; (void)f; }
static uint32_t cp_ms(void *ctx) { (void)ctx; return now_ms; }
static void copro_run(void) {
    if (!copro_ready) {
        static ftc_bus_t bus;
        bus.rd = cp_rd; bus.wr = cp_wr; bus.status = cp_status; bus.irq = cp_irq; bus.ms = cp_ms;
        ftc_init(&copro, &bus);
        copro_ready = 1;
    }
    for (int k = 0; k < 100000 && ftc_poll(&copro); k++) {}
}

static int c_ft_open(uint32_t t) { (void)t; return ZM_OK; }
static int c_ft_write(uint32_t a, const uint32_t *w, uint32_t n) {
    if (a % 4 || a + n * 4 > 0x400000) { printf("ft_write out of range %06X %u\n", a, n); return ZM_ERROR; }
    memcpy(ft + a, w, n * 4u);
    if (a == RAM_DL) dl_words = n;
    if (a == REG_DLSWAP) frames++;
    if ((a <= FTC_REG_CMD_WRITE && a + n * 4 > FTC_REG_CMD_WRITE) || a == FTC_REG_CPURESET) copro_run();
    return ZM_OK;
}
static int c_ft_read(uint32_t a, uint32_t *v) {
    *v = a == REG_FRAMES ? frames : a == REG_DLSWAP ? 0 : peek32(a);
    return ZM_OK;
}
/* LINK ядра FT812 (TSAFARI_RAMG3=1): FT_CFG/FT_STAT 0x5C — бит 0 записи → бит 27 чтения (RAM_G
 * 3 МБ, ft.c ft_ramg_3mb). Без ключа LINK нет — браузер остаётся в 2 МБ, снимки (ramg.bin — 2 МБ)
 * рисуются как прежде. */
static uint32_t ft_stat = 1u << 12;              /* хост FT812 — ESP */
static int c_link_read(uint32_t a, uint32_t *w, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) w[i] = a + 4 * i == 0x5C ? ft_stat : 0;
    return ZM_OK;
}
static int c_link_write(uint32_t a, const uint32_t *w, uint32_t n) {
    if (a == 0x5C && n) ft_stat = (ft_stat & ~(1u << 27)) | (w[0] & 1u) << 27;
    return ZM_OK;
}
static int c_wifi(char ssid[33], char ip[16], char mac[18]) {
    if (!wifi_up) return ZM_ERROR;
    strcpy(ssid, "ZX-Home"); strcpy(ip, "192.168.1.110"); strcpy(mac, "00:11:22:33:44:55");
    return ZM_OK;
}

/* --- поддельная сеть: сайт https://test.local/ — файлы tests/pages (как net_open прошивки:
 * поток только при ответе 2xx, иначе NULL и код в *status) --- */
typedef struct { uint8_t *data; size_t len, at; char ctype[64], clen[16], final[600]; } net_t;
static const char *pages_dir = "tests/pages";
static char last_get[600], last_post[2000];   /* что дошло до «сервера» последним */
static int moved_old, moved_new;              /* картинка страницы переадресации: просили с old/ и с new/ */
static int net_images;                        /* сколько раз картинку брали из сети (не .html) */
static void *c_net_open(const char *url, const char *extra, uint32_t t, int *status) {
    (void)extra; (void)t;
    const char *host = "https://test.local/";
    if (strncmp(url, host, strlen(host))) { *status = 0; return NULL; }      /* «нет соединения» */
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", pages_dir, url + strlen(host));
    snprintf(last_get, sizeof(last_get), "%s", url);
    if (!strncmp(url + strlen(host), "echo", 4)) {             /* ответ формы GET: что пришло */
        const char *qs = strchr(url, '?');
        net_t *s = calloc(1, sizeof(net_t));
        s->data = malloc(1200);
        s->len = (size_t)snprintf((char *)s->data, 1200, "<html><body><h1>Echo GET</h1><pre>%s</pre></body></html>", qs ? qs + 1 : "");
        strcpy(s->ctype, "text/html; charset=utf-8");
        snprintf(s->clen, sizeof(s->clen), "%u", (unsigned)s->len);
        *status = 200;
        return s;
    }
    /* Переадресация, как google.com → www.google.com: «old/…» сервер отправляет на «new/…», и
     * прошивка сама берёт ответ оттуда (адрес — в «:url»). Своих файлов у old/ нет: картинка,
     * спрошенная по набранному адресу, — 404. */
    char final[600];
    snprintf(final, sizeof(final), "%s", url);
    if (!strstr(url, ".htm")) net_images++;
    if (!strncmp(url + strlen(host), "old/", 4)) {
        if (strstr(url, ".png")) moved_old++;
        if (!strstr(url, ".html")) { *status = 404; return NULL; }
        snprintf(final, sizeof(final), "%snew/%s", host, url + strlen(host) + 4);
        snprintf(path, sizeof(path), "%s/new/%s", pages_dir, url + strlen(host) + 4);
    }
    if (!strncmp(url + strlen(host), "new/", 4) && strstr(url, ".png")) moved_new++;
    char *q = strchr(path, '?');
    if (q) *q = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { *status = 404; return NULL; }
    net_t *s = calloc(1, sizeof(net_t));
    fseek(f, 0, SEEK_END); s->len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    s->data = malloc(s->len + 1);
    s->len = fread(s->data, 1, s->len, f);
    fclose(f);
    strcpy(s->ctype, strstr(path, ".koi8") ? "text/html; charset=koi8-r" : strstr(path, ".txt") ? "text/plain" :
                     strstr(path, ".ZIP") || strstr(path, ".zip") ? "application/zip" :
                     strstr(path, ".nolen") ? "application/octet-stream" :
                     strstr(path, ".webp") ? "image/webp" : strstr(path, ".png") ? "image/png" :
                     strstr(path, ".jpg") || strstr(path, ".jpeg") ? "image/jpeg" : strstr(path, ".gif") ? "image/gif" :
                     strstr(path, ".svg") ? "image/svg+xml" : "text/html");
    /* «.nolen» — сервер не говорит длину (тогда загрузка кончается ответом «конец файла») */
    if (!strstr(path, ".nolen")) snprintf(s->clen, sizeof(s->clen), "%u", (unsigned)s->len);
    snprintf(s->final, sizeof(s->final), "%s", final);
    *status = 200;
    return s;
}
/* Как HttpStream::header прошивки: значение — в ОДНОМ общем буфере, следующий вызов затирает
 * прежнее (иначе стенд не поймает, что значение берут не сразу). Нет заголовка — NULL, буфер
 * не трогается. */
static const char *c_net_header(void *st, const char *name) {
    static char value[256];
    const char *v = !strcmp(name, "content-length") ? ((net_t *)st)->clen :
                    !strcmp(name, "content-type") ? ((net_t *)st)->ctype :
                    !strcmp(name, ":url") ? ((net_t *)st)->final : NULL;     /* прошивка 0.1.119+ */
    if (!v || !v[0]) return NULL;
    snprintf(value, sizeof(value), "%s", v);
    return value;
}
static int c_net_read(void *st, void *d, uint32_t n, uint32_t t) {
    (void)t;
    net_t *s = st;
    if (s->at >= s->len) return -1;
    if (n > 1000) n = 1000;                               /* кусками, как из сети */
    if (n > s->len - s->at) n = (uint32_t)(s->len - s->at);
    memcpy(d, s->data + s->at, n);
    s->at += n;
    return (int)n;
}
static void c_net_close(void *st) { net_t *s = st; free(s->data); free(s); }

/* --- поддельные сокеты: http://test.local:80 — POST /echo отвечает страницей с телом запроса
 * (кусками, как из сети), остальное — 404 --- */
static char sock_req[4096], sock_resp[4096];
static size_t sock_req_n, sock_resp_n, sock_resp_at;
static int sock_answered;
static int c_sock_open(int udp, const char *host, uint16_t port, uint32_t t) {
    (void)t;
    if (udp || strcmp(host, "test.local") || (port != 80 && port != 443)) return ZM_ERROR;
    sock_req_n = sock_resp_n = sock_resp_at = 0;
    sock_answered = 0;
    return 0;
}
static int c_sock_send(int s, const void *d, uint32_t n) {
    (void)s;
    if (sock_req_n + n >= sizeof(sock_req)) return ZM_ERROR;
    memcpy(sock_req + sock_req_n, d, n);
    sock_req_n += n;
    sock_req[sock_req_n] = 0;
    return ZM_OK;
}
static int c_sock_recv(int s, void *d, uint32_t n, uint32_t t) {
    (void)s; (void)t;
    if (!sock_answered) {                                    /* запрос целиком: заголовки и тело по длине */
        char *end = strstr(sock_req, "\r\n\r\n");
        const char *cl = strstr(sock_req, "Content-Length: ");
        if (!end || !cl || sock_req_n < (size_t)(end + 4 - sock_req) + (size_t)atoi(cl + 16)) return 0;
        sock_answered = 1;
        snprintf(last_post, sizeof(last_post), "%s", end + 4);
        if (!strncmp(sock_req, "POST /echo HTTP/1.0\r\nHost: test.local\r\n", 39))
            sock_resp_n = (size_t)snprintf(sock_resp, sizeof(sock_resp), "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                                           "\r\n<html><body><h1>Echo POST</h1><pre>%s</pre></body></html>", end + 4);
        else
            sock_resp_n = (size_t)snprintf(sock_resp, sizeof(sock_resp), "HTTP/1.1 404 Not Found\r\n\r\nno");
    }
    if (sock_resp_at >= sock_resp_n) return -1;
    if (n > 100) n = 100;
    if (n > sock_resp_n - sock_resp_at) n = (uint32_t)(sock_resp_n - sock_resp_at);
    memcpy(d, sock_resp + sock_resp_at, n);
    sock_resp_at += n;
    return (int)n;
}
static void c_sock_close(int s) { (void)s; }
/* sock_open_tls (ABI 1.12): тот же «сервер» на 443 — шифрование стенду не нужно */
static int c_sock_open_tls(const char *host, uint16_t port, uint32_t t) { return c_sock_open(0, host, port, t); }

/* --- ввод, как его шлёт программа Z80 --- */
static int mouse_x = 512, mouse_y = 384;           /* куда браузер поставил курсор (по нашим пакетам) */
static int goal_x = 512, goal_y = 384;
static uint8_t cnt_x, cnt_y, buttons, wheel;
static uint8_t keys[256];
static unsigned key_n;

static void key_bytes(const uint8_t *b, unsigned n) { while (n-- && key_n < sizeof(keys)) keys[key_n++] = *b++; }
static void key_press(uint8_t code, int ext) {
    uint8_t b[5]; unsigned n = 0;
    if (ext) b[n++] = 0xE0;
    b[n++] = code;
    if (ext) b[n++] = 0xE0;
    b[n++] = 0xF0; b[n++] = code;
    key_bytes(b, n);
}
static void type_text(const char *s) {
    static const char *const map = "a\x1C" "b\x32" "c\x21" "d\x23" "e\x24" "f\x2B" "g\x34" "h\x33" "i\x43" "j\x3B"
                                   "k\x42" "l\x4B" "m\x3A" "n\x31" "o\x44" "p\x4D" "q\x15" "r\x2D" "s\x1B" "t\x2C"
                                   "u\x3C" "v\x2A" "w\x1D" "x\x22" "y\x35" "z\x1A" ".\x49" "/\x4A" "-\x4E"
                                   "0\x45" "1\x16" "2\x1E" "3\x26" "4\x25" "5\x2E" "6\x36" "7\x3D" "8\x3E" "9\x46"
                                   " \x29";
    for (; *s; s++)
        for (const char *m = map; *m; m += 2)
            if (*m == *s) { key_press((uint8_t)m[1], 0); break; }
}

static void send_packet(void) {
    int dx = goal_x - mouse_x, dy = goal_y - mouse_y;   /* курсор: 2 точки на счёт мыши */
    int sx = dx / 2, sy = dy / 2;
    if (sx > 60) sx = 60;
    if (sx < -60) sx = -60;
    if (sy > 60) sy = 60;
    if (sy < -60) sy = -60;
    if (!sx && dx) sx = dx > 0 ? 1 : -1;
    if (!sy && dy) sy = dy > 0 ? 1 : -1;
    cnt_x = (uint8_t)(cnt_x + sx);
    cnt_y = (uint8_t)(cnt_y - sy);                    /* Y Kempston растёт вверх */
    mouse_x += sx * 2; mouse_y += sy * 2;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_x > 1023) mouse_x = 1023;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_y > 767) mouse_y = 767;
    uint8_t p[4 + 24];
    p[0] = 0;
    p[1] = (uint8_t)((wheel << 4) | (~buttons & 7));
    p[2] = cnt_x; p[3] = cnt_y;
    unsigned n = key_n > 24 ? 24 : key_n;
    memcpy(p + 4, keys, n);
    memmove(keys, keys + n, key_n - n);
    key_n -= n;
    tsafari_host_command(0x75, p, (uint16_t)(4 + n));
}

/* --- поддельная программа Z80 загрузок (как launcher/download.asm): событие E7 → куски
 * 76/F6 по 8 за шаг 10 мс (~200 КБ/с, как ожидается на плате) → итог 77. Файл — рядом со
 * снимками: <out>/../evo_<имя>. --- */
static struct { int on; uint16_t seq; uint32_t size, at; char name[64]; uint8_t *buf; } zdl;
static int dl_saved;
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void z80_download(void) {
    if (ev_pending) {
        ev_pending = 0;
        zdl.on = 1; zdl.seq = (uint16_t)(ev_buf[0] | ev_buf[1] << 8); zdl.size = rd32(ev_buf + 2); zdl.at = 0;
        snprintf(zdl.name, sizeof(zdl.name), "%s", (const char *)ev_buf + 6);
        zdl.buf = realloc(zdl.buf, zdl.size == 0xFFFFFFFFu ? 8u << 20 : zdl.size + 1);   /* длины нет — с запасом */
        printf("  z80: download %s, %u bytes\n", zdl.name, (unsigned)zdl.size);
        uint8_t ack[14] = {(uint8_t)zdl.seq, (uint8_t)(zdl.seq >> 8), 3, 0};   /* «принято, начинаю» */
        memcpy(ack + 4, "2026-11-04", 10);
        tsafari_host_command(0x77, ack, 14);
        return;                                       /* куски — со следующего шага */
    }
    if (!zdl.on) return;
    uint8_t q[14] = {(uint8_t)zdl.seq, (uint8_t)(zdl.seq >> 8)};
    for (int k = 0; k < 8 && zdl.at < zdl.size; k++) {
        q[2] = (uint8_t)zdl.at; q[3] = (uint8_t)(zdl.at >> 8); q[4] = (uint8_t)(zdl.at >> 16); q[5] = (uint8_t)(zdl.at >> 24);
        tsafari_host_command(0x76, q, 6);
        unsigned count = reply_len >= 9 ? (unsigned)(reply_buf[7] | reply_buf[8] << 8) : 0;
        if (reply_code == 0xF6 && reply_buf[2] == 2) return;            /* куска ещё нет — позже */
        if (reply_code == 0xF6 && reply_buf[2] == 3 && rd32(reply_buf + 3) == zdl.at) {   /* конец файла */
            zdl.size = zdl.at;
            break;
        }
        if (reply_code != 0xF6 || reply_buf[2] != 1 || rd32(reply_buf + 3) != zdl.at || reply_len != 9 + count || !count) {
            printf("  z80: piece refused at %u\n", (unsigned)zdl.at);
            q[2] = 2; q[3] = 0; memcpy(q + 4, "2026-11-04", 10);
            tsafari_host_command(0x77, q, 14);
            zdl.on = 0;
            return;
        }
        memcpy(zdl.buf + zdl.at, reply_buf + 9, count);
        zdl.at += count;
    }
    if (zdl.at < zdl.size) return;
    char path[600];
    snprintf(path, sizeof(path), "%s/../evo_%s", out_dir, zdl.name);
    FILE *o = fopen(path, "wb"); fwrite(zdl.buf, 1, zdl.size, o); fclose(o);
    q[2] = 0; q[3] = 0; memcpy(q + 4, "2026-11-04", 10);
    tsafari_host_command(0x77, q, 14);
    zdl.on = 0; dl_saved++;
}

/* --- поддельная программа Z80 кэша картинок (как launcher/imgcache.asm): событие E8 → список,
 * чтение, запись или удаление файла в папке card_dir (по умолчанию <out>/../card/Cache/Images).
 * За шаг 10 мс — 4 куска чтения по 1016 байт или 8 кусков записи по 246 (~400 и ~200 КБ/с). --- */
static struct { int on, op; uint16_t seq; uint32_t size, at; char name[24]; uint8_t *buf; } zc;
static char card_dir[600];
static uint16_t rd16h(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void zc_report(int result, int code) {
    uint8_t q[14] = {(uint8_t)zc.seq, (uint8_t)(zc.seq >> 8), (uint8_t)result, (uint8_t)code};
    memcpy(q + 4, "2026-11-07", 10);
    tsafari_host_command(0x77, q, 14);
    if (result != 3) zc.on = 0;
}
static void zc_list(void) {
    char pat[640];
    snprintf(pat, sizeof(pat), "%s/*", card_dir);
    uint8_t f[1024];
    unsigned fill = 2;
    f[0] = (uint8_t)zc.seq; f[1] = (uint8_t)(zc.seq >> 8);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    for (int more = h != -1; more; more = _findnext(h, &fd) == 0) {
        if (fd.attrib & _A_SUBDIR) continue;
        size_t k = strlen(fd.name);
        if (k > 47) continue;
        if (fill + 9 + k + 1 > 1000) {                /* кадр полон — отправить */
            tsafari_host_command(0x78, f, (uint16_t)fill);
            if (reply_code != 0xF8 || reply_buf[2] != 1) { _findclose(h); zc_report(2, 0); return; }
            fill = 2;
        }
        struct tm *t = localtime(&fd.time_write);
        uint16_t date = (uint16_t)((t->tm_year - 80) << 9 | (t->tm_mon + 1) << 5 | t->tm_mday);
        uint16_t time = (uint16_t)(t->tm_hour << 11 | t->tm_min << 5 | t->tm_sec / 2);
        uint32_t size = (uint32_t)fd.size;
        f[fill] = (uint8_t)size; f[fill + 1] = (uint8_t)(size >> 8); f[fill + 2] = (uint8_t)(size >> 16); f[fill + 3] = (uint8_t)(size >> 24);
        f[fill + 4] = (uint8_t)date; f[fill + 5] = (uint8_t)(date >> 8);
        f[fill + 6] = (uint8_t)time; f[fill + 7] = (uint8_t)(time >> 8);
        f[fill + 8] = 0;
        memcpy(f + fill + 9, fd.name, k + 1);
        fill += (unsigned)(9 + k + 1);
    }
    if (h != -1) _findclose(h);
    if (fill > 2) {
        tsafari_host_command(0x78, f, (uint16_t)fill);
        if (reply_code != 0xF8 || reply_buf[2] != 1) { zc_report(2, 0); return; }
    }
    zc_report(0, 0);
}
static void z80_cache(void) {
    char path[700];
    if (ev8_pending) {
        ev8_pending = 0;
        zc.on = 1; zc.op = ev8_buf[0]; zc.seq = rd16h(ev8_buf + 1); zc.size = rd32(ev8_buf + 3); zc.at = 0;
        snprintf(zc.name, sizeof(zc.name), "%s", (const char *)ev8_buf + 7);
        snprintf(path, sizeof(path), "%s/%s", card_dir, zc.name);
        zc_report(3, 0);                              /* «принято, начинаю» */
        if (zc.op == 1) {
            char cmd[700];
            snprintf(cmd, sizeof(cmd), "mkdir \"%s\" 2>nul", card_dir);
            for (char *c = cmd; *c; c++) if (*c == '/') *c = '\\';
            system(cmd);
            zc_list();
        } else if (zc.op == 2) {
            FILE *fi = fopen(path, "rb");
            if (!fi) { zc_report(1, 0xF1); return; }
            fseek(fi, 0, SEEK_END); zc.size = (uint32_t)ftell(fi); fseek(fi, 0, SEEK_SET);
            zc.buf = realloc(zc.buf, zc.size + 1);
            zc.size = (uint32_t)fread(zc.buf, 1, zc.size, fi);
            fclose(fi);
        } else if (zc.op == 3) {
            zc.buf = realloc(zc.buf, zc.size + 1);
        } else if (zc.op == 4) {
            zc_report(remove(path) ? 1 : 0, 0xF1);
        } else {
            zc.on = 0;
        }
        return;                                       /* куски — со следующего шага */
    }
    if (!zc.on) return;
    /* TSAFARI_SLOW: карта — как через Z80 на плате (оценка): чтение — кадр 1016 байт за шаг 10 мс
     * (~100 КБ/с), запись — кусок 246 байт за шаг (~24 КБ/с) */
    int slow = getenv("TSAFARI_SLOW") != NULL;
    if (zc.op == 2) {
        uint8_t f[6 + 1016];
        for (int k = 0; k < (slow ? 1 : 4) && zc.at < zc.size; k++) {
            uint32_t n = zc.size - zc.at > 1016 ? 1016 : zc.size - zc.at;
            f[0] = (uint8_t)zc.seq; f[1] = (uint8_t)(zc.seq >> 8);
            f[2] = (uint8_t)zc.at; f[3] = (uint8_t)(zc.at >> 8); f[4] = (uint8_t)(zc.at >> 16); f[5] = (uint8_t)(zc.at >> 24);
            memcpy(f + 6, zc.buf + zc.at, n);
            tsafari_host_command(0x79, f, (uint16_t)(6 + n));
            if (reply_code != 0xF9 || reply_buf[2] != 1) { zc_report(2, 0); return; }
            zc.at += n;
        }
        if (zc.at >= zc.size) zc_report(0, 0);
    } else if (zc.op == 3) {
        uint8_t q[6] = {(uint8_t)zc.seq, (uint8_t)(zc.seq >> 8)};
        for (int k = 0; k < (slow ? 1 : 8) && zc.at < zc.size; k++) {
            q[2] = (uint8_t)zc.at; q[3] = (uint8_t)(zc.at >> 8); q[4] = (uint8_t)(zc.at >> 16); q[5] = (uint8_t)(zc.at >> 24);
            tsafari_host_command(0x76, q, 6);
            unsigned count = reply_len >= 9 ? (unsigned)(reply_buf[7] | reply_buf[8] << 8) : 0;
            if (reply_code != 0xF6 || reply_buf[2] != 1 || rd32(reply_buf + 3) != zc.at || reply_len != 9 + count || !count) {
                zc_report(2, 0);
                return;
            }
            memcpy(zc.buf + zc.at, reply_buf + 9, count);
            zc.at += count;
        }
        if (zc.at < zc.size) return;
        snprintf(path, sizeof(path), "%s/%s", card_dir, zc.name);
        FILE *o = fopen(path, "wb");
        if (!o) { zc_report(1, 0xFF); return; }
        fwrite(zc.buf, 1, zc.size, o);
        fclose(o);
        zc_report(0, 0);
    }
}

/* Сводка кэша картинок — строкой в вывод стенда. */
static void cache_stats_line(const char *name) {
    unsigned files, r, w;
    uint32_t bytes;
    imgcache_host_stats(&files, &bytes, &r, &w);
    printf("%s: image cache %u files, %u KB, read from card %u, written %u, images from network %d\n", name, files,
           (unsigned)(bytes / 1024u), r, w, net_images);
}

/* Поток по кругу (download.c): кольцо 1000 байт, файл 5000 байт; сеть дописывает, сколько
 * влезает, «программа Z80» забирает куски командами 76 — файл байт в байт, ответ «позже»,
 * когда сеть не успела. На стенде задача загрузки не ждёт Z80 (один поток), поэтому в
 * сценарии кольцо — на весь файл, а запись по кругу проверяется здесь. */
static int stream_ring_test(void) {
    static uint8_t file[5000], out[5000];
    for (unsigned i = 0; i < sizeof(file); i++) file[i] = (uint8_t)(i * 7 + i / 251);
    download_stream_ring(1000);
    if (!download_stream_begin("https://x.ru/ring.bin", "", sizeof(file))) return 0;
    download_tick(now_ms);                            /* событие E7 — номер загрузки */
    ev_pending = 0;
    uint16_t seq = (uint16_t)(ev_buf[0] | ev_buf[1] << 8);
    uint32_t put = 0, got = 0;
    unsigned later = 0;
    while (got < sizeof(file)) {
        uint32_t room = download_stream_room();
        if (room > 700) room = 700;                   /* сеть — кусками по 700 */
        if (room > sizeof(file) - put) room = sizeof(file) - put;
        if (room && !download_stream_put(file + put, room, NULL)) return 0;
        put += room;
        if (put == sizeof(file) && room) download_stream_end(1, "");
        for (int k = 0; k < 4 && got < sizeof(file); k++) {   /* Z80 быстрее сети: будут и «позже» */
            uint8_t q[6] = {(uint8_t)seq, (uint8_t)(seq >> 8), (uint8_t)got, (uint8_t)(got >> 8), 0, 0};
            tsafari_host_command(0x76, q, 6);
            if (reply_code != 0xF6) return 0;
            if (reply_buf[2] == 2) { if (++later > 10000) return 0; break; }
            unsigned count = (unsigned)(reply_buf[7] | reply_buf[8] << 8);
            if (reply_buf[2] != 1 || rd32(reply_buf + 3) != got || !count) return 0;
            memcpy(out + got, reply_buf + 9, count);
            got += count;
        }
    }
    uint8_t done[14] = {(uint8_t)seq, (uint8_t)(seq >> 8), 0, 0};
    memcpy(done + 4, "2026-11-04", 10);
    tsafari_host_command(0x77, done, 14);
    download_tick(now_ms);                            /* кольцо — прочь */
    download_stream_ring(16u << 20);                  /* сценарий: кольцо — на весь файл */
    printf("stream ring: %u bytes through a 1000-byte ring, %u «later» answers, %s\n", (unsigned)got, later,
           memcmp(file, out, sizeof(file)) ? "DIFFERS" : "identical");
    return !memcmp(file, out, sizeof(file));
}

static int run(unsigned ms) {
    for (unsigned t = 0; t < ms; t += 10) {
        now_ms += 10;
        send_packet();
        if (tsafari_host_step(now_ms) < 0) {                  /* 3 — вышел по F12, так и надо */
            if (tsafari_host_state() != 3) printf("step failed at %u ms, state %d\n", (unsigned)now_ms, tsafari_host_state());
            return 0;
        }
        z80_download();
        z80_cache();
    }
    return 1;
}

/* Щелчок по элементу формы номер id: где он — спросить form.c; не виден — листать PgDn/PgUp. */
int form_host_rect(int id, int *x, int *y, int *w, int *h);
int page_top(void);
void image_host_stats(int top, int *seen, int *ready, int *failed, uint32_t *used);

/* Сводка картинок видимой части — строкой в вывод стенда. */
static void image_stats_line(const char *name) {
    int seen, ready, failed;
    uint32_t used;
    image_host_stats(page_top(), &seen, &ready, &failed, &used);
    printf("%s: top %d, images seen %d, ready %d, unavailable %d, RAM_G pool %u KB\n", name, page_top(),
           seen, ready, failed, (unsigned)(used / 1024u));
    if (getenv("TSAFARI_MEMLOG"))
        printf("    MEM %s: used %u KB, image caches %u KB, card queue %u KB\n", name, (unsigned)(mem_used / 1024u),
               image_host_kept() / 1024u, imgcache_host_queued() / 1024u);
}
static int click_control(int id) {
    int x, y, w, h, ok = 1;
    if (!form_host_rect(id, &x, &y, &w, &h)) { printf("control %d is not on the page\n", id); return 0; }
    for (int k = 0; k < 4 && (y - page_top() + h > 760 || y < page_top()); k++) {
        key_press(y < page_top() ? 0x7D : 0x7A, 1);
        ok &= run(900);
    }
    goal_x = x + w / 2; goal_y = y - page_top() + h / 2;
    ok &= run(700);
    buttons = 1; ok &= run(50); buttons = 0;
    ok &= run(300);
    return ok;
}

static void snapshot(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", out_dir, name);
#ifdef _WIN32
    char cmd[600]; snprintf(cmd, sizeof(cmd), "mkdir \"%s\" 2>nul", path); system(cmd);
#endif
    char f[600];
    snprintf(f, sizeof(f), "%s/ramg.bin", path);
    FILE *o = fopen(f, "wb"); fwrite(ft, 1, 0x200000, o); fclose(o);
    snprintf(f, sizeof(f), "%s/dl.bin", path);
    o = fopen(f, "wb"); fwrite(ft + RAM_DL, 4, dl_words, o); fclose(o);
    printf("snapshot %s: DL %u words, frames %u, cursor (%d,%d), %u ms\n", name, dl_words, frames, mouse_x, mouse_y,
           (unsigned)now_ms);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);                 /* вывод сразу: при зависании видно, где */
    out_dir = argc > 1 ? argv[1] : ".";
    ft = calloc(1, 0x400000);
    static zm_core_t core;
    core.size = sizeof(core); core.abi_major = ZM_APP_ABI_MAJOR; core.abi_minor = ZM_APP_ABI_MINOR;
    core.firmware = "host";
    core.alloc = c_alloc; core.realloc = c_realloc; core.free = c_free; core.log = c_log;
    core.millis = c_millis; core.delay = c_delay;
    core.mutex_create = c_mutex; core.mutex_lock = c_lock; core.mutex_unlock = c_lock;
    core.reply = c_reply; core.event = c_event;
    core.ft_open = c_ft_open; core.ft_write = c_ft_write; core.ft_read = c_ft_read;
    if (getenv("TSAFARI_RAMG3")) { core.link_read = c_link_read; core.link_write = c_link_write; }
    core.wifi_info = c_wifi;
    core.net_open = c_net_open; core.net_header = c_net_header; core.net_read = c_net_read; core.net_close = c_net_close;
    core.sock_open = c_sock_open; core.sock_send = c_sock_send; core.sock_recv = c_sock_recv; core.sock_close = c_sock_close;
    core.sock_open_tls = c_sock_open_tls;
    /* имена файлов на карте — правила ZiFi SPG (download.c) */
    static const char *const names[][3] = {
        {"https://vtrd.in/full_ver/ALTEREGO.ZIP", "", "ALTEREGO.ZIP"},
        {"https://x.ru/files/%D0%98%D0%B3%D1%80%D0%B0.trd?x=1", "", "\x88\xa3\xe0\xa0.trd"},
        {"https://x.ru/get.php?id=5", "attachment; filename=\"demo.scl\"", "demo.scl"},
        {"https://x.ru/get.php", "attachment; filename*=UTF-8''%D0%81%D0%B6.tap", "\xf0\xa6.tap"},
        {"https://x.ru/a:b*c.trd.$$$", "", "a_b_c.trd_$$$"},
        {"https://x.ru/", "", "download"},
        {"https://x.ru/dots...", "", "dots"},
        {"https://x.ru/0123456789012345678901234567890123456789012345678901234.tar.gz", "",
         "012345678901234567890123456789012345678901234.gz"},
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char got[64];
        download_name(names[i][0], names[i][1], got, sizeof(got));
        if (strcmp(got, names[i][2])) { printf("download_name %s: \"%s\", want \"%s\"\n", names[i][0], got, names[i][2]); return 4; }
    }
    printf("download names ok\n");
    now_ms = 1000;
    download_stream_ring(16u << 20);
    /* «карта» кэша картинок: TSAFARI_CARD или рядом со снимками; TSAFARI_CACHE_MAX — предел */
    if (getenv("TSAFARI_CARD")) snprintf(card_dir, sizeof(card_dir), "%s", getenv("TSAFARI_CARD"));
    else snprintf(card_dir, sizeof(card_dir), "%s/../card/Cache/Images", out_dir);
    if (getenv("TSAFARI_CACHE_MAX")) imgcache_host_limit((uint32_t)atol(getenv("TSAFARI_CACHE_MAX")));
    if (!tsafari_host_init(&core)) { printf("init failed, state %d\n", tsafari_host_state()); return 1; }
    if (!stream_ring_test()) { printf("stream ring test failed\n"); return 5; }
    int ok = 1;
    /* Одна страница из другой папки (настоящий сайт, сохранённый на диск): TSAFARI_PAGES — папка,
     * TSAFARI_URL — что набрать в строке адреса (test.local/имя), снимки — экран за экраном.
     * TSAFARI_SCREENS — сколько экранов листать (6); больше 6 — снимки только первых шести, потом
     * Home и снимок «top_again» (картинки, убранные из RAM_G, должны вернуться). На каждом экране —
     * сводка: сколько мест картинок видно, сколько готово и недоступно, сколько пула занято. */
    if (getenv("TSAFARI_PAGES") && getenv("TSAFARI_URL")) {
        int screens = getenv("TSAFARI_SCREENS") ? atoi(getenv("TSAFARI_SCREENS")) : 6;
        pages_dir = getenv("TSAFARI_PAGES");
        wifi_up = 1;
        ok &= run(400);
        key_press(0x1F, 1);
        ok &= run(300);
        type_text(getenv("TSAFARI_URL"));
        key_press(0x5A, 0);
        ok &= run(3000);
        goal_x = 900; goal_y = 700;
        ok &= run(500);  snapshot("p1");
        image_stats_line("p1");
        for (int k = 2; k <= screens; k++) {
            char name[8];
            key_press(0x7A, 1);                           /* PgDn */
            ok &= run(1200);
            snprintf(name, sizeof(name), "p%d", k);
            if (k <= 6) snapshot(name);
            image_stats_line(name);
        }
        /* TSAFARI_IDLE — столько мс ничего не трогать: картинки должны успокоиться (на плате 07.11
         * они грузились по кругу — «показал картинку, потом опять загружает»); журнал TSAFARI_IMGLOG
         * между «idle begin» и «idle end» должен быть пуст (после первых секунд догрузки) */
        if (getenv("TSAFARI_IDLE")) {
            puts("idle begin");
            ok &= run((unsigned)atoi(getenv("TSAFARI_IDLE")));
            puts("idle end");
            image_stats_line("idle");
        }
        /* TSAFARI_UPDOWN=n — n раз пролистать страницу вверх до начала и обратно вниз (на плате
         * 07.11: «картинки перезагружаются при скролле верх-вниз»); журнал TSAFARI_IMGLOG между
         * «updown begin» и «updown end» показывает, какие картинки раскладывались заново */
        if (getenv("TSAFARI_UPDOWN")) {
            int cycles = atoi(getenv("TSAFARI_UPDOWN"));
            puts("updown begin");
            for (int c = 0; c < cycles; c++) {
                for (int k = 1; k < screens; k++) {
                    char name[24];
                    key_press(0x7D, 1);                   /* PgUp */
                    ok &= run(1200);
                    snprintf(name, sizeof(name), "up%d_%d", c, k);
                    image_stats_line(name);
                }
                for (int k = 1; k < screens; k++) {
                    char name[24];
                    key_press(0x7A, 1);                   /* PgDn */
                    ok &= run(1200);
                    snprintf(name, sizeof(name), "down%d_%d", c, k);
                    image_stats_line(name);
                }
            }
            puts("updown end");
        }
        if (screens > 6) {
            key_press(0x6C, 1);                           /* Home — в начало */
            ok &= run(3000);
            snapshot("top_again");
            image_stats_line("top_again");
            for (int k = 2; k <= 3; k++) {                /* схема платы и т. п. — снова в RAM_G */
                char name[16];
                key_press(0x7A, 1);
                ok &= run(3000);
                snprintf(name, sizeof(name), "p%d_again", k);
                snapshot(name);
                image_stats_line(name);
            }
        }
        key_press(0x07, 0);
        run(50);
        cache_stats_line("cache");
        printf("single page: %s\n", ok ? "ok" : "failed");
        return ok ? 0 : 2;
    }
    ok &= run(300);   snapshot("01_wifi_connecting");
    ok &= run(21000); snapshot("02_wifi_failed");
    wifi_up = 1;
    ok &= run(400);   snapshot("03_page");
    goal_x = 2; goal_y = 2;
    ok &= run(700);   snapshot("04_nav_open");
    key_press(0x1F, 1);                               /* Win */
    ok &= run(300);
    type_text("zx.example.com/tsafari");
    ok &= run(300);   snapshot("05_address");
    key_press(0x76, 0);                               /* Esc */
    goal_x = 560; goal_y = 420;
    ok &= run(1500);
    wheel = (uint8_t)((wheel + 2) & 15);              /* два щелчка к себе (PS/2: +1 за щелчок) — вниз */
    ok &= run(500);   snapshot("06_scrolled");
    key_press(0x69, 1);                               /* End */
    ok &= run(1500);  snapshot("07_end");
    /* этап 1: сайт из файлов — адрес в строке, Enter */
    key_press(0x1F, 1);                               /* Win */
    ok &= run(300);
    type_text("test.local/index.html");
    key_press(0x5A, 0);                               /* Enter */
    ok &= run(600);   snapshot("08_index");
    key_press(0x7A, 1);                               /* PgDn */
    ok &= run(600);   snapshot("09_index_pgdn");
    key_press(0x1F, 1);
    ok &= run(300);
    type_text("test.local/cp1251.html");
    key_press(0x5A, 0);
    ok &= run(600);   snapshot("10_cp1251");
    key_press(0x1F, 1);
    ok &= run(300);
    type_text("test.local/koi.koi8");
    key_press(0x5A, 0);
    ok &= run(600);   snapshot("11_koi8");
    key_press(0x66, 0);                               /* Backspace — назад */
    ok &= run(600);   snapshot("12_back_cp1251");
    key_press(0x1F, 1);
    ok &= run(300);
    type_text("test.local/missing.html");
    key_press(0x5A, 0);
    ok &= run(600);   snapshot("13_error_404");
    /* цвета, размеры, CSS, таблицы со столбцами */
    key_press(0x1F, 1);
    ok &= run(300);
    type_text("test.local/styles.html");
    key_press(0x5A, 0);
    ok &= run(900);   snapshot("13b_styles");
    key_press(0x7A, 1);                               /* PgDn */
    ok &= run(900);   snapshot("13c_styles_pgdn");
    /* формы: поле (набор и Enter — GET), флажок, переключатель, список, многострочное поле,
     * кнопка (POST по HTTP через сокет), POST по HTTPS — ошибка с объяснением */
    key_press(0x1F, 1);
    ok &= run(300);
    type_text("test.local/forms.html");
    key_press(0x5A, 0);
    ok &= run(900);   snapshot("19a_forms");
    ok &= click_control(0);                           /* поле запроса: каретка в конце */
    type_text(" evo");
    ok &= run(300);   snapshot("19b_typed");
    key_press(0x5A, 0);                               /* Enter — отправить */
    ok &= run(900);   snapshot("19c_get_echo");
    if (strcmp(last_get, "https://test.local/echo?q=ZX+evo&lang=ru")) { printf("form GET: %s\n", last_get); ok = 0; }
    else printf("form GET ok\n");
    key_press(0x66, 0);                               /* назад к форме */
    ok &= run(900);
    ok &= click_control(6);                           /* флажок «Новости» */
    ok &= click_control(8);                           /* переключатель «Текст и картинки» */
    ok &= click_control(9);   snapshot("19d_select_open");
    key_press(0x72, 1);                               /* ↓ — Pentagon 128 */
    key_press(0x5A, 0);                               /* Enter — выбрать */
    ok &= run(300);
    ok &= click_control(10);                          /* многострочное поле: конец второй строки */
    type_text(" zx");
    ok &= run(300);   snapshot("19e_filled");
    ok &= click_control(11);                          /* «Отправить» — POST */
    ok &= run(900);   snapshot("19f_post_echo");
    if (strcmp(last_post, "name=&pw=secret&keep=on&news=yes&mode=img&machine=Pentagon+128&note=%D0%9F%D0%B5%D1%80%D0%B2%D0%B0%D1%8F+%D1%81%D1%82%D1%80%D0%BE%D0%BA%D0%B0%0D%0A%D0%92%D1%82%D0%BE%D1%80%D0%B0%D1%8F+%D1%81%D1%82%D1%80%D0%BE%D0%BA%D0%B0+zx&go=1")) { printf("form POST: %s\n", last_post); ok = 0; }
    else printf("form POST ok\n");
    key_press(0x66, 0);
    ok &= run(900);
    ok &= click_control(14);                          /* POST по HTTPS (sock_open_tls) */
    ok &= run(900);   snapshot("19g_https_post");
    if (strcmp(last_post, "x=1")) { printf("form POST https: %s\n", last_post); ok = 0; }
    else printf("form POST https ok\n");
    /* картинки: JPEG и PNG — сопроцессор (тот же код, что BL616), GIF — ESP; без размера в HTML —
     * страница раскладывается заново; недоступные — заглушки (дизайн Codex 08b) */
    key_press(0x1F, 1);
    ok &= run(300);
    type_text("test.local/images.html");
    key_press(0x5A, 0);
    ok &= run(400);   snapshot("20a_images_loading");
    ok &= run(4000);  snapshot("20b_images");
    key_press(0x7A, 1);                               /* PgDn */
    ok &= run(1200);  snapshot("20c_images_pgdn");
    /* переадресация (как google.com → www.google.com): картинка «logo.png» — с нового адреса,
     * в строке адреса — новый адрес */
    key_press(0x1F, 1);
    ok &= run(300);
    type_text("test.local/old/page.html");
    key_press(0x5A, 0);
    ok &= run(2500);  snapshot("21_redirect");
    if (moved_old || !moved_new) { printf("redirect base: old %d, new %d\n", moved_old, moved_new); ok = 0; }
    else printf("redirect base ok\n");
    /* фреймы, как на vtrd.in: меню сверху, под чертой — содержимое */
    key_press(0x1F, 1);
    ok &= run(300);
    type_text("test.local/frames.html");
    key_press(0x5A, 0);
    ok &= run(900);   snapshot("14_frames");
    goal_x = 140; goal_y = 25;                        /* «Игры» в меню (target="Down") */
    ok &= run(700);
    buttons = 1; ok &= run(50); buttons = 0;
    ok &= run(900);   snapshot("15_frame_games");
    key_press(0x66, 0);                               /* Backspace — назад: снова новости */
    ok &= run(900);   snapshot("16_frame_back");
    /* файл по ссылке меню (target="Down"): на карту Evo, страница остаётся */
    goal_x = 445; goal_y = 25;                        /* «ZIP» в меню */
    ok &= run(700);
    buttons = 1; ok &= run(50); buttons = 0;
    ok &= run(150);   snapshot("17_download");
    ok &= run(1500);  snapshot("18_download_done");
    /* файл без длины (сервер не сказал Content-Length): поток, конец — ответом «конец файла» */
    key_press(0x1F, 1);
    ok &= run(300);
    type_text("test.local/big.nolen");
    key_press(0x5A, 0);
    ok &= run(3000);  snapshot("18b_download_nolen");
    key_press(0x07, 0);                               /* F12 */
    run(50);
    {
        char a[600], b[600];
        snprintf(a, sizeof(a), "%s/big.nolen", pages_dir);
        snprintf(b, sizeof(b), "%s/../evo_big.nolen", out_dir);
        FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
        int same = fa && fb;
        while (same) { int x = fgetc(fa), y = fgetc(fb); if (x != y) same = 0; if (x == EOF) break; }
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        printf("download without length: file %s\n", same ? "identical" : "DIFFERS");
        if (!same) ok = 0;
    }
    /* сохранённый файл — байт в байт с тем, что раздал «сайт» */
    {
        char a[600], b[600];
        snprintf(a, sizeof(a), "%s/ALTEREGO.ZIP", pages_dir);
        snprintf(b, sizeof(b), "%s/../evo_ALTEREGO.ZIP", out_dir);
        FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
        int same = fa && fb;
        while (same) { int x = fgetc(fa), y = fgetc(fb); if (x != y) same = 0; if (x == EOF) break; }
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        printf("download: %d saved, file %s\n", dl_saved, same ? "identical" : "DIFFERS");
        if (!same || dl_saved != 2) ok = 0;
    }
    printf("state %d (3 = exit by F12), %s\n", tsafari_host_state(), ok ? "steps ok" : "step failed");
    return tsafari_host_state() == 3 && ok ? 0 : 2;  /* шаг не прошёл — и run_host.sh не пройдёт */
}
