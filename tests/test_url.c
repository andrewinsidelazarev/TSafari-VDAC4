/* Проверка main/url.c на ПК: clang tests/test_url.c main/url.c -Imain */
#include <stdio.h>
#include <string.h>
#include "url.h"

static int bad;
static void res(const char *base, const char *ref, const char *want) {
    char out[URL_MAX];
    int ok = url_resolve(base, ref, out, sizeof(out));
    const char *got = ok ? out : "(fail)";
    if (strcmp(got, want)) { printf("FAIL resolve %s + %s = %s, want %s\n", base, ref, got, want); bad++; }
}
static void in(const char *t, const char *want) {
    char out[URL_MAX];
    int ok = url_from_input(t, out, sizeof(out));
    const char *got = ok ? out : "(fail)";
    if (strcmp(got, want)) { printf("FAIL input %s = %s, want %s\n", t, got, want); bad++; }
}
int main(void) {
    const char *b = "http://a/b/c/d;p?q";
    /* RFC 3986, 5.4.1 (без фрагментов) */
    res(b, "g", "http://a/b/c/g"); res(b, "./g", "http://a/b/c/g"); res(b, "g/", "http://a/b/c/g/");
    res(b, "/g", "http://a/g"); res(b, "//g", "http://g"); res(b, "?y", "http://a/b/c/d;p?y");
    res(b, "g?y", "http://a/b/c/g?y"); res(b, "#s", "http://a/b/c/d;p?q"); res(b, "g#s", "http://a/b/c/g");
    res(b, ".", "http://a/b/c/"); res(b, "./", "http://a/b/c/"); res(b, "..", "http://a/b/"); res(b, "../", "http://a/b/");
    res(b, "../g", "http://a/b/g"); res(b, "../..", "http://a/"); res(b, "../../g", "http://a/g");
    res(b, "../../../g", "http://a/g"); res(b, "", "http://a/b/c/d;p?q");
    res("https://vtrd.in", "forum/?x=1", "https://vtrd.in/forum/?x=1");
    res("https://vtrd.in/", "/a/../b.html?z=../1", "https://vtrd.in/b.html?z=../1");
    res("https://x.org/p/q.html", "https://y.org/./a/../b", "https://y.org/b");
    res("https://x.org/p/q.html", "mailto:me@x.org", "(fail)");
    res("https://x.org/p/q.html", "javascript:void(0)", "(fail)");
    res("https://x.org", "?page=2", "https://x.org/?page=2");
    in("vtrd.in", "https://vtrd.in"); in("  zx-pk.ru/forum ", "https://zx-pk.ru/forum");
    in("http://old.example", "http://old.example"); in("tsafari:about", "tsafari:about");
    in("zx spectrum", "https://html.duckduckgo.com/html/?q=zx+spectrum"); in("ftp://x.org", "(fail)");
    printf(bad ? "%d failed\n" : "url: all ok\n", bad);
    return bad != 0;
}
