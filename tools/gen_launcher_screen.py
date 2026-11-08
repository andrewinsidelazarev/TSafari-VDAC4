#!/usr/bin/env python3
"""Окно запуска TSafari для программы Z80 (launcher/ftscreen.asm): то же окно, что у браузера на
старте (design/DESIGN.md, «стартовое окно Wi-Fi»), пока Z80 ищет папку и передаёт ELF.

Z80 сам текст не рисует: маски строк L4 готовятся здесь тем же шрифтом, что у ELF (Noto Sans Mono
20 px, ячейка 12×34, базовая линия 26 — tools/gen_font.py). Всё лежит в RAM_G по тем же адресам,
что у браузера: статический пакет Codex (design/ft/ui_assets.bin) с TSA_UI_RAM_G_BASE, маски
строк с RAMG_WIFI (main/ui.c). Когда ESP возьмёт FT812, браузер пишет в те же места свои строки.

Выход:
  build/launcher_screen.bin — образ RAM_G [LS_BASE, LS_END), по 16 КБ на страницу SPG;
  build/launcher_screen.inc — адреса, листы двух состояний (STATE_LOAD, STATE_SEARCH), смещение
                               слова кадра глобуса в листе и полоса передачи ELF.

Полоса передачи ELF (пользователь 08.11: «нужен прогресс-бар загрузки … между Starting TSafari... и
Please wait.») — как полоса загрузки у браузера (main/ui.c draw_download): дорожка цвета EDGE,
заполнение MINT, высота 6, концы скруглены. В листе она последняя перед DISPLAY: Z80 пишет на место
её первого слова DISPLAY, когда передачи нет (ELF уже в кэше ESP, поиск по карте), а при передаче
меняет только правую вершину заполнения — X = LS_BAR_X0 + доля (0…LS_BAR_SPAN точек).
"""
from __future__ import annotations

import re
import struct
import sys
import unicodedata as ud
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from gen_font import TTF, CELL_W, CELL_H, BASELINE, SIZE  # noqa: E402

HEADER = ROOT / "design" / "ui_design.h"
ASSETS = ROOT / "design" / "ft" / "ui_assets.bin"
RAMG_WIFI = 0x0E6000                       # main/ui.c
WIFI_COLS = 34
WIFI_STRIDE = WIFI_COLS * CELL_W // 2
WIFI_BYTES = WIFI_STRIDE * CELL_H
BAR_SPAN = 320                             # ход заполнения в точках: 256 долей файла + четверть
BAR_H = 6                                  # как TSA_UI_DOWNLOAD_BAR_HEIGHT
BAR_R = BAR_H // 2
BAR_W = BAR_SPAN + BAR_H                   # заполнение — от «точки» шириной 6 до всей дорожки
LINES = ["Web browser for VDAC4 ZX Evolution",      # 0 — как у браузера
         "Starting TSafari...",                     # 1 — запуск
         "Please wait.",                            # 2
         "Looking for TSAFARI.ELF...",              # 3 — первый запуск: поиск по карте
         "First start: scanning the SD card."]      # 4


def design() -> dict[str, int]:
    out = {}
    for m in re.finditer(r"#define (TSA_UI_\w+) \(?(-?(?:0x[0-9A-Fa-f]+|\d+))u?\)?\s*$", HEADER.read_text(encoding="utf-8"), re.M):
        out[m[1]] = int(m[2], 0)
    return out


FONT = ImageFont.truetype(str(TTF), SIZE)


def cell(ch: str) -> Image.Image:
    """Клетка 12×34 — как gen_font.py (широкие знаки вписываются по ширине)."""
    tile = Image.new("L", (CELL_W, CELL_H))
    d = ImageDraw.Draw(tile)
    box = d.textbbox((0, BASELINE), ch, font=FONT, anchor="ls")
    if box[0] < 0 or box[1] < 0 or box[2] > CELL_W or box[3] > CELL_H:
        wide = Image.new("L", (box[2] - box[0], CELL_H))
        ImageDraw.Draw(wide).text((-box[0], BASELINE), ch, font=FONT, fill=255, anchor="ls")
        return wide.resize((CELL_W, CELL_H), Image.Resampling.LANCZOS)
    d.text((0, BASELINE), ch, font=FONT, fill=255, anchor="ls")
    return tile


