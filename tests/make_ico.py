#!/usr/bin/env python3
"""Значки ICO для стенда (tests/pages/ico/*.ico и страница tests/pages/ico.html) — свои, не с сайтов.

Какие бывают в жизни (DuckDuckGo отдаёт favicon сайтов как есть, 08.11.2026): BMP 32 бита с
прозрачностью (github, facebook — 16×16 и 32×32 в одном файле), BMP с палитрой и маской
(zxart.ee — 16×16, 318 байт), старые 32 бита без прозрачности, у которых прозрачность — в маске,
и PNG внутри ICO. Здесь — по одному каждого вида; проверка — в tests/host.c (стенд «значки ICO»).

    python tests/make_ico.py
"""
from io import BytesIO
from pathlib import Path
import struct

OUT = Path(__file__).resolve().parent / "pages"


def pad4(b: bytes) -> bytes:
    return b + b"\0" * (-len(b) % 4)


def bmp_entry(w, h, bpp, px, palette=None, alpha_byte=True):
    """Картинка BMP для ICO: заголовок, палитра, точки снизу вверх, маска И (1 — прозрачно).
    px[y][x] = (r, g, b, a) для 24/32 бит или (номер цвета, a) для палитры."""
    rows, mask = [], []
    for y in reversed(range(h)):
        if bpp == 32:
            row = b"".join(struct.pack("BBBB", p[2], p[1], p[0], p[3] if alpha_byte else 0) for p in px[y])
        elif bpp == 24:
            row = b"".join(struct.pack("BBB", p[2], p[1], p[0]) for p in px[y])
        elif bpp == 4:
            row = bytes((px[y][x][0] << 4) | px[y][x + 1][0] for x in range(0, w, 2))
        else:
            raise ValueError(bpp)
        rows.append(pad4(row))
        bits = 0
        mb = bytearray()
        for x in range(w):
            bits = bits << 1 | (1 if px[y][x][-1] == 0 else 0)
            if x % 8 == 7:
                mb.append(bits)
                bits = 0
        mask.append(pad4(bytes(mb)))
    pal = b"".join(struct.pack("BBBB", c[2], c[1], c[0], 0) for c in palette) if palette else b""
    data = b"".join(rows) + b"".join(mask)
    head = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, bpp, 0, len(data), 0, 0, len(palette or []), 0)
    return head + pal + data


def ico(entries):
    """entries: (w, h, bpp, bytes)."""
    out = struct.pack("<HHH", 0, 1, len(entries))
    off = 6 + 16 * len(entries)
    body = b""
    for w, h, bpp, data in entries:
        out += struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, bpp, len(data), off + len(body))
        body += data
    return out + body


def disc(w, h, color, inner=None):
    """Круг цвета color на прозрачном (край — полупрозрачный), в середине — точка inner."""
    px = []
    cx, cy, r = (w - 1) / 2, (h - 1) / 2, w / 2
    for y in range(h):
        row = []
        for x in range(w):
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
            a = 255 if d < r - 1 else 128 if d < r else 0
            c = inner if inner and d < r / 3 else color
            row.append((c[0], c[1], c[2], a))
        px.append(row)
    return px


def main() -> int:
    from PIL import Image, ImageDraw
    (OUT / "ico").mkdir(parents=True, exist_ok=True)
    # 1. BMP 32 бита с прозрачностью, два размера: 16×16 красный, 32×32 синий
    a32 = ico([
        (16, 16, 32, bmp_entry(16, 16, 32, disc(16, 16, (220, 40, 40)))),
        (32, 32, 32, bmp_entry(32, 32, 32, disc(32, 32, (40, 80, 220), (255, 255, 255)))),
    ])
    (OUT / "ico" / "a32.ico").write_bytes(a32)
    (OUT / "ico" / "a32b.ico").write_bytes(a32)            # тот же — без размера на странице
    # 2. BMP 4 бита с палитрой и маской (прозрачные углы), как у zxart.ee
    pal = [(0, 0, 0), (255, 200, 0), (0, 160, 80)] + [(i * 16, i * 16, i * 16) for i in range(3, 16)]
    px4 = [[((1 if (x + y) % 4 < 2 else 2), 0 if (x < 3 and y < 3) or (x > 12 and y > 12) else 255)
            for x in range(16)] for y in range(16)]
    (OUT / "ico" / "p4.ico").write_bytes(ico([(16, 16, 4, bmp_entry(16, 16, 4, px4, pal))]))
    # 3. BMP 24 бита без прозрачности (маска пустая) — точки RGB565
    px24 = [[(x * 16, y * 16, 128, 255) for x in range(16)] for y in range(16)]
    (OUT / "ico" / "rgb24.ico").write_bytes(ico([(16, 16, 24, bmp_entry(16, 16, 24, px24))]))
    # 4. Старый 32 бита: четвёртый байт всюду 0, прозрачность — только в маске
    (OUT / "ico" / "old32.ico").write_bytes(ico([(16, 16, 32, bmp_entry(16, 16, 32, disc(16, 16, (0, 150, 0)),
                                                                           alpha_byte=False))]))
    # 5. PNG внутри ICO, 32×32 RGBA
    im = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    ImageDraw.Draw(im).rounded_rectangle((2, 2, 29, 29), 7, fill=(250, 120, 0, 255))
    buf = BytesIO()
    im.save(buf, "PNG")
    (OUT / "ico" / "png.ico").write_bytes(ico([(32, 32, 32, buf.getvalue())]))
    (OUT / "ico.html").write_text(
        "<html><head><meta charset=\"utf-8\"><title>ICO</title></head><body>\n"
        "<h2>Значки ICO</h2>\n"
        "<p>32 бита, место 16: <img src=\"ico/a32.ico\" width=16 height=16> — "
        "без размера (самый большой, 32): <img src=\"ico/a32b.ico\"></p>\n"
        "<p>4 бита с маской: <img src=\"ico/p4.ico\" width=16 height=16>, 24 бита: "
        "<img src=\"ico/rgb24.ico\" width=16 height=16>, старый 32 бита: <img src=\"ico/old32.ico\" width=16 height=16>, "
        "PNG внутри: <img src=\"ico/png.ico\" width=16 height=16></p>\n"
        "</body></html>\n", encoding="utf-8", newline="\n")
    print("ico:", ", ".join(p.name for p in (OUT / "ico").iterdir()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
