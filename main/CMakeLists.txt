idf_component_register(
    SRCS app.c ft.c font.c font_data.c page.c doc.c demo.c ui.c ui_data.c input.c
         url.c fetch.c download.c charset.c charset_data.c html.c
         ../third_party/miniz/miniz_tinfl.c
    INCLUDE_DIRS "." "../include" "../design" "../third_party/miniz")
# miniz — только распаковка (tinfl), без stdio, malloc и архивов
target_compile_definitions(${COMPONENT_LIB} PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_MALLOC
    MINIZ_NO_DEFLATE_APIS MINIZ_NO_ZLIB_APIS MINIZ_NO_ARCHIVE_APIS NDEBUG)
# -fno-ivopts: «адрес минус смещение» в .text у модуля ELF становится указателем шины кода
# (docs VDAC4, check_elf_pointers.py)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -O2 -fno-ivopts -fno-strict-aliasing -Wall -Wextra
    -Wno-unused-parameter -Wno-missing-field-initializers)
