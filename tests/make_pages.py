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


def main() -> int:
    OUT.mkdir(exist_ok=True)
    (OUT / "index.html").write_bytes(INDEX.encode("utf-8"))
    (OUT / "cp1251.html").write_bytes(CP1251.encode("cp1251"))
    (OUT / "koi.koi8").write_bytes(KOI8.encode("koi8_r"))
    for name, text in (("frames.html", FRAMES), ("menu.html", MENU), ("news.html", NEWS), ("games.html", GAMES)):
        (OUT / name).write_bytes(text.encode("utf-8"))
    # «архив» 50 000 байт: файл, а не страница — браузер сохраняет его на карту Evo
    (OUT / "ALTEREGO.ZIP").write_bytes(bytes((i * 131 + i // 256 * 7) & 255 for i in range(50000)))
    print("pages:", ", ".join(p.name for p in OUT.iterdir()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
