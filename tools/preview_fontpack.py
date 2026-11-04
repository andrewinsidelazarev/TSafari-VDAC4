#!/usr/bin/env python3
"""Проверка пакета TSAFARI.FNT глазами: строки всех письменностей, нарисованные из самого пакета
(основа — латиница, цифры, пробел — тем же способом, что gen_font.py).

Здесь же — образец того, что сделает раскладка браузера: арабские буквы по положению в слове
(формы Arabic Presentation Forms-B из Unicode) и запись справа налево; знаки (класс 3) ложатся
на предыдущую букву. Выход — build/fontpack_preview.png.
"""
from __future__ import annotations

import struct
import sys
import unicodedata as ud
import zlib
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from gen_font import TTF as BASE_TTF, CELL_H, BASELINE, SIZE  # noqa: E402

SAMPLES = [
    "中文: 你好，世界！欢迎使用 TSafari 浏览器。",
    "日本語: こんにちは世界、カタカナとひらがな。",
    "한국어: 안녕하세요, 세계! 브라우저입니다.",
    "العربية: مرحبا بالعالم، هذا متصفح",
    "فارسی: سلام دنیا",
    "עברית: שלום עולם",
    "ไทย: สวัสดีชาวโลก ภาษาไทย",
    "हिन्दी: नमस्ते दुनिया",
    "বাংলা: হ্যালো বিশ্ব",
    "தமிழ்: வணக்கம் உலகம்",
    "తెలుగు: హలో ప్రపంచం   ಕನ್ನಡ: ನಮಸ್ಕಾರ",
    "മലയാളം: ഹലോ   ગુજરાતી: નમસ્તે   ਪੰਜਾਬੀ: ਸਤ ਸ੍ਰੀ ਅਕਾਲ",
    "සිංහල: ආයුබෝවන්   ខ្មែរ: សួស្តី   ລາວ: ສະບາຍດີ",
    "မြန်မာ: မင်္ဂလာပါ   አማርኛ: ሰላም ልዑል",
    "ქართული: გამარჯობა მსოფლიო   Հայերեն: Բարեւ աշխարհ",
    "Русский, English, Ελληνικά — основа в ELF",
]


class Pack:
    def __init__(self, path: Path):
        d = path.read_bytes()
        assert d[:4] == b"TSFN"
        _, npages, _ = struct.unpack_from("<HHI", d, 4)
        self.pages = {}
        for i in range(npages):
            p, _, off, zl, rl = struct.unpack_from("<HHIII", d, 12 + 16 * i)
            self.pages[p] = (d, off, zl)
        self.cache = {}

    def glyph(self, c: int):
        """(шаг, сдвиг x, верх, картинка L) или None — знака нет в пакете."""
        p = c >> 8
        if p not in self.pages:
            return None
        if p not in self.cache:
            d, off, zl = self.pages[p]
            self.cache[p] = zlib.decompress(d[off:off + zl])
        raw = self.cache[p]
        e = raw[(c & 255) * 8:(c & 255) * 8 + 8]
        if not any(e):
            return None
        adv, xoff, top, w, rows = struct.unpack_from("<BbbBB", e)
        at = int.from_bytes(e[5:8], "little")
        img = Image.new("L", (max(w, 1), max(rows, 1)))
        px = img.load()
        for r in range(rows):
            for b in range(w // 2):
                v = raw[at + r * (w // 2) + b]
                px[2 * b, r] = (v >> 4) * 17
                px[2 * b + 1, r] = (v & 15) * 17
        return adv, xoff, top, img


BASE_FONT = ImageFont.truetype(str(BASE_TTF), SIZE)


def base_glyph(ch: str) -> Image.Image:
    img = Image.new("L", (12, CELL_H))
    ImageDraw.Draw(img).text((0, BASELINE), ch, font=BASE_FONT, fill=255, anchor="ls")
    return img


# --- арабские формы: тип соединения и формы из разложений Unicode ---
FORMS: dict[tuple[int, str], int] = {}
for c in range(0xFE70, 0xFF00):
    dec = ud.decomposition(chr(c)).split()
    if len(dec) == 2 and dec[0] in ("<isolated>", "<final>", "<initial>", "<medial>"):
        FORMS[int(dec[1], 16), dec[0][1:-1]] = c
RIGHT_ONLY = set(map(ord, "اأإآدذرزوؤةىٱ")) | {0x0698, 0x06C0, 0x06D2, 0x06D3}


def joins_both(c: int) -> bool:
    return (c, "medial") in FORMS or (c, "initial") in FORMS


def arabic_letter(c: int) -> bool:
    return (c, "isolated") in FORMS or (c, "final") in FORMS


def shape_arabic(text: str) -> str:
    cs = [ord(ch) for ch in text]
    out = []
    for i, c in enumerate(cs):
        if not arabic_letter(c):
            out.append(c)
            continue
        prev = cs[i - 1] if i > 0 else 0
        nxt = cs[i + 1] if i + 1 < len(cs) else 0
        left = arabic_letter(prev) and joins_both(prev) and prev not in RIGHT_ONLY     # соединение с прошлой
        right = arabic_letter(nxt) and c not in RIGHT_ONLY and joins_both(c)           # с следующей
        form = "medial" if left and right else "final" if left else "initial" if right else "isolated"
        out.append(FORMS.get((c, form), FORMS.get((c, "isolated"), c)))
    return "".join(map(chr, out))


def visual(text: str) -> str:
    """Упрощённый bidi для строки: если в ней есть письмо справа налево — строка RTL: порядок
    кусков обратный, буквы внутри RTL-кусков — задом наперёд, латиница и цифры — как есть."""
    rtl = lambda ch: ud.bidirectional(ch) in ("R", "AL")
    if not any(rtl(ch) for ch in text):
        return text
    text = shape_arabic(text)
    runs, cur, cur_rtl = [], "", None
    for ch in text:
        d = ud.bidirectional(ch)
        r = True if d in ("R", "AL", "NSM") else False if d == "L" or d in ("EN", "AN") else cur_rtl
        if r is None:
            r = True
        if cur and r != cur_rtl:
            runs.append((cur, cur_rtl)); cur = ""
        cur += ch; cur_rtl = r
    if cur:
        runs.append((cur, cur_rtl))
    return "".join(s[::-1] if r else s for s, r in reversed(runs))


def main() -> int:
    pack = Pack(ROOT / "dist" / "TSAFARI" / "TSAFARI.FNT")
    W, LINE = 1024, 36
    sheet = Image.new("RGB", (W, 16 + LINE * len(SAMPLES)), (247, 242, 232))
    for row, sample in enumerate(SAMPLES):
        text = visual(sample)
        x, y = 8, 8 + row * LINE
        layer = Image.new("L", sheet.size)
        for ch in text:
            g = pack.glyph(ord(ch))
            if g is None:
                img = base_glyph(ch)
                if x + 12 > W:
                    break
                layer.paste(Image.composite(img, layer.crop((x, y, x + 12, y + CELL_H)), img), (x, y))
                x += 12
                continue
            adv, xoff, top, img = g
            gx, gy = x + xoff, y + top
            if gx + img.width > W:
                break
            layer.paste(Image.composite(img, layer.crop((gx, gy, gx + img.width, gy + img.height)), img), (gx, gy))
            x += adv
        sheet.paste(Image.new("RGB", sheet.size, (31, 42, 46)), (0, 0), layer)
    out = ROOT / "build" / "fontpack_preview.png"
    out.parent.mkdir(exist_ok=True)
    sheet.save(out)
    print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
