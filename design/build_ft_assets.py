#!/usr/bin/env python3
"""Этап 2: PNG -> FT812, размещение ресурсов и автономный заголовок C.

Не пишет в main, не загружает ничего в устройство. Все файлы — внутри design.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import struct

os.environ["TEMP"] = os.environ["TMP"] = r"E:\zx\cache\tmp\codex_tsafari_ui"
from PIL import Image, ImageDraw, ImageFont
import render_previews as preview
import stage3_previews as stage3

ROOT = Path(__file__).resolve().parent
FT = ROOT / "ft"
UI_BASE, UI_END = 0x0E1000, 0x0F0000
ADDRESS_BASE = 0x0F0000
FORMAT = {"L4": 2, "ARGB4": 6}
CORE_STATIC_BYTES = 18008
INPUT_ASSET_BASE = 0x0EDB70  # После прежних буферов и резерва; их адреса фиксированы.

# Имена и порядок сохраняют принятые PNG. Кадры глобуса — представления одной ленты.
CANONICAL = preview.ICON_NAMES + ["globe_strip", "cursor_zuma", "js_label", "wifi_title"] + list(preview.INPUT_LABELS)

GEOMETRY = {
    "SCREEN_WIDTH":1024, "SCREEN_HEIGHT":768, "SAFE_MARGIN":8,
    "PAGE_MARGIN":8, "PAGE_CONTENT_WIDTH":1008, "PAGE_BLOCK_RIGHT":1008,
    "PAGE_BLOCK_WIDTH":1000,
    "HOT_X":0, "HOT_Y":0, "HOT_WIDTH":4, "HOT_HEIGHT":4,
    "NAV_X":8, "NAV_Y":8, "NAV_WIDTH":176, "NAV_HEIGHT":40, "NAV_RADIUS":10,
    "NAV_BUTTON_COUNT":4, "NAV_FIRST_CENTER_X":30, "NAV_CENTER_Y":28,
    "NAV_PITCH":44, "NAV_TRAVEL":184, "NAV_BUTTON_HIT_RADIUS":18,
    "NAV_HOVER_RADIUS":18, "NAV_PRESSED_RADIUS":18, "PRESSED_ICON_OFFSET":1,
    "GLOBE_REST_CENTER_X":28, "GLOBE_OPEN_CENTER_X":212,
    "GLOBE_CENTER_Y":28, "GLOBE_RADIUS":20,
    "GLOBE_BITMAP_WIDTH":32, "GLOBE_BITMAP_HEIGHT":32,
    "GLOBE_BITMAP_OFFSET_X":-16, "GLOBE_BITMAP_OFFSET_Y":-16,
    "GLOBE_FRAME_COUNT":12, "GLOBE_IDLE_FRAME":0,
    "ICON_WIDTH":24, "ICON_HEIGHT":24, "ICON_HALF_WIDTH":12,
    "ADDRESS_X":192, "ADDRESS_Y":8, "ADDRESS_WIDTH":824, "ADDRESS_HEIGHT":40,
    "ADDRESS_RADIUS":20, "ADDRESS_BORDER_WIDTH":1, "ADDRESS_TRAVEL":784,
    "ADDRESS_CLIP_X":212, "ADDRESS_CLIP_Y":8,
    "ADDRESS_CLIP_WIDTH":804, "ADDRESS_CLIP_HEIGHT":40,
    "ADDRESS_TEXT_X":244, "ADDRESS_TEXT_Y":8, "ADDRESS_VISIBLE_COLUMNS":54,
    "ADDRESS_TEXT_WIDTH":648, "ADDRESS_TEXT_HEIGHT":34,
    "ADDRESS_TEXT_CLIP_X":244, "ADDRESS_TEXT_CLIP_Y":8,
    "ADDRESS_TEXT_CLIP_WIDTH":648, "ADDRESS_TEXT_CLIP_HEIGHT":34,
    "ADDRESS_SEPARATOR_X":236, "ADDRESS_SEPARATOR_Y0":18,
    "ADDRESS_SEPARATOR_Y1":38,
    "SELECTION_Y":15, "SELECTION_HEIGHT":27, "SELECTION_RADIUS":2,
    "CARET_Y":17, "CARET_HEIGHT":23, "CARET_WIDTH":2,
    "INPUT_LANG_WIDTH":32, "INPUT_LANG_HEIGHT":22, "INPUT_LANG_Y":17,
    "INPUT_LANG_OFFSET_FROM_CARET_X":6, "INPUT_LANG_GAP_FROM_CARET_RIGHT":4,
    "INPUT_LANG_OFFSET_FROM_CARET_Y":0, "INPUT_LANG_MAX_CARET_X":892,
    "INPUT_LANG_MAX_RIGHT":930, "INPUT_LANG_GO_GAP":8,
    "INPUT_LANG_CLIP_X":244, "INPUT_LANG_CLIP_Y":8,
    "INPUT_LANG_CLIP_WIDTH":686, "INPUT_LANG_CLIP_HEIGHT":40,
    "INPUT_LANG_EN":0, "INPUT_LANG_RU":1,
    "INPUT_LANG_SWITCH_CTRL":1, "INPUT_LANG_SWITCH_SHIFT":1,
    "INPUT_LANG_SHOW_AT_TEXT_END_ONLY":1, "INPUT_LANG_HIDE_WITH_SELECTION":1,
    "INPUT_LANG_STEADY_DURING_CARET_BLINK":1,
    "GO_X":938, "GO_Y":16, "GO_CENTER_X":950, "GO_CENTER_Y":28,
    "SETTINGS_X":984, "SETTINGS_Y":16,
    "SETTINGS_CENTER_X":996, "SETTINGS_CENTER_Y":28,
    "SETTINGS_HOVER_RADIUS":17, "SETTINGS_HIT_RADIUS":18,
    "SETTINGS_SEPARATOR_X":972, "SETTINGS_SEPARATOR_Y0":18,
    "SETTINGS_SEPARATOR_Y1":38,
    "SUGGESTIONS_X":236, "SUGGESTIONS_Y":54,
    "SUGGESTIONS_WIDTH":780, "SUGGESTIONS_HEIGHT":152, "SUGGESTIONS_RADIUS":8,
    "SUGGESTIONS_BORDER_WIDTH":1, "SUGGESTION_COUNT":3,
    "SUGGESTION_STEP":46, "SUGGESTION_SELECTED_X":244, "SUGGESTION_SELECTED_Y":62,
    "SUGGESTION_SELECTED_WIDTH":764, "SUGGESTION_SELECTED_HEIGHT":44,
    "SUGGESTION_SELECTED_RADIUS":6,
    "SUGGESTION_TEXT_X":288, "SUGGESTION_TEXT_Y":66,
    "SUGGESTION_TEXT_WIDTH":320, "SUGGESTION_TEXT_HEIGHT":34,
    "SUGGESTION_MAX_COLUMNS":26, "SUGGESTION_ICON_X":252, "SUGGESTION_ICON_Y":70,
    "SUGGESTION_GO_X":978, "SUGGESTION_GO_Y":72,
    "STATUS_X":8, "STATUS_Y":720, "STATUS_HEIGHT":40,
    "STATUS_MAX_WIDTH":1008, "STATUS_RADIUS":6,
    "STATUS_TEXT_X":20, "STATUS_TEXT_Y":724,
    "STATUS_MAX_COLUMNS":82, "STATUS_TEXT_WIDTH":984, "STATUS_TEXT_HEIGHT":34,
    "STATUS_HORIZONTAL_PADDING":12, "STATUS_ELLIPSIS_PREFIX_COLUMNS":50,
    "STATUS_ELLIPSIS_COLUMNS":3, "STATUS_ELLIPSIS_SUFFIX_COLUMNS":29,
    "SCROLL_TRACK_X":1012, "SCROLL_TRACK_Y":8,
    "SCROLL_TRACK_WIDTH":4, "SCROLL_TRACK_HEIGHT":752, "SCROLL_TRACK_RADIUS":2,
    "SCROLL_THUMB_WIDTH":4, "SCROLL_THUMB_RADIUS":2,
    "SCROLL_DRAG_X":1010, "SCROLL_DRAG_WIDTH":6, "SCROLL_DRAG_RADIUS":3,
    "SCROLL_MIN_THUMB_HEIGHT":36, "SCROLL_HOVER_X":1004, "SCROLL_HOVER_WIDTH":20,
    "SCROLL_HIT_X":1000, "SCROLL_HIT_WIDTH":16,
    "JS_BADGE_X":906, "JS_BADGE_Y":710, "JS_BADGE_WIDTH":102,
    "JS_BADGE_HEIGHT":40, "JS_BADGE_RADIUS":6,
    "JS_ICON_X":914, "JS_ICON_Y":718, "JS_TEXT_X":946, "JS_TEXT_Y":718,
    "CURSOR_WIDTH":38, "CURSOR_HEIGHT":38, "CURSOR_HOTSPOT_X":1, "CURSOR_HOTSPOT_Y":1,
    "FONT_SIZE":20, "FONT_CELL_WIDTH":12, "FONT_CELL_HEIGHT":34,
    "FONT_ADVANCE":12, "FONT_BASELINE":26, "BODY_LINE_STEP":34,
    "H1_SIZE":32, "H1_ADVANCE":19, "H1_CELL_HEIGHT":55, "H1_BASELINE":42,
    "H2_SIZE":26, "H2_ADVANCE":16, "H2_CELL_HEIGHT":45, "H2_BASELINE":34,
    "H3_SIZE":22, "H3_ADVANCE":13, "H3_CELL_HEIGHT":38, "H3_BASELINE":29,
    "SECONDARY_SIZE":18, "SECONDARY_ADVANCE":11,
    "SECONDARY_CELL_HEIGHT":31, "SECONDARY_BASELINE":23,
    "CAPTION_SIZE":14, "CAPTION_ADVANCE":8,
    "CAPTION_CELL_HEIGHT":24, "CAPTION_BASELINE":18,
    "BOLD_OFFSET_X":1, "LINK_UNDERLINE_OFFSET_FROM_BASELINE":3,
    "QUOTE_BAR_WIDTH":3, "PRE_PADDING_X":16, "TABLE_ROW_HEIGHT":44,
    "FORM_FIELD_HEIGHT":44, "FORM_FIELD_RADIUS":6,
    "FORM_BUTTON_HEIGHT":44, "FORM_BUTTON_RADIUS":8,
    "WIFI_X":256, "WIFI_Y":204, "WIFI_WIDTH":512, "WIFI_HEIGHT":360,
    "WIFI_RADIUS":16, "WIFI_BORDER_WIDTH":1, "WIFI_CENTER_X":512,
    "WIFI_TITLE_X":440, "WIFI_TITLE_Y":226, "WIFI_TITLE_TEXT_OFFSET_X":5,
    "WIFI_TITLE_FONT_SIZE":32,
    "WIFI_SUBTITLE_X":308, "WIFI_SUBTITLE_Y":284,
    "WIFI_SUBTITLE_FONT_SIZE":20, "WIFI_SUBTITLE_COLUMNS":34,
    "WIFI_INDICATOR_CENTER_X":512, "WIFI_INDICATOR_CENTER_Y":350,
    "WIFI_INDICATOR_RADIUS":24, "WIFI_GLOBE_X":496, "WIFI_GLOBE_Y":334,
    "WIFI_WARNING_X":500, "WIFI_WARNING_Y":338,
    "WIFI_STATE_X":352, "WIFI_STATE_Y":390,
    "WIFI_HINT_X":272, "WIFI_HINT_Y":456, "WIFI_HINT_LINE_STEP":34,
    "WIFI_EXIT_KEY_F_NUMBER":12,
}
TIMING = {
    "HOT_DWELL_MS":120, "NAV_ANIMATION_MS":240, "NAV_HIDE_DELAY_MS":600,
    "ADDRESS_ANIMATION_MS":200, "SCROLL_HIDE_DELAY_MS":700,
    "GLOBE_FRAME_MS":100, "GLOBE_CYCLE_MS":1200,
    "ANIMATION_TABLE_STEP_MS":1, "PREVIEW_FRAME_MS":40,
}
ALPHA = {
    "GLOBE_IDLE_ALPHA":112, "GLOBE_HOVER_ALPHA":220, "GLOBE_OPEN_ALPHA":255,
    "NAV_HIDDEN_ALPHA":0, "NAV_VISIBLE_ALPHA":255,
    "BUTTON_NORMAL_ALPHA":255, "BUTTON_HOVER_ALPHA":255,
    "BUTTON_PRESSED_ALPHA":255, "BUTTON_DISABLED_ALPHA":255,
    "STOP_ACTIVE_ALPHA":255, "ADDRESS_ALPHA":255, "SELECTION_ALPHA":255,
    "CARET_ALPHA":255, "SUGGESTION_ALPHA":255, "STATUS_ALPHA":255,
    "SCROLL_HIDDEN_ALPHA":0, "SCROLL_VISIBLE_ALPHA":255,
    "SCROLL_DRAG_ALPHA":255, "JS_BADGE_ALPHA":255, "CURSOR_ALPHA":255,
    "WIFI_PANEL_ALPHA":255, "WIFI_TEXT_ALPHA":255, "WIFI_INDICATOR_ALPHA":255,
    "INPUT_LANG_ALPHA":255,
}
ROLE_COLORS = {
    "BUTTON_BG":"navy", "BUTTON_NORMAL":"ui_text", "BUTTON_HOVER_BG":"hover",
    "BUTTON_HOVER":"ui_text", "BUTTON_PRESSED_BG":"pressed", "BUTTON_PRESSED":"ui_text",
    "BUTTON_DISABLED":"disabled", "STOP_ACTIVE":"amber", "GLOBE":"mint", "GLOBE_BG":"navy",
    "ADDRESS_BG":"navy", "ADDRESS_BORDER":"mint", "ADDRESS_TEXT":"ui_text",
    "ADDRESS_SEPARATOR":"edge", "SELECTION_BG":"selection", "CARET":"mint",
    "SUGGESTIONS_BG":"navy", "SUGGESTIONS_BORDER":"edge",
    "SUGGESTION_SELECTED_BG":"panel", "SUGGESTION_SELECTED_TEXT":"ui_text",
    "SUGGESTION_TEXT":"ui_muted", "SUGGESTION_SELECTED_ICON":"mint",
    "SUGGESTION_ICON":"ui_muted", "STATUS_BG":"navy", "STATUS_TEXT":"ui_text",
    "SCROLL_TRACK":"soft", "SCROLL_THUMB":"muted", "SCROLL_DRAG":"link",
    "JS_BADGE_BG":"panel", "JS_BADGE_ICON":"mint", "JS_BADGE_TEXT":"ui_text",
    "WIFI_BG":"navy", "WIFI_BORDER":"edge", "WIFI_TITLE":"ui_text",
    "WIFI_SUBTITLE":"ui_muted", "WIFI_INDICATOR_BG":"panel",
    "WIFI_CONNECTING":"mint", "WIFI_FAILED":"amber",
    "WIFI_STATE":"ui_text", "WIFI_HINT":"ui_muted",
    "INPUT_LANG":"mint", "INPUT_LANG_BACKGROUND":"navy",
}
GEOMETRY.update({"DOWNLOAD_"+k:v for k,v in stage3.DOWNLOAD_G.items()})
GEOMETRY.update({"CURSOR_LINK_WIDTH":38,"CURSOR_LINK_HEIGHT":38,"CURSOR_LINK_HOTSPOT_X":1,
                 "CURSOR_LINK_HOTSPOT_Y":1,"CURSOR_LINK_VISITED_SAME":1})
TIMING.update({"DOWNLOAD_"+k:v for k,v in stage3.DOWNLOAD_T.items()})
ALPHA.update({"DOWNLOAD_ALPHA":255,"DOWNLOAD_TEXT_ALPHA":255,"CURSOR_LINK_ALPHA":255})
ROLE_COLORS.update({"DOWNLOAD_BG":"navy","DOWNLOAD_HOVER_BG":"panel","DOWNLOAD_TEXT":"ui_text",
                    "DOWNLOAD_TRACK":"edge","DOWNLOAD_FILL":"mint","DOWNLOAD_WRITING":"amber",
                    "DOWNLOAD_ERROR_ICON":"amber","DOWNLOAD_DONE":"mint","DOWNLOAD_ICON":"mint",
                    "DOWNLOAD_ACTION":"ui_muted","DOWNLOAD_ACTION_HOVER":"mint",
                    "DOWNLOAD_ACTION_HOVER_BG":"hover","CURSOR_LINK":"mint"})


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")


def pack_l4(im):
    """Строки сверху вниз; старшая тетрада — левый (чётный) пиксель."""
    a=im.getchannel("A")
    pixels=a.load();out=bytearray()
    for y in range(im.height):
        for x in range(0,im.width,2):
            hi=min(15,(pixels[x,y]+8)//17)
            lo=min(15,(pixels[x+1,y]+8)//17) if x+1<im.width else 0
            out.append((hi<<4)|lo)
    return bytes(out)


def pack_argb4(im):
    """Точно алгоритм make_cursor.py Zuma: A4R4G4B4, little-endian, усечение."""
    out=bytearray()
    for red,green,blue,alpha in im.get_flattened_data():
        word=((alpha>>4)<<12)|((red>>4)<<8)|((green>>4)<<4)|(blue>>4)
        out.extend(struct.pack("<H",word))
    return bytes(out)


def round_even(numerator,denominator):
    """Целочисленный эквивалент принятого round, включая половины к чётному."""
    q,r=divmod(numerator,denominator)
    return q+int(2*r>denominator or (2*r==denominator and q%2==1))


def animation_tables():
    return {
        "nav_offset_px":[round_even(184*(240-t),240) for t in range(241)],
        "nav_globe_alpha":[112+round_even(143*t,240) for t in range(241)],
        "address_offset_px":[-round_even(784*(200-t),200) for t in range(201)],
    }


def make_js_label():
    # Метка с уже принятыми размерами шрифта; атлас глифов в RAM_G не требуется.
    im=Image.new("RGBA",(24,34),(255,255,255,0))
    alpha=Image.new("L",im.size)
    f=ImageFont.truetype(str(ROOT/"fonts/NotoSansMono-Regular.ttf"),20)
    d=ImageDraw.Draw(alpha)
    for x,ch in ((0,"J"),(12,"S")):d.text((x,26),ch,font=f,anchor="ls",fill=255)
    im.putalpha(alpha.point([((v+8)//17)*17 for v in range(256)]))
    im.save(ROOT/"icons/js_label.png")


def emit_header(manifest):
    lines=[
        "/* TSafari: дизайн, этап 3. Создан build_ft_assets.py.",
        " * Только константы и таблицы. Нет SDK, типов из stdint или функций.",
        " * Все координаты — логические пиксели 1024x768; диапазоны памяти [base,end).",
        " * RGB888 совпадает с развёрнутым RGB565 принятого превью. */",
        "#ifndef TSAFARI_UI_DESIGN_H", "#define TSAFARI_UI_DESIGN_H", "",
    ]
    def define(name,value):
        lines.append(f"#define TSA_UI_{name} {value}")
    for label,values in (("Геометрия",GEOMETRY),("Время, миллисекунды",TIMING),("COLOR_A, диапазон 0..255",ALPHA)):
        lines.extend(["",f"/* {label}. */"])
        for name,value in values.items():define(name,f"({value})" if value<0 else str(value))
    lines.extend(["", "/* Заголовок Wi-Fi — готовый ASSET_WIFI_TITLE, шрифт 32 px в ELF не нужен. */"])
    define("WIFI_SUBTITLE_TEXT",json.dumps(preview.WIFI_SUBTITLE))
    lines.extend(["", "/* Цветовые константы. */"])
    for name,value in preview.C.items():
        rgb=(value[0]<<16)|(value[1]<<8)|value[2]
        define(f"COLOR_{name.upper()}_RGB888",f"0x{rgb:06X}u")
        define(f"COLOR_{name.upper()}_RGB565",f"0x{preview.rgb565(value):04X}u")
    lines.extend(["", "/* Цвета состояний; disabled меняет цвет, а не прозрачность. */"])
    for role,color in ROLE_COLORS.items():
        for fmt in ("RGB888","RGB565"):
            define(f"{role}_{fmt}",f"TSA_UI_COLOR_{color.upper()}_{fmt}")
    define("CURSOR_TINT_RGB888","0xFFFFFFu")
    define("CURSOR_TINT_RGB565","0xFFFFu")
    lines.extend(["", "/* Форматы FT812 и принятая карта RAM_G. */"])
    for name,value in (("FORMAT_L4",2),("FORMAT_ARGB4",6),("FORMAT_PALETTED565",14)):
        define(name,str(value))
    memory={
        "PAGE_RING_BASE":0, "PAGE_RING_WIDTH":1024,"PAGE_RING_ROWS":896,
        "PAGE_RING_STRIDE":1024,"PAGE_RING_BYTES":1024*896,
        "PAGE_PALETTE_BASE":0x0E0000,"PAGE_PALETTE_COLORS":256,
        "PAGE_PALETTE_BYTES":512,"PAGE_PALETTE_COUNT":2,
        "PAGE_PALETTE_SECOND_BASE":0x0E0200,
        "RAM_G_BASE":UI_BASE,"RAM_G_END":UI_END,"RAM_G_CAPACITY":UI_END-UI_BASE,
        "STATIC_BYTES":manifest["bundle"]["bytes"],
        "RAM_G_USED_WITH_RESERVE":manifest["budget"]["ui_used_with_reserve"],
        "RAM_G_FREE_BYTES":manifest["budget"]["ui_free_bytes"],
        "ADDRESS_RAM_G_BASE":ADDRESS_BASE,"ADDRESS_L4_STRIDE":324,"ADDRESS_L4_BYTES":11016,
        "ADDRESS_RAM_G_END":ADDRESS_BASE+11016,
        "DOWNLOAD_RAM_G_BASE":0xF4000,"DOWNLOAD_RAM_G_END":0xF7FC0,"DOWNLOAD_RAM_G_LIMIT":0xF8000,
        "DOWNLOAD_TEXT_STRIDE":240,"DOWNLOAD_TEXT_BYTES":16320,
        "DL_ESTIMATE_WORDS":250,"DL_ESTIMATE_RESERVE_WORDS":24,"DL_LIMIT_WORDS":250,
    }
    for name,value in memory.items():
        define(name,f"0x{value:06X}u" if "BASE" in name or name.endswith("END") else str(value)+"u")
    lines.extend(["", "/* Курсор на ссылках использует тот же ресурс; заменить COLOR_RGB перед его выводом. */"])
    for suffix in ("RAM_G","FORMAT","STRIDE","BYTES"):
        define("CURSOR_LINK_"+suffix,"TSA_UI_ASSET_CURSOR_ZUMA_"+suffix)
    lines.extend(["", "/* Неизвестный объём: p=t%1400; x=BAR_X+384*(p<=700?p:1400-p)/700.",
                  " * Известный объём: width=BAR_WIDTH*received/total, с 64-битным промежуточным произведением.",
                  " * Загрузка текста: один L4 bitmap 480x68; y=DOWNLOAD_Y+4, либо Y_WITH_STATUS+4.",
                  " * Автоскрытие терминальных состояний приостановить под курсором.",
                  " * Не возвращать плашку вниз, пока курсор внутри неё; отмена только при приёме из сети. */"])
    lines.extend(["", "/* Пакет загружается ДВУМЯ диапазонами: file_offset -> ram_g.",
                  " * Не записывать весь ui_assets.bin подряд с RAM_G_BASE: там прежние текстовые буферы. */"])
    define("BUNDLE_SEGMENT_COUNT",str(len(manifest["load_plan"]))+"u")
    for n,segment in enumerate(manifest["load_plan"]):
        for field in ("file_offset","ram_g","bytes"):
            value=segment[field]
            define(f"BUNDLE_SEGMENT_{n}_{field.upper()}",f"0x{value:06X}u" if field=="ram_g" else str(value)+"u")
    for asset in manifest["assets"]:
        name="ASSET_"+asset["name"].upper()
        lines.append("")
        for field,suffix in (("format_code","FORMAT"),("width","WIDTH"),("height","HEIGHT"),
                             ("stride","STRIDE"),("bytes","BYTES"),("bundle_offset","OFFSET"),
                             ("ram_g","RAM_G"),("cell_height","CELL_HEIGHT"),
                             ("cell_count","CELL_COUNT"),("cell_bytes","CELL_BYTES")):
            value=asset[field]
            define(f"{name}_{suffix}",f"0x{value:06X}u" if field=="ram_g" else str(value)+"u")
        if "cell_index" in asset:
            define(f"{name}_CELL_INDEX",str(asset["cell_index"])+"u")
    lines.extend(["", "/* Динамические маски: очистить до рисования, потом обновлять из ESP. */"])
    for buf in manifest["buffers"]:
        name="BUFFER_"+buf["name"].upper()
        for field in ("ram_g","width","height","stride","bytes"):
            value=buf[field]
            define(name+"_"+field.upper(),f"0x{value:06X}u" if field=="ram_g" else str(value)+"u")
    lines.extend(["", "/* Стартовое окно Wi-Fi: переиспользование буферов, НЕ дополнительные области.",
                  " * Эти маски и обычные подсказки/статус взаимоисключающие. После смены режима перерисовать. */"])
    for buf in manifest["startup_views"]:
        for field in ("ram_g","width","height","stride","bytes"):
            value=buf[field]
            define("WIFI_BUFFER_"+buf["name"].upper()+"_"+field.upper(),
                   f"0x{value:06X}u" if field=="ram_g" else str(value)+"u")
    lines.extend(["", "/* Центры раскрытых кнопок: Back, Forward, Stop, Reload. */",
                  "static const unsigned short tsa_ui_nav_centers_x[4] = {30, 74, 118, 162};",
                  "", "/* Линейный выезд принятого превью, lookup по целым миллисекундам.",
                  " * Индекс t предварительно ограничить [0,NAV_ANIMATION_MS] или [0,ADDRESS_ANIMATION_MS].",
                  " * Обратный ход: индекс duration-elapsed. Float на кадр не требуется.",
                  " * nav_x = NAV_X - nav_offset_px[t]; globe_x = GLOBE_OPEN_CENTER_X - nav_offset_px[t].",
                  " * address_x = ADDRESS_X + address_offset_px[t]; все элементы поля получают тот же сдвиг.",
                  " * Глобус рисуется последним поверх поля. Alpha применяется отдельно к POINTS и BITMAPS.",
                  " * Между двумя состояниями: порядок навигация -> адрес при открытии, обратный при закрытии. */"])
    for name,values in animation_tables().items():
        kind="unsigned char" if "alpha" in name else "short" if name.startswith("address") else "unsigned short"
        lines.append(f"static const {kind} tsa_ui_{name}[{len(values)}] = {{")
        for offset in range(0,len(values),16):
            lines.append("    "+", ".join(str(v) for v in values[offset:offset+16])+",")
        lines.append("};")
    lines.extend(["", "#endif /* TSAFARI_UI_DESIGN_H */", ""])
    (ROOT/"ui_design.h").write_text("\n".join(lines),encoding="utf-8")


def build():
    FT.mkdir(exist_ok=True)
    accepted=ROOT/"reports/accepted_stage1_sha256.json"
    if not accepted.exists():
        old=json.loads((ROOT/"reports/validation.json").read_text(encoding="utf-8"))
        write_json(accepted,{name:value for name,value in old["artifact_sha256"].items()
                             if Path(name).suffix in (".png",".apng")})
    make_js_label()
    preview.make_wifi_title().save(ROOT/"icons/wifi_title.png")
    for name,label in preview.INPUT_LABELS.items():
        preview.make_input_label(label).save(ROOT/"icons"/(name+".png"))
    assets=[];bundle=bytearray();lookup={}
    for name in CANONICAL:
        source=ROOT/"icons"/(name+".png")
        im=Image.open(source).convert("RGBA")
        fmt="ARGB4" if name=="cursor_zuma" else "L4"
        data=pack_argb4(im) if fmt=="ARGB4" else pack_l4(im)
        filename=name+(".argb4" if fmt=="ARGB4" else ".l4")
        (FT/filename).write_bytes(data)
        while len(bundle)%4:bundle.append(0)
        offset=len(bundle);bundle.extend(data)
        address=INPUT_ASSET_BASE+offset-CORE_STATIC_BYTES if name in preview.INPUT_LABELS else UI_BASE+offset
        cell_height=32 if name=="globe_strip" else im.height
        asset={"name":name,"file":filename,"png":"../icons/"+source.name,
               "format":fmt,"format_code":FORMAT[fmt],"width":im.width,"height":im.height,
               "stride":im.width*2 if fmt=="ARGB4" else (im.width+1)//2,
               "bytes":len(data),"bundle_offset":offset,"ram_g":address,
               "ram_g_hex":f"0x{address:06X}","cell_height":cell_height,
               "cell_count":12 if name=="globe_strip" else 1,
               "cell_bytes":512 if name=="globe_strip" else len(data),
               "canonical":True,"sha256":digest(data),"png_sha256":digest(source.read_bytes())}
        assets.append(asset);lookup[name]=asset
    strip=lookup["globe_strip"]
    for n in range(12):
        name=f"globe_{n:02}"
        source=ROOT/"icons"/(name+".png")
        data=pack_l4(Image.open(source).convert("RGBA"))
        (FT/(name+".l4")).write_bytes(data)
        offset=strip["bundle_offset"]+n*512
        assets.append({"name":name,"file":name+".l4","png":"../icons/"+source.name,
                       "format":"L4","format_code":2,"width":32,"height":32,"stride":16,
                       "bytes":512,"bundle_offset":offset,"ram_g":UI_BASE+offset,
                       "ram_g_hex":f"0x{UI_BASE+offset:06X}","cell_height":32,"cell_count":1,
                       "cell_bytes":512,"canonical":False,"alias_of":"globe_strip","cell_index":n,
                       "sha256":digest(data),"png_sha256":digest(source.read_bytes())})
    (FT/"ui_assets.bin").write_bytes(bundle)
    assert lookup["input_en"]["bundle_offset"]==CORE_STATIC_BYTES
    cursor=UI_BASE+CORE_STATIC_BYTES;buffers=[]
    for name,width in [(f"suggestion_{n}",320) for n in range(3)]+[("status_text",984)]:
        count=width*34//2
        buffers.append({"name":name,"format":"L4","format_code":2,"ram_g":cursor,
                        "ram_g_hex":f"0x{cursor:06X}","width":width,"height":34,
                        "stride":width//2,"bytes":count,"init":"Очистить до первого вывода; обновляет ESP"})
        cursor+=count
    buffers.append({"name":"address_text","format":"L4","format_code":2,"ram_g":ADDRESS_BASE,
                    "ram_g_hex":"0x0F0000","width":648,"height":34,"stride":324,"bytes":11016,
                    "init":"Очистить до первого вывода; обновляет ESP"})
    startup_views=[]
    for name,owners,width,height in (("subtitle",["suggestion_0","suggestion_1"],408,34),
                                    ("state",["suggestion_2"],320,34),
                                    ("hint",["status_text"],480,68)):
        allocations=[next(b for b in buffers if b["name"]==owner) for owner in owners]
        assert all(a["ram_g"]+a["bytes"]==b["ram_g"] for a,b in zip(allocations,allocations[1:]))
        count=width*height//2
        assert count<=sum(b["bytes"] for b in allocations)
        startup_views.append({"name":name,"alias_of_buffers":owners,"format":"L4","format_code":2,
                              "ram_g":allocations[0]["ram_g"],"width":width,"height":height,
                              "stride":width//2,"bytes":count,"lifetime":"Только модальное окно Wi-Fi"})
    reserve=1024
    assert cursor+reserve==INPUT_ASSET_BASE
    extra_bytes=len(bundle)-CORE_STATIC_BYTES
    allocation_end=INPUT_ASSET_BASE+extra_bytes
    used=allocation_end-UI_BASE-reserve
    assert allocation_end<=UI_END
    buffers.append({"name":"download_text","format":"L4","format_code":2,"ram_g":0xF4000,
                    "ram_g_hex":"0x0F4000","width":480,"height":68,"stride":240,"bytes":16320,
                    "init":"Очистить до первого вывода; две строки TXTVIEW2 20 px, обновляет ESP"})
    budget={"ui_capacity":UI_END-UI_BASE,"static_bytes":len(bundle),
            "dynamic_bytes_in_ui":used-len(bundle),"ui_used_bytes":used,
            "reserve_bytes":reserve,"ui_used_with_reserve":used+reserve,
            "ui_free_bytes":UI_END-allocation_end,"address_bytes":11016,
            "ui_plus_address_with_reserve":used+reserve+11016,
            "download_bytes":16320,"download_capacity":16384,"download_free_bytes":64,
            "total_with_download":used+reserve+11016+16320,
            "dl_worst_with_reserve":250,"dl_reserve_words":24,"dl_limit_words":250}
    manifest={"version":3,"stage":3,"frame":[1024,768],
              "packing":{"L4":"По строкам; старшая тетрада = левый/чётный x; маска alpha; без дополнения строк",
                         "ARGB4":"По строкам; little-endian A4R4G4B4; каналы >> 4, как у Zuma"},
              "memory":{"page_ring":{"base":0,"end":0xE0000,"format":"PALETTED565",
                                     "width":1024,"rows":896,"stride":1024,"bytes":917504},
                        "page_palettes":{"base":0xE0000,"count":2,"colors":256,"bytes_each":512,
                                         "note":"Вторая палитра указана в прочитанном main/page.h"},
                        "ui_region":{"base":UI_BASE,"end":UI_END,"bytes":UI_END-UI_BASE},
                        "reserve":{"base":cursor,"end":cursor+reserve,"bytes":reserve},
                        "ui_free":{"base":allocation_end,"end":UI_END,"bytes":UI_END-allocation_end},
                        "address_region":{"base":ADDRESS_BASE,"end":ADDRESS_BASE+11016,"bytes":11016},
                        "download_region":{"base":0xF4000,"end":0xF7FC0,"limit":0xF8000,"bytes":16320,"free":64}},
              "bundle":{"file":"ui_assets.bin","segmented":True,"bytes":len(bundle),"sha256":digest(bundle)},
              "load_plan":[{"file":"ui_assets.bin","file_offset":0,"ram_g":UI_BASE,"bytes":CORE_STATIC_BYTES},
                           {"file":"ui_assets.bin","file_offset":CORE_STATIC_BYTES,"ram_g":INPUT_ASSET_BASE,"bytes":extra_bytes}],
              "assets":assets,"buffers":buffers,"startup_views":startup_views,"budget":budget,
              "notes":["Загрузить пакет по ДВУМ диапазонам load_plan (file_offset -> ram_g); сплошная запись всего файла с 0x0E1000 затрёт текстовые буферы.",
                       "Первые 18008 байт пакета и все прежние RAM_G-адреса сохранены. input_en/input_ru дописаны в конец файла.",
                       "globe_00..11 — представления globe_strip, не дополнительная RAM.",
                       "globe_strip height=384 — высота хранения; BITMAP_LAYOUT height=32, CELL=0..11.",
                       "L4 окрашивается COLOR_RGB, без premultiply; курсор белый над текстом, mint 0x73DFC6 над ссылками.",
                       "Этап 3 не добавляет статических ресурсов: весь ui_assets.bin и оба диапазона load_plan сохранены.",
                       "download_text — динамическая L4-маска с 0x0F4000 в отдельно разрешённой области; в пакет не входит.",
                       "Динамические текстовые маски строит ESP из атласа 20 px: ячейка 12x34, baseline 26.",
                       "wifi_title — готовая L4-маска TSafari 32 px, без атласа 32 px в ELF.",
                       "startup_views subtitle занимает начало объединённых suggestion_0 и suggestion_1; обычные подсказки на время Wi-Fi отключены."]}
    write_json(FT/"assets.json",manifest)
    emit_header(manifest)
    report={"kind":"Этап 3: точные размеры ресурсов, проектная оценка DL без платы",
            "ram_g_total_bytes":budget["total_with_download"],
            "ram_g_ui_region_bytes":budget["ui_used_with_reserve"],
            "ram_g_ui_region_capacity":61440,"ram_g_ui_region_free":budget["ui_free_bytes"],
            "ram_g_address_bytes":11016,"ram_g_download_bytes":16320,"dl_worst_with_reserve":250,"dl_reserve_words":24,
            "dl_download_components":{"context":2,"panel":6,"text_bitmap":8,"state_and_action_icons":9,"bar":8,"hover":4},
            "dl_states":{"idle":29,"navigation":87,"status_bitmap":113,"address_bitmap":121,
                         "address_language":130,"suggestions":172,"suggestions_js":188,"wifi_modal":96,
                         "download_and_cursor_tint_extra":38,"maximum_before_reserve":226},"allocation":manifest["memory"],
            "page_benchmark_ms":{"PALETTED565":12.5,"RGB565":17.0,
                                 "source":"Сообщение пользователя; замер здесь не повторялся"}}
    write_json(ROOT/"reports/budget.json",report)
    print(json.dumps({"assets":len(assets),"static_bytes":len(bundle),**budget},ensure_ascii=False))


if __name__=="__main__":
    build()
