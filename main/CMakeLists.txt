# libwebp — только разбор WebP (third_party/libwebp/README.md)
file(GLOB WEBP_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../third_party/libwebp/src/dec/*.c"
                       "${CMAKE_CURRENT_LIST_DIR}/../third_party/libwebp/src/dsp/*.c"
                       "${CMAKE_CURRENT_LIST_DIR}/../third_party/libwebp/src/utils/*.c")
idf_component_register(
    SRCS app.c ft.c font.c font_data.c page.c doc.c demo.c ui.c ui_data.c input.c
         url.c fetch.c download.c charset.c charset_data.c html.c form.c image.c gif.c
         ../third_party/miniz/miniz_tinfl.c ${WEBP_SOURCES}
    INCLUDE_DIRS "." "../include" "../design" "../third_party/miniz" "../third_party/libwebp")
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
