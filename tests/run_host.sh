#!/bin/sh
# Стенд TSafari на ПК: собрать tests/host.c с кодом ELF (clang), пройти сценарий, нарисовать
# снимки на стенде ядра Slot1. Всё временное — в E:\zx\cache\tmp\tsafari\host.
set -e
cd "$(dirname "$0")/.."
O=/e/zx/cache/tmp/tsafari/host
mkdir -p "$O"
export TMP=E:/zx/cache/tmp TEMP=E:/zx/cache/tmp
CL=/e/zx/llvm/bin/clang
FL="-std=gnu11 -O1 -g -DTSAFARI_HOST -D_CRT_SECURE_NO_WARNINGS -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_MALLOC
    -DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_ZLIB_APIS -DMINIZ_NO_ARCHIVE_APIS -DNDEBUG -Imain -Iinclude -Idesign
    -Ithird_party/miniz -Wall -Wno-unused-function"
for f in main/ft.c main/font.c main/font_data.c main/page.c main/doc.c main/demo.c main/ui.c main/ui_data.c \
         main/input.c main/url.c main/fetch.c main/download.c main/charset.c main/charset_data.c main/html.c main/form.c main/image.c main/gif.c \
         main/svg.c main/imgcache.c tests/host.c; do
    $CL $FL -Itests/copro -Ithird_party/libwebp -Ithird_party/nanosvg -c "$f" -o "$O/$(basename "$f" .c).o"
done
# nanosvg (third_party/nanosvg/README.md): чужой код — без предупреждений
$CL $FL -Ithird_party/nanosvg -w -c main/svg_lib.c -o "$O/svg_lib.o"
# Сопроцессор FT812 — копия кода BL616 (tests/copro/README.md); распаковку tinfl он берёт из
# third_party/miniz/miniz_tinfl.c сам (включает его в ft_inflate.c), отдельного файла не нужно.
# WebP он разбирает своей libwebp (tests/copro/libwebp, с правками автора прошивки BL616).
rm -f "$O/miniz_tinfl.o"
CW=tests/copro/libwebp
CFL="-std=gnu11 -O1 -g -D_CRT_SECURE_NO_WARNINGS -DNDEBUG -DHAVE_CONFIG_H -DWEBP_REDUCE_SIZE -DWEBP_REDUCE_CSP -I. -Itests/copro -I$CW -w"
for f in tests/copro/ft_copro.c tests/copro/ft_image.c tests/copro/ft_inflate.c tests/copro/ft_3d.c; do
    $CL $CFL -c "$f" -o "$O/$(basename "$f" .c).o"
done
mkdir -p "$O/cpwebp"
rm -f "$O"/cpwebp/*.o
for s in $(cat "$CW/vdac4_sources.txt"); do
    $CL $CFL -c "$CW/$s" -o "$O/cpwebp/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
done
# У браузера своя libwebp с теми же именами функций: имена libwebp сопроцессора — с приставкой
# bl616_ (и в её файлах, и в ft_image.o, который их зовёт)
/e/zx/llvm/bin/llvm-nm --defined-only -g "$O"/cpwebp/*.o | awk 'NF == 3 { print $3 " bl616_" $3 }' | sort -u > "$O/cpwebp.map"
for o in "$O"/cpwebp/*.o "$O/ft_image.o"; do /e/zx/llvm/bin/llvm-objcopy --redefine-syms="$O/cpwebp.map" "$o"; done
# libwebp (третья сторона, third_party/libwebp/README.md): память — через функции браузера
for f in third_party/libwebp/src/dec/*.c third_party/libwebp/src/dsp/*.c third_party/libwebp/src/utils/*.c; do
    $CL -std=gnu11 -O1 -g -D_CRT_SECURE_NO_WARNINGS -DNDEBUG -DHAVE_CONFIG_H -DWEBP_SWAP_16BIT_CSP=1 -DWEBP_EXTERN=extern \
        -include main/webp_alloc.h -Ithird_party/libwebp -w -c "$f" -o "$O/webp_$(basename "$f" .c).o"
done
$CL $FL -Dmain=tsafari_elf_main -c main/app.c -o "$O/app.o"
$CL "$O"/*.o "$O"/cpwebp/*.o -o "$O/host.exe"
# «Карта» кэша картинок (host.c: snap/../card) — пустая: сценарий считает, что пришло по сети
# (переадресация, 21_redirect), а картинки из прошлого прогона пришли бы с «карты»
rm -rf "$O/card"
"$O/host.exe" 'E:\zx\cache\tmp\tsafari\host\snap'
PYTHONIOENCODING=utf-8 E:/zx/esp/pio-venv/Scripts/python.exe tests/render_host.py 'E:\zx\cache\tmp\tsafari\host\snap'
