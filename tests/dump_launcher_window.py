"""Окно запуска, как его отправила программа Z80 на модели (tests/test_launcher.py), — в снимки для
стенда ядра Slot1 (tests/render_host.py): RAM_G 2 МБ и листы поиска и запуска."""
from pathlib import Path
import struct, sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_launcher as t  # noqa: E402

out = Path(sys.argv[1] if len(sys.argv) > 1 else r"E:\zx\cache\tmp\tsafari\host\launcher")
t.setUpModule()
esp = t.Esp(exit_after=3)
m = t.Machine(t.disk_image(), esp)
m.run()
ramg = bytearray(0x200000)
for a, v in m.ft.mem.items():
    if a < len(ramg):
        ramg[a] = v
picked = {"01_search": next(dl for st, dl in m.ft.dls if st == 5),
          "02_start": [dl for st, dl in m.ft.dls if st >= 6][-1]}
for name, dl in picked.items():
    d = out / name
    d.mkdir(parents=True, exist_ok=True)
    (d / "ramg.bin").write_bytes(bytes(ramg))
    (d / "dl.bin").write_bytes(struct.pack(f"<{len(dl)}I", *dl))
    print(name, len(dl), "words")
