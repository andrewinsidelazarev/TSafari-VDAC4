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
         main/input.c main/url.c main/fetch.c main/download.c main/charset.c main/charset_data.c main/html.c \
         third_party/miniz/miniz_tinfl.c tests/host.c; do
    $CL $FL -c "$f" -o "$O/$(basename "$f" .c).o"
done
$CL $FL -Dmain=tsafari_elf_main -c main/app.c -o "$O/app.o"
$CL "$O"/*.o -o "$O/host.exe"
"$O/host.exe" 'E:\zx\cache\tmp\tsafari\host\snap'
PYTHONIOENCODING=utf-8 E:/zx/esp/pio-venv/Scripts/python.exe tests/render_host.py 'E:\zx\cache\tmp\tsafari\host\snap'
