#!/usr/bin/env python3
"""Пакет шрифтов TSafari для письменностей вне основного атласа → dist/TSAFARI/TSAFARI.FNT.

Основа (латиница, кириллица, греческий — Noto Sans Mono из TXTVIEW2) лежит в ELF
(tools/gen_font.py). Здесь — остальные основные письменности мира из шрифтов Noto (SIL OFL 1.1,
fonts/noto): CJK, арабская, иврит, тайская, индийские, армянская, грузинская, эфиопская и др.

Строка — как у основы: высота 34 точки, базовая линия 26, кегль 20. Ширина шага — своя у
каждого знака, из шрифта (арабские буквы смыкаются, армянские и грузинские не растянуты);
моноширинная сетка остаётся только у основы. Комбинируемый знак (Mn, Me) шага не имеет и ложится
на предыдущую букву по своему сдвигу из шрифта. У глифа хранится только прямоугольник с точками.

Формат (числа — little endian):
  заголовок  'TSFN', u16 версия = 2, u16 страниц, u32 глифов
  таблица    на страницу: u16 номер (код >> 8), u16 глифов, u32 смещение, u32 сжато, u32 без сжатия
  страница   zlib: 256 × {u8 шаг, i8 сдвиг точек по x от пера, i8 верх точек от верха строки,
             u8 ширина точек (чётная), u8 строк, u24 смещение точек от начала страницы}
             + точки L4 (строка — ширина / 2 байт, левая точка — старшая тетрада).
             Знака нет — все 8 байт нулевые.
"""
from __future__ import annotations

import json
import struct
import sys
import unicodedata as ud
import zlib
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from gen_font import RANGES as BASE_RANGES, TTF as BASE_TTF, cmap_codes  # noqa: E402

NOTO = ROOT / "fonts" / "noto"
CELL_H, BASELINE, SIZE = 34, 26, 20

SCRIPTS = [
    ("NotoSansCJKsc-Regular.otf", [(0x1100, 0x11FF), (0x2E80, 0x2FDF), (0x3000, 0x33FF), (0x4E00, 0x9FFF),
                                   (0xA960, 0xA97F), (0xAC00, 0xD7FF), (0xF900, 0xFAFF), (0xFE30, 0xFE4F),
                                   (0xFF00, 0xFFEF)]),
    ("NotoSansArabic-Regular.ttf", [(0x0600, 0x06FF), (0x0750, 0x077F), (0x08A0, 0x08FF), (0xFB50, 0xFDFF),
                                    (0xFE70, 0xFEFF)]),
    ("NotoSansHebrew-Regular.ttf", [(0x0590, 0x05FF), (0xFB1D, 0xFB4F)]),
    ("NotoSansThai-Regular.ttf", [(0x0E00, 0x0E7F)]),
    ("NotoSansLao-Regular.ttf", [(0x0E80, 0x0EFF)]),
    ("NotoSansArmenian-Regular.ttf", [(0x0530, 0x058F), (0xFB13, 0xFB17)]),
    ("NotoSansGeorgian-Regular.ttf", [(0x10A0, 0x10FF), (0x1C90, 0x1CBF), (0x2D00, 0x2D2F)]),
    ("NotoSansDevanagari-Regular.ttf", [(0x0900, 0x097F), (0xA8E0, 0xA8FF)]),
    ("NotoSansBengali-Regular.ttf", [(0x0980, 0x09FF)]),
    ("NotoSansGurmukhi-Regular.ttf", [(0x0A00, 0x0A7F)]),
    ("NotoSansGujarati-Regular.ttf", [(0x0A80, 0x0AFF)]),
    ("NotoSansTamil-Regular.ttf", [(0x0B80, 0x0BFF)]),
    ("NotoSansTelugu-Regular.ttf", [(0x0C00, 0x0C7F)]),
    ("NotoSansKannada-Regular.ttf", [(0x0C80, 0x0CFF)]),
    ("NotoSansMalayalam-Regular.ttf", [(0x0D00, 0x0D7F)]),
    ("NotoSansSinhala-Regular.ttf", [(0x0D80, 0x0DFF)]),
    ("NotoSansKhmer-Regular.ttf", [(0x1780, 0x17FF), (0x19E0, 0x19FF)]),
    ("NotoSansMyanmar-Regular.ttf", [(0x1000, 0x109F)]),
    ("NotoSansEthiopic-Regular.ttf", [(0x1200, 0x139F), (0x2D80, 0x2DDF)]),
]


