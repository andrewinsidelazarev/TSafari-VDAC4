"""main/js_prelude.js → main/js_prelude.h (строка C: пролог JavaScript, js.c выполняет его первым).

Комментарии и отступы в начале строк выбрасываются (меньше места в ELF и меньше разбирать на ESP),
переводы строк остаются — номера строк в ошибках совпадают с js_prelude.js.
Запуск: python tools/gen_js_prelude.py (сборка: tools/build.ps1, стенд: tests/run_host.sh)."""
import pathlib
import re

root = pathlib.Path(__file__).resolve().parents[1]
src = (root / 'main' / 'js_prelude.js').read_text(encoding='utf-8')


def strip(text: str) -> str:
    """Убрать /* … */ и // … вне строк и регулярных выражений (простой разбор: кавычки и `…`)."""
    out, i, n = [], 0, len(text)
    quote = None
    prev_sig = ''                                      # последний значимый знак — различить / деления и /re/
    while i < n:
        c = text[i]
        if quote:
            out.append(c)
            if c == '\\' and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue
        if c in '"\'`':
            quote = c
            out.append(c)
            i += 1
            prev_sig = c
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j))   # переводы строк — на месте
            i = j
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        if c == '/' and prev_sig and prev_sig in '(,=:[!&|?{};+-*%<>~^' :   # начало /регулярного выражения/
            j = i + 1
            cls = False
            while j < n:
                d = text[j]
                if d == '\\':
                    j += 2
                    continue
                if d == '[':
                    cls = True
                elif d == ']':
                    cls = False
                elif d == '/' and not cls:
                    break
                j += 1
            out.append(text[i:j + 1])
            i = j + 1
            prev_sig = '/'
            continue
        out.append(c)
        if not c.isspace():
            prev_sig = c
        i += 1
    return ''.join(out)


body = strip(src)
lines = [ln.strip() for ln in body.split('\n')]
c_lines = []
for ln in lines:
    esc = ln.replace('\\', '\\\\').replace('"', '\\"')
    esc = re.sub(r'[^\x20-\x7e]', lambda m: ''.join('\\%03o' % b for b in m.group(0).encode('utf-8')), esc)
    c_lines.append('"%s\\n"' % esc)
header = ('/* Создано tools/gen_js_prelude.py из main/js_prelude.js — не править руками. */\n'
          '#pragma once\n'
          'static const char js_prelude[] =\n' + '\n'.join(c_lines) + ';\n')
out = root / 'main' / 'js_prelude.h'
old = out.read_bytes() if out.exists() else b''
new = header.encode('utf-8')
if old != new:
    out.write_bytes(new)
print('js_prelude.h: %d lines, %d bytes of JS' % (len(lines), len('\n'.join(lines))))
