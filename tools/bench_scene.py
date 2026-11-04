#!/usr/bin/env python3
"""Сцена «страница браузера + курсор» для стенда ядра FT812 (fpga/vdac2/tb/vsys).

Отвечает на вопрос этапа 0: успевает ли ядро Slot1 нарисовать плотную страницу TSafari,
когда курсор двигается каждый кадр (каждый ход мыши — новый дисплей-лист).

Итог замера: способ ниже (битмап на кусок строки) — 28–53 мс на кадр, дольше кадра; браузер
поэтому рисует страницу целиком в одну картинку PALETTED565 (main/page.c, 11–12,5 мс). Скрипт
оставлен, чтобы замер можно было повторить.

Страница — как задумано в docs/ARCHITECTURE.md: каждый кусок строки одного цвета — свой битмап
L4 (шрифт Noto Sans Mono из TXTVIEW2, 20 px, шаг 12), цвет — COLOR_RGB; фон и подчёркивания —
RECTS; картинка — RGB565; кнопки — POINTS и иконки ARGB4; курсор Zuma — ARGB4 последним.

Выход в каталоге --out: ramg.bin (RAM_G с адреса 0), trace.bin (VTR1: шаг 0 — лист установки,
дальше лист с курсором в новой точке), scene.png (как должен выглядеть кадр) и scene.json.

Запуск стенда:  vsys.exe --trace trace.bin --ramg ramg.bin --no-skip --seconds 0.5
"""
from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT = Path(r"E:\zx\esp\wc-improved\source\plugins\txthex_viewer_VDAC2\fonts\NotoSansMono-Regular.ttf")
CURSOR = Path(r"E:\zx\Zuna-Deluxe-VDAC2-FT812\Graphics\Original\cursor.png")
PHOTO = Path(r"E:\zx\Tang Nano 20k VDAC\VDAC4\Tang Nano 20k\FTVIEW4\artykan0.jpg")
ICONS = Path(__file__).resolve().parents[1] / "design" / "icons"

W, H = 1024, 768
ADV, FONT_PX, LINE_H = 12, 20, 22          # шаг, кегль и высота строки основного текста
TOP = 64                                   # отступ сверху: под кнопками

# --- команды дисплей-листа FT812 -------------------------------------------------------------
L4, ARGB4, RGB565, PALETTED565 = 2, 6, 7, 14
BITMAPS, POINTS, RECTS = 1, 2, 9


def clear_color(rgb): return (0x02 << 24) | rgb
def clear(): return (0x26 << 24) | 7
def color_rgb(rgb): return (0x04 << 24) | rgb
def color_a(a): return (0x10 << 24) | a
def begin(p): return (0x1F << 24) | p
def end(): return 0x21 << 24
def vertex2f(x, y): return 0x40000000 | ((int(x * 16) & 0x7FFF) << 15) | (int(y * 16) & 0x7FFF)
def bitmap_handle(h): return (0x05 << 24) | h
def bitmap_source(a): return (0x01 << 24) | (a & 0x3FFFFF)
def palette_source(a): return (0x2A << 24) | (a & 0x3FFFFF)
def point_size(r): return (0x0D << 24) | int(r * 16)
def line_width(w): return (0x0E << 24) | int(w * 16)
def display(): return 0


def bitmap(fmt, stride, w, h):
    """BITMAP_LAYOUT/_H и BITMAP_SIZE/_H (NEAREST, BORDER). Старшие слова — всегда: ручка их
    помнит, и битмап после большого иначе получает чужие старшие биты размера."""
    return [(0x07 << 24) | (fmt << 19) | ((stride & 0x3FF) << 9) | (h & 0x1FF),
            (0x28 << 24) | (((stride >> 10) & 3) << 2) | ((h >> 9) & 3),
            (0x08 << 24) | ((w & 0x1FF) << 9) | (h & 0x1FF),
            (0x29 << 24) | (((w >> 9) & 3) << 2) | ((h >> 9) & 3)]


# --- RAM_G -----------------------------------------------------------------------------------
class RamG:
    def __init__(self):
        self.data = bytearray()

    def put(self, blob: bytes) -> int:
        while len(self.data) % 4:
            self.data.append(0)
        at = len(self.data)
        self.data += blob
        return at


