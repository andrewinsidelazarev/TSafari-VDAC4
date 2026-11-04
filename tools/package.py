"""Сборка файла TSAFARI.SPG — программы для Z80 в формате SPG (его запускает Wild Commander).

SPG — это заголовок и «блоки»: каждый блок говорит, в какую страницу памяти Z80 положить кусок
данных. Здесь блоки такие: программа Z80 (launcher/tsafari.asm), драйвер FAT32 и образ окна
запуска (картинки для FT812, страницы с LS_PAGE0 = #40, launcher/ftscreen.asm).
TSAFARI.ELF и TSAFARI.FNT в SPG не входят — лежат рядом файлами в dist/TSAFARI.
Образец — tools/package.py DOOM VDAC4."""
from pathlib import Path
import hashlib
import json
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    build = ROOT / "build"
    out = ROOT / "dist" / "TSAFARI"
    out.mkdir(parents=True, exist_ok=True)
    ini = build / "tsafari_spg.ini"
    # страница 2 — программа (окно #8000), #0E — код драйвера FAT32 (#4000), #0F — его рабочая
    # страница с SD-ZC (#0000)
    blocks = [(2, build / "tsafari_z80.bin"), (14, ROOT / "third_party/fat32/build/fat32.bin"),
              (15, ROOT / "third_party/fat32/build/fat32-work.bin")]
    # окно запуска (tools/gen_launcher_screen.py): образ RAM_G по 16 КБ — страницы с #40
    # (LS_PAGE0 в launcher/ftscreen.asm)
    image = (build / "launcher_screen.bin").read_bytes()
    for i in range(0, len(image), 16384):
        part = build / f"launcher_screen_p{i // 16384}.bin"
        part.write_bytes(image[i:i + 16384])
        blocks.append((0x40 + i // 16384, part))
    ini.write_text("Desc = TSafari VDAC4\nStart = #8000\nStack = #bff0\nResident = #5b00\nPage3 = 0\n"
                   "Clock = 2\nINT = 0\nPager = 0\n" +
                   "".join(f"Block = #c000, {page}, {path.relative_to(ROOT).as_posix()}\n" for page, path in blocks),
                   encoding="ascii")
    tool = os.environ.get("SPGBLD", r"E:\zx\z80\zuma\spgbld.exe")
    subprocess.run([tool, "-b", str(ini), str(out / "TSAFARI.SPG"), "-c", "0"], cwd=ROOT, check=True)
    manifest = {p.name: {"bytes": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                for p in sorted(out.glob("*")) if p.suffix in (".SPG", ".ELF", ".FNT")}
    (ROOT / "dist" / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    for name, info in manifest.items():
        print(f"{name}: {info['bytes']} bytes, {info['sha256'][:16]}")


if __name__ == "__main__":
    main()
