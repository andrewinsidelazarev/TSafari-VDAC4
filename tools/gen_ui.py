#!/usr/bin/env python3
"""Ресурсы интерфейса TSafari в ELF → main/ui_data.c.

Что это. Картинки интерфейса (иконки кнопок, кадры глобуса, курсор Zuma, заголовок окна Wi-Fi,
метки языка ввода) рисует Codex; готовый пакет байтов в форматах FT812 лежит в
design/ft/ui_assets.bin. Браузер при старте копирует его в память видеочипа (RAM_G).

Пакет кладётся не одним куском: план загрузки (load_plan в design/ft/assets.json) говорит, какой
отрезок файла в какой адрес RAM_G писать — между отрезками в RAM_G лежат текстовые буферы, их
трогать нельзя. Здесь пакет превращается в массив C и таблицу этих отрезков.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BLOB = ROOT / "design" / "ft" / "ui_assets.bin"
MANIFEST = ROOT / "design" / "ft" / "assets.json"
HEADER = ROOT / "design" / "ui_design.h"


def main() -> int:
    data = BLOB.read_bytes()
    want = int(re.search(r"#define TSA_UI_STATIC_BYTES (\d+)u", HEADER.read_text(encoding="utf-8")).group(1))
    if len(data) != want:
        raise SystemExit(f"ui_assets.bin: {len(data)} bytes, ui_design.h says {want}")
    plan = json.loads(MANIFEST.read_text(encoding="utf-8"))["load_plan"]
    if sum(p["bytes"] for p in plan) != len(data):
        raise SystemExit("load_plan does not cover ui_assets.bin")
    for p in plan:                                   # FT812 пишет словами: адрес и длина кратны 4
        if p["ram_g"] % 4 or p["bytes"] % 4 or p["file_offset"] % 4:
            raise SystemExit(f"load_plan range not word aligned: {p}")
    lines = ["/* Создано tools/gen_ui.py из design/ft/ui_assets.bin — не править вручную. */",
             '#include "ui_data.h"', "", "const uint8_t ui_assets[UI_ASSET_BYTES] = {"]
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join(str(b) for b in data[i:i + 24]) + ",")
    lines += ["};", "", "/* Куда в RAM_G класть какой отрезок пакета (load_plan из design/ft/assets.json). */",
              "const ui_range_t ui_plan[UI_PLAN_RANGES] = {"]
    lines += [f"    {{{p['file_offset']}u, 0x{p['ram_g']:06X}u, {p['bytes']}u}}," for p in plan]
    lines += ["};", ""]
    (ROOT / "main" / "ui_data.c").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    (ROOT / "main" / "ui_data.h").write_text(
        "/* Создано tools/gen_ui.py — не править вручную.\n"
        " * Пакет картинок интерфейса (дизайн Codex) и план его загрузки в RAM_G по отрезкам. */\n"
        "#pragma once\n#include <stdint.h>\n\n"
        f"#define UI_ASSET_BYTES {len(data)}u\n#define UI_PLAN_RANGES {len(plan)}\n\n"
        "typedef struct {\n"
        "    uint32_t offset;     /* откуда в пакете */\n"
        "    uint32_t ram_g;      /* куда в RAM_G */\n"
        "    uint32_t bytes;      /* сколько байтов */\n"
        "} ui_range_t;\n\n"
        "extern const uint8_t ui_assets[UI_ASSET_BYTES];\nextern const ui_range_t ui_plan[UI_PLAN_RANGES];\n",
        encoding="utf-8", newline="\n")
    print(f"ui_assets.bin: {len(data)} bytes in {len(plan)} ranges")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