def to_l4(img: Image.Image) -> tuple[bytes, int]:
    """Серое L → L4: точка 0 — старшая тетрада; строка — до кратного 4 байтам."""
    w, h = img.size
    stride = ((w + 1) // 2 + 3) & ~3
    px = img.load()
    out = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            v = px[x, y] >> 4
            i = y * stride + x // 2
            out[i] |= v << 4 if x % 2 == 0 else v
    return bytes(out), stride


def to_argb4(img: Image.Image) -> bytes:
    img = img.convert("RGBA")
    out = bytearray()
    for r, g, b, a in img.getdata():
        out += struct.pack("<H", ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4))
    return bytes(out)


def to_rgb565(img: Image.Image) -> bytes:
    out = bytearray()
    for r, g, b in img.convert("RGB").getdata():
        out += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return bytes(out)


# --- содержание страницы ---------------------------------------------------------------------
TEXT = (
    "ZX Evolution — компьютер, совместимый с ZX Spectrum 128, на ПЛИС Altera EP1K50. "
    "Конфигурация TS-Conf добавляет графические режимы 256 и 16 цветов, спрайты, тайловые "
    "слои, DMA и аппаратную прокрутку. The board keeps the classic Z80 at 3.5, 7 or 14 MHz, "
    "a PS/2 keyboard and mouse, an SD card and the General Sound card. Плата VDAC4 выводит "
    "картинку Evo на HDMI через Tang Nano 20K, а ESP32-S3 даёт сеть, звук и эмуляцию FT812. "
    "TSafari is a web browser that runs on that ESP and draws pages through the FT812 core in "
    "slot 1: text in Noto Sans Mono, pictures decoded by the BL616, the mouse cursor from Zuma. "
)
WORDS = TEXT.split()
COL_TEXT, COL_LINK, COL_VISITED, COL_HEAD = 0x1E2228, 0x1A5FB4, 0x6A3D9A, 0x0B1F3A


