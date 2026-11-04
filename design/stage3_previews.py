"""Этап 3: плашка скачивания и умножение цвета исходного ARGB4-курсора.

Рисуется только дизайн, без браузера, сети или доступа к плате.
"""
from PIL import Image, ImageChops, ImageDraw

DOWNLOAD_G = {
    "X":8,"Y":656,"Y_WITH_STATUS":608,"WIDTH":512,"HEIGHT":104,"RADIUS":8,
    "STATUS_SHIFT":48,"STATUS_GAP":8,"TEXT_X":24,"TEXT_Y_OFFSET":4,
    "TEXT_WIDTH":480,"TEXT_HEIGHT":68,"TEXT_LINE_STEP":34,"TEXT_COLUMNS":40,
    "TITLE_OFFSET_X":36,"TITLE_COLUMNS":34,"FILENAME_MAX_CHARS":48,
    "ELLIPSIS_CHARS":3,"FILENAME_SUFFIX_CHARS":11,
    "ICON_X":24,"ICON_Y_OFFSET":16,"ICON_WIDTH":24,"ICON_HEIGHT":24,
    "ACTION_X":484,"ACTION_Y_OFFSET":16,"ACTION_WIDTH":24,"ACTION_HEIGHT":24,
    "ACTION_CENTER_X":496,"ACTION_CENTER_Y_OFFSET":28,"ACTION_HOVER_RADIUS":16,
    "ACTION_HIT_X":480,"ACTION_HIT_Y_OFFSET":12,"ACTION_HIT_WIDTH":32,"ACTION_HIT_HEIGHT":32,
    "BAR_X":24,"BAR_Y_OFFSET":84,"BAR_WIDTH":480,"BAR_HEIGHT":6,"BAR_RADIUS":3,
    "INDETERMINATE_WIDTH":96,"INDETERMINATE_TRAVEL":384,
    "KNOWN":0,"UNKNOWN":1,"WRITING":2,"DONE":3,"ERROR":4,"CANCELLED":5,
    "CANCEL_NETWORK_ONLY":1,"TERMINAL_CLICK_DISMISSES":1,"HOVER_PAUSES_HIDE":1,
    "KEEP_POSITION_WHILE_HOVERED":1,"VISIBLE_DURING_ADDRESS_EDIT":1,
}
DOWNLOAD_T = {"DONE_HIDE_MS":5000,"ERROR_HIDE_MS":8000,"CANCELLED_HIDE_MS":3000,
              "INDETERMINATE_HALF_MS":700,"INDETERMINATE_CYCLE_MS":1400,
              "TEXT_HOVER_DELAY_MS":800,"TEXT_SCROLL_STEP_MS":160}
SAVE_DIR = "\\zifi\\downloads\\2026-11-04\\"
LONG_NAME = "ALTEREGO_DELUXE_ZX_EVOLUTION_SPEC_2026_FINAL.ZIP"
P = None


def bind(preview):
    global P
    P=preview


def shorten(text, columns):
    if len(text)<=columns:return text
    tail=11
    return text[:columns-3-tail]+"..."+text[-tail:]


