#!/usr/bin/env python3
"""Этап 2: независимое декодирование FT812, карта памяти, заголовок C/C++.

Компилятор используется только с -fsyntax-only. Временные файлы — на E:.
"""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parent
TMP=Path(r"E:\zx\cache\tmp\codex_tsafari_ui")
os.environ["TEMP"]=os.environ["TMP"]=str(TMP)
from PIL import Image, ImageChops, ImageDraw, ImageFont
import build_ft_assets as build
import render_previews as preview


def sha(data):return hashlib.sha256(data).hexdigest()


def decode(asset,data):
    im=Image.new("RGBA",(asset["width"],asset["height"]))
    pix=im.load()
    for y in range(im.height):
        for x in range(im.width):
            if asset["format"]=="L4":
                v=data[y*asset["stride"]+x//2]
                alpha=((v>>4) if x%2==0 else (v&15))*17
                pix[x,y]=(255,255,255,alpha)
            else:
                at=y*asset["stride"]+x*2
                word=data[at]|(data[at+1]<<8)
                pix[x,y]=(((word>>8)&15)*17,((word>>4)&15)*17,(word&15)*17,(word>>12)*17)
    return im


def main():
    TMP.mkdir(exist_ok=True)
    checks=[]
    def check(ok,label):
        if not ok:raise AssertionError(label)
        checks.append(label)

    # Сохраняем контрольные суммы до второй сборки и проверяем повторяемость.
    build.build()
    paths=[p for p in (ROOT/"ft").iterdir() if p.suffix in (".l4", ".argb4", ".bin") or p.name=="assets.json"]
    paths += [ROOT/"ui_design.h",ROOT/"icons/js_label.png",ROOT/"icons/wifi_title.png"]
    paths += [ROOT/"icons"/(name+".png") for name in preview.INPUT_LABELS]
    before={str(p.relative_to(ROOT)):sha(p.read_bytes()) for p in paths if p.is_file()}
    build.build()
    check(all(sha((ROOT/p).read_bytes())==h for p,h in before.items()),"Повторная сборка FT-ресурсов, манифеста и заголовка побайтно совпадает")
    approved=json.loads((ROOT/"reports/accepted_stage1_sha256.json").read_text(encoding="utf-8"))
    check(len(approved)==72,"Зафиксированы 72 принятых PNG/APNG и ресурса этапа 1")
    check(all(sha((ROOT/name).read_bytes())==value for name,value in approved.items()),"Все 72 принятых PNG/APNG и ресурса этапа 1 сохранены побайтно")
    manifest=json.loads((ROOT/"ft/assets.json").read_text(encoding="utf-8"))
    bundle=(ROOT/"ft/ui_assets.bin").read_bytes()
    check(len(bundle)==manifest["bundle"]["bytes"]==18712,"Статический пакет: 18 712 байт")
    check(sha(bundle)==manifest["bundle"]["sha256"],"SHA-256 пакета совпадает с манифестом")
    entries={a["name"]:a for a in manifest["assets"]}
    check(len(entries)==len(manifest["assets"])==34,"34 уникальных ресурса в манифесте")
    old=json.loads((ROOT/"reports/pre_language_assets.json").read_text(encoding="utf-8"))
    check(sha(bundle[:old["bundle"]["bytes"]])==old["bundle"]["sha256"],"Прежние 18008 байт пакета не изменены, новые маски только в конце")
    check(all(all(entries[a["name"]][key]==a[key] for key in ("ram_g","bundle_offset","bytes","stride","format","sha256"))
              for a in old["assets"]),"Все прежние адреса, смещения и бинарные ресурсы сохранены")
    check([b for b in manifest["buffers"] if b["name"]!="download_text"]==old["buffers"] and manifest["startup_views"]==old["startup_views"] and
          manifest["memory"]["reserve"]==old["memory"]["reserve"],"Все прежние текстовые буферы, Wi-Fi views и резерв сохранили адреса и размеры")
    previous=json.loads((ROOT/"reports/pre_language_validation.json").read_text(encoding="utf-8"))["artifact_sha256"]
    previous={name:h for name,h in previous.items() if Path(name).suffix in (".png",".apng")}
    check(all(sha((ROOT/name).read_bytes())==h for name,h in previous.items()),"Все PNG/APNG предыдущей версии сохранены побайтно")
    segments=manifest["load_plan"]
    check(manifest["version"]==3 and manifest["bundle"]["segmented"] and len(segments)==2 and
          [(s["file_offset"],s["ram_g"],s["bytes"]) for s in segments]==[(0,0xE1000,18008),(18008,0xEDB70,704)],
          "План загрузки: прежний блок и хвост 704 байта в свободную область 0x0EDB70")
    simulated=bytearray([0xA5])*61440
    for segment in segments:
        lo=segment["ram_g"]-0xE1000;off=segment["file_offset"];count=segment["bytes"]
        simulated[lo:lo+count]=bundle[off:off+count]
    check(len(simulated)==61440 and all(simulated[b["ram_g"]-0xE1000:b["ram_g"]-0xE1000+b["bytes"]]==bytes([0xA5])*b["bytes"]
              for b in manifest["buffers"] if b["ram_g"]<0xF0000),"Загрузка двух диапазонов не записывает ничего в динамические UI-буферы")
    decoded={}
    for asset in manifest["assets"]:
        name=asset["name"];data=(ROOT/"ft"/asset["file"]).read_bytes()
        check(len(data)==asset["bytes"]==asset["stride"]*asset["height"],f"Размер и stride: {name}")
        check(sha(data)==asset["sha256"],f"SHA-256 ресурса: {name}")
        off=asset["bundle_offset"]
        check(bundle[off:off+len(data)]==data,f"Размещение внутри пакета: {name}")
        segment=next(s for s in segments if s["file_offset"]<=off and off+len(data)<=s["file_offset"]+s["bytes"])
        check(asset["ram_g"]==segment["ram_g"]+off-segment["file_offset"] and asset["ram_g"]%4==0 and
              simulated[asset["ram_g"]-0xE1000:asset["ram_g"]-0xE1000+len(data)]==data,f"Адрес, выравнивание и загрузка: {name}")
        source=Image.open(ROOT/"ft"/asset["png"]).convert("RGBA")
        out=decode(asset,data);decoded[name]=out
        check(out.size==source.size,f"Нативный размер: {name}")
        if asset["format"]=="L4":
            check(ImageChops.difference(source.getchannel("A"),out.getchannel("A")).getbbox() is None,
                  f"L4: альфа после декодирования совпадает с PNG: {name}")
        else:
            expected=Image.merge("RGBA",tuple(ch.point([(v>>4)*17 for v in range(256)]) for ch in source.split()))
            check(expected.tobytes()==out.tobytes(),"ARGB4: все каналы соответствуют усечению до 4 бит")
    strip=(ROOT/"ft/globe_strip.l4").read_bytes()
    check(strip==b"".join((ROOT/"ft"/f"globe_{n:02}.l4").read_bytes() for n in range(12)),"Лента глобуса — точная конкатенация 12 кадров")
    check(entries["globe_strip"]["cell_height"]==32 and entries["globe_strip"]["cell_bytes"]==512,
          "Глобус: высота ячейки 32, шаг 512 байт")
    for n in range(12):
        check(entries[f"globe_{n:02}"]["ram_g"]==entries["globe_strip"]["ram_g"]+512*n,
              f"Адрес кадра глобуса {n}")

    # Эталонные байты исключают одинаковую ошибку упаковщика и декодера.
    sample=Image.new("RGBA",(16,1),(255,255,255,0))
    sample.putalpha(Image.frombytes("L",(16,1),bytes(i*17 for i in range(16))))
    check(build.pack_l4(sample)==bytes.fromhex("01 23 45 67 89 AB CD EF"),"Эталон L4: старшая тетрада слева")
    sample=Image.new("RGBA",(1,1),(0x12,0x34,0x56,0x78))
    check(build.pack_argb4(sample)==bytes.fromhex("35 71"),"Эталон ARGB4: A4R4G4B4 little-endian")
    zuma=(ROOT/"zuma_cursor/cursor_p00.bin").read_bytes()   # ресурс FT812 Zuma Deluxe (make_cursor.py)
    check((ROOT/"ft/cursor_zuma.argb4").read_bytes()==zuma[:2888],"Курсор совпадает с 2888 байтами оригинального FT812-ресурса Zuma")

    ranges=[(a["ram_g"],a["ram_g"]+a["bytes"],a["name"]) for a in manifest["assets"] if a["canonical"]]
    ranges += [(b["ram_g"],b["ram_g"]+b["bytes"],b["name"]) for b in manifest["buffers"] if b["ram_g"]<0xF0000]
    reserve=manifest["memory"]["reserve"];ranges.append((reserve["base"],reserve["end"],"reserve"))
    ranges.sort()
    check(all(0xE1000<=lo<hi<=0xF0000 and lo%4==0 and (hi-lo)%4==0 for lo,hi,_ in ranges),"Все UI-диапазоны внутри [0x0E1000,0x0F0000), кратны 4")
    check(all(a[1]<=b[0] for a,b in zip(ranges,ranges[1:])),"UI-ресурсы, динамические маски и резерв не пересекаются")
    check(ranges[-1][1]==0xEDE30,"Конец занятой области UI с резервом: 0x0EDE30")
    address=next(b for b in manifest["buffers"] if b["name"]=="address_text")
    check(address["ram_g"]==0xF0000 and address["bytes"]==11016 and address["stride"]==324,
          "Адресная маска: 0x0F0000, 648x34 L4, 11 016 байт")
    for view in manifest["startup_views"]:
        owners=[next(b for b in manifest["buffers"] if b["name"]==name) for name in view["alias_of_buffers"]]
        contiguous=all(a["ram_g"]+a["bytes"]==b["ram_g"] for a,b in zip(owners,owners[1:]))
        check(contiguous and view["ram_g"]==owners[0]["ram_g"] and view["bytes"]==view["stride"]*view["height"]<=sum(b["bytes"] for b in owners),
              f"Wi-Fi: маска {view['name']} помещается в переиспользуемый буфер")
    title=entries["wifi_title"]
    check(title["canonical"] and title["format"]=="L4" and title["ram_g"]==0xE46E0 and
          title["width"]==144 and title["height"]==55 and title["bytes"]==3960,
          "TSafari 32 px — статическая L4-маска 144x55 в пакете с 0x0E46E0")
    views=sorted(manifest["startup_views"],key=lambda v:v["ram_g"])
    check(len(views)==3 and all(a["ram_g"]+a["bytes"]<=b["ram_g"] for a,b in zip(views,views[1:])) and
          all(v["ram_g"]>=title["ram_g"]+title["bytes"] for v in views),
          "Три динамические маски Wi-Fi не пересекаются друг с другом или готовым заголовком")
    subtitle=next(v for v in views if v["name"]=="subtitle")
    check(subtitle["width"]==408 and subtitle["height"]==34 and subtitle["bytes"]==6936,
          "Подпись Wi-Fi: 34 ячейки по 12 px, L4 408x34, 6936 байт")
    expected_title=preview.Canvas("navy",size=(144,55))
    expected_title.text(5,0,"TSafari",32,"ui_text")
    decoded_title=Image.new("RGB",(144,55),preview.C["navy"])
    decoded_title.paste(preview.C["ui_text"],(0,0),decoded["wifi_title"].getchannel("A"))
    check(decoded_title.tobytes()==expected_title.im.tobytes(),"Готовая wifi_title сохраняет прежний вид TSafari 32 px")
    expected_subtitle=preview.Canvas("navy",size=(408,34))
    expected_subtitle.text(0,0,"Web browser for VDAC4 ZX Evolution",20,"ui_muted")
    for name in ("14a_wifi_connecting.png","14b_wifi_unavailable.png","14c_wifi_ready.png"):
        im=Image.open(ROOT/"previews"/name)
        check(im.size==(1024,768) and im.mode=="RGB",f"Wi-Fi: размер и режим {name}")
        if name!="14c_wifi_ready.png":
            check(im.crop((308,284,716,318)).tobytes()==expected_subtitle.im.tobytes(),
                  f"Полная подпись 20 px в центре окна без обрезки: {name}")
            check(im.crop((440,226,584,281)).tobytes()==decoded_title.tobytes(),
                  f"Заголовок превью совпадает с декодированным wifi_title.l4: {name}")
    ready=Image.open(ROOT/"previews/14c_wifi_ready.png")
    home=Image.open(ROOT/"previews/09_home.png")
    check(ready.tobytes()==home.tobytes(),"После появления сети Wi-Fi окно исчезает, показана стартовая страница")
    motion=Image.open(ROOT/"previews/14_wifi_connecting.apng")
    first=motion.convert("RGB");duration=0;inside=True
    for n in range(motion.n_frames):
        motion.seek(n);duration+=motion.info["duration"]
        if n:
            bounds=ImageChops.difference(first,motion.convert("RGB")).getbbox()
            inside &= bounds is not None and bounds[0]>=496 and bounds[1]>=334 and bounds[2]<=528 and bounds[3]<=366
    check(motion.n_frames==12 and duration==1200 and inside,"Wi-Fi APNG: 12 кадров / 1200 мс, изменяется только глобус 32x32")

    # Флажки: изображение должно совпадать с FT-маской, в середине текста оно скрыто.
    for name in list(preview.ICON_NAMES)+[f"globe_{n:02}" for n in range(12)]+list(preview.INPUT_LABELS):
        preview.ASSETS[name]=Image.open(ROOT/"icons"/(name+".png")).convert("RGBA")
    for lang,filename in (("en","15a_input_en.png"),("ru","15b_input_ru.png")):
        a=entries["input_"+lang]
        check((a["width"],a["height"],a["stride"],a["bytes"],a["format"])==(32,22,16,352,"L4"),f"{lang}: метка 32x22 L4, не выше каретки 23 px")
        expected=Image.new("RGB",(32,22),preview.C["navy"])
        expected.paste(preview.C["mint"],(0,0),decoded["input_"+lang].getchannel("A"))
        actual=Image.open(ROOT/"previews"/filename)
        check(actual.size==(1024,768) and actual.crop((334,17,366,39)).tobytes()==expected.tobytes(),f"{lang}: метка после vtrd.in, x=334, y=17, совпадает с FT812")
    detail=Image.open(ROOT/"previews/15c_input_language_detail.png")
    check(detail.size==(1024,768),"Крупный вид EN/RU: 1024x768")
    def address_frame(**kwargs):
        c=preview.Canvas();preview.toolbar(c,opened=True,url=kwargs.pop("url","vtrd.in"),**kwargs)
        return c.im
    for kwargs in ({"caret_index":3},{"selection":True},{"address_focused":False},{"address":0.5}):
        check(address_frame(input_language="en",**kwargs).tobytes()==address_frame(**kwargs).tobytes(),f"Метка скрыта: {kwargs}")
    for url in ("", "x"*54):
        decorated=address_frame(url=url,input_language="ru");plain=address_frame(url=url)
        x=244+len(url)*12+6
        check(x+32<=930 and decorated.crop((x,17,x+32,39)).tobytes()!=plain.crop((x,17,x+32,39)).tobytes(),
              f"Метка у конца строки длины {len(url)}: полностью видна, зазор перед Go не меньше 8 px")
    diff=ImageChops.difference(Image.open(ROOT/"previews/15a_input_en.png"),Image.open(ROOT/"previews/15b_input_ru.png")).getbbox()
    check(diff is not None and diff[0]>=334 and diff[1]>=17 and diff[2]<=366 and diff[3]<=39,"Смена EN/RU изменяет только флажок, URL vtrd.in остаётся прежним")

    header=(ROOT/"ui_design.h").read_text(encoding="utf-8")
    clean=re.sub(r"/\*.*?\*/","",header,flags=re.S)
    check("#include" not in clean and not re.search(r"\b(float|double|typedef|struct|extern)\b",clean),"Заголовок без зависимостей, float, функций и пользовательских типов")
    text=re.sub(r"^\s*#.*$","",clean,flags=re.M)
    text=re.sub(r"static const (?:unsigned (?:char|short)|short) \w+\[\d+\]\s*=\s*\{[\d,\s-]+\};","",text)
    check(not text.strip(),"В заголовке только макросы, include guard и простые const-таблицы")
    def carray(name):
        result=re.search(rf"tsa_ui_{name}\[\d+\]\s*=\s*\{{([^}}]+)\}}",header,re.S)
        return [int(v) for v in re.findall(r"-?\d+",result.group(1))]
    nav=carray("nav_offset_px");alpha=carray("nav_globe_alpha");addr=carray("address_offset_px")
    check(len(nav)==241 and nav[0]==184 and nav[-1]==0 and all(a>=b for a,b in zip(nav,nav[1:])),"Таблица навигации: 240 мс, монотонность и точные концы")
    check(len(addr)==201 and addr[0]==-784 and addr[-1]==0 and all(a<=b for a,b in zip(addr,addr[1:])),"Таблица адреса: 200 мс, монотонность и точные концы")
    check(alpha[0]==112 and alpha[-1]==255 and all(a<=b for a,b in zip(alpha,alpha[1:])),"Прозрачность глобуса: 112..255")
    check([nav[t] for t in range(0,241,40)]==[184,153,123,92,61,31,0],"Положения навигации совпадают с принятыми кадрами APNG")
    check([addr[t] for t in range(0,201,40)]==[-784,-627,-470,-314,-157,0],"Положения адреса совпадают с принятыми кадрами APNG")

    gcc_root=Path(r"E:\zx\esp\.espressif\tools\xtensa-esp-elf")
    gcc=next(gcc_root.glob("*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc.exe"))
    gpp=gcc.with_name("xtensa-esp32s3-elf-g++.exe")
    source=TMP/"tsafari_ui_header_check.c"
    source.write_text('''/* Проверка автономного заголовка без SDK. */
#include "ui_design.h"
#include "ui_design.h"
#define CHECK(name,expr) typedef char name[(expr)?1:-1]
CHECK(screen, TSA_UI_SCREEN_WIDTH == 1024 && TSA_UI_SCREEN_HEIGHT == 768);
CHECK(hot, TSA_UI_HOT_WIDTH == 4 && TSA_UI_HOT_HEIGHT == 4);
CHECK(ram, TSA_UI_RAM_G_BASE == 0xE1000 && TSA_UI_RAM_G_END == 0xF0000);
CHECK(addr, TSA_UI_ADDRESS_RAM_G_BASE == 0xF0000 && TSA_UI_ADDRESS_L4_BYTES == 11016);
CHECK(capacity, TSA_UI_RAM_G_USED_WITH_RESERVE + TSA_UI_RAM_G_FREE_BYTES == TSA_UI_RAM_G_CAPACITY);
CHECK(nav_count, sizeof(tsa_ui_nav_offset_px)/sizeof(tsa_ui_nav_offset_px[0]) == TSA_UI_NAV_ANIMATION_MS+1);
CHECK(addr_count, sizeof(tsa_ui_address_offset_px)/sizeof(tsa_ui_address_offset_px[0]) == TSA_UI_ADDRESS_ANIMATION_MS+1);
CHECK(font, TSA_UI_FONT_CELL_WIDTH == 12 && TSA_UI_FONT_CELL_HEIGHT == 34 && TSA_UI_FONT_BASELINE == 26);
CHECK(wifi_title, TSA_UI_ASSET_WIFI_TITLE_RAM_G >= TSA_UI_RAM_G_BASE && TSA_UI_ASSET_WIFI_TITLE_RAM_G + TSA_UI_ASSET_WIFI_TITLE_BYTES <= TSA_UI_RAM_G_END);
CHECK(wifi_subtitle, TSA_UI_WIFI_BUFFER_SUBTITLE_WIDTH == 408 && TSA_UI_WIFI_SUBTITLE_X * 2 + TSA_UI_WIFI_BUFFER_SUBTITLE_WIDTH == TSA_UI_SCREEN_WIDTH);
CHECK(wifi_text, sizeof(TSA_UI_WIFI_SUBTITLE_TEXT) - 1 == TSA_UI_WIFI_SUBTITLE_COLUMNS);
CHECK(input_height, TSA_UI_INPUT_LANG_HEIGHT <= TSA_UI_CARET_HEIGHT);
CHECK(input_gap, TSA_UI_INPUT_LANG_OFFSET_FROM_CARET_X == TSA_UI_CARET_WIDTH + TSA_UI_INPUT_LANG_GAP_FROM_CARET_RIGHT);
CHECK(input_bounds, TSA_UI_INPUT_LANG_MAX_CARET_X + TSA_UI_INPUT_LANG_OFFSET_FROM_CARET_X + TSA_UI_INPUT_LANG_WIDTH == TSA_UI_INPUT_LANG_MAX_RIGHT);
CHECK(input_keys, TSA_UI_INPUT_LANG_SWITCH_CTRL == 1 && TSA_UI_INPUT_LANG_SWITCH_SHIFT == 1);
CHECK(input_tail, TSA_UI_ASSET_INPUT_EN_RAM_G == 0xEDB70 && TSA_UI_ASSET_INPUT_RU_RAM_G == 0xEDCD0);
CHECK(segments, TSA_UI_BUNDLE_SEGMENT_COUNT == 2 && TSA_UI_BUNDLE_SEGMENT_1_FILE_OFFSET == 18008);
CHECK(download_ram, TSA_UI_DOWNLOAD_RAM_G_BASE == 0xF4000 && TSA_UI_DOWNLOAD_RAM_G_END <= 0xF8000);
CHECK(download_mask, TSA_UI_DOWNLOAD_TEXT_BYTES == TSA_UI_DOWNLOAD_TEXT_STRIDE * TSA_UI_DOWNLOAD_TEXT_HEIGHT);
CHECK(download_size, TSA_UI_DOWNLOAD_TEXT_WIDTH == 480 && TSA_UI_DOWNLOAD_TEXT_HEIGHT == 68);
CHECK(cursor_link, TSA_UI_CURSOR_LINK_RAM_G == TSA_UI_ASSET_CURSOR_ZUMA_RAM_G && TSA_UI_CURSOR_LINK_HOTSPOT_X == 1);
int main(void) {
    return tsa_ui_nav_offset_px[0] + tsa_ui_address_offset_px[0]
        + tsa_ui_nav_globe_alpha[0] + tsa_ui_nav_centers_x[0];
}
''',encoding="utf-8")
    compilers=[]
    for compiler,std,language in ((gcc,"c99","c"),(gcc,"c11","c"),(gpp,"c++11","c++")):
        command=[str(compiler),"-x",language,"-std="+std,"-Wall","-Wextra","-Werror","-pedantic-errors",
                 "-fsyntax-only","-I",str(ROOT),str(source)]
        result=subprocess.run(command,cwd=TMP,capture_output=True,text=True)
        check(result.returncode==0 and not result.stderr.strip(),f"Автономный заголовок: {std}, ESP32-S3 GCC, без предупреждений; {result.stderr.strip()}")
        compilers.append({"compiler":str(compiler),"standard":std,"exit_code":result.returncode})

    # Лист визуальной сверки: слева PNG, справа декодированный FT812, нативный размер.
    sheet=preview.Canvas("navy")
    sheet.text(8,8,"PNG / FT812: проверка ресурсов",26,"ui_text")
    sheet.text(8,56,"Слева исходник, справа декодирование. Размер 1:1.",18,"ui_muted")
    names=[a["name"] for a in manifest["assets"] if a["name"] not in ("globe_strip","wifi_title","input_en","input_ru")]
    for index,name in enumerate(names):
        x=12+(index%6)*168;y=110+(index//6)*122
        asset=entries[name];png=Image.open(ROOT/"ft"/asset["png"]).convert("RGBA")
        for at,im in ((x+24,png),(x+82,decoded[name])):
            if asset["format"]=="L4":sheet.im.paste(preview.C["mint"],(at,y),im.getchannel("A"))
            else:sheet.im.paste(im,(at,y),im)
        sheet.text(x,y+52,name,14,"ui_text")
    title_png=Image.open(ROOT/"icons/wifi_title.png")
    for x,im in ((24,title_png),(216,decoded["wifi_title"])):
        sheet.im.paste(preview.C["mint"],(x,700),im.getchannel("A"))
    sheet.text(408,710,"wifi_title / 144 x 55 / L4",18,"ui_text")
    for x,name in ((760,"input_en"),(888,"input_ru")):
        png=Image.open(ROOT/"icons"/(name+".png"))
        for at,im in ((x,png),(x+42,decoded[name])):
            sheet.im.paste(preview.C["mint"],(at,700),im.getchannel("A"))
        sheet.text(x,730,name,14,"ui_text")
    sheet.im.save(ROOT/"reports/ft_roundtrip.png")
    report={"stage":2,"passed":len(checks),"checks":checks,"compilers":compilers,
            "approved_artifacts_unchanged":72,"previous_version_artifacts_unchanged":len(previous),"reproducible_files":len(before),
            "budget":manifest["budget"],"bundle_sha256":sha(bundle),
            "limitations":"DL и плата не запускались. 12,5/17,0 мс — замер пользователя, не этого скрипта."}
    (ROOT/"reports/stage2_validation.json").write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"passed":len(checks),"approved_unchanged":72,"reproducible":len(before),"C_Cpp":"OK"}))


if __name__=="__main__":main()
