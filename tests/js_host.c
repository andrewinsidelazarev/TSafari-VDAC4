/* Стенд JavaScript TSafari на ПК: страница из папки → скрипты (main/js.c, QuickJS) → итоговый HTML.
 *
 *   js_host.exe папка адрес [итог.html]
 * папка — сохранённый сайт (адрес https://test.local/путь → файл папка/путь, без «?…»); внешние
 * скрипты берутся оттуда же. Печатает журнал скриптов, сколько памяти взял движок, время.
 * Проверки страниц tests/pages/js*.html: строка «JS-CHECK имя ok|FAIL» в журнале (console.log). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "ts.h"
#include "js.h"
#include "dom.h"

extern uint32_t js_mem_limit;
static zm_core_t core;
static size_t mem_now, mem_peak;
typedef struct { size_t n, pad; } mh_t;
static void *c_alloc(size_t n, uint32_t caps) {
    (void)caps;
    mh_t *h = malloc(sizeof(mh_t) + n);
    if (!h) return NULL;
    h->n = n;
    mem_now += n;
    if (mem_now > mem_peak) mem_peak = mem_now;
    return h + 1;
}
static void c_free(void *p) { if (!p) return; mh_t *h = (mh_t *)p - 1; mem_now -= h->n; free(h); }
static void *c_realloc(void *p, size_t n) {
    if (!p) return c_alloc(n, 0);
    mh_t *h = (mh_t *)p - 1;
    void *q = c_alloc(n, 0);
    if (!q) return NULL;
    memcpy(q, p, h->n < n ? h->n : n);
    c_free(p);
    return q;
}
static uint32_t c_millis(void) { return (uint32_t)GetTickCount(); }
static void c_log(const char *f, ...) { (void)f; }

static char *load(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)l + 1);
    *n = fread(b, 1, (size_t)l, f);
    b[*n] = 0;
    fclose(f);
    return b;
}
/* https://сайт/путь?… → папка/путь */
static char *local(const char *dir, const char *url, size_t *n) {
    const char *p = strstr(url, "://");
    p = p ? strchr(p + 3, '/') : url;
    if (!p) return NULL;
    char path[1024];
    size_t k = (size_t)snprintf(path, sizeof(path), "%s", dir);
    for (; *p && *p != '?' && *p != '#' && k < sizeof(path) - 1; p++) path[k++] = *p;
    path[k] = 0;
    return load(path, n);
}

int main(int argc, char **argv) {
    if (argc < 3) { puts("js_host папка адрес [итог.html]"); return 2; }
    core.alloc = c_alloc; core.free = c_free; core.realloc = c_realloc; core.millis = c_millis; core.log = c_log;
    ts_core = &core;
    if (getenv("TSAFARI_JSMEM")) js_mem_limit = (uint32_t)atoi(getenv("TSAFARI_JSMEM")) * 1024u;
    size_t n;
    char *page = local(argv[1], argv[2], &n);
    if (!page) { printf("no page %s\n", argv[2]); return 1; }
    DWORD t0 = GetTickCount();
    int begun = js_begin(page, n, argv[2]);
    if (begun <= 0) { puts(begun < 0 ? "no memory for the page tree" : "no scripts"); return 0; }
    int ext = 0, got = 0;
    for (const char *u; (u = js_script_next()) != NULL;) {
        size_t sl = 0;
        char *s = local(argv[1], u, &sl);
        ext++;
        if (s) got++;
        else printf("  missing script %s\n", u);
        js_script_body(u, (const uint8_t *)s, s ? sl : 0);
        free(s);
    }
    int changed = js_run(getenv("TSAFARI_JSBUDGET") ? (uint32_t)atoi(getenv("TSAFARI_JSBUDGET")) : 20000);
    size_t heap, peak, dom;
    uint32_t ms;
    int ok, failed;
    js_stats(&heap, &peak, &dom, &ms, &ok, &failed);
    for (const char *l; (l = js_log_next()) != NULL;) printf("  %s\n", l);
    size_t rl = 0;
    char *res = js_result(&rl);
    printf("js: %d external (%d found), scripts ok %d failed %d, changed %d, run %u ms (total %lu ms), "
           "engine heap %zu KB (peak %zu KB), dom %zu KB, all memory peak %zu KB, result %zu B\n", ext, got, ok, failed,
           changed, ms, (unsigned long)(GetTickCount() - t0), heap / 1024, peak / 1024, dom / 1024, mem_peak / 1024, rl);
    if (argc > 3 && res) {
        FILE *f = fopen(argv[3], "wb");
        if (f) { fwrite(res, 1, rl, f); fclose(f); }
    }
    if (res) ts_core->free(res);
    js_end();
    printf("after js_end: memory still held %zu B\n", mem_now);
    free(page);
    return 0;
}
