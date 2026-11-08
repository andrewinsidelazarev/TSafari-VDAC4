# libwebp — только разбор WebP (third_party/libwebp/README.md)
file(GLOB WEBP_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../third_party/libwebp/src/dec/*.c"
                       "${CMAKE_CURRENT_LIST_DIR}/../third_party/libwebp/src/dsp/*.c"
                       "${CMAKE_CURRENT_LIST_DIR}/../third_party/libwebp/src/utils/*.c")
# QuickJS — движок JavaScript (third_party/quickjs/README.md)
set(QJS_SOURCES ../third_party/quickjs/quickjs.c ../third_party/quickjs/libregexp.c
                ../third_party/quickjs/libunicode.c ../third_party/quickjs/dtoa.c)
idf_component_register(
    SRCS app.c ft.c font.c font_data.c page.c doc.c demo.c ui.c ui_data.c input.c
         url.c fetch.c download.c imgcache.c charset.c charset_data.c css.c html.c form.c image.c gif.c ico.c svg.c svg_lib.c
         dom.c js.c js_sel.c start.c
         ../third_party/miniz/miniz_tinfl.c ${WEBP_SOURCES} ${QJS_SOURCES}
    INCLUDE_DIRS "." "../include" "../design" "../third_party/miniz" "../third_party/libwebp"
                 "../third_party/nanosvg" "../third_party/quickjs")
# QuickJS: без Atomics (потоков pthread и 64-битных атомарных операций у прошивки для модулей нет),
# без исходного текста функций (вдвое меньше памяти), интерпретатор на switch (таблица адресов меток
# в коде — то, что check_elf_pointers.py не отличит от ошибки «адрес минус смещение»); предупреждения
# чужого кода — не ошибки
set_source_files_properties(${QJS_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "__STDC_NO_ATOMICS__=1;JS_NO_FUNC_SOURCE;JS_NO_DIRECT_DISPATCH"
    COMPILE_OPTIONS "-w")
# nanosvg (third_party/nanosvg/README.md): чужой код в svg_lib.c — без предупреждений
set_source_files_properties(svg_lib.c PROPERTIES COMPILE_OPTIONS "-w")
# libwebp: без SSE/NEON (src/webp/config.h), 16-битные точки — младшим байтом вперёд, как у FT812;
# malloc/free — функции браузера (webp_alloc.h); предупреждения чужого кода — не ошибки
set_source_files_properties(${WEBP_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "HAVE_CONFIG_H;WEBP_SWAP_16BIT_CSP=1"
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/webp_alloc.h;-w")
# miniz — только распаковка (tinfl), без stdio, malloc и архивов.
# WEBP_EXTERN=extern: функции и переменные libwebp — не «видимые снаружи», а как все в модуле. Ссылку
# на видимую (переменную VP8GetCPUInfo) загрузчик ESP32 ищет по имени в таблицах прошивки, не находит —
# и модуль не грузится (на плате 04.11 «relocate failed (-88)»). Заодно компоновщик выбрасывает
# ненужное (кодировщик).
target_compile_definitions(${COMPONENT_LIB} PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_MALLOC
    MINIZ_NO_DEFLATE_APIS MINIZ_NO_ZLIB_APIS MINIZ_NO_ARCHIVE_APIS NDEBUG WEBP_EXTERN=extern)
# -fno-ivopts: «адрес минус смещение» в .text у модуля ELF становится указателем шины кода
# (docs VDAC4, check_elf_pointers.py)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -O2 -fno-ivopts -fno-strict-aliasing -Wall -Wextra
    -Wno-unused-parameter -Wno-missing-field-initializers)
