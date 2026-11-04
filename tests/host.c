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

/* --- поддельное ядро --- */
static void *c_alloc(size_t n, uint32_t caps) { (void)caps; return calloc(1, n ? n : 1); }
static void *c_realloc(void *p, size_t n) { return realloc(p, n); }
static void c_free(void *p) { free(p); }
static void c_log(const char *f, ...) { va_list a; va_start(a, f); printf("  log: "); vprintf(f, a); printf("\n"); va_end(a); }
static uint32_t c_millis(void) { return now_ms; }
static void c_delay(uint32_t ms) { now_ms += ms; }
static void *c_mutex(void) { static int m; return &m; }
static void c_lock(void *m) { (void)m; }
/* Ответ на команду программы Z80 — запомнить: его читает поддельная программа Z80 загрузок.
 * Кадр к Z80 — не больше 256 байт (больше на плате теряется), событие — не больше 63. */
static uint8_t reply_buf[256], reply_code, ev_buf[64];
static uint16_t reply_len;
static int ev_pending;
static int c_reply(uint8_t r, const void *d, uint16_t n) {
    if (n > 256) { printf("reply %02X: %u bytes > 256\n", r, n); exit(3); }
    reply_code = r; reply_len = n; memcpy(reply_buf, d, n);
    return ZM_OK;
}
static int c_event(uint8_t code, const void *d, uint16_t n, uint32_t w) {
    (void)w;
    if (n > 63) { printf("event %02X: %u bytes > 63\n", code, n); exit(3); }
    if (code == 0xE7) { memcpy(ev_buf, d, n); ev_pending = 1; }
    return ZM_OK;
}
static int c_ft_open(uint32_t t) { (void)t; return ZM_OK; }
static int c_ft_write(uint32_t a, const uint32_t *w, uint32_t n) {
    if (a % 4 || a + n * 4 > 0x400000) { printf("ft_write out of range %06X %u\n", a, n); return ZM_ERROR; }
    memcpy(ft + a, w, n * 4u);
    if (a == RAM_DL) dl_words = n;
    if (a == REG_DLSWAP) frames++;
    return ZM_OK;
}
static int c_ft_read(uint32_t a, uint32_t *v) {
    *v = a == REG_FRAMES ? frames : a == REG_DLSWAP ? 0 : 0;
    return ZM_OK;
}
static int c_wifi(char ssid[33], char ip[16], char mac[18]) {
    if (!wifi_up) return ZM_ERROR;
    strcpy(ssid, "ZX-Home"); strcpy(ip, "192.168.1.110"); strcpy(mac, "00:11:22:33:44:55");
    return ZM_OK;
}

/* --- поддельная сеть: сайт https://test.local/ — файлы tests/pages (как net_open прошивки:
 * поток только при ответе 2xx, иначе NULL и код в *status) --- */
typedef struct { uint8_t *data; size_t len, at; char ctype[64], clen[16]; } net_t;
static const char *pages_dir = "tests/pages";
static void *c_net_open(const char *url, const char *extra, uint32_t t, int *status) {
    (void)extra; (void)t;
    const char *host = "https://test.local/";
    if (strncmp(url, host, strlen(host))) { *status = 0; return NULL; }      /* «нет соединения» */
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", pages_dir, url + strlen(host));
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
                     strstr(path, ".ZIP") || strstr(path, ".zip") ? "application/zip" : "text/html");
    snprintf(s->clen, sizeof(s->clen), "%u", (unsigned)s->len);
    *status = 200;
    return s;
}
/* Как HttpStream::header прошивки: значение — в ОДНОМ общем буфере, следующий вызов затирает
 * прежнее (иначе стенд не поймает, что значение берут не сразу). Нет заголовка — NULL, буфер
 * не трогается. */
static const char *c_net_header(void *st, const char *name) {
    static char value[256];
    const char *v = !strcmp(name, "content-length") ? ((net_t *)st)->clen :
                    !strcmp(name, "content-type") ? ((net_t *)st)->ctype : NULL;
    if (!v) return NULL;
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
                                   "0\x45" "1\x16" "2\x1E" "3\x26" "4\x25" "5\x2E" "6\x36" "7\x3D" "8\x3E" "9\x46";
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
        zdl.buf = realloc(zdl.buf, zdl.size + 1);
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

static int run(unsigned ms) {
    for (unsigned t = 0; t < ms; t += 10) {
        now_ms += 10;
        send_packet();
        if (tsafari_host_step(now_ms) < 0) return 0;
        z80_download();
    }
    return 1;
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
    printf("snapshot %s: DL %u words, frames %u, cursor (%d,%d)\n", name, dl_words, frames, mouse_x, mouse_y);
}

int main(int argc, char **argv) {
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
    core.wifi_info = c_wifi;
    core.net_open = c_net_open; core.net_header = c_net_header; core.net_read = c_net_read; core.net_close = c_net_close;
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
    if (!tsafari_host_init(&core)) { printf("init failed, state %d\n", tsafari_host_state()); return 1; }
    int ok = 1;
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
    key_press(0x07, 0);                               /* F12 */
    run(50);
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
        if (!same || dl_saved != 1) ok = 0;
    }
    printf("state %d (3 = exit by F12), %s\n", tsafari_host_state(), ok ? "steps ok" : "step failed");
    return tsafari_host_state() == 3 ? 0 : 2;
}
