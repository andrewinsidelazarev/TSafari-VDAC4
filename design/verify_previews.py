#!/usr/bin/env python3
"""Проверки артефактов этапа 1; запись только в design/reports и временную на E:."""
import hashlib
import json
import os
from pathlib import Path

os.environ["TEMP"] = os.environ["TMP"] = r"E:\zx\cache\tmp\codex_tsafari_ui"
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
from PIL import Image, ImageChops, ImageDraw, ImageFont
import render_previews as r

ROOT=Path(__file__).resolve().parent
manifest=json.loads((ROOT/"reports/manifest.json").read_text(encoding="utf-8"))
checks=[]


def check(condition,message):
    if not condition:raise AssertionError(message)
    checks.append(message)


check(manifest["preview_count"]==62,"62 состояния: прежние 43 и 19 новых для скачивания и курсора")
for item in manifest["previews"]:
    im=Image.open(ROOT/"previews"/item["file"])
    check(im.size==(1024,768) and im.mode=="RGB",f"Размер и режим: {item['file']}")
for item in manifest["icons"]:
    im=Image.open(ROOT/"icons"/item["file"])
    check(im.mode=="RGBA" and im.size==tuple(item["size"]),f"RGBA и размер: {item['file']}")
    if item["file"]!="cursor_zuma.png":
        check(all(v%17==0 for v in im.getchannel("A").get_flattened_data()),f"Альфа 4 бита: {item['file']}")

# Курсор — с оригиналом в проекте (design/zuma_cursor, его делает make_cursor.py из arrow1.png
# Zuma Deluxe); шрифт — с исходником TXTVIEW2, если он есть на этом ПК.
sources={
    "icons/cursor_zuma.png":ROOT/"zuma_cursor/cursor.png",
    "fonts/NotoSansMono-Regular.ttf":Path(r"E:\zx\esp\wc-improved\source\plugins\txthex_viewer_VDAC2\fonts\NotoSansMono-Regular.ttf"),
    "fonts/OFL.txt":Path(r"E:\zx\esp\wc-improved\source\plugins\txthex_viewer_VDAC2\fonts\OFL.txt"),
}
for dest,src in sources.items():
    if src.exists():
        check((ROOT/dest).read_bytes()==src.read_bytes(),f"Побайтовое совпадение с исходником: {dest}")

# Независимый эталон по TXTVIEW2: общая baseline=26; отдельные ячейки 12x34.
ft=ImageFont.truetype(str(ROOT/"fonts/NotoSansMono-Regular.ttf"),20)
sample="Evolution window Spectrum gyjp АБВ Ёё Дд руф,.;: / 0123456789"
reference=Image.new("RGB",(len(sample)*12,34),r.C["paper"])
for n,ch in enumerate(sample):
    tile=Image.new("L",(12,34));d=ImageDraw.Draw(tile)
    box=d.textbbox((0,26),ch,font=ft,anchor="ls")
    if box[0]<0 or box[2]>12:
        wide=Image.new("L",(box[2]-box[0],34))
        ImageDraw.Draw(wide).text((-box[0],26),ch,font=ft,anchor="ls",fill=255)
        tile=wide.resize((12,34),Image.Resampling.LANCZOS)
    else:d.text((0,26),ch,font=ft,anchor="ls",fill=255)
    tile=tile.point([((v+8)//17)*17 for v in range(256)])
    reference.paste(r.C["ink"],(n*12,0),tile)
actual=r.Canvas(size=reference.size);actual.text(0,0,sample)
check(ImageChops.difference(reference,actual.im).getbbox() is None,
      "Evolution / window / Spectrum и кириллица совпадают с ячейками TXTVIEW2 12x34, baseline 26")

sheet=r.Canvas()
sheet.text(8,8,"Проверка базовой линии / TXTVIEW2",26)
sheet.text(8,72,"Эталон: ячейка 12x34, шаг 12, baseline 26",18,"muted")
sheet.im.paste(reference,(8,118))
sheet.text(8,174,"Отрисовщик превью: пиксели совпадают с эталоном",18,"muted")
sheet.im.paste(actual.im,(8,220))
sheet.text(8,286,"Линии ячеек и общая базовая линия (синий цвет):",18,"muted")
sheet.im.paste(actual.im,(8,336))
sheet.line(8,362,8+reference.width,362,"link")
for n in range(len(sample)+1):sheet.line(8+n*12,336,8+n*12,370,"line")
sheet.text(8,426,"20 px: абзацы и текст адресной строки",20)
sheet.text(8,484,"26 px: заголовок h2 — общая линия",26)
sheet.text(8,556,"32 px: Evolution / window",32)
sheet.text(8,660,"Глифы не выравниваются по своему верхнему краю.",18,"muted")
sheet.im.save(ROOT/"reports/font_baseline.png")

animations={"05_loading.apng":1200,"globe_10fps.apng":1200,
            "01a_navigation_swipe.apng":2400,"01b_address_swipe.apng":2880,
            "14_wifi_connecting.apng":1200,"16_download_indeterminate.apng":1400}
motion={}
for name,duration in animations.items():
    im=Image.open(ROOT/"previews"/name);total=0
    frames=[]
    for n in range(im.n_frames):
        im.seek(n);total+=im.info["duration"];frames.append(im.convert("RGBA").copy())
    check(im.n_frames>1 and total==duration,f"APNG {name}: {im.n_frames} кадров, {total:g} мс")
    motion[name]={"frames":im.n_frames,"duration_ms":total}
    if name=="05_loading.apng":
        for frame in frames[1:]:
            bbox=ImageChops.difference(frames[0].convert("RGB"),frame.convert("RGB")).getbbox()
            check(bbox is not None and bbox[0]>=196 and bbox[1]>=12 and bbox[2]<=228 and bbox[3]<=44,
                  "Загрузка: изменения ограничены битмапом глобуса 32x32")

budget=json.loads((ROOT/"reports/budget.json").read_text(encoding="utf-8"))
check(budget["dl_worst_with_reserve"]<=250,"Проектная оценка DL с резервом укладывается в 250 слов")
check(budget["ram_g_ui_region_bytes"]<=61440,"Прежнее окно UI укладывается в 60 КиБ")
check(budget["allocation"]["download_region"]["base"]==0xF4000 and budget["allocation"]["download_region"]["end"]<=0xF8000,
      "Новый текст скачивания помещается в отдельно разрешённую область 0x0F4000..0x0F7FFF")

hashes={str(p.relative_to(ROOT)).replace("\\","/"):hashlib.sha256(p.read_bytes()).hexdigest()
        for directory in (ROOT/"previews",ROOT/"icons") for p in sorted(directory.iterdir()) if p.is_file()}
result={"passed":len(checks),"checks":checks,"animations":motion,"artifact_sha256":hashes,
        "limits":"Аппаратное исполнение и интерактивный hit-test не проверялись; превью этапов 1–3."}
(ROOT/"reports/validation.json").write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
print(json.dumps({"passed":len(checks),"animations":motion},ensure_ascii=False))
