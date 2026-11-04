#!/usr/bin/env python3
"""Атлас шрифта TSafari для ESP: Noto Sans Mono из просмотрщика TXTVIEW2 (WC Improved).

Тот же набор, что у TXTVIEW2 (tools/gen_assets.py просмотрщика): диапазоны Unicode, составные
буквы с диакритикой и кириллица с ударением отдельными глифами, ячейка 12×34, базовая линия 26,
кегль 20 px. Отличие — раскладка: строки глифа идут сверху вниз (TXTVIEW2 хранит столбцы для
CMD_MEMCPY), потому что строки страницы собирает ESP, а не сопроцессор.

Выход — main/font_data.c: коды (по возрастанию), пары композиции, быстрая таблица U+0000…U+05FF,
точки глифов L4 (старшая тетрада — левая точка), сжатые zlib (распаковка — при старте ELF).
fontTools не нужен: таблица cmap разбирается здесь же.

Кроме основного кегля 20 px — ещё пять начертаний по дизайну Codex (design/DESIGN.md, «Шрифт и
стиль страницы»): h1, h2, h3, вторичный текст и поля форм, подписи. В них не все знаки, а только
частые (FACE_RANGES: латиница с расширением A, греческий, основная кириллица, пунктуация):
номера их глифов — те же, что у основного атласа (font_sub_glyphs), чего нет — рисуется основным.
"""
from __future__ import annotations

import hashlib
import json
import struct
import unicodedata as ud
import zlib
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
# тот же файл, что у TXTVIEW2 (WC Improved, plugins/txthex_viewer_VDAC2/fonts), — копия в репозитории
TTF = ROOT / "design/fonts/NotoSansMono-Regular.ttf"
CELL_W, CELL_H, BASELINE, SIZE = 12, 34, 26, 20
RANGES = [(0x20, 0x24f), (0x300, 0x52f), (0x1c80, 0x1c8f),
          (0x1e00, 0x1eff), (0x2000, 0x206f), (0x20a0, 0x20cf),
          (0x2100, 0x214f), (0x2190, 0x22ff), (0x2500, 0x25ff),
          (0x2c60, 0x2c7f), (0xa640, 0xa69f), (0xa720, 0xa7ff),
          (0xab30, 0xab6f)]
FAST_HIGH = 6                       # быстрая таблица: U+0000…U+05FF одним чтением
# Начертания сверх основного: (имя, кегль, ширина клетки = шаг, высота клетки, базовая линия)
FACES = [("H1", 32, 19, 55, 42), ("H2", 26, 16, 45, 34), ("H3", 22, 13, 38, 29),
         ("SMALL", 18, 11, 31, 23), ("CAPTION", 14, 8, 24, 18)]
FACE_RANGES = [(0x20, 0x17F), (0x391, 0x3C9), (0x400, 0x45F), (0x2010, 0x2027), (0x2030, 0x203A),
               (0x20AC, 0x20AC), (0x2116, 0x2116), (0x2122, 0x2122), (0x2190, 0x2193), (0x2022, 0x2022)]


def cmap_codes(path: Path) -> set[int]:
    """Коды из таблицы cmap TTF: подтаблицы формата 4 (BMP) и 12 (весь Unicode)."""
    data = path.read_bytes()
    num = struct.unpack_from(">H", data, 4)[0]
    cmap = next(struct.unpack_from(">4sIII", data, 12 + 16 * i)[2] for i in range(num)
                if struct.unpack_from(">4s", data, 12 + 16 * i)[0] == b"cmap")
    codes: set[int] = set()
    for i in range(struct.unpack_from(">H", data, cmap + 2)[0]):
        _, _, off = struct.unpack_from(">HHI", data, cmap + 4 + 8 * i)
        t = cmap + off
        fmt = struct.unpack_from(">H", data, t)[0]
        if fmt == 4:
            segs = struct.unpack_from(">H", data, t + 6)[0] // 2
            ends = struct.unpack_from(f">{segs}H", data, t + 14)
            starts = struct.unpack_from(f">{segs}H", data, t + 16 + 2 * segs)
            deltas = struct.unpack_from(f">{segs}h", data, t + 16 + 4 * segs)
            ro_at = t + 16 + 6 * segs
            ranges = struct.unpack_from(f">{segs}H", data, ro_at)
            for s in range(segs):
                for c in range(starts[s], ends[s] + 1):
                    if c == 0xFFFF:
                        continue
                    if ranges[s] == 0:
                        g = (c + deltas[s]) & 0xFFFF
                    else:
                        g = struct.unpack_from(">H", data, ro_at + 2 * s + ranges[s] + 2 * (c - starts[s]))[0]
                        g = (g + deltas[s]) & 0xFFFF if g else 0
                    if g:
                        codes.add(c)
        elif fmt == 12:
            groups = struct.unpack_from(">I", data, t + 12)[0]
            for k in range(groups):
                a, b, g = struct.unpack_from(">III", data, t + 16 + 12 * k)
                codes.update(c for c in range(a, b + 1) if g + (c - a))
    return codes


