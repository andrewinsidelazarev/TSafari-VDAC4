"""Проверка этапа 3 без платы: память, состояния, маски и цвет курсора."""
import hashlib
import json
import os
from pathlib import Path

os.environ["TEMP"]=os.environ["TMP"]=r"E:\zx\cache\tmp\codex_tsafari_ui"
from PIL import Image, ImageChops
import render_previews as p
import stage3_previews as s

ROOT=Path(__file__).resolve().parent
s.bind(p)
for name in p.ICON_NAMES+[f"globe_{n:02}" for n in range(12)]:
    p.ASSETS[name]=Image.open(ROOT/"icons"/(name+".png")).convert("RGBA")


def sha(data):return hashlib.sha256(data).hexdigest()


def luminance(rgb):
    def linear(v):
        v=v/255
        return v/12.92 if v<=.04045 else ((v+.055)/1.055)**2.4
    return sum(w*linear(v) for w,v in zip((.2126,.7152,.0722),rgb))


def contrast(a,b):
    lo,hi=sorted((luminance(a),luminance(b)))
    return (hi+.05)/(lo+.05)


def main():
    checks=[]
    def check(ok,label):
        if not ok:raise AssertionError(label)
        checks.append(label)
    manifest=json.loads((ROOT/"ft/assets.json").read_text(encoding="utf-8"))
    old=json.loads((ROOT/"reports/pre_stage3_assets.json").read_text(encoding="utf-8"))
    blob=(ROOT/"ft/ui_assets.bin").read_bytes()
    check(len(blob)==18712 and sha(blob)==old["bundle"]["sha256"],"Все 18712 байт прежнего ui_assets.bin сохранены")
    check(manifest["assets"]==old["assets"] and manifest["load_plan"]==old["load_plan"],"Все 34 ресурса и два диапазона load_plan сохранены точно")
    check([b for b in manifest["buffers"] if b["name"]!="download_text"]==old["buffers"],"Ни один прежний текстовый буфер не перемещён")
    previous=json.loads((ROOT/"reports/pre_stage3_validation.json").read_text(encoding="utf-8"))["artifact_sha256"]
    previous={n:h for n,h in previous.items() if Path(n).suffix in (".png",".apng")}
    check(all(sha((ROOT/n).read_bytes())==h for n,h in previous.items()),f"Прежние {len(previous)} PNG/APNG побайтно прежние")
    buf=next(b for b in manifest["buffers"] if b["name"]=="download_text")
    check((buf["ram_g"],buf["width"],buf["height"],buf["stride"],buf["bytes"],buf["format"])==
          (0xF4000,480,68,240,16320,"L4"),"Download: 480x68 L4, 16320 байт с 0x0F4000")
    check(buf["ram_g"]+buf["bytes"]==0xF7FC0<0xF8000,"Текст скачивания оставляет 64 байта в разрешённом диапазоне")
    all_ranges=[(b["ram_g"],b["ram_g"]+b["bytes"]) for b in manifest["buffers"]]
    all_ranges += [(a["ram_g"],a["ram_g"]+a["bytes"]) for a in manifest["assets"] if a["canonical"]]
    all_ranges.sort()
    check(all(a[1]<=b[0] for a,b in zip(all_ranges,all_ranges[1:])),"Статические ресурсы, старые и новые динамические маски не пересекаются")
    names=sorted((ROOT/"previews").glob("16_*.png"))
    check(len(names)==19 and all(Image.open(n).size==(1024,768) for n in names),"19 новых PNG 16_* имеют размер 1024x768")
    states=("known","unknown","writing","done","no_space","sd_error","network_error","cancelled")
    for state in states:
        for name in ("ALTEREGO.ZIP",s.LONG_NAME):
            first,second=s.download_lines(state,name)
            mask=s.text_mask(first,second)
            check(len(first)<=34 and len(second)<=40 and mask.size==(480,68) and all(v%17==0 for v in mask.get_flattened_data()),
                  f"Маска TXTVIEW2 20 px: {state}, имя {len(name)} знаков, текст не обрезан")
    check(len(s.LONG_NAME)==48 and s.shorten(s.LONG_NAME,34).endswith(".ZIP") and "..." in s.shorten(s.LONG_NAME,34),"Имя 48 знаков сокращается в середине, расширение сохраняется")
    check(s.download_lines("done")[1]==s.SAVE_DIR+"ALTEREGO.ZIP","Готово: полный путь из задания")
    check(s.hover_text(s.LONG_NAME,40,9999)==s.LONG_NAME[-40:],"Наведение даёт прочитать конец длинного имени без утраты расширения")
    check(s.progress_geometry("known")== (0,192) and s.progress_geometry("done")== (0,480),"Известный объём: 48/120=40%, готово — полный бар")
    for state in ("unknown","writing"):
        check("%" not in " ".join(s.download_lines(state)) and s.progress_geometry(state,0)==(0,96) and
              s.progress_geometry(state,700)==(384,96) and s.progress_geometry(state,1400)==(0,96),
              f"{state}: без выдуманного процента, движение 0..384..0 за 1400 мс")
    for state in ("no_space","sd_error","network_error","cancelled"):
        check(s.progress_geometry(state)[1]==0,"Нет ложного прогресса в состоянии "+state)
    # Во время записи SD угол действия остаётся чистым, у сети там крестик.
    writing=s.download_frame("writing")
    region=writing.crop((484,672,508,696))
    check(region.getextrema()==tuple((v,v) for v in p.C["navy"]),"Во время записи на SD нет кнопки отмены")
    check(s.download_lines("known",hover="action")[1]=="Cancel download","Наведение поясняет действие отмены")
    check(s.DOWNLOAD_T["DONE_HIDE_MS"]==5000 and s.DOWNLOAD_T["ERROR_HIDE_MS"]==8000 and
          s.DOWNLOAD_G["HOVER_PAUSES_HIDE"]==1,"Готово 5 с, ошибка 8 с, таймер останавливается при наведении")
    normal=(8,656,520,760);raised=(8,608,520,712);status=(8,720,1016,760);js=(906,710,1008,750);scroll=(1000,8,1016,760)
    def separate(a,b):return a[2]<=b[0] or b[2]<=a[0] or a[3]<=b[1] or b[3]<=a[1]
    check(separate(raised,status) and separate(raised,js) and separate(raised,scroll) and separate(normal,js) and separate(normal,scroll),
          "Плашка не пересекает статус ссылки, JS или область мыши полосы прокрутки")
    anim=Image.open(ROOT/"previews/16_download_indeterminate.apng")
    first=anim.convert("RGB");duration=0;confined=True
    for n in range(anim.n_frames):
        anim.seek(n);duration+=anim.info["duration"]
        bbox=ImageChops.difference(first,anim.convert("RGB")).getbbox()
        if bbox:confined &= bbox[0]>=24 and bbox[1]>=740 and bbox[2]<=504 and bbox[3]<=746
    check(duration==1400 and anim.n_frames>1 and confined,"APNG неизвестного объёма: 1400 мс, меняется только узкая полоса")
    src=s.cursor_texel_image();mint=s.cursor_texel_image(p.C["mint"])
    check(src.size==mint.size==(38,38) and src.getchannel("A").tobytes()==mint.getchannel("A").tobytes(),"Форма, альфа и размер курсора неизменны")
    check(all(b==tuple(a[i]*p.C["mint"][i]//255 for i in range(3))+(a[3],)
              for a,b in zip(src.get_flattened_data(),mint.get_flattened_data())),"Все RGB-каналы курсора точно умножены на COLOR_RGB с делением на 255 и отбрасыванием")
    categories={"fill":lambda a:a==(255,255,255,255),
                "outline":lambda a:a[:3]==(0,0,0) and a[3]==255,
                "gray_transition":lambda a:a[3]==255 and 0<max(a[:3])<255,
                "alpha_edge":lambda a:0<a[3]<255}
    samples=[]
    for name,predicate in categories.items():
        x,y=next((x,y) for y in range(38) for x in range(38) if predicate(src.getpixel((x,y))))
        a=src.getpixel((x,y));b=mint.getpixel((x,y))
        samples.append({"kind":name,"xy":[x,y],"argb4_rgba":a,"tinted_rgba":b,
                        "on_paper":[(b[i]*b[3]+p.C["paper"][i]*(255-b[3])+127)//255 for i in range(3)],
                        "on_purple":[(b[i]*b[3]+(51,0,102)[i]*(255-b[3])+127)//255 for i in range(3)]})
    ratios={"mint_on_purple":contrast(p.C["mint"],(51,0,102)),"link_teal_on_purple":contrast(p.C["link"],(51,0,102)),
            "outline_on_paper":contrast((0,0,0),p.C["paper"]),"mint_on_paper":contrast(p.C["mint"],p.C["paper"])}
    check(ratios["mint_on_purple"]>5 and ratios["mint_on_purple"]>ratios["link_teal_on_purple"] and ratios["outline_on_paper"]>10,
          "На #330066 читается мятная заливка, на светлой бумаге — тёмный контур")
    for dark in (False,True):
        for kind in ("link","visited","text"):
            expected=s.cursor_page(dark,kind)
            index=20+(3 if dark else 0)+{"link":0,"visited":1,"text":2}[kind]
            file=ROOT/"previews"/f"16_{index}_cursor_{'dark' if dark else 'paper'}_{kind}.png"
            check(Image.open(file).tobytes()==expected.tobytes(),f"Курсор в превью: {kind}, {'тёмный' if dark else 'светлый'} фон")
    budget=json.loads((ROOT/"reports/budget.json").read_text(encoding="utf-8"))
    check(sum(budget["dl_download_components"].values())==37 and 188+37+1+24==budget["dl_worst_with_reserve"]==250,
          "Модель DL: 37 слов плашка + 1 запас COLOR_RGB, максимум 250 с общим резервом 24")
    check(budget["ram_g_total_bytes"]==80120 and budget["ram_g_ui_region_bytes"]==52784,
          "Общий UI с новым буфером 80120 байт; прежнее окно UI остаётся 52784 байта")
    report={"stage":3,"passed":len(checks),"checks":checks,"old_artifacts_unchanged":len(previous),
            "bundle_unchanged_sha256":sha(blob),"cursor_samples":samples,"contrast_ratios":ratios,
            "budget":manifest["budget"],"limits":"Сверены PNG, бинарные ресурсы и арифметика. Плата, сеть, реальный hit-test и время DL не проверялись."}
    (ROOT/"reports/stage3_validation.json").write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({"passed":len(checks),"old_artifacts":len(previous),"samples":samples,"contrast":ratios},ensure_ascii=False))


if __name__=="__main__":main()
