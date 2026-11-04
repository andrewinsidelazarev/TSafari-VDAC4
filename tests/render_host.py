#!/usr/bin/env python3
"""Снимки стенда на ПК (tests/host.c) → кадры ядра FT812 Slot1 (стенд RTL fpga/vdac2/tb/vsys).

Каждый каталог снимка: ramg.bin (RAM_G 2 МБ) и dl.bin (лист). Для vsys — трасса VTR1 из двух шагов
с этим листом; кадр из SDRAM (--dump) → frame.png. Все снимки считаются параллельно.

    python tests/render_host.py E:\\zx\\cache\\tmp\\tsafari\\host\\snap
"""
from __future__ import annotations

import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image

VSYS = Path(r"E:\zx\Tang Nano 20k VDAC\fpga\vdac2\sim\vsys\c145\vsys.exe")


def to_png(hex_path: Path, png: Path) -> None:
    words = [int(v, 16) for v in hex_path.read_text().split()]
    h = len(words) // 512
    img = Image.new("RGB", (1024, h))
    px = img.load()
    for y in range(h):
        for k in range(512):
            w = words[y * 512 + k]
            for j, p in enumerate((w & 0xFFFF, w >> 16)):
                r5, g6, b5 = p >> 11, (p >> 5) & 63, p & 31
                px[2 * k + j, y] = ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))
    img.save(png)


def main() -> int:
    root = Path(sys.argv[1])
    snaps = sorted(d for d in root.iterdir() if (d / "dl.bin").exists())
    procs = []
    for d in snaps:
        dl = (d / "dl.bin").read_bytes()
        words = len(dl) // 4
        with open(d / "trace.bin", "wb") as f:
            f.write(b"VTR1" + struct.pack("<I", 2))
            for k in range(2):
                f.write(struct.pack("<5I", k, 0, 1400, 0, words) + dl)
        log = open(d / "vsys.txt", "w")
        procs.append((d, log, subprocess.Popen([str(VSYS), "--trace", str(d / "trace.bin"), "--ramg", str(d / "ramg.bin"),
                                                "--no-skip", "--loop", "--seconds", "0.08", "--dump", str(d / "frame.hex")],
                                               stdout=log, stderr=subprocess.STDOUT)))
    bad = 0
    for d, log, p in procs:
        p.wait()
        log.close()
        if p.returncode or not (d / "frame.hex").exists():
            print(f"{d.name}: vsys failed ({p.returncode})")
            bad += 1
            continue
        to_png(d / "frame.hex", d / "frame.png")
        rt = [l for l in (d / "vsys.txt").read_text(encoding="utf-8", errors="replace").splitlines() if l.startswith("ИТОГ")]
        print(f"{d.name}: {rt[0] if rt else ''}")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