def to_l4_rows(tile: Image.Image) -> bytes:
    """Ячейка → L4 строками: (ширина + 1) / 2 байт на строку, левая точка — старшая тетрада;
    у нечётной ширины последняя тетрада строки пустая."""
    w, h = tile.size
    px = list(tile.get_flattened_data())
    out = bytearray()
    for y in range(h):
        row = [(v + 8) // 17 for v in px[y * w:(y + 1) * w]] + [0]
        out += bytes((row[i] << 4) | row[i + 1] for i in range(0, w, 2))
    return bytes(out)


def render(glyphs, size, cell_w, cell_h, baseline):
    """Глифы в клетки cell_w×cell_h с общей базовой линией; широкие вписываются по ширине.
    Возвращает (байты L4, сколько вписано)."""
    font = ImageFont.truetype(str(TTF), size)
    raw = bytearray()
    fitted = 0
    for text in glyphs:
        if len(text) == 1 and ud.combining(text):
            text = "\u25cc" + text              # одинокая диакритика — на пунктирном кружке
        tile = Image.new("L", (cell_w, cell_h))
        draw = ImageDraw.Draw(tile)
        box = draw.textbbox((0, baseline), text, font=font, anchor="ls")
        if box[0] < 0 or box[1] < 0 or box[2] > cell_w or box[3] > cell_h:
            # широкие диграфы и выносные знаки: рисуем целиком и вписываем по ширине
            fitted += 1
            wide = Image.new("L", (max(1, box[2] - box[0]), cell_h))
            ImageDraw.Draw(wide).text((-box[0], baseline), text, font=font, fill=255, anchor="ls")
            tile = wide.resize((cell_w, cell_h), Image.Resampling.LANCZOS)
        else:
            draw.text((0, baseline), text, font=font, fill=255, anchor="ls")
        raw += to_l4_rows(tile)
    return bytes(raw), fitted


def c_array(name: str, ctype: str, values, per_line: int, fmt: str) -> str:
    items = [fmt.format(v) for v in values]
    lines = [", ".join(items[i:i + per_line]) for i in range(0, len(items), per_line)]
    return f"const {ctype} {name}[{len(items)}] = {{\n    " + ",\n    ".join(lines) + "\n};\n"


def main() -> int:
    available = cmap_codes(TTF)
    codes = sorted({c for lo, hi in RANGES for c in range(lo, hi + 1)
                    if c in available and ud.category(chr(c)) not in ("Cc", "Cf", "Cn")} | {0xFFFD, 0x25CC})
    glyphs = [chr(c) for c in codes]
    index = {c: i for i, c in enumerate(codes)}
    pairs: dict[tuple[int, int], int] = {}
    for c in codes:
        dec = ud.decomposition(chr(c)).split()
        if len(dec) != 2 or dec[0].startswith("<"):
            continue
        first, mark = (int(v, 16) for v in dec)
        if first in index and 0x300 <= mark <= 0x36F and ud.normalize("NFC", chr(first) + chr(mark)) == chr(c):
            pairs[index[first], mark] = index[c]
    # кириллица с ударением (острым и тупым) — отдельные глифы, как в TXTVIEW2
    for c in codes:
        if 0x400 <= c <= 0x52F and ud.category(chr(c)).startswith("L"):
            for mark in (0x300, 0x301):
                if (index[c], mark) not in pairs:
                    pairs[index[c], mark] = len(glyphs)
                    glyphs.append(chr(c) + chr(mark))
    assert len(glyphs) <= 4096

    raw, fitted = render(glyphs, SIZE, CELL_W, CELL_H, BASELINE)
    packed = zlib.compress(bytes(raw), 9)
    # начертания сверх основного: частые знаки, номера глифов — основного атласа
    sub = [index[c] for c in codes if any(lo <= c <= hi for lo, hi in FACE_RANGES)]
    faces = []
    for name, size, w, h, base in FACES:
        fraw, _ = render([glyphs[g] for g in sub], size, w, h, base)
        faces.append((name, size, w, h, base, fraw, zlib.compress(fraw, 9)))
    replacement = index[0xFFFD]
    fast = [index.get(c, replacement) for c in range(FAST_HIGH * 256)]
    compose = [v for (base, mark), res in sorted(pairs.items()) for v in (base, mark, res)]

    out = ROOT / "main" / "font_data.c"
    src = [
        "/* Создано tools/gen_font.py — не править вручную.",
        f" * Noto Sans Mono Regular (SIL OFL 1.1), {SIZE} px, ячейка {CELL_W}×{CELL_H}, базовая линия {BASELINE}.",
        f" * Кодов {len(codes)}, глифов {len(glyphs)}, пар композиции {len(pairs)}; L4 {len(raw)} байт,",
        f" * сжато {len(packed)}. TTF SHA-256 {hashlib.sha256(TTF.read_bytes()).hexdigest()[:16]}…",
        " */",
        '#include "font.h"',
        "",
        c_array("font_codes", "uint16_t", codes, 12, "0x{:04X}"),
        c_array("font_compose", "uint16_t", compose, 12, "0x{:04X}"),
        c_array("font_fast", "uint16_t", fast, 12, "{}"),
        c_array("font_l4_zlib", "uint8_t", packed, 20, "{}"),
        c_array("font_sub_glyphs", "uint16_t", sub, 12, "{}"),
    ]
    for name, size, w, h, base, fraw, fpacked in faces:
        src.append(f"/* {name}: {size} px, клетка {w}×{h}, базовая линия {base}; L4 {len(fraw)} байт */")
        src.append(c_array(f"font_{name.lower()}_zlib", "uint8_t", fpacked, 20, "{}"))
    src.append("const font_face_t font_faces[FACE_COUNT] = {")
    src.append(f"    {{{CELL_W}, {CELL_H}, {BASELINE}, {(CELL_W + 1) // 2}, NULL, 0, 0}},")
    for name, size, w, h, base, fraw, fpacked in faces:
        src.append(f"    {{{w}, {h}, {base}, {(w + 1) // 2}, font_{name.lower()}_zlib, {len(fpacked)}, {len(fraw)}}},")
    src.append("};")
    out.write_text("\n".join(src), encoding="utf-8", newline="\n")
    hdr = ROOT / "main" / "font_gen.h"
    hdr.write_text(
        "/* Создано tools/gen_font.py — не править вручную. */\n"
        "#pragma once\n"
        f"#define FONT_CELL_W {CELL_W}\n#define FONT_CELL_H {CELL_H}\n#define FONT_BASELINE {BASELINE}\n"
        f"#define FONT_CODES {len(codes)}\n#define FONT_GLYPHS {len(glyphs)}\n#define FONT_COMPOSE {len(pairs)}\n"
        f"#define FONT_FAST_LIMIT 0x{FAST_HIGH * 256:X}\n#define FONT_REPLACEMENT {replacement}\n"
        f"#define FONT_DOTTED_CIRCLE {index[0x25CC]}\n#define FONT_L4_BYTES {len(raw)}\n"
        f"#define FONT_L4_ZLIB_BYTES {len(packed)}\n#define FONT_SUB_GLYPHS {len(sub)}\n",
        encoding="utf-8", newline="\n")
    info = {"codes": len(codes), "glyphs": len(glyphs), "compose": len(pairs), "fitted": fitted,
            "l4_bytes": len(raw), "zlib_bytes": len(packed), "sub_glyphs": len(sub),
            "faces": {f[0]: [len(f[5]), len(f[6])] for f in faces}}
    print(json.dumps(info))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
