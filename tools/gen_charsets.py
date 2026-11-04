#!/usr/bin/env python3
"""Таблицы однобайтовых кодировок → main/charset_data.c: верхняя половина (0x80…0xFF) → Unicode.
Берутся из кодеков Python; неопределённый байт — U+FFFD."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SETS = [("cp1251", "cp1251"), ("koi8r", "koi8_r"), ("cp866", "cp866"), ("cp1252", "cp1252")]


def main() -> int:
    lines = ["/* Создано tools/gen_charsets.py — не править вручную. */", '#include "charset.h"', ""]
    for name, codec in SETS:
        values = [ord(bytes([b]).decode(codec, errors="replace")) for b in range(0x80, 0x100)]
        lines.append(f"const uint16_t charset_{name}[128] = {{")
        for i in range(0, 128, 12):
            lines.append("    " + ", ".join(f"0x{v:04X}" for v in values[i:i + 12]) + ",")
        lines.append("};")
    (ROOT / "main" / "charset_data.c").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print("charsets:", ", ".join(n for n, _ in SETS))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
