/* Создано tools/gen_ui.py — не править вручную.
 * Пакет картинок интерфейса (дизайн Codex) и план его загрузки в RAM_G по отрезкам. */
#pragma once
#include <stdint.h>

#define UI_ASSET_BYTES 18712u
#define UI_PLAN_RANGES 2

typedef struct {
    uint32_t offset;     /* откуда в пакете */
    uint32_t ram_g;      /* куда в RAM_G */
    uint32_t bytes;      /* сколько байтов */
} ui_range_t;

extern const uint8_t ui_assets[UI_ASSET_BYTES];
extern const ui_range_t ui_plan[UI_PLAN_RANGES];
