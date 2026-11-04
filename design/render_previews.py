#!/usr/bin/env python3
"""Этап 1 TSafari: повторяемые PNG, собственные иконки и анимация глобуса.

Только Pillow и стандартная библиотека. Это макет, не код браузера или FT812.
Все выходные файлы находятся рядом со скриптом, временные — только на E:.
"""
from __future__ import annotations

import hashlib
import html
import json
import math
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent
TMP = Path(r"E:\zx\cache\tmp\codex_tsafari_ui")
os.environ["TEMP"] = os.environ["TMP"] = str(TMP)
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"

from PIL import Image, ImageChops, ImageDraw, ImageFont

PREV = ROOT / "previews"
ICONS = ROOT / "icons"
REPORTS = ROOT / "reports"
FONT = ROOT / "fonts" / "NotoSansMono-Regular.ttf"
W, H = 1024, 768
RAW = {
    "paper": "F3F0E7", "white": "FFFCF4", "ink": "20363D",
    "muted": "566A70", "line": "CFD7D2", "soft": "E4E9DF",
    "navy": "142832", "panel": "223D48", "edge": "3C5963",
    "mint": "75DDC4", "amber": "F4BD75", "ui_text": "EFF5ED",
    "ui_muted": "B2C5C9", "disabled": "637780", "hover": "365661",
    "pressed": "395F61", "selection": "376C70", "link": "096D70",
    "visited": "755589", "link_bg": "D5E9DC", "red": "AD453C",
    "error_bg": "F3DFD3", "code": "E1E6DC",
}


def rgb565(value):
    if isinstance(value, str):
        value = tuple(bytes.fromhex(value))
    r, g, b = value[:3]
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def expand565(v):
    r, g, b = v >> 11, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


C = {k: expand565(rgb565(v)) for k, v in RAW.items()}
FONTS = {}
GLYPHS = {}
BOUNDS = []


def font(size):
    if size not in FONTS:
        FONTS[size] = ImageFont.truetype(str(FONT), size)
    return FONTS[size]


def advance(size):
    return round(size * .6)


def quantize565(im):
    r, g, b = im.convert("RGB").split()
    lut5 = [((i >> 3) << 3) | (i >> 5) for i in range(256)]
    lut6 = [((i >> 2) << 2) | (i >> 6) for i in range(256)]
    return Image.merge("RGB", (r.point(lut5), g.point(lut6), b.point(lut5)))


def quantize4(im, mask=False):
    parts = im.convert("RGBA").split()
    lut = [min(15, round(i / 17)) * 17 for i in range(256)]
    if mask:
        return Image.merge("RGBA", (Image.new("L", im.size, 255),) * 3 + (parts[3].point(lut),))
    return Image.merge("RGBA", tuple(p.point(lut) for p in parts))