def mask(text: str) -> bytes:
    img = Image.new("L", (WIFI_COLS * CELL_W, CELL_H))
    for i, ch in enumerate(text[:WIFI_COLS]):
        img.paste(cell(ch), (i * CELL_W, 0))
    px = list(img.get_flattened_data())
    return bytes((((px[i] + 8) // 17) << 4) | ((px[i + 1] + 8) // 17) for i in range(0, len(px), 2))


# --- дисплей-лист (как ui.c draw_wifi) ---
def v(x, y): return 0x40000000 | (((x * 16) & 0x7FFF) << 15) | ((y * 16) & 0x7FFF)
def rgb(c): return (0x04 << 24) | c
def bitmap(addr, fmt, stride, w, h):
    return [(0x01 << 24) | addr, (0x07 << 24) | (fmt << 19) | ((stride & 0x3FF) << 9) | (h & 0x1FF),
            (0x28 << 24) | (((stride >> 10) & 3) << 2) | ((h >> 9) & 3),
            (0x08 << 24) | ((w & 0x1FF) << 9) | (h & 0x1FF), (0x29 << 24) | (((w >> 9) & 3) << 2) | ((h >> 9) & 3)]


def bar_geometry(d: dict[str, int]) -> tuple[int, int]:
    """Левый край и верх полосы: по центру окна, посередине между строкой состояния (ячейка
    высотой CELL_H) и подсказкой."""
    x = d["TSA_UI_WIFI_CENTER_X"] - BAR_W // 2
    top = d["TSA_UI_WIFI_STATE_Y"] + CELL_H
    y = (top + d["TSA_UI_WIFI_HINT_Y"]) // 2 - BAR_R + 2   # +2: у строк сверху пустое место ячейки
    return x, y


def build_dl(d: dict[str, int], state_line: int, hint_line: int) -> tuple[list[int], int, int, int]:
    L4 = 2
    dl = [(0x02 << 24) | d["TSA_UI_COLOR_PAPER_RGB888"], (0x26 << 24) | 7, (0x05 << 24) | 1]
    x, y, w, h, r = (d["TSA_UI_WIFI_X"], d["TSA_UI_WIFI_Y"], d["TSA_UI_WIFI_WIDTH"], d["TSA_UI_WIFI_HEIGHT"],
                     d["TSA_UI_WIFI_RADIUS"])
    dl += [(0x1F << 24) | 9, (0x10 << 24) | 255]
    dl += [rgb(d["TSA_UI_COLOR_EDGE_RGB888"]), (0x0E << 24) | (r * 16), v(x + r, y + r), v(x + w - r, y + h - r)]
    dl += [rgb(d["TSA_UI_COLOR_NAVY_RGB888"]), (0x0E << 24) | ((r - 1) * 16), v(x + r, y + r), v(x + w - r, y + h - r)]
    dl += [0x21 << 24, (0x1F << 24) | 2, rgb(d["TSA_UI_COLOR_PANEL_RGB888"]), (0x0D << 24) | (d["TSA_UI_WIFI_INDICATOR_RADIUS"] * 16),
           v(d["TSA_UI_WIFI_INDICATOR_CENTER_X"], d["TSA_UI_WIFI_INDICATOR_CENTER_Y"]), 0x21 << 24, (0x1F << 24) | 1]
    dl += [rgb(d["TSA_UI_COLOR_UI_TEXT_RGB888"])] + bitmap(d["TSA_UI_ASSET_WIFI_TITLE_RAM_G"], L4, d["TSA_UI_ASSET_WIFI_TITLE_STRIDE"],
                                                            d["TSA_UI_ASSET_WIFI_TITLE_WIDTH"], d["TSA_UI_ASSET_WIFI_TITLE_HEIGHT"])
    dl += [v(d["TSA_UI_WIFI_TITLE_X"], d["TSA_UI_WIFI_TITLE_Y"])]
    dl += [rgb(d["TSA_UI_COLOR_UI_MUTED_RGB888"])] + bitmap(RAMG_WIFI, L4, WIFI_STRIDE, WIFI_COLS * CELL_W, CELL_H)
    dl += [v(d["TSA_UI_WIFI_SUBTITLE_X"], d["TSA_UI_WIFI_SUBTITLE_Y"])]
    dl += [rgb(d["TSA_UI_COLOR_MINT_RGB888"])]
    globe_word = len(dl)                                  # BITMAP_SOURCE кадра глобуса
    dl += bitmap(d["TSA_UI_ASSET_GLOBE_00_RAM_G"], L4, 16, 32, 32) + [v(d["TSA_UI_WIFI_GLOBE_X"], d["TSA_UI_WIFI_GLOBE_Y"])]
    for line, y_, col in ((state_line, d["TSA_UI_WIFI_STATE_Y"], d["TSA_UI_COLOR_UI_TEXT_RGB888"]),
                          (hint_line, d["TSA_UI_WIFI_HINT_Y"], d["TSA_UI_COLOR_UI_MUTED_RGB888"])):
        x_ = d["TSA_UI_WIFI_CENTER_X"] - len(LINES[line]) * CELL_W // 2
        dl += [rgb(col)] + bitmap(RAMG_WIFI + line * WIFI_BYTES, L4, WIFI_STRIDE, WIFI_COLS * CELL_W, CELL_H) + [v(x_, y_)]
    # полоса передачи ELF: прямоугольники с толщиной линии BAR_R — скруглённые концы (как rrect в ui.c)
    bx, by = bar_geometry(d)
    bar_word = len(dl)                                    # BEGIN RECTS — сюда Z80 пишет DISPLAY
    dl += [(0x1F << 24) | 9, (0x0E << 24) | (BAR_R * 16)]
    dl += [rgb(d["TSA_UI_COLOR_EDGE_RGB888"]), v(bx + BAR_R, by + BAR_R), v(bx + BAR_W - BAR_R, by + BAR_R)]
    dl += [rgb(d["TSA_UI_COLOR_MINT_RGB888"]), v(bx + BAR_R, by + BAR_R)]
    fill_word = len(dl)                                   # правая вершина заполнения
    dl += [v(bx + BAR_R, by + BAR_R)]
    dl += [0x21 << 24, 0]
    return dl, globe_word, bar_word, fill_word


def main() -> int:
    d = design()
    base, assets = d["TSA_UI_RAM_G_BASE"], ASSETS.read_bytes()
    assert len(assets) == d["TSA_UI_STATIC_BYTES"]
    end = RAMG_WIFI + len(LINES) * WIFI_BYTES
    assert end <= d["TSA_UI_RAM_G_END"], hex(end)
    image = bytearray(end - base)
    image[:len(assets)] = assets
    for i, text in enumerate(LINES):
        assert len(text) <= WIFI_COLS and all(ord(c) < 128 for c in text), text
        at = RAMG_WIFI - base + i * WIFI_BYTES
        image[at:at + WIFI_BYTES] = mask(text)
    (ROOT / "build").mkdir(exist_ok=True)
    (ROOT / "build" / "launcher_screen.bin").write_bytes(bytes(image))
    dl_load, g1, b1, f1 = build_dl(d, 1, 2)
    dl_search, g2, b2, f2 = build_dl(d, 3, 4)
    assert (g1, b1, f1) == (g2, b2, f2) and len(dl_load) == len(dl_search)
    bx, by = bar_geometry(d)
    x0 = bx + BAR_R
    # Z80 правит в вершине только байты 2 и 3 (X · 16 << 15): младший бит X·16 всегда 0, Y — в байтах 0–1
    assert x0 + BAR_SPAN < 1024 and dl_load[f1] & 0xFFFF == v(x0 + BAR_SPAN, by + BAR_R) & 0xFFFF
    pages = (len(image) + 16383) // 16384
    inc = ["; Создано tools/gen_launcher_screen.py — не править вручную.",
           f"LS_BASE         EQU #{base:06X}           ; образ RAM_G окна запуска",
           f"LS_BYTES        EQU {len(image)}",
           f"LS_PAGES        EQU {pages}                 ; страниц SPG с LS_PAGE0",
           f"LS_GLOBE0       EQU #{d['TSA_UI_ASSET_GLOBE_00_RAM_G']:06X}           ; кадр глобуса 0, шаг 512",
           f"LS_GLOBE_WORD   EQU {g1 * 4}                ; смещение BITMAP_SOURCE глобуса в листе",
           f"LS_GLOBE_FRAMES EQU {d['TSA_UI_GLOBE_FRAME_COUNT']}",
           f"LS_DL_BYTES     EQU {len(dl_load) * 4}",
           f"LS_BAR_WORD     EQU {b1 * 4}                ; смещение BEGIN RECTS полосы (без передачи — DISPLAY)",
           f"LS_BAR_FILL     EQU {f1 * 4}                ; смещение правой вершины заполнения",
           f"LS_BAR_X0       EQU {x0}               ; X вершины при доле 0",
           f"LS_BAR_SPAN     EQU {BAR_SPAN}               ; ход заполнения в точках",
           "LsDlLoad:", *[f"        dd #{w:08X}" for w in dl_load],
           "LsDlSearch:", *[f"        dd #{w:08X}" for w in dl_search], ""]
    (ROOT / "build" / "launcher_screen.inc").write_text("\n".join(inc), encoding="utf-8", newline="\n")
    preview(d, image, base, dl_load)
    print(f"launcher screen: {len(image)} bytes, {pages} pages, DL {len(dl_load)} words")
    return 0


def preview(d, image, base, dl):
    """Как будет на экране (для глаз): окно поиска — рисуется прямо по маскам из образа."""
    out = Image.new("RGB", (1024, 768), tuple((d["TSA_UI_COLOR_PAPER_RGB888"] >> s) & 255 for s in (16, 8, 0)))
    dr = ImageDraw.Draw(out)
    x, y, w, h, r = (d["TSA_UI_WIFI_X"], d["TSA_UI_WIFI_Y"], d["TSA_UI_WIFI_WIDTH"], d["TSA_UI_WIFI_HEIGHT"],
                     d["TSA_UI_WIFI_RADIUS"])
    c = lambda k: tuple((d[k] >> s) & 255 for s in (16, 8, 0))
    dr.rounded_rectangle([x, y, x + w - 1, y + h - 1], r, fill=c("TSA_UI_COLOR_NAVY_RGB888"), outline=c("TSA_UI_COLOR_EDGE_RGB888"))
    cx, cy, ir = d["TSA_UI_WIFI_INDICATOR_CENTER_X"], d["TSA_UI_WIFI_INDICATOR_CENTER_Y"], d["TSA_UI_WIFI_INDICATOR_RADIUS"]
    dr.ellipse([cx - ir, cy - ir, cx + ir, cy + ir], fill=c("TSA_UI_COLOR_PANEL_RGB888"))

    def put(addr, stride, wpx, hpx, px, py, colour):
        m = Image.new("L", (wpx, hpx))
        p = m.load()
        for yy in range(hpx):
            for xx in range(0, wpx, 2):
                b = image[addr - base + yy * stride + xx // 2]
                p[xx, yy] = (b >> 4) * 17
                p[xx + 1, yy] = (b & 15) * 17
        out.paste(Image.new("RGB", (wpx, hpx), colour), (px, py), m)
    put(d["TSA_UI_ASSET_WIFI_TITLE_RAM_G"], d["TSA_UI_ASSET_WIFI_TITLE_STRIDE"], d["TSA_UI_ASSET_WIFI_TITLE_WIDTH"],
        d["TSA_UI_ASSET_WIFI_TITLE_HEIGHT"], d["TSA_UI_WIFI_TITLE_X"], d["TSA_UI_WIFI_TITLE_Y"], c("TSA_UI_COLOR_UI_TEXT_RGB888"))
    put(RAMG_WIFI, WIFI_STRIDE, WIFI_COLS * CELL_W, CELL_H, d["TSA_UI_WIFI_SUBTITLE_X"], d["TSA_UI_WIFI_SUBTITLE_Y"],
        c("TSA_UI_COLOR_UI_MUTED_RGB888"))
    put(d["TSA_UI_ASSET_GLOBE_00_RAM_G"], 16, 32, 32, d["TSA_UI_WIFI_GLOBE_X"], d["TSA_UI_WIFI_GLOBE_Y"], c("TSA_UI_COLOR_MINT_RGB888"))
    bx, by = bar_geometry(d)                              # полоса передачи — заполнена на 40 %
    dr.rounded_rectangle([bx, by, bx + BAR_W - 1, by + BAR_H - 1], BAR_R, fill=c("TSA_UI_COLOR_EDGE_RGB888"))
    dr.rounded_rectangle([bx, by, bx + BAR_H + BAR_SPAN * 2 // 5 - 1, by + BAR_H - 1], BAR_R, fill=c("TSA_UI_COLOR_MINT_RGB888"))
    for line, yy, k in ((1, d["TSA_UI_WIFI_STATE_Y"], "TSA_UI_COLOR_UI_TEXT_RGB888"), (2, d["TSA_UI_WIFI_HINT_Y"], "TSA_UI_COLOR_UI_MUTED_RGB888")):
        put(RAMG_WIFI + line * WIFI_BYTES, WIFI_STRIDE, WIFI_COLS * CELL_W, CELL_H,
            d["TSA_UI_WIFI_CENTER_X"] - len(LINES[line]) * CELL_W // 2, yy, c(k))
    out.save(ROOT / "build" / "launcher_screen_preview.png")


if __name__ == "__main__":
    raise SystemExit(main())