def hover_text(text, columns, tick):
    shift=min(max(0,len(text)-columns),max(0,tick-800)//160)
    return text[shift:shift+columns]


def download_lines(state, filename="ALTEREGO.ZIP", hover=None, tick=0):
    first=hover_text(filename,34,tick) if hover=="body" else shorten(filename,34)
    second={"known":"48 KiB / 120 KiB","unknown":"48 KiB received",
            "writing":"Writing to Evo SD...","no_space":"No space on Evo SD",
            "sd_error":"SD write failed","network_error":"Network error",
            "cancelled":"Download cancelled"}.get(state,"")
    if state=="done":
        full=SAVE_DIR+filename
        if len(full)<=40:first,second="Saved to Evo SD",full
        else:
            first=SAVE_DIR
            second=hover_text(filename,40,tick) if hover=="body" else shorten(filename,40)
    if hover=="action" and state in ("known","unknown"):second="Cancel download"
    return first,second


def text_mask(first,second):
    # Две строки TXTVIEW2: реальные 12x34, baseline=26, 4 бита alpha.
    alpha=Image.new("L",(480,68))
    for x,y,text in ((36,0,first),(0,34,second)):
        assert x+len(text)*12<=480
        for n,ch in enumerate(text):
            tile=Image.new("L",(12,34))
            ImageDraw.Draw(tile).text((0,26),ch,font=P.font(20),anchor="ls",fill=255)
            alpha.paste(tile,(x+n*12,y))
    return alpha.point([min(15,(v+8)//17)*17 for v in range(256)])


def progress_geometry(state,tick=0):
    if state=="known":return 0,480*48//120
    if state=="done":return 0,480
    if state in ("unknown","writing"):
        phase=tick%1400
        return 384*(phase if phase<=700 else 1400-phase)//700,96
    return 0,0


def draw_download(c,state="known",filename="ALTEREGO.ZIP",status_visible=False,hover=None,tick=0):
    y=608 if status_visible else 656
    c.box(8,y,512,104,"panel" if hover=="body" else "navy",8)
    # Для hover меняется только фон. Текстовая маска остаётся одного цвета.
    first,second=download_lines(state,filename,hover,tick)
    c.im.paste(P.C["ui_text"],(24,y+4),text_mask(first,second))
    failed=state in ("no_space","sd_error","network_error")
    icon="check" if state=="done" else "warning" if failed else "close" if state=="cancelled" else "download"
    c.icon(icon,24,y+16,"amber" if failed or state=="writing" else "mint")
    if state!="writing":
        if hover=="action":c.dot(496,y+28,16,"hover")
        c.icon("close",484,y+16,"mint" if hover=="action" else "ui_muted")
    c.box(24,y+84,480,6,"edge",3)
    off,width=progress_geometry(state,tick)
    if width:c.box(24+off,y+84,width,6,"amber" if state=="writing" else "mint",3)
    return (8,y,520,y+104)


def cursor_texel_image(tint=(255,255,255)):
    # Декодируем именно существующий FT-ресурс, а не 8-битный оригинал PNG.
    data=(P.ROOT/"ft/cursor_zuma.argb4").read_bytes()
    rgba=[]
    for i in range(0,len(data),2):
        w=data[i]|data[i+1]<<8
        src=(((w>>8)&15)*17,((w>>4)&15)*17,(w&15)*17)
        # ft_rd_exec.sv: RGB * COLOR_RGB / 255 с отбрасыванием, alpha не меняется.
        rgba.append(tuple(v*t//255 for v,t in zip(src,tint))+((w>>12)*17,))
    im=Image.new("RGBA",(38,38));im.putdata(rgba)
    return im


def draw_cursor(c,x,y,link=False):
    im=cursor_texel_image(P.C["mint"] if link else (255,255,255))
    c.im.paste(im,(x-1,y-1),im)


def download_frame(state="known",filename="ALTEREGO.ZIP",hover=None,coexist=False,tick=0):
    c=P.Canvas();P.article(c,hover=True if coexist else False,scrolled=False);c.page_done()
    P.toolbar(c,loading=state in ("known","unknown"),service=coexist)
    if coexist:
        P.scrollbar(c,drag=True,pos=400)
        P.status(c,"https://zx-notes.example/evo/build-notes")
    draw_download(c,state,filename,coexist,hover,tick)
    y=608 if coexist else 656
    if hover=="action":draw_cursor(c,496,y+28)
    elif hover=="body":draw_cursor(c,402,y+57)
    elif coexist:draw_cursor(c,192,472,True)
    else:draw_cursor(c,714,196)
    return c.im


def cursor_page(dark=False,kind="link"):
    c=P.Canvas();P.article(c)
    if dark:
        # Точный пользовательский фон, без притязания воспроизвести страницу vtrd.in.
        c.d.rectangle((0,0,1023,246),fill=(0x33,0,0x66))
        c.text(56,8,"Frame sample / #330066",20,"ui_text")
        c.text(24,72,"Download archive",20,"ui_text")
        c.line(24,101,216,101,"mint")
        c.text(360,72,"Visited archive",20,"ui_muted")
        c.line(360,101,540,101,"mint")
        c.text(24,150,"Plain text / ordinary white cursor",20,"ui_text")
        pointer={"link":(148,90),"visited":(452,90),"text":(260,170)}[kind]
    else:pointer={"link":(192,472),"visited":(468,472),"text":(714,196)}[kind]
    # Сохраняем точный #330066 в образце; реальные палитры страницы задаёт ESP.
    P.toolbar(c)
    if kind!="text":P.status(c,"https://zx-notes.example/"+("archive" if kind=="visited" else "download"))
    draw_cursor(c,*pointer,link=kind!="text")
    return c.im


def cursor_detail():
    c=P.Canvas("navy")
    c.text(24,18,"Cursor / original ARGB4 x COLOR_RGB",26,"ui_text")
    c.text(24,62,"Links and visited links: mint #73DFC6. Text: white.",18,"ui_muted")
    for x,tint,label in ((24,(255,255,255),"Text / white"),(360,P.C["mint"],"Link / mint"),(696,P.C["mint"],"Visited / mint")):
        c.text(x,104,label,20,"ui_text")
        src=cursor_texel_image(tint).resize((304,304),Image.Resampling.NEAREST)
        for y,bg in ((146,P.C["paper"]),(456,(0x33,0,0x66))):
            c.d.rectangle((x,y,x+303,y+303),fill=bg)
            c.im.paste(src,(x,y),src)
    return c.im


def generate(preview):
    bind(preview)
    items=[
        ("16_01_download_known.png","Скачивание: известен объём",dict()),
        ("16_02_download_unknown.png","Скачивание: неизвестен объём",dict(state="unknown",tick=320)),
        ("16_03_download_writing.png","Получено из сети: запись на карту Evo",dict(state="writing",tick=320)),
        ("16_04_download_done.png","Сохранено: путь, автоскрытие через 5 с",dict(state="done")),
        ("16_05_download_no_space.png","Ошибка: нет места на карте",dict(state="no_space")),
        ("16_06_download_sd_error.png","Ошибка записи на SD",dict(state="sd_error")),
        ("16_07_download_network_error.png","Ошибка сети",dict(state="network_error")),
        ("16_08_download_hover_cancel.png","Наведение на отмену сетевого скачивания",dict(hover="action")),
        ("16_09_download_long_name.png","Имя до 48 знаков: сохранено расширение",dict(filename=LONG_NAME)),
        ("16_10_download_coexist.png","Плашка, статус ссылки, JS и прокрутка",dict(coexist=True)),
        ("16_11_download_done_long_hover.png","Длинный путь: чтение имени при наведении",dict(state="done",filename=LONG_NAME,hover="body",tick=1440)),
        ("16_12_download_cancelled.png","Скачивание отменено",dict(state="cancelled")),
    ]
    for name,title,args in items:
        download_frame(**args).save(P.PREV/name)
        P.SCENES.append({"file":name,"title":title,"size":[1024,768]})
    for n,(dark,kind) in enumerate([(False,"link"),(False,"visited"),(False,"text"),(True,"link"),(True,"visited"),(True,"text")],20):
        name=f"16_{n}_cursor_{'dark' if dark else 'paper'}_{kind}.png"
        cursor_page(dark,kind).save(P.PREV/name)
        P.SCENES.append({"file":name,"title":f"Курсор: {'#330066' if dark else 'светлая бумага'}, {kind}","size":[1024,768]})
    cursor_detail().save(P.PREV/"16_26_cursor_detail_8x.png")
    P.SCENES.append({"file":"16_26_cursor_detail_8x.png","title":"Курсор 8x: исходный ARGB4 и умножение COLOR_RGB","size":[1024,768]})
    return [download_frame(state="unknown",tick=t) for t in range(0,1400,40)]
