#!/usr/bin/env python3
"""Тестовые страницы стенда на ПК (tests/host.c раздаёт их как сайт https://test.local/):
UTF-8 со всем, что умеет разбор HTML этапа 1; Windows-1251 с <meta charset>; KOI8-R, у которой
кодировка — только в заголовке ответа (файл .koi8 → Content-Type с charset=koi8-r)."""
from pathlib import Path

OUT = Path(__file__).resolve().parent / "pages"

INDEX = """<!DOCTYPE html>
<html lang="ru"><head><meta charset="utf-8"><title>Тест TSafari &mdash; разбор HTML</title>
<style>body { color: red }</style><script>document.write("JS не выполняется");</script></head>
<body>
<h1>Заголовок первого уровня</h1>
<p>Обычный абзац: <b>жирный</b>, <i>курсив</i>, сущности &laquo;ёлочки&raquo;, &copy; 2026, 5&nbsp;кг,
&#8470;&nbsp;1, &#x2192; стрелка. Длинная строка для переноса по словам: ZX Evolution — компьютер,
совместимый с ZX Spectrum 128, на ПЛИС; TS-Conf добавляет графику, спрайты и DMA.</p>
<p>Ссылки: <a href="cp1251.html">страница в Windows-1251</a>, <a href="/koi.koi8">в KOI8-R</a>,
<a href="sub/../missing.html">несуществующая</a>, <a href="mailto:x@y.z">почта</a> (не ссылка),
<a href="#top">якорь</a>.</p>
<h2>Списки</h2>
<ul><li>первый пункт</li><li>второй пункт с <a href="https://test.local/index.html">ссылкой</a></li>
<li>вложенный:<ol><li>раз</li><li>два</li><li value="10">десять</li></ol></li></ul>
<blockquote>Цитата с отступом: любопытство не измеряется мегагерцами.</blockquote>
<pre>
10 PRINT "HELLO"
20 GOTO 10

  отступ в pre сохраняется
</pre>
<table><tr><th>Машина</th><th>Процессор</th></tr><tr><td>ZX Spectrum</td><td>Z80</td></tr>
<tr><td>Amiga</td><td>68000</td></tr></table>
<hr>
<form>Поиск: <input type="text" placeholder="запрос"> <input type="submit" value="Найти">
<input type="checkbox" checked> да <input type="radio"> нет <input type="hidden" value="x">
<select><option>один</option><option>два</option></select></form>
<p>Картинка: <img src="pic.png" alt="ZX Spectrum 48K"> и без подписи <img src="x.gif">.</p>
<noscript>Этот текст виден: JS в TSafari не выполняется.</noscript>
<!-- комментарий не виден -->
<p>Конец страницы &amp; всё.</p>
</body></html>
"""

CP1251 = """<html><head><meta http-equiv="Content-Type" content="text/html; charset=windows-1251">
<title>Кодировка Windows-1251</title></head><body>
<h1>Windows-1251</h1>
<p>Съешь же ещё этих мягких французских булок, да выпей чаю. Ёжик, «кавычки», №5, — тире.</p>
<p><a href="index.html">Назад к тесту</a></p>
</body></html>
"""

# Цвета, размеры, выравнивание, CSS, таблицы с рамкой, colspan и таблица в таблице.
STYLES = """<html><head><title>Стили</title></head>
<body bgcolor="#102030" text="#e0e0e0" link="#80d0ff" vlink="#c0a0ff">
<center><h1>Заголовок по центру</h1></center>
<p align=right>Абзац вправо.</p>
<p>Текст <font color="#ff8080">красный</font>, <font size=5>крупный</font>, <font size=1>мелкий</font>,
<u>подчёркнутый</u>, <s>зачёркнутый</s>, <span style="color: yellow; font-weight: bold">жёлтый жирный</span>,
<span style="background: #405060">с фоном</span>, <mark>метка</mark>.</p>
<h2>Таблица с рамкой</h2>
<table border=1 width=100%>
<tr><th>Модель</th><th>Процессор</th><th>Память</th></tr>
<tr><td>ZX Spectrum 48K</td><td>Z80 3,5 МГц</td><td bgcolor="#204060">48 КБ</td></tr>
<tr><td colspan=2 align=center>ZX Evolution — две ячейки в одной</td><td>4 МБ</td></tr>
</table>
<h3>Таблица в таблице</h3>
<table cellpadding=4><tr><td bgcolor="#304050" width=200>Левая колонка с меню: <a href="index.html">тест</a></td>
<td><table border=1><tr><td>A</td><td>B</td></tr><tr><td>C</td><td>D</td></tr></table>
Длинный текст правой колонки переносится по словам внутри своей ячейки и не залезает в левую колонку.</td></tr></table>
<div style="display:none">Этого не видно <div>и этого</div> тоже</div>
<p style="text-align:center; font-size: 24px">CSS: по центру, 24 px</p>
</body></html>
"""