def runs_for_line(cols: int, line: int, links: int):
    """Строка шириной cols знаков: куски (текст, цвет, ссылка?) — links ссылок на строку."""
    words, n = [], 0
    k = line * 7
    while True:
        wd = WORDS[k % len(WORDS)]
        if n + len(wd) + (1 if words else 0) > cols:
            break
        n += len(wd) + (1 if words else 0)
        words.append(wd)
        k += 1
    # links ссылок по два слова, равномерно по строке; между ними — обычный текст
    starts = sorted({(j + 1) * len(words) // (links + 1) for j in range(links)})
    out, i = [], 0
    for s in starts:
        s = max(s, i)
        if s + 2 > len(words):
            break
        if s > i:
            out.append((" ".join(words[i:s]), COL_TEXT, False))
        out.append((" ".join(words[s:s + 2]), COL_LINK if (line + s) % 3 else COL_VISITED, True))
        i = s + 2
    if i < len(words):
        out.append((" ".join(words[i:]), COL_TEXT, False))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=r"E:\zx\cache\tmp\tsafari\bench\typical")
    ap.add_argument("--links", type=int, default=1, help="ссылок на строку текста")
    ap.add_argument("--steps", type=int, default=240, help="шагов с курсором после листа установки")
    ap.add_argument("--no-photo", action="store_true")
    ap.add_argument("--mode", choices=("runs", "fb", "pal"), default="runs",
                    help="runs — кусок строки = битмап L4; fb — вся страница одним RGB565 (кольцо строк); "
                         "pal — то же кольцо в PALETTED565 (байт на точку, палитра 256 цветов)")
    ap.add_argument("--ring", type=int, default=896, help="fb: строк в кольце RAM_G (768 + запас прокрутки)")
    ap.add_argument("--top-row", type=int, default=300, help="fb: строка кольца вверху экрана (проверка стыка)")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    font = ImageFont.truetype(str(FONT), FONT_PX)
    font_h1 = ImageFont.truetype(str(FONT), 30)
    ram = RamG()
    ref = Image.new("RGB", (W, H), (0xFA, 0xFA, 0xF7))
    refd = ImageDraw.Draw(ref)
    dl: list[int] = [clear_color(0xFAFAF7), clear(), bitmap_handle(0)]
    stats = {"text_bitmaps": 0, "text_bytes": 0, "links": 0}

    def text_bitmap(text, f, x, y, color, h):
        w = ((len(text) * (f.size * 3 // 5) + 7) // 8) * 8
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((0, 0), text, fill=255, font=f)
        blob, stride = to_l4(img)
        at = ram.put(blob)
        dl.extend([color_rgb(color), bitmap_source(at), *bitmap(L4, stride, w, h), vertex2f(x, y)])
        stats["text_bitmaps"] += 1
        stats["text_bytes"] += len(blob)
        refd.text((x, y), text, fill=((color >> 16) & 255, (color >> 8) & 255, color & 255), font=f)
        return w

    # рамка картинки (полосы-заголовка у TSafari нет: пользователь — «это лишнее»)
    dl += [begin(RECTS)]
    photo = None
    if not a.no_photo:
        px, py, pw, ph = W - 24 - 320 - 8, TOP + 60, 320, 240
        dl += [color_rgb(0xD0D6DE), vertex2f(px - 4, py - 4), vertex2f(px + pw + 4, py + ph + 4), end()]
        refd.rectangle([px - 4, py - 4, px + pw + 4, py + ph + 4], fill=(0xD0, 0xD6, 0xDE))
        photo = (px, py, pw, ph)
    else:
        dl.append(end())

    dl += [begin(BITMAPS)]
    text_bitmap("TSafari: ZX Evolution и VDAC4", font_h1, 28, TOP + 4, COL_HEAD, 36)
    if photo:
        img = photo_img = Image.open(PHOTO).convert("RGB").resize((photo[2], photo[3]), Image.LANCZOS)
        at = ram.put(to_rgb565(img))
        dl += [color_rgb(0xFFFFFF), bitmap_source(at), *bitmap(RGB565, photo[2] * 2, photo[2], photo[3]),
               vertex2f(photo[0], photo[1])]
        ref.paste(img, (photo[0], photo[1]))

    underlines = []
    y, line = TOP + 60, 0
    while y + LINE_H <= H - 8:
        right = photo[0] - 16 if photo and photo[1] - 4 <= y <= photo[1] + photo[3] else W - 32
        cols = (right - 28) // ADV
        x = 28
        for text, color, is_link in runs_for_line(cols, line, a.links):
            w = text_bitmap(text, font, x, y, color, FONT_PX + 2)
            if is_link:
                underlines.append((x, y + FONT_PX, x + len(text) * ADV, y + FONT_PX + 1, color))
                stats["links"] += 1
            x += len(text) * ADV + ADV
        y += LINE_H
        line += 1
    dl.append(end())

    # подчёркивания ссылок — прямоугольники в 1 px
    if underlines:
        dl.append(begin(RECTS))
        for x0, y0, x1, y1, c in underlines:
            dl += [color_rgb(c), vertex2f(x0, y0), vertex2f(x1, y1)]
            refd.rectangle([x0, y0, x1 - 1, y1 - 1], fill=((c >> 16) & 255, (c >> 8) & 255, c & 255))
        dl.append(end())

    if a.mode in ("fb", "pal"):
        # Страница целиком — кольцо строк (рисует ESP), без смешения: две точки за такт.
        # На экран — два куска кольца (стык на top_row); картинка сайта — своим битмапом поверх.
        # pal: байт на точку и палитра RGB565 на 256 цветов — вдвое меньше выборки текселей.
        ram = RamG()
        pal = a.mode == "pal"
        if pal:
            q = ref.quantize(256, method=Image.Quantize.MEDIANCUT)
            rows, fmt, line = bytes(q.getdata()), PALETTED565, W
            colors = q.getpalette()[:768]
            palette = b"".join(struct.pack("<H", ((colors[i] >> 3) << 11) | ((colors[i + 1] >> 2) << 5) |
                                           (colors[i + 2] >> 3)) for i in range(0, 768, 3))
        else:
            rows, fmt, line = to_rgb565(ref), RGB565, W * 2
        ring, r0 = a.ring, a.top_row
        buf = bytearray(line * ring)
        for y in range(H):
            rr = (r0 + y) % ring
            buf[rr * line:(rr + 1) * line] = rows[y * line:(y + 1) * line]
        ring_at = ram.put(bytes(buf))
        na = min(H, ring - r0)
        dl = [clear_color(0xFAFAF7), clear(), bitmap_handle(0), begin(BITMAPS), color_rgb(0xFFFFFF)]
        if pal:
            dl.append(palette_source(ram.put(palette)))
        dl += [bitmap_source(ring_at + r0 * line), *bitmap(fmt, line, W, na), vertex2f(0, 0)]
        if na < H:
            dl += [bitmap_source(ring_at), *bitmap(fmt, line, W, H - na), vertex2f(0, na)]
        if photo:
            at = ram.put(to_rgb565(photo_img))
            dl += [bitmap_source(at), *bitmap(RGB565, photo[2] * 2, photo[2], photo[3]), vertex2f(photo[0], photo[1])]
        dl.append(end())
        stats = {"mode": "fb", "ring_lines": ring, "ring_bytes": len(buf),
                 "hidden_rows_at": ((r0 + H) % ring) * line, "hidden_rows": ring - H}

    # полоса прокрутки
    dl += [begin(RECTS), line_width(4), color_rgb(0xDDE1E6), vertex2f(W - 14, 12), vertex2f(W - 8, H - 12),
           color_rgb(0x8A93A0), vertex2f(W - 14, 60), vertex2f(W - 8, 220), line_width(1), end()]
    refd.rounded_rectangle([W - 18, 8, W - 4, H - 8], 4, fill=(0xDD, 0xE1, 0xE6))
    refd.rounded_rectangle([W - 18, 56, W - 4, 224], 4, fill=(0x8A, 0x93, 0xA0))

    # кнопки: назад, вперёд, стоп, обновить и глобус — круги POINTS и иконки ARGB4
    # иконки — утверждённые Codex (design/icons), своих картинок стенд не рисует
    icon_at = []
    for name in ("back", "forward", "stop", "reload", "globe_00"):
        im = Image.open(ICONS / f"{name}.png").convert("RGBA")
        icon_at.append((ram.put(to_argb4(im)), im.width))
    bx = [28 + 46 * i for i in range(4)] + [28 + 46 * 4 + 6]
    dl += [begin(POINTS), color_rgb(0xB8C0CC)]
    for i, x in enumerate(bx):
        r = 22 if i == 4 else 19
        dl += [point_size(r + 1), vertex2f(x, 30)]
        refd.ellipse([x - r - 1, 30 - r - 1, x + r + 1, 30 + r + 1], fill=(0xB8, 0xC0, 0xCC))
    dl.append(color_rgb(0xFFFFFF))
    for i, x in enumerate(bx):
        r = 22 if i == 4 else 19
        dl += [point_size(r), vertex2f(x, 30)]
        refd.ellipse([x - r, 30 - r, x + r, 30 + r], fill=(0xFF, 0xFF, 0xFF))
    dl += [end(), begin(BITMAPS)]
    for (at, side), x in zip(icon_at, bx):
        dl += [bitmap_source(at), *bitmap(ARGB4, side * 2, side, side), vertex2f(x - side // 2, 30 - side // 2)]

    # курсор Zuma последним; его VERTEX2F меняется от шага к шагу
    cur = Image.open(CURSOR).convert("RGBA")
    cur_at = ram.put(to_argb4(cur))
    dl += [bitmap_source(cur_at), *bitmap(ARGB4, cur.width * 2, cur.width, cur.height)]
    cursor_word = len(dl)
    dl += [vertex2f(500, 400), end(), display()]

    # трасса: шаг 0 — лист установки, дальше курсор по кругу через всю страницу
    import math
    steps = []
    for k in range(a.steps + 1):
        t = k / max(1, a.steps)
        cx = 512 + 420 * math.cos(2 * math.pi * t) - 1
        cy = 384 + 300 * math.sin(2 * math.pi * t) - 1
        d = list(dl)
        d[cursor_word] = vertex2f(int(cx), int(cy))
        steps.append(d)
    with open(out / "trace.bin", "wb") as f:
        f.write(b"VTR1" + struct.pack("<I", len(steps)))
        for k, d in enumerate(steps):
            # кадр, такты Z80: t_v, t_l, t_e (хост — ESP, его время здесь не меряем), слова листа
            f.write(struct.pack("<5I", k, 0, 1400, 0, len(d)))
            f.write(struct.pack(f"<{len(d)}I", *d))
    (out / "ramg.bin").write_bytes(bytes(ram.data))
    ref.alpha_composite if False else None
    ref_rgba = ref.convert("RGBA")
    ref_rgba.alpha_composite(cur, (500 - 1, 400 - 1))
    ref_rgba.convert("RGB").save(out / "scene.png")
    info = {"dl_words": len(dl), "ramg_bytes": len(ram.data), **stats, "steps": len(steps)}
    (out / "scene.json").write_text(json.dumps(info, ensure_ascii=False, indent=1), encoding="utf-8")
    print(json.dumps(info, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