def render(ch: str, font: ImageFont.FreeTypeFont) -> tuple[int, int, int, int, int, bytes]:
    """Глиф с собственной шириной: перо в (0, базовая линия). Возвращает ширину шага, сдвиг
    точек от пера по x, верх точек от верха строки (базовая линия 26), ширину и число строк точек,
    точки L4. Знак без ширины шага (Mn, Me) сам ложится на предыдущую букву, как в шрифте."""
    adv = int(round(font.getlength(ch)))
    if ud.category(ch) in ("Mn", "Me"):
        adv = 0
    big = Image.new("L", (160, 120))
    px, py = 60, 80
    ImageDraw.Draw(big).text((px, py), ch, font=font, fill=255, anchor="ls")
    box = big.getbbox()
    if not box:
        return adv, 0, 0, 0, 0, b""
    ink = big.crop(box)
    if ink.height > 48 or ink.width > 64:                  # редкий великан — уменьшить
        s = min(48 / ink.height, 64 / ink.width)
        ink = ink.resize((max(1, int(ink.width * s)), max(1, int(ink.height * s))), Image.Resampling.LANCZOS)
    xoff, top = box[0] - px, BASELINE - (py - box[1])
    w = ink.width + (ink.width & 1)
    pix = list(ink.get_flattened_data())
    out = bytearray()
    for y in range(ink.height):
        r = pix[y * ink.width:(y + 1) * ink.width] + [0] * (w - ink.width)
        out += bytes((((r[i] + 8) // 17) << 4) | ((r[i + 1] + 8) // 17) for i in range(0, w, 2))
    return adv, xoff, top, w, ink.height, bytes(out)


def main() -> int:
    base = {c for lo, hi in BASE_RANGES for c in range(lo, hi + 1)} & cmap_codes(BASE_TTF)
    glyphs: dict[int, tuple[int, int, int, int, int, bytes]] = {}
    per_script = {}
    for name, ranges in SCRIPTS:
        path = NOTO / name
        have = cmap_codes(path)
        font = ImageFont.truetype(str(path), SIZE)
        n = 0
        for lo, hi in ranges:
            for c in range(lo, hi + 1):
                if c in base or c in glyphs or c not in have or ud.category(chr(c)) in ("Cc", "Cf", "Cn", "Co", "Cs"):
                    continue
                glyphs[c] = render(chr(c), font)
                n += 1
        per_script[name] = n
        print(f"{name}: {n}", flush=True)

    pages = sorted({c >> 8 for c in glyphs})
    table, blobs, raw_total = [], [], 0
    for p in pages:
        head, data = bytearray(), bytearray()
        count = 0
        for lo in range(256):
            c = (p << 8) | lo
            if c in glyphs:
                adv, xoff, top, w, rows, bits = glyphs[c]
                at = 256 * 8 + len(data)
                head += struct.pack("<BbbBB", adv, xoff, top, w, rows) + at.to_bytes(3, "little")
                data += bits
                count += 1
            else:
                head += bytes(8)
        raw = bytes(head + data)
        z = zlib.compress(raw, 9)
        table.append((p, count, len(z), len(raw)))
        blobs.append(z)
        raw_total += len(raw)
    header = b"TSFN" + struct.pack("<HHI", 2, len(pages), len(glyphs))
    offset = len(header) + 16 * len(pages)
    entries = bytearray()
    for (p, count, zl, rl), z in zip(table, blobs):
        entries += struct.pack("<HHIII", p, count, offset, zl, rl)
        offset += zl
    out = ROOT / "dist" / "TSAFARI" / "TSAFARI.FNT"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(header + bytes(entries) + b"".join(blobs))
    report = {"glyphs": len(glyphs), "pages": len(pages), "bytes": out.stat().st_size, "raw_bytes": raw_total,
              "largest_page_raw": max(t[3] for t in table), "largest_page_zlib": max(t[2] for t in table),
              "scripts": per_script}
    (ROOT / "build").mkdir(exist_ok=True)
    (ROOT / "build" / "fontpack.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "scripts"}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