# Формы: поиск (GET), анкета (POST по HTTP), POST по HTTPS (прошивка не умеет — ошибка),
# форма между строками таблицы (как на старых сайтах).
FORMS = """<html><head><title>Формы</title></head><body>
<h2>Поиск</h2>
<form action="echo" method="get">
Запрос: <input name="q" value="ZX" size=30> <input type="submit" value="Найти">
<input type="hidden" name="lang" value="ru">
</form>
<h3>Анкета</h3>
<form action="http://test.local/echo" method="post">
<p>Имя: <input name="name" placeholder="Ваше имя"> Пароль: <input type=password name=pw value="secret"></p>
<p><input type=checkbox name=keep checked> Запомнить <input type=checkbox name=news value=yes> Новости
<input type=radio name=mode value=text checked> Текст <input type=radio name=mode value=img> Текст и картинки</p>
<p>Машина: <select name=machine><option>ZX Spectrum 48K<option selected value="Evo">ZX Evolution<option>Pentagon 128</select>
<textarea name=note rows=3 cols=40>
Первая строка
Вторая строка</textarea></p>
<p><button type=submit name=go value=1>Отправить</button> <input type=reset value="Сбросить"></p>
</form>
<form action="https://test.local/echo" method="post"><input name=x value=1> <input type=submit value="POST по HTTPS"></form>
<table border=1><form action="echo"><tr><td>В таблице: <input name=t value="cell"></td><td><input type=submit value="OK"></td></tr></form></table>
</body></html>
"""

# Картинки: JPEG baseline (с размером в HTML и без — тогда страница раскладывается заново, и
# уменьшенная), PNG 8 бит (палитра с прозрачностью, RGBA, серый), GIF с прозрачностью в ссылке;
# недоступные: прогрессивный JPEG, PNG 4 бита, большая (не влезает в RAM_G), нет файла.
IMAGES = """<html><head><title>Картинки</title></head><body>
<h2>Картинки</h2>
<p>JPEG с размером: <img src="photo.jpg" width=240 height=150 alt="фото"> и вдвое меньше:
<img src="photo.jpg" width=120 height=75></p>
<p>JPEG без размера (страница раскладывается заново): <img src="wide.jpg" alt="широкое фото"></p>
<p>PNG палитра с прозрачностью <img src="pal.png">, RGBA <img src="rgba.png">, серый <img src="gray.png">.</p>
<p>GIF-ссылка: <a href="index.html"><img src="logo.gif" alt="логотип"></a> — щелчок ведёт на главную.</p>
<p>WebP: <img src="photo.webp" alt="фото WebP"> и с прозрачностью <img src="alpha.webp"></p>
<p>Недоступные: <img src="prog.jpg" width=200 height=120 alt="прогрессивный JPEG">
<img src="pal4.png" width=64 height=64> <img src="missing.png" width=150 height=100 alt="нет файла"></p>
<p><img src="big.jpg" width=600 height=400 alt="большое фото"></p>
<p>Конец страницы.</p>
</body></html>
"""

# Фреймы — как на vtrd.in: меню сверху (Top) и содержимое (Down), ссылки меню — в Down.
FRAMES = """<html><head><title>Фреймы</title></head>
<script>window.dataLayer = [];</script>
<frameset rows="20,1*" frameborder="NO">
  <frame src="menu.html" name="Top" scrolling="NO" noresize>
  <frame src="news.html" name="Down">
</frameset>
<noframes><body><p>Нужен браузер с фреймами.</p></body></noframes>
</html>
"""

MENU = """<html><head><title>Меню</title></head><body>
<b><a href="news.html" target="Down">Новости</a></b> [<a href="games.html" target="Down">Игры</a>]
[<a href="koi.koi8" target="Down">KOI8</a>] [<a href="index.html" target="_top">Весь тест</a>]
[<a href="ALTEREGO.ZIP" target="Down">ZIP</a>]
</body></html>
"""

NEWS = """<html><head><title>Новости</title></head><body>
<h2>Новости</h2>
<p>Страница фрейма Down. Ссылка без target открывается в этом же фрейме: <a href="games.html">к играм</a>.</p>
</body></html>
"""