class Canvas:
    """Плоские RECTS/POINTS/LINES и маски; сглаживание — приближение EVE."""
    def __init__(self, bg="paper", size=(W, H)):
        self.im = Image.new("RGB", size, C[bg])
        self.d = ImageDraw.Draw(self.im)

    def box(self, x, y, w, h, color="white", radius=0, border=None, bw=1):
        scale = 4
        mask = Image.new("L", (w * scale, h * scale))
        md = ImageDraw.Draw(mask)
        md.rounded_rectangle((0, 0, w * scale - 1, h * scale - 1), radius=radius * scale, fill=255)
        mask = mask.resize((w, h), Image.Resampling.LANCZOS)
        self.im.paste(C[border or color], (x, y), mask)
        if border:
            self.box(x + bw, y + bw, w - 2 * bw, h - 2 * bw, color, max(0, radius - bw))

    def line(self, x1, y1, x2, y2, color="line", width=1):
        self.d.line((x1, y1, x2, y2), fill=C[color], width=width)

    def dot(self, x, y, radius, color):
        self.box(x - radius, y - radius, radius * 2, radius * 2, color, radius)

    def text(self, x, y, text, size=20, color="ink", bold=False, audit=True):
        # TXTVIEW2: ячейка 12x34, baseline=26 при 20 px. Общая baseline,
        # а не верх индивидуального bbox. Другие размеры масштабируют метрики.
        step = advance(size)
        height = math.ceil(34*size/20)
        baseline = round(26*size/20)
        for n, ch in enumerate(text):
            key=(size,ch,bold)
            if key not in GLYPHS:
                tile=Image.new("L",(step,height))
                td=ImageDraw.Draw(tile)
                td.text((0,baseline),ch,font=font(size),fill=255,anchor="ls")
                if bold:td.text((1,baseline),ch,font=font(size),fill=255,anchor="ls")
                GLYPHS[key]=tile.point([min(15,(i+8)//17)*17 for i in range(256)])
            self.im.paste(C[color],(x+n*step,y),GLYPHS[key])
        if audit:BOUNDS.append((x, y, len(text) * step, height, text))
        return len(text) * step

    def lines(self, x, y, lines, size=20, color="ink", leading=34):
        for n, line in enumerate(lines):
            self.text(x, y + n * leading, line, size, color)

    def link(self, x, y, label, visited=False, hover=False, size=20):
        width = len(label) * advance(size)
        baseline=round(26*size/20)
        if hover:
            self.box(x - 2, y+8, width + 4, size + 10, "link_bg", 3)
        self.text(x, y, label, size, "visited" if visited else "link")
        self.line(x, y+baseline+3, x+width, y+baseline+3, "visited" if visited else "link")

    def icon(self, name, x, y, color="ui_text"):
        src = ASSETS[name]
        self.im.paste(C[color], (x, y), src.getchannel("A"))

    def cursor(self, x, y):
        # Координаты — острие. Исходник 38x38 не масштабируем и не меняем.
        self.im.paste(CURSOR, (x - 1, y - 1), CURSOR)

    def page_done(self):
        self.im = quantize565(self.im)
        self.d = ImageDraw.Draw(self.im)


def make_icon(name):
    s = 4
    im = Image.new("RGBA", (24 * s, 24 * s))
    d = ImageDraw.Draw(im)
    fg = (255, 255, 255, 255)

    def line(points, width=2):
        p = [(int(x * s), int(y * s)) for x, y in points]
        d.line(p, fill=fg, width=width * s, joint="curve")
        for x, y in (p[0], p[-1]):
            r = width * s / 2
            d.ellipse((x-r, y-r, x+r, y+r), fill=fg)

    def rect(x, y, w, h, fill=False):
        d.rectangle((x*s, y*s, (x+w)*s, (y+h)*s), fill=fg if fill else None, outline=fg, width=2*s)

    if name in ("back", "forward"):
        points = [(13, 5), (6, 12), (13, 19)]
        if name == "forward": points = [(24-x, y) for x, y in points]
        line(points)
        line([(7 if name == "back" else 17, 12), (19 if name == "back" else 5, 12)])
    elif name == "stop":
        rect(6, 6, 12, 12, True)
    elif name == "reload":
        points = [(12+7*math.cos(a), 12+7*math.sin(a)) for a in [math.radians(35+i*14) for i in range(21)]]
        line(points)
        line([(13, 4), (19, 5), (18, 11)])
    elif name == "go":
        line([(4, 12), (19, 12)])
        line([(13, 6), (19, 12), (13, 18)])
    elif name == "close":
        line([(6, 6), (18, 18)]); line([(18, 6), (6, 18)])
    elif name == "settings":
        for yy, xx in [(6, 8), (12, 16), (18, 10)]:
            line([(4, yy), (20, yy)], 1)
            d.ellipse(((xx-2)*s, (yy-2)*s, (xx+2)*s, (yy+2)*s), fill=fg)
    elif name == "bookmark":
        line([(7, 20), (7, 4), (17, 4), (17, 20), (12, 16), (7, 20)], 1)
    elif name == "history":
        d.ellipse((4*s, 4*s, 20*s, 20*s), outline=fg, width=2*s)
        line([(12, 7), (12, 12), (16, 14)], 1)
    elif name == "search":
        d.ellipse((4*s, 4*s, 15*s, 15*s), outline=fg, width=2*s)
        line([(15, 15), (20, 20)])
    elif name == "download":
        line([(12, 3), (12, 15)]); line([(7, 10), (12, 15), (17, 10)])
        line([(5, 17), (5, 21), (19, 21), (19, 17)], 1)
    elif name == "check":
        line([(4, 12), (10, 18), (20, 6)])
    elif name == "warning":
        line([(12, 3), (22, 20), (2, 20), (12, 3)], 1)
        line([(12, 8), (12, 13)]); rect(11, 16, 2, 2, True)
    elif name == "image":
        rect(3, 4, 18, 16)
        line([(4, 18), (10, 12), (14, 16), (17, 12), (20, 15)], 1)
        d.ellipse((6*s, 7*s, 9*s, 10*s), fill=fg)
    elif name == "chevron":
        line([(6, 9), (12, 15), (18, 9)])
    elif name == "service":
        line([(8, 5), (4, 5), (4, 19), (8, 19)], 1)
        line([(16, 5), (20, 5), (20, 19), (16, 19)], 1)
        line([(13, 8), (10, 16)], 2)
    return quantize4(im.resize((24, 24), Image.Resampling.LANCZOS), mask=True)


def make_globes():
    # Собственная схематичная карта: материки нарисованы вручную, без внешних изображений.
    world = Image.new("L", (360, 180))
    d = ImageDraw.Draw(world)
    continents = [
        [(-166,63),(-135,72),(-105,63),(-77,55),(-58,48),(-82,22),(-100,15),(-117,32),(-145,48)],
        [(-82,12),(-59,9),(-35,-7),(-49,-31),(-70,-55),(-76,-18)],
        [(-17,35),(9,38),(34,28),(49,10),(34,-25),(20,-35),(3,-17),(-13,5)],
        [(-10,37),(-8,60),(24,72),(60,65),(92,76),(150,60),(174,51),(141,36),(120,18),(105,1),(77,7),(53,26),(29,35)],
        [(111,-12),(137,-10),(154,-26),(143,-39),(116,-34)],
        [(-55,60),(-30,66),(-38,81),(-61,79)],
    ]
    for points in continents:
        d.polygon([(x+180, 90-y) for x,y in points], fill=255)
    frames = []
    s, radius = 4, 14
    for frame in range(12):
        im = Image.new("RGBA", (32*s, 32*s))
        px, mp = im.load(), world.load()
        for yy in range(32*s):
            for xx in range(32*s):
                nx = ((xx+.5)/s-16)/radius
                ny = (16-(yy+.5)/s)/radius
                z2 = 1-nx*nx-ny*ny
                if z2 < 0: continue
                latitude = math.asin(ny)
                longitude = math.atan2(nx, math.sqrt(z2))
                lon = (math.degrees(longitude) + frame*30 + 180) % 360
                lat = max(0, min(179, int(90-math.degrees(latitude))))
                land = mp[int(lon), lat] != 0
                grid = abs(math.sin(3*(longitude+math.radians(frame*30)))) < .055 or abs(ny) < .025
                alpha = 255 if land else 68 if grid else 0
                if z2 < .07: alpha=255
                px[xx, yy] = (255,255,255,alpha)
        frames.append(quantize4(im.resize((32,32), Image.Resampling.LANCZOS),mask=True))
    return frames


ICON_NAMES = "back forward stop reload go close settings bookmark history search download check warning image chevron service".split()
ASSETS = {}
CURSOR = None
SCENES = []


INPUT_LABELS = {"input_en":"EN", "input_ru":"RU"}


def make_input_label(label):
    # Собственная рамка и Noto Sans Mono; готовая маска, атлас 16 px в ELF не нужен.
    w,h,s=32,22,4
    frame=Image.new("L",(w*s,h*s))
    ImageDraw.Draw(frame).rounded_rectangle((2,2,w*s-3,h*s-3),radius=4*s,outline=255,width=s)
    alpha=frame.resize((w,h),Image.Resampling.LANCZOS)
    for n,ch in enumerate(label):
        tile=Image.new("L",(10,h))
        ImageDraw.Draw(tile).text((0,17),ch,font=font(16),anchor="ls",fill=255)
        letter=Image.new("L",(w,h));letter.paste(tile,(6+10*n,0))
        alpha=ImageChops.lighter(alpha,letter)
    im=Image.new("RGBA",(w,h),(255,255,255,0))
    im.putalpha(alpha.point([min(15,(v+8)//17)*17 for v in range(256)]))
    return im


def toolbar(c, *, opened=False, selection=False, suggestions=False, loading=False,
            first=False, hover=None, pressed=None, globe_frame=0, service=False,
            url="https://zx-notes.example/evo/field-notes", long_url=False,
            nav=0.0, address=None, input_language=None, caret_index=None, address_focused=True):
    if opened or suggestions:nav=1.0
    if address is None:address=1.0 if opened else 0.0
    shift=round(184*(1-nav))
    globe_x=28+round(184*nav)
    if nav>0:c.box(8-shift,8,176,40,"navy",10)
    for name, cx in zip(("back", "forward", "stop", "reload"), (30,74,118,162)):
        if nav<=0:break
        cx-=shift
        disabled = (first and name in ("back", "forward")) or (name == "stop" and not loading) or (name == "reload" and loading)
        if name == hover and not disabled: c.dot(cx, 28, 18, "hover")
        if name == pressed and not disabled: c.dot(cx, 28, 18, "pressed")
        color = "disabled" if disabled else "amber" if name == "stop" else "ui_text"
        c.icon(name,cx-12+int(name==pressed),16+int(name==pressed),color)
    if address>0:
        # Настоящий сдвиг тела поля вправо с отсечением слева под глобусом.
        # В целевом интерфейсе это SCISSOR + сдвиг вершин, не большая текстура.
        a=Canvas();a.im=c.im.copy();a.d=ImageDraw.Draw(a.im)
        dx=-round(784*(1-address))
        a.box(192+dx,8,824,40,"navy",20,"mint",1)
        a.line(236+dx,18,236+dx,38,"edge")
        if long_url:
            url = ".../2026/evolution/notes?view=plain&lang=en"
        if selection:
            a.box(244+dx+12*8,15,12*16,27,"selection",2)
        a.text(244+dx,8,url,20,"ui_text",audit=False)
        caret_column=(24 if selection else len(url)) if caret_index is None else caret_index
        caret=244+dx+12*caret_column
        a.line(caret,17,caret,39,"mint",2)
        if input_language and address_focused and not selection and caret_column==len(url) and address==1:
            assert input_language in ("en","ru")
            # Последние 38 px помещаются в существующий зазор перед Go.
            assert caret+6+32<=930
            a.icon("input_"+input_language,caret+6,17,"mint")
        a.icon("go",938+dx,16,"mint")
        a.line(972+dx,18,972+dx,38,"edge")
        if hover=="settings":a.dot(996+dx,28,17,"hover")
        a.icon("settings",984+dx,16,"ui_text")
        c.im.paste(a.im.crop((globe_x,8,1016,48)),(globe_x,8))
    # Альфа применяется только к маленькой кнопке 40x40 и её битмапу.
    opacity=round(112+143*nav) if hover!="globe" else 220
    old=c.im.crop((globe_x-20,8,globe_x+20,48))
    c.dot(globe_x,28,20,"navy")
    if opacity<255:
        new=c.im.crop((globe_x-20,8,globe_x+20,48))
        c.im.paste(Image.blend(old,new,opacity/255),(globe_x-20,8))
    # COLOR_A применяется к каждому примитиву по отдельности, как у FT812.
    globe_mask=ASSETS[f"globe_{globe_frame:02}"].getchannel("A")
    globe_mask=globe_mask.point([(v*opacity+127)//255 for v in range(256)])
    c.im.paste(C["mint"],(globe_x-16,12),globe_mask)
    if service:
        # Сведения о преобразовании — локальная метка в самой странице.
        c.box(906,710,102,40,"panel",6)
        c.icon("service",914,718,"mint")
        c.text(946,718,"JS",20,"ui_text")
    if suggestions:
        c.box(236,54,780,152,"navy",8,"edge")
        c.box(244,62,764,44,"panel",6)
        for n, (icon, label) in enumerate([("bookmark", "ZX Evolution / field notes"), ("history", "Eight-bit reading room"), ("history", "TSafari / getting started")]):
            y=66+n*46
            c.icon(icon,252,y+4,"mint" if n==0 else "ui_muted")
            # В целевом дизайне каждая строка — обновляемый ESP кэш L4 320x34.
            c.text(288,y,label,20,"ui_text" if n==0 else "ui_muted")
        c.icon("go",978,72,"mint")


def scrollbar(c, drag=False, pos=0, length=190):
    c.box(1012,8,4,752,"soft",2)
    c.box(1010 if drag else 1012,8+pos,6 if drag else 4,length,"link" if drag else "muted",3 if drag else 2)


def status(c, text):
    label=text if len(text)<=82 else text[:50]+"..."+text[-29:]
    width=min(1008,24+12*len(label))
    c.box(8,720,width,40,"navy",6)
    c.text(20,724,label,20,"ui_text")


def button(c, x, y, label, width=None, primary=True):
    width = width or (len(label)*11+32)
    c.box(x,y,width,44,"navy" if primary else "white",8, None if primary else "line")
    c.text(x+(width-len(label)*11)//2,y+7,label,18,"ui_text" if primary else "ink")


def field(c, x,y,w,value,focus=False):
    c.box(x,y,w,44,"white",6,"link" if focus else "line",2 if focus else 1)
    c.text(x+14,y+7,value,18,"ink")


def page_heading(c, tag, title, subtitle=None):
    c.text(56,8,tag,14,"link")
    c.text(8,44,title,32,"ink")
    if subtitle:c.text(8,100,subtitle,18,"muted")
    c.line(8,144,1008,144)


def page_footer(c, left="ZX NOTES  /  A small web, well read", right="01 / 03"):
    c.line(8,716,1008,716)
    c.text(8,730,left,14,"muted")
    c.text(936,730,right,14,"muted")


def article(c, hover=False, scrolled=False):
    if scrolled:
        c.lines(8,8,["У хорошей страницы есть ритм: заголовок, короткий абзац, ссылка.","На экране остаётся место для самой мысли. A small page is still a complete page."])
        c.text(8,90,"02. Keep the page readable",26)
        for n,line in enumerate(["Keep a local copy of the manual.","Сохраняйте адрес вместе с заметкой.","Return to the source when details change."]):
            c.dot(14,168+n*38,3,"link"); c.text(32,146+n*38,line)
        c.box(8,280,1000,86,"soft",4);c.box(8,280,3,86,"link")
        c.lines(24,292,["«Маленький компьютер учит выбирать главное.»","One page, one idea, room to explore."],20)
        c.text(8,386,"Read with the keyboard nearby",26)
        c.lines(8,444,["В блокноте важна не только запись, но и путь обратно к источнику.","Положите рядом ссылку на проект и короткое описание результата.","Так следующий вечер начнётся с идеи, а не с поиска потерянного адреса.","", "Keep the page small enough to understand at a glance.","Let the notes grow one useful detail at a time."])
        c.link(8,688,"Back to the first note")
        return
    c.link(56,8,"ZX notes / Index",hover=hover=="top")
    c.text(356,10,"FIELD NOTES / 04 NOV 2026",14,"muted")
    c.text(8,54,"ZX Evolution: a window to the web",32)
    c.text(8,112,"A personal notebook / Личные заметки о маленьком компьютере",18,"muted")
    c.lines(8,160,["У ZX Spectrum был простой способ пригласить к работе: курсор на пустой строке.","Здесь приглашение продолжается — открой адрес, прочитай заметку, перейди по ссылке.","A tiny machine can still open a wide window. Start with something worth reading."])
    c.text(8,282,"01. Small screen, clear ideas",26)
    c.lines(8,334,["Нам нравится веб, в котором содержание видно сразу. Текст остаётся текстом,", "а ссылки ведут к новым находкам. В блокноте рядом с ZX Evolution есть место", "для коротких программ, заметок о железе и идей на следующий вечер."])
    c.link(8,451,"Read the build notes",hover=hover is True)
    c.link(356,451,"Архив заметок",visited=True)
    c.box(8,506,1000,82,"soft",4);c.box(8,506,3,82,"link")
    c.text(24,514,"«Любопытство не измеряется мегагерцами.»",20)
    c.text(24,551,"From the workbench / Запись из мастерской",14,"muted")
    c.text(8,602,"A page for the next evening",22)
    c.lines(8,648,["Сохраните полезный адрес вместе с пояснением: что здесь стоит попробовать.","Keep one useful idea, then follow the next link. The rest of the screen is yours.","Следующая запись: маленькая программа, которую легко прочитать целиком."])


def typography(c):
    page_heading(c,"ZX NOTES / READING ROOM", "A notebook for an eight-bit day", "Заголовок h1 / 32 px — начало собственной статьи")
    c.lines(8,166,["Открываем блокнот рядом с ZX Evolution. Здесь живут адреса и короткие программы.","Запишем одну идею для следующего вечера. One useful detail at a time."])
    c.text(8,260,"01. Собираем наблюдения",26)
    c.text(8,318,"Read, try, remember",22)
    c.text(8,372,"Главная мысль",20,bold=True)
    c.text(188,372,"должна быть видна и без нового шрифта.",20)
    c.link(8,424,"Новая ссылка")
    c.link(300,424,"Visited link",visited=True)
    c.link(600,424,"Hovered link",hover=True)
    for n,t in enumerate(["Запишите адрес источника.","Сохраните маленький пример."]):
        c.dot(14,504+38*n,3,"link");c.text(32,482+38*n,t)
    c.text(600,482,"1. Read the source.")
    c.text(600,520,"2. Try one change.")
    c.box(8,590,1000,88,"soft",4);c.box(8,590,3,88,"link")
    c.lines(24,602,["«Хорошая заметка переживает один вечер.»","Keep the useful part. Leave space for the next idea."],20)
    page_footer(c,right="01 / 03")


def table_images(c):
    page_heading(c,"ZX NOTES / READING ROOM", "02. A shelf for small discoveries", "Таблица, иллюстрация и состояния загрузки изображений")
    rows=[("Notebook","Keep","Зачем"),("Build notes","Text + link","Повторить опыт"),("Screen study","PNG + caption","Сравнить идеи")]
    for r,row in enumerate(rows):
        y=166+r*44
        c.box(8,y,1000,44,"soft" if r==0 else "white")
        for x,t in zip((20,352,686),row):c.text(x,y+7,t,20,"ink")
        c.line(8,y+43,1008,y+43)
    c.line(340,166,340,298);c.line(674,166,674,298)
    c.text(8,322,"A small picture can carry a whole evening.",20,"muted")
    # Иллюстрация статьи — содержимое RGB565, а не UI-ресурс RAM_G.
    c.box(8,384,320,196,"navy",6)
    for n,col in enumerate(["red","amber","mint","edge"]):
        c.box(32+n*14,410,10,16,col)
    c.text(32,451,"10 PRINT",20,"ui_text")
    c.text(32,489,'"HELLO, EVO"',20,"mint")
    c.text(8,596,"A note from the screen",14,"muted")
    for x,failed in ((348,False),(688,True)):
        c.box(x,384,320,196,"soft",6,"line")
        c.icon("image",x+148,410,"muted")
        c.text(x+24,465,"Image unavailable" if failed else "Loading image...",20,"muted")
        c.text(x+24,515,"board-photo.png",14,"muted")
        c.text(x,596,"Alt: плата на столе",14,"muted")
    page_footer(c,right="02 / 03")


def forms(c):
    page_heading(c,"ZX NOTES / READING ROOM", "03. Try a line, leave a note", "Код и простая форма / pre, input, button, checkbox, radio, select")
    c.box(8,166,1000,104,"code",4)
    c.lines(24,178,['10 PRINT "HELLO, ZX EVOLUTION"',"20 GO TO 10"],20)
    c.text(8,292,"Небольшой пример удобно хранить рядом с описанием.",20)
    c.line(8,350,1008,350,"muted")
    c.text(8,374,"Your note",18,"muted")
    field(c,8,414,774,"Keep the next idea here",True)
    button(c,800,414,"Save note",208)
    c.box(10,500,20,20,"link",3)
    c.icon("check",8,498,"white")
    c.text(44,490,"Keep a local copy",18)
    for x,label,checked in ((480,"Text",True),(690,"Text + images",False)):
        c.dot(x+10,510,10,"muted");c.dot(x+10,510,8,"white")
        if checked:c.dot(x+10,510,5,"link")
        c.text(x+30,490,label,18)
    c.text(8,565,"Notebook",18,"muted")
    field(c,160,558,400,"Workbench / мастерская")
    c.icon("chevron",526,568,"ink")
    page_footer(c,right="03 / 03")


def home(c):
    c.text(56,8,"A LITTLE WINDOW. A WIDE WORLD.",14,"link")
    c.text(8,60,"TSafari",56)
    c.text(8,166,"Start with a question or an address.",20,"muted")
    field(c,8,230,834,"");c.icon("search",24,240,"muted")
    c.text(56,237,"Search the web or enter an address",18,"muted")
    button(c,860,230,"Search",148)
    c.text(8,326,"BOOKMARKS",14,"link")
    for i,(title,desc) in enumerate([("ZX field notes","zx-notes.example"),("Reading room","reading.example"),("Workbench","workbench.example")]):
        x=8+i*340
        c.box(x,370,320,144,"white",6,"line")
        c.icon("bookmark",x+18,390,"link")
        c.text(x+18,429,title,20)
        c.text(x+18,476,desc,14,"muted")
    c.line(8,588,1008,588)
    c.text(8,610,"Win / Cmd",18,"ink")
    c.text(156,610,"Open the address bar",18,"muted")
    c.text(8,664,"Top-left corner: reveal navigation. Click the globe: enter an address.",18,"muted")


ERRORS = {
    "offline":("NETWORK / OFFLINE","No network connection",["TSafari cannot reach the network.","Check the connection, then try again."],"Retry","Open settings","No response from the network"),
    "dns":("NETWORK / DNS","Address not found",["The host name could not be resolved.","Check the spelling or try another address."],"Edit address","Try again","zx-notse.example"),
    "tls":("CONNECTION / TLS","Secure connection failed",["The TLS connection could not be established.","Check the device clock and the server certificate."],"Try again","Go back","The page was not opened"),
    "404":("HTTP / 404","This page is missing",["The server is here, but this page is not.","The link may have changed or the page was removed."],"Go back","Edit address","https://zx-notes.example/old-notes"),
}


def error_page(c,key):
    tag,title,lines,primary,secondary,detail=ERRORS[key]
    c.text(56,8,tag,16,"red")
    c.box(8,70,48,48,"error_bg",8);c.icon("warning",20,82,"red")
    c.text(76,68,title,32)
    c.lines(8,160,lines,20)
    c.box(8,280,1000,68,"white",6,"line")
    c.text(24,298,detail,18,"muted")
    button(c,8,394,primary,224);button(c,248,394,secondary,224,False)
    c.line(8,520,1008,520)
    c.text(8,544,"Your previous page and bookmarks are still available.",18,"muted")


def js_request(c):
    page_heading(c,"PAGE / JAVASCRIPT", "This page needs JavaScript", "https://workbench.example/interactive")
    c.lines(8,170,["TSafari can ask a JS service to prepare a readable page.","The service receives this page address.","Continue only if you want to use that service."],20)
    c.box(8,316,1000,100,"white",6,"line")
    c.text(24,330,"JS SERVICE",14,"link");c.text(24,366,"https://render.example/",20)
    button(c,8,462,"Open via JS service",278)
    button(c,302,462,"Stay on this page",256,False)
    c.text(8,564,"Pages prepared by the service carry a JS badge.",18,"muted")
    c.text(8,604,"You can change the service address in Settings.",18,"muted")


def download(c,mode="progress"):
    page_heading(c,"DOWNLOADS / EVO CARD", "A file for your workbench", "build-notes.zip")
    c.box(8,166,1000,260,"white",6,"line")
    c.icon("check" if mode=="done" else "download",24,190,"link")
    c.text(64,180,"Download complete" if mode=="done" else "Downloading file",22)
    c.text(24,246,"SAVE TO",14,"muted")
    c.text(24,280,"\\zifi\\downloads\\2026-11-04\\",20)
    c.text(24,320,"build-notes.zip",18,"muted")
    c.box(24,382,968,8,"soft",4)
    if mode == "unknown":
        # Неизвестный объём: количественной шкалы нет.
        c.box(24,370,968,32,"white")
        c.text(24,370,"Total size not provided by server",18,"muted")
    else:
        c.box(24,382,968 if mode=="done" else 484,8,"link",4)
    c.text(8,462,"1.50 MiB saved" if mode=="done" else "768 KiB received" if mode=="unknown" else "768 KiB / 1.50 MiB",20)
    if mode!="unknown":c.text(936,454,"100%" if mode=="done" else "50%",24,"link")
    button(c,8,530,"Back to page" if mode=="done" else "Cancel download",242)
    c.text(8,628,"Saved on the Evo card." if mode=="done" else "Keep the Evo card available until the download finishes.",18,"muted")


def settings(c):
    page_heading(c,"TSAFARI / PREFERENCES", "Make room for your web", "Settings are saved on this device.")
    settings_fields=[(166,"JS service","https://render.example/"),(286,"Home page","tsafari:home"),(406,"Search engine","https://search.example/?q={query}")]
    for y,label,value in settings_fields:
        c.text(8,y,label,18,"muted");field(c,8,y+38,1000,value,label=="JS service")
    c.text(8,518,"{query} is replaced with your URL-encoded search text.",16,"muted")
    button(c,8,586,"Save settings",224);button(c,248,586,"Cancel",160,False)
    c.text(8,698,"No JS service is contacted until you choose to use it.",16,"muted")


def save_scene(filename, title, page_fn, *, pointer=(702,294), scroll=False,
               drag=False, scroll_pos=0, status_text=None, **toolbar_args):
    c=Canvas()
    start=len(BOUNDS)
    page_fn(c)
    c.page_done()
    toolbar(c,**toolbar_args)
    if scroll:scrollbar(c,drag,scroll_pos)
    if status_text:status(c,status_text)
    if pointer:c.cursor(*pointer)
    c.im.save(PREV/filename)
    SCENES.append({"file":filename,"title":title,"size":[W,H]})
    bad=[t for t in BOUNDS[start:] if t[0]<8 or t[1]<8 or t[0]+t[2]>1016 or t[1]+t[3]>760]
    if bad:raise ValueError(f"Текст вышел за безопасную зону в {filename}: {bad}")
    return c.im


WIFI_SUBTITLE = "Web browser for VDAC4 ZX Evolution"


def make_wifi_title():
    # Готовая маска: 32 px собираются на ПК, атлас 32 px в ELF не нужен.
    im=Image.new("RGBA",(144,55),(255,255,255,0))
    alpha=Image.new("L",im.size)
    for n,ch in enumerate("TSafari"):
        tile=Image.new("L",(19,55))
        ImageDraw.Draw(tile).text((0,42),ch,font=font(32),anchor="ls",fill=255)
        alpha.paste(tile,(5+n*19,0))
    im.putalpha(alpha.point([min(15,(v+8)//17)*17 for v in range(256)]))
    return im


def wifi_frame(failed=False, frame=0):
    # Стартовое модальное окно: непрозрачный RECTS, четыре текстовые маски.
    # Фон до подключения пустой, без полноэкранного полупрозрачного слоя.
    c=Canvas()
    c.box(256,204,512,360,"navy",16,"edge")
    c.icon("wifi_title",440,226,"ui_text")
    c.text((W-len(WIFI_SUBTITLE)*12)//2,284,WIFI_SUBTITLE,20,"ui_muted")
    c.dot(512,350,24,"panel")
    if failed:c.icon("warning",500,338,"amber")
    else:c.icon(f"globe_{frame:02}",496,334,"mint")
    label="Unable to connect." if failed else "Connecting to Wi-Fi..."
    c.text((W-len(label)*12)//2,390,label,20,"ui_text")
    hint="Retrying automatically." if failed else "Please wait."
    c.text((W-len(hint)*12)//2,456,hint,20,"ui_muted")
    c.text(410,490,"F12: Exit browser",20,"ui_muted")
    c.cursor(818,620)
    return c.im


def wifi_scenes():
    for filename,title,im in (
        ("14a_wifi_connecting.png","Старт: подключение к Wi-Fi",wifi_frame()),
        ("14b_wifi_unavailable.png","Wi-Fi недоступен: автоповтор и выход F12",wifi_frame(True)),
        ("14c_wifi_ready.png","Сеть появилась: окно закрыто, стартовая страница",Image.open(PREV/"09_home.png"))):
        im.save(PREV/filename)
        SCENES.append({"file":filename,"title":title,"size":[W,H]})
    return [wifi_frame(frame=n) for n in range(12)]


def input_language_scenes():
    for lang,label in (("en","Латиница / EN"),("ru","Русская раскладка / RU")):
        save_scene(f"15{'a' if lang=='en' else 'b'}_input_{lang}.png",label+": каретка после vtrd.in",
                   article,opened=True,url="vtrd.in",input_language=lang,pointer=(607,37))
    c=Canvas("navy")
    c.text(32,24,"Input language / Ctrl + Shift",26,"ui_text")
    c.text(32,80,"Shown to the right of the caret at the end of the address.",18,"ui_muted")
    for x,lang,label in ((120,"en","EN / Latin"),(568,"ru","RU / Russian")):
        c.text(x,140,label,26,"ui_text")
        c.text(x,200,"1:1",18,"ui_muted");c.icon("input_"+lang,x+92,207,"mint")
        c.text(x,252,"8x / nearest pixel",18,"ui_muted")
        mask=ASSETS["input_"+lang].getchannel("A").resize((256,176),Image.Resampling.NEAREST)
        c.im.paste(C["mint"],(x,298),mask)
    c.text(32,526,"32 x 22 px / gap 4 px / L4 / 352 bytes each",20,"ui_text")
    c.text(32,572,"Address examples / 1:1",18,"ui_muted")
    for y,lang in ((616,"en"),(682,"ru")):
        strip=Canvas();toolbar(strip,opened=True,url="vtrd.in",input_language=lang)
        c.im.paste(strip.im.crop((192,8,1016,48)),(100,y))
    c.im.save(PREV/"15c_input_language_detail.png")
    SCENES.append({"file":"15c_input_language_detail.png","title":"Метки EN/RU: размер 1:1 и увеличение 8x","size":[W,H]})


def budget_report():
    # После этапа 2 не возвращаем старую схему памяти при пересборке PNG.
    if (ROOT/"ft/assets.json").exists():
        from build_ft_assets import build
        build()
        return json.loads((REPORTS/"budget.json").read_text(encoding="utf-8"))
    # Консервативная проектная модель, не измерение готового дисплей-листа.
    # Общая группа учитывает SAVE/RESTORE, состояния, настройки bitmap и курсор.
    common={"frame_state":8,"nav_background_and_states":23,"nav_icons":11,
            "globe":10,"settings":12,"scrollbar":14,"cursor":9}
    base=sum(common.values())
    estimates={
        "idle_globe_and_cursor":29,
        "navigation_open":base,
        "link_hover_max_82_glyphs":base+18+82,
        "address_edit_max_54_glyphs":base+26+54,
        "suggestions_max_address":base+26+54+42,
        "suggestions_with_js_badge":base+26+54+42+16,
        "loading":base+8,
        "drag_scrollbar":base+8,
        "js_service_badge":base+16,
    }
    reserve=24
    assets=[
        {"name":"Глобус: 12 кадров L4 32×32, цвет COLOR_RGB","bytes":12*32*32//2},
        {"name":"16 иконок L4 24×24","bytes":16*24*24//2},
        {"name":"Курсор Zuma ARGB4 38×38","bytes":38*38*2},
        {"name":"UI-шрифт 20 px: ASCII 96, ячейка 12×34 L4","bytes":96*12*34//2},
        {"name":"Кэш 16 Unicode-глифов 12×34 L4","bytes":16*12*34//2},
        {"name":"3 строки подсказок: 320×34 L4","bytes":3*320*34//2},
        {"name":"Выравнивание и служебный запас","bytes":1024},
    ]
    total=sum(x["bytes"] for x in assets)
    data={"kind":"Оценка этапа 1, не аппаратное измерение","dl_common":common,
          "dl_common_total":base,"dl_state_estimates":estimates,"dl_reserve_words":reserve,
          "dl_worst_with_reserve":max(estimates.values())+reserve,
          "ram_g_assets":assets,"ram_g_total_bytes":total,"ram_g_free_to_64k":65536-total}
    assert data["dl_worst_with_reserve"]<=250
    assert total<=65536
    (REPORTS/"budget.json").write_text(json.dumps(data,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    return data


def review_materials(animations):
    # Обзор также 1024x768; исходные состояния всегда просматриваются 1:1.
    overview=Canvas("navy")
    overview.text(24,18,"TSafari / Stage 1",26,"ui_text")
    accepted_scenes=SCENES[:37]  # Принятый обзор этапа 1 сохраняется побайтно.
    overview.text(24,56,f"{len(accepted_scenes)} previews / 1024 x 768 / open index.html for full size",14,"ui_muted")
    rows=math.ceil(len(accepted_scenes)/6);ch=660//rows;th=ch-12;tw=round(th*4/3)
    for n,item in enumerate(accepted_scenes):
        x=16+(n%6)*168;y=90+(n//6)*ch
        small=Image.open(PREV/item["file"]).resize((tw,th),Image.Resampling.LANCZOS)
        overview.im.paste(small,(x,y))
        overview.text(x+tw+3,y,item["file"].split("_")[0],10,"mint")
    overview.im.save(PREV/"00_overview.png")
    for name,(frames,duration) in animations.items():
        frames[0].save(PREV/name,save_all=True,append_images=frames[1:],duration=duration,loop=0,disposal=0,blend=0)
    # Маленький самостоятельный APNG показывает исходные 32x32, без масштабирования.
    gs=[]
    for n in range(12):
        g=Image.new("RGBA",(32,32),(*C["mint"],0));g.putalpha(ASSETS[f"globe_{n:02}"].getchannel("A"));gs.append(g)
    gs[0].save(PREV/"globe_10fps.apng",save_all=True,append_images=gs[1:],duration=100,loop=0,disposal=0,blend=0)
    cells=[]
    for item in SCENES:
        name=html.escape(item["file"]); title=html.escape(item["title"])
        cells.append(f'<figure id="{name[:-4]}"><figcaption>{name} — {title}</figcaption><a href="{name}"><img src="{name}" width="1024" height="768" loading="lazy" alt="{title}"></a></figure>')
    doc='''<!doctype html><html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TSafari — галерея дизайна</title><style>
@font-face{font-family:Noto;src:url('../fonts/NotoSansMono-Regular.ttf')}
*{box-sizing:border-box}body{margin:0;background:#142832;color:#eff5ed;font:16px/1.6 Noto,monospace}
header,main{max-width:1080px;margin:auto;padding:24px}h1{font-size:30px;font-weight:400;margin:0}
p{color:#b2c5c9}a{color:#75ddc4}figure{margin:28px 0 48px}figcaption{margin-bottom:12px}
img{display:block;max-width:100%;height:auto;border:1px solid #3c5963}.motion{width:1024px}.tiny{width:128px;image-rendering:pixelated}
</style><header><h1>TSafari / интерфейс браузера</h1><p>Исходный размер 1024×768. Нажмите на картинку для просмотра 1:1.<br>
Принятые превью и новые состояния: Wi-Fi, EN/RU, скачивание поверх страницы, курсор над ссылками. Адреса .example — иллюстративные.</p>
<a href="../STAGE3.md">Отчёт этапа 3</a> · <a href="#16_01_download_known">Скачивание</a> · <a href="#16_26_cursor_detail_8x">Курсор 8x</a> · <a href="../DESIGN.md">Описание дизайна</a> · <a href="00_overview.png">Обзорный лист</a>
<p>Глобус: 12 кадров, 10 кадров/с, один оборот за 1,2 с.</p><img class="tiny" src="globe_10fps.apng" alt="Вращение глобуса">
<p>Горячий угол: выезд кнопок, уход курсора, задержка и сворачивание.</p><img class="motion" src="01a_navigation_swipe.apng" alt="Выезд навигации">
<p>Нажатие на глобус: навигация и выезд адресной строки из-под глобуса.</p><img class="motion" src="01b_address_swipe.apng" alt="Выезд адресной строки">
<p>Загрузка в контексте экрана:</p><img class="motion" src="05_loading.apng" alt="Загрузка страницы">
<p>Старт браузера: подключение к Wi-Fi.</p><img class="motion" src="14_wifi_connecting.apng" alt="Подключение к Wi-Fi">
<p>Неизвестный объём скачивания: движущийся сегмент без процентов.</p><img class="motion" src="16_download_indeterminate.apng" alt="Скачивание неизвестного объёма"></header><main>'''+"\n".join(cells)+"</main></html>"
    (PREV/"index.html").write_text(doc,encoding="utf-8")


def main():
    global CURSOR
    for p in (TMP,PREV,ICONS,REPORTS):p.mkdir(parents=True,exist_ok=True)
    CURSOR=Image.open(ICONS/"cursor_zuma.png").convert("RGBA")
    assert CURSOR.size==(38,38)
    for name in ICON_NAMES:
        ASSETS[name]=make_icon(name);ASSETS[name].save(ICONS/(name+".png"))
    globes=make_globes()
    strip=Image.new("RGBA",(32,32*12))
    for n,g in enumerate(globes):
        name=f"globe_{n:02}"
        ASSETS[name]=g;g.save(ICONS/(name+".png"));strip.paste(g,(0,n*32))
    strip.save(ICONS/"globe_strip.png")
    ASSETS["wifi_title"]=make_wifi_title()
    ASSETS["wifi_title"].save(ICONS/"wifi_title.png")
    for name,label in INPUT_LABELS.items():
        ASSETS[name]=make_input_label(label);ASSETS[name].save(ICONS/(name+".png"))
    save_scene("01_page.png","Покой: только полупрозрачный глобус",article,pointer=(714,196))
    save_scene("01a_top_left_link.png","Верхняя ссылка кликабельна; панель скрыта",lambda c:article(c,"top"),pointer=(90,26),status_text="https://zx-notes.example/")
    for prefix,pct in (("01b",.25),("01c",.5),("01d",1.0)):
        save_scene(f"{prefix}_navigation_{int(pct*100):03}.png",f"Горячий угол: выезд кнопок {int(pct*100)}%",article,nav=pct,pointer=(1,1))
    for prefix,pct in (("01e",.25),("01f",.5),("01g",.75)):
        save_scene(f"{prefix}_address_{int(pct*100):03}.png",f"Выезд адреса из-под глобуса {int(pct*100)}%",article,nav=1,address=pct,pointer=(212,28))
    save_scene("01h_globe_hover.png","Наведение на глобус: кнопки остаются скрыты",article,hover="globe",pointer=(28,28))
    save_scene("02_link_hover.png","Ссылка под курсором",lambda c:article(c,True),pointer=(192,472),status_text="https://zx-notes.example/evo/build-notes")
    save_scene("02b_long_link.png","Длинный URL в статусе",lambda c:article(c,True),pointer=(192,472),status_text="https://zx-notes.example/archive/evolution/2026/11/04/very-long-build-notes-and-reading-list?view=plain&language=ru&section=hardware")
    save_scene("03_address_edit.png","Адрес, выделение и каретка",article,opened=True,selection=True,pointer=(607,37))
    save_scene("03b_long_address.png","Длинный адрес: горизонтальная прокрутка",article,opened=True,long_url=True,pointer=(790,37))
    save_scene("04_address_suggestions.png","История и закладки",article,opened=True,suggestions=True,url="zx",pointer=(530,90))
    save_scene("05_loading.png","Загрузка; выдвинутые кнопки и активный Stop",article,nav=1,loading=True,globe_frame=3,pointer=(120,36))
    save_scene("05b_loading_idle.png","Загрузка при скрытых кнопках",article,loading=True,globe_frame=3,pointer=(714,196))
    save_scene("06a_first_page.png","Назад и вперёд недоступны",article,nav=1,first=True,pointer=(692,296))
    save_scene("06b_button_hover.png","Кнопка Reload под курсором",article,nav=1,hover="reload",pointer=(162,35))
    save_scene("06c_button_pressed.png","Кнопка Reload нажата",article,nav=1,pressed="reload",pointer=(162,35))
    save_scene("07_scroll_drag.png","Перетаскивание тонкой полосы прокрутки",lambda c:article(c,scrolled=True),scroll=True,drag=True,scroll_pos=300,pointer=(1012,415))
    save_scene("07a_scroll_hover.png","Полоса появляется у правого края",article,scroll=True,pointer=(1008,198))
    save_scene("07b_scroll_idle.png","После прокрутки полоса снова скрыта",lambda c:article(c,scrolled=True),pointer=(714,470))
    save_scene("08a_typography.png","h1–h3, жирность, ссылки, списки, цитата",typography,pointer=(712,447))
    save_scene("08b_tables_images.png","Таблица и три состояния изображений",table_images,pointer=(706,610))
    save_scene("08c_code_forms.png","Код, черта и элементы форм",forms,pointer=(672,440))
    save_scene("09_home.png","Стартовая страница",home,first=True,pointer=(534,254))
    for key in ERRORS:
        prefix={"offline":"10a","dns":"10b","tls":"10c","404":"10d"}[key]
        save_scene(f"{prefix}_error_{key}.png",ERRORS[key][1],lambda c,key=key:error_page(c,key),pointer=(176,420))
    save_scene("11a_js_required.png","Предложение JS-сервиса",js_request,pointer=(248,490))
    save_scene("11b_js_active.png","Страница через JS-сервис; метка внизу",article,service=True,pointer=(713,194))
    save_scene("12a_download.png","Скачивание с известным размером",download,loading=True,pointer=(464,479))
    save_scene("12b_download_unknown.png","Скачивание без известного размера",lambda c:download(c,"unknown"),loading=True,pointer=(464,479))
    save_scene("12c_download_done.png","Файл сохранён",lambda c:download(c,"done"),pointer=(227,554))
    save_scene("13_settings.png","Настройки JS-сервиса, домашней страницы и поиска",settings,pointer=(768,229))
    save_scene("13b_settings_entry.png","Вход в настройки из адресной строки",article,opened=True,hover="settings",pointer=(994,29))
    base=Canvas();article(base);base.page_done()
    def motion_frame(nav=0,address=0,pointer=(714,196),loading=False,frame=0):
        c=Canvas();c.im=base.im.copy();c.d=ImageDraw.Draw(c.im)
        toolbar(c,nav=nav,address=address,loading=loading,globe_frame=frame)
        c.cursor(*pointer);return c.im
    loading_frames=[]
    for n in range(12):
        loading_frames.append(motion_frame(nav=1,loading=True,frame=n,pointer=(118,35)))
    nav_frames=[motion_frame() for _ in range(10)]
    nav_frames += [motion_frame(pointer=(1,1)) for _ in range(3)]
    nav_frames += [motion_frame(nav=n/6,pointer=(1,1)) for n in range(1,7)]
    nav_frames += [motion_frame(nav=1,pointer=(162,28)) for _ in range(10)]
    nav_frames += [motion_frame(nav=1) for _ in range(15)]
    nav_frames += [motion_frame(nav=n/6) for n in range(5,-1,-1)]
    nav_frames += [motion_frame() for _ in range(10)]
    addr_frames=[motion_frame() for _ in range(10)]
    addr_frames += [motion_frame(nav=n/6,pointer=(28+round(184*n/6),28)) for n in range(1,7)]
    addr_frames += [motion_frame(nav=1,address=n/5,pointer=(212,28)) for n in range(1,6)]
    addr_frames += [motion_frame(nav=1,address=1,pointer=(620,31)) for _ in range(25)]
    addr_frames += [motion_frame(nav=1,address=n/5) for n in range(4,-1,-1)]
    addr_frames += [motion_frame(nav=1) for _ in range(15)]
    addr_frames += [motion_frame(nav=n/6) for n in range(5,-1,-1)]
    wifi_frames=wifi_scenes()
    input_language_scenes()
    import sys
    from stage3_previews import generate
    download_frames=generate(sys.modules[__name__])
    review_materials({"05_loading.apng":(loading_frames,100),"01a_navigation_swipe.apng":(nav_frames,40),"01b_address_swipe.apng":(addr_frames,40),"14_wifi_connecting.apng":(wifi_frames,100),"16_download_indeterminate.apng":(download_frames,40)})
    budget=budget_report()
    assets_info=[]
    for p in sorted(ICONS.glob("*.png")):
        im=Image.open(p)
        assets_info.append({"file":p.name,"size":list(im.size),"mode":im.mode})
    manifest={"stage":3,"preview_count":len(SCENES),"previews":SCENES,"icons":assets_info,
              "font_sha256":hashlib.sha256(FONT.read_bytes()).hexdigest(),
              "cursor_sha256":hashlib.sha256((ICONS/"cursor_zuma.png").read_bytes()).hexdigest(),
              "palette":{k:{"rgb565":f"0x{rgb565(v):04X}","display_rgb":C[k]} for k,v in RAW.items()}}
    (REPORTS/"manifest.json").write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"previews":len(SCENES),"icons":len(assets_info),"dl_words":budget["dl_worst_with_reserve"],"ram_g_bytes":budget["ram_g_total_bytes"]},ensure_ascii=False))


if __name__=="__main__":
    main()