GAMES = """<html><head><base target="Down"><title>Игры</title></head><body>
<h2>Игры</h2>
<ul><li>Elite</li><li>Exolon</li><li>R-Type</li></ul>
<p><a href="news.html">Назад к новостям</a> (target из &lt;base&gt;).</p>
</body></html>
"""

KOI8 = """<html><head><title>KOI8-R</title></head><body>
<h1>KOI8-R</h1><p>Широкая электрификация южных губерний даст мощный толчок подъёму сельского хозяйства.</p>
</body></html>
"""


def make_images() -> None:
    """Картинки страницы images.html (Pillow)."""
    from PIL import Image, ImageDraw

    def photo(w, h):
        im = Image.new("RGB", (w, h))
        px = im.load()
        for y in range(h):
            for x in range(w):
                px[x, y] = (x * 255 // w, y * 255 // h, (x + y) * 255 // (w + h))
        d = ImageDraw.Draw(im)
        d.ellipse((w // 4, h // 4, w * 3 // 4, h * 3 // 4), outline=(255, 255, 255), width=3)
        d.text((8, 8), "ZX", fill=(255, 255, 255))
        return im

    photo(240, 150).save(OUT / "photo.jpg", quality=88)
    photo(320, 200).save(OUT / "wide.jpg", quality=88)
    photo(200, 120).save(OUT / "prog.jpg", quality=88, progressive=True)
    photo(1500, 1000).save(OUT / "big.jpg", quality=70)
    pal = Image.new("P", (64, 64))
    pal.putpalette([v for i in range(64) for v in (i * 4, 255 - i * 4, 128)])
    d = ImageDraw.Draw(pal)
    for i in range(64):
        d.line((i, 0, 63 - i, 63), fill=i)
    d.ellipse((16, 16, 47, 47), fill=0)
    pal.save(OUT / "pal.png", transparency=0)            # 64 цвета — 8 бит, прозрачный цвет 0
    pal4 = pal.quantize(16)
    pal4.save(OUT / "pal4.png")                            # 16 цветов — PNG по 4 бита
    rgba = Image.new("RGBA", (96, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(rgba)
    d.rounded_rectangle((4, 4, 91, 59), 12, fill=(30, 140, 200, 180), outline=(255, 255, 255, 255), width=2)
    rgba.save(OUT / "rgba.png")
    (OUT / "new").mkdir(exist_ok=True)                    # страница переадресации old/ → new/
    rgba.save(OUT / "new" / "logo.png")
    (OUT / "new" / "page.html").write_text(
        "<html><head><title>Moved</title></head><body><h2>Страница после переадресации</h2>"
        "<p>Картинка по относительному адресу: <img src=\"logo.png\" alt=\"logo\"></p></body></html>\n",
        encoding="utf-8", newline="\n")
    gray = Image.linear_gradient("L").resize((64, 64))
    gray.save(OUT / "gray.png")
    gif = Image.new("P", (120, 40), 0)
    gif.putpalette([255, 0, 255] + [v for i in range(1, 64) for v in (0, i * 4, 255 - i * 4)])
    d = ImageDraw.Draw(gif)
    d.rectangle((2, 2, 117, 37), fill=20, outline=60)
    d.text((10, 12), "TSafari", fill=63)
    gif.save(OUT / "logo.gif", transparency=0)
    photo(300, 180).save(OUT / "photo.webp", quality=80)  # WebP с потерями, без прозрачности
    rgba.save(OUT / "alpha.webp", lossless=True)           # WebP без потерь, с прозрачностью


def main() -> int:
    OUT.mkdir(exist_ok=True)
    (OUT / "index.html").write_bytes(INDEX.encode("utf-8"))
    (OUT / "cp1251.html").write_bytes(CP1251.encode("cp1251"))
    (OUT / "koi.koi8").write_bytes(KOI8.encode("koi8_r"))
    make_images()
    for name, text in (("styles.html", STYLES), ("forms.html", FORMS), ("images.html", IMAGES), ("frames.html", FRAMES), ("menu.html", MENU), ("news.html", NEWS), ("games.html", GAMES)):
        (OUT / name).write_bytes(text.encode("utf-8"))
    # «архив» 50 000 байт: файл, а не страница — браузер сохраняет его на карту Evo
    (OUT / "ALTEREGO.ZIP").write_bytes(bytes((i * 131 + i // 256 * 7) & 255 for i in range(50000)))
    # файл без длины в ответе (стенд не шлёт Content-Length для «.nolen»): загрузка потоком до «конца файла»
    (OUT / "big.nolen").write_bytes(bytes((i * 37 + i // 1000) & 255 for i in range(120000)))
    print("pages:", ", ".join(p.name for p in OUT.iterdir()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
