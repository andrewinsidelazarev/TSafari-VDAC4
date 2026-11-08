"""Программа Z80 TSafari (launcher/tsafari.asm) на модели: настоящий Z80-код и драйвер FAT32,
подделаны только сектора карты и ответы ESP (как tests/test_launcher.py DOOM VDAC4).

Кадры ESP → Z80 — не больше 256 байт payload (очередь ZiFi). Запуск:
    E:\\zx\\esp\\pio-venv\\Scripts\\python.exe -m unittest tests.test_launcher
"""
from pathlib import Path
import os, re, struct, subprocess, sys, unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "third_party/fat32/tests"))
from harness import Disk, symbols  # noqa: E402
BOARD = Path(os.environ.get("VDAC4_ROOT", r"E:\zx\Tang Nano 20k VDAC\VDAC4"))
sys.path.insert(0, str(BOARD / "ESP32 S3 Zero N8R8/Network/tests"))
from test_spi_link_z80 import SpiLinkMixin, frame as wire_frame  # noqa: E402
from spi_link_model import LinkCore  # noqa: E402
import z80  # noqa: E402

ELF = bytes((i * 79 + 11) & 255 for i in range(5321))
BREAK = getattr(z80.Z80Machine, "_BREAKPOINT_HIT", 2)    # 1 у z80 1.2.0, 2 у 1.0b3
INIT, READ, WRITE, SYNC = 0xC100, 0xC110, 0xC120, 0xC130


def frame(c, p):
    assert len(p) <= 256, (hex(c), len(p))
    return wire_frame(c, p)


def setUpModule():
    r = subprocess.run([os.environ.get("SJASMPLUS", r"E:\zx\z80\zuma\sjasmplus.exe"), "--nologo", "--inc=launcher",
                        "--inc=build", "--inc=third_party/fat32/include", "--sym=build/tsafari_z80.sym",
                        "--lst=build/tsafari_z80.lst", "launcher/tsafari.asm"], cwd=ROOT, capture_output=True)
    assert r.returncode == 0, r.stdout.decode(errors="replace") + r.stderr.decode(errors="replace")


def entry(name, cluster, size=0, directory=False):
    a, _, b = name.partition(".")
    e = bytearray(32)
    e[:11] = (a.ljust(8) + b.ljust(3)).encode("ascii")
    e[11] = 0x10 if directory else 0x20
    struct.pack_into("<H", e, 20, cluster >> 16)
    struct.pack_into("<HHHI", e, 22, 0x55AA, 0x5B05, cluster & 65535, size)
    return e


def disk_image(elf=True, others=()):
    """Карта: /PROGRAMS/WEB/TSAFARI.ELF (и чужие файлы рядом)."""
    disk = Disk(partition=2048)
    for c in (3, 7):
        disk.set_fat(c, 0x0FFFFFFF)
    disk.write(disk.data_start, entry("PROGRAMS", 3, directory=True) + bytes(480))
    disk.write(disk.data_start + disk.spc, entry("WEB", 7, directory=True) + bytes(480))
    size = 512 * disk.spc
    clusters = [100 + 3 * i for i in range((len(ELF) + size - 1) // size)]
    for i, c in enumerate(clusters):
        disk.set_fat(c, clusters[i + 1] if i + 1 < len(clusters) else 0x0FFFFFFF)
        for s in range(disk.spc):
            at = i * size + s * 512
            disk.write(disk.data_start + (c - 2) * disk.spc + s, ELF[at:at + 512].ljust(512, b"\0"))
    records = b"".join(entry(n, 0, 0) for n in others)
    if elf:
        records += entry("TSAFARI.ELF", clusters[0], len(ELF))
    disk.write(disk.data_start + (7 - 2) * disk.spc, records.ljust(512, b"\0"))
    return disk


class Esp:
    """Ответы ESP: кэш модулей, приложение TSafari (состояние 2 — работает, 3 — выход по F12)."""
    def __init__(self, cached=False, exit_after=5, start_answer=0):
        self.cached = cached; self.data = bytearray(); self.committed = False; self.started = False
        self.stopped = 0; self.done = False; self.packets = []; self.status = 0; self.exit_after = exit_after
        self.start_answer = start_answer; self.query = None; self.msel_at_start = None; self.machine = None

    def __call__(self, c, p):
        if c == 0x6D:                                   # APP_STOP
            self.stopped += 1
            return frame(0xED, b"\1")
        if c in (0x2A, 0x2B):                           # path.cfg: прошивка без него
            return frame(0xEE, b"unsupported\0")
        if c == 0x69:                                   # MOD_QUERY
            self.query = (p[9:].rstrip(b"\0"), *struct.unpack("<II", p[1:9]))
            return frame(0xE9, bytes([0 if self.cached else 1]) + struct.pack("<I", 2000000))
        if c == 0x6A:                                   # MOD_PUT
            assert struct.unpack("<I", p[:4])[0] == len(self.data)
            self.data += p[4:]
            return frame(0xEA, b"\1" + struct.pack("<I", len(self.data)))
        if c == 0x6B:
            self.committed = True
            return frame(0xEB, b"\1")
        if c == 0x6C:                                   # APP_START
            assert p == b"TSAFARI.ELF\0", p
            self.msel_at_start = list(self.machine.vconfig)
            self.started = self.start_answer == 0
            return frame(0xEC, bytes([self.start_answer]))
        if c == 0x75:                                   # мышь и клавиатура
            assert 4 <= len(p) <= 4 + 24, p.hex()
            self.packets.append(bytes(p))
            return frame(0xF5, b"\1")
        if c == 0x73:                                   # состояние браузера
            self.status += 1
            state = 3 if self.status >= self.exit_after else 2
            return frame(0xF3, bytes([state, 1]) + bytes(4))
        raise AssertionError(hex(c))


class DownloadEsp(Esp):
    """Браузер скачал файл: со второго вопроса о состоянии — событие E7 [номер, размер, имя];
    куски — ответами F6 на 76 (до 246 байт); итог — 77. NTP (#22) отвечает строкой ntp.
    drop — номера запросов 76, ответ на которые «теряется»; refuse_at — с этого запроса отказ
    (как при отмене); later — номера запросов, на которые ответ «куска ещё нет» (файл идёт из
    сети потоком); unknown — длины нет (E7 с размером #FFFFFFFF), конец — ответом [3].
    После итога браузер выходит (состояние 3)."""
    def __init__(self, payload, name=b"ALTEREGO.ZIP", ntp=b"20261104123456", drop=(), refuse_at=None,
                 later=(), unknown=False):
        super().__init__(exit_after=10 ** 9)
        self.payload = payload; self.name = name; self.ntp = ntp; self.drop = set(drop)
        self.refuse_at = refuse_at; self.sent_event = False; self.result = None; self.pulls = 0
        self.result_at = 0; self.ntp_asked = 0; self.acks = 0
        self.later = set(later); self.unknown = unknown; self.later_sent = 0; self.eof_sent = 0

    def __call__(self, c, p):
        if c == 0x73:
            self.status += 1
            state = 3 if self.result is not None and self.status > self.result_at + 1 else 2
            out = frame(0xF3, bytes([state, 1]) + bytes(4))
            if not self.sent_event and self.status >= 2:
                self.sent_event = True
                size = 0xFFFFFFFF if self.unknown else len(self.payload)
                out += frame(0xE7, struct.pack("<HI", 7, size) + self.name + b"\0")
            return out
        if c == 0x22:                                   # NTP: ГГГГММДДЧЧММСС
            self.ntp_asked += 1
            return frame(0xA2, self.ntp)
        if c == 0x76:
            seq, at = struct.unpack("<HI", p)
            self.pulls += 1
            if self.pulls in self.drop:
                return b""                              # кадр не дошёл
            if seq != 7 or (self.refuse_at and self.pulls >= self.refuse_at):
                return frame(0xF6, struct.pack("<HBIH", seq, 0, 0, 0))
            if self.pulls in self.later:                    # сеть не успела: «спроси позже»
                self.later_sent += 1
                return frame(0xF6, struct.pack("<HBIH", seq, 2, at, 0))
            if self.unknown and at >= len(self.payload):    # файл без длины кончился
                self.eof_sent += 1
                return frame(0xF6, struct.pack("<HBIH", seq, 3, at, 0))
            count = min(246, len(self.payload) - at)
            return frame(0xF6, struct.pack("<HBIH", seq, 1, at, count) + self.payload[at:at + count])
        if c == 0x77 and p[2] == 3:                    # «принято, начинаю» — до первого куска
            assert self.pulls == 0 and self.result is None
            self.acks += 1
            return frame(0xF7, b"\1")
        if c == 0x77:
            self.result = (struct.unpack("<H", p[:2])[0], p[2], p[3], bytes(p[4:14]))
            self.result_at = self.status
            return frame(0xF7, b"\1")
        return super().__call__(c, p)


class CacheEsp(Esp):
    """Кэш картинок (launcher/imgcache.asm): события E8 [операция, номер, размер, имя] по одному —
    следующее после доклада 77 о прошлом; кадры списка 78 и куски файла 79 собираются; куски
    записи — ответами F6 на 76. stop_read_at — с этого числа принятых байт чтения (у записи — с
    этого смещения куска) ответ «хватит». Когда операции кончились, браузер выходит (состояние 3)."""
    def __init__(self, ops, stop_read_at=None):
        super().__init__(exit_after=10 ** 9)
        self.ops = list(ops); self.seq = 0x8000; self.cur = None; self.results = []
        self.entries = []; self.data = bytearray(); self.acks = 0; self.done_at = None
        self.stop_read_at = stop_read_at; self.frames = 0; self.big_frames = 0

    def __call__(self, c, p):
        p = bytes(p)
        if c == 0x73:
            self.status += 1
            state = 3 if self.done_at is not None and self.status > self.done_at + 1 else 2
            out = frame(0xF3, bytes([state, 1]) + bytes(4))
            if self.cur is None and self.ops:
                op, name, payload, *stop = self.ops.pop(0)
                self.stop_read_at = stop[0] if stop else None
                self.seq += 1
                self.cur = (op, name, payload); self.entries = []; self.data = bytearray()
                out += frame(0xE8, struct.pack("<BHI", op, self.seq, len(payload or b"")) + name + b"\0")
            elif self.cur is None and self.done_at is None:
                self.done_at = self.status
            return out
        if c == 0x77:
            assert struct.unpack("<H", p[:2])[0] == self.seq
            if p[2] == 3:
                self.acks += 1
                return frame(0xF7, b"\1")
            op, name, payload = self.cur
            self.results.append((op, name, p[2], p[3], list(self.entries) if op == 1 else bytes(self.data)))
            self.cur = None
            return frame(0xF7, b"\1")
        if c == 0x76:
            seq, at = struct.unpack("<HI", p)
            assert seq == self.seq and self.cur[0] == 3
            if self.stop_read_at is not None and at >= self.stop_read_at:   # «хватит»: чтение важнее
                return frame(0xF6, struct.pack("<HB", seq, 0) + bytes(6))
            payload = self.cur[2]
            count = min(246, len(payload) - at)
            return frame(0xF6, struct.pack("<HBIH", seq, 1, at, count) + payload[at:at + count])
        if c == 0x78:
            assert len(p) <= 1002, len(p)
            seq = struct.unpack("<H", p[:2])[0]
            q = p[2:]
            while q:
                size, date, time, flag = struct.unpack("<IHHB", q[:9])
                end = q.index(0, 9)
                self.entries.append((q[9:end].decode("cp866"), size, date, time, flag))
                q = q[end + 1:]
            return frame(0xF8, struct.pack("<HB", seq, 1))
        if c == 0x79:
            seq, at = struct.unpack("<HI", p[:6])
            assert seq == self.seq and at == len(self.data) and len(p) <= 6 + 8192, (at, len(self.data), len(p))
            self.data += p[6:]
            self.frames += 1
            self.big_frames += len(p) > 6 + 1016          # порция DMA (прошивка 0.1.121+)
            stop = self.stop_read_at is not None and len(self.data) >= self.stop_read_at
            return frame(0xF9, struct.pack("<HB", seq, 0 if stop else 1))
        return super().__call__(c, p)


def cache_files(disk, folder="Images"):
    """Файлы Cache\\Images (или Cache\\CSS) в папке программы (/PROGRAMS/WEB, кластер 7):
    {имя: содержимое}."""
    cache = disk.get("Cache", 7)["cluster"]
    sub = disk.get(folder, cache)["cluster"]
    return {e["name"]: disk.contents(e["name"], sub) for e in disk.entries(sub) if not e["attr"] & 0x10}


def saved(disk, date, name):
    """Файлы каталога \\zifi\\downloads\\<date> на модели карты: {имя: содержимое}."""
    zifi = disk.get("zifi")["cluster"]
    downloads = disk.get("downloads", zifi)["cluster"]
    day = disk.get(date, downloads)["cluster"]
    return {e["name"]: disk.contents(e["name"], day) for e in disk.entries(day) if not e["attr"] & 0x10}


class Ft812:
    """FT812 за SPI Evo (выбор 7 на #77) для окна запуска (launcher/ftscreen.asm): команды хоста
    (3 байта; 4F — след BootTrace), память байтами, чтение — по адресу после пустого байта.
    REG_ID = #7C (answer), корень шрифтов ПЗУ появляется сразу, REG_FRAMES растёт на чтение,
    смена листа — сразу: снимок листа — в dls (с меткой BootStage в момент смены)."""
    def __init__(self, m, answer=True):
        self.m = m; self.answer = answer; self.mem = {}; self.cmds = []; self.dls = []; self.frames = 0
        self.tx = bytearray()
    def read(self, a):
        if a == 0x302000: return 0x7C if self.answer else 0
        if 0x302004 <= a < 0x302008:
            if a == 0x302004: self.frames += 1
            return self.frames >> 8 * (a - 0x302004) & 255
        if 0x2FFFFC <= a < 0x300000: return 0x201EE0 >> 8 * (a - 0x2FFFFC) & 255
        if 0x302054 <= a < 0x302058: return 0
        return self.mem.get(a, 0)
    def begin(self): self.tx = bytearray()
    def exchange(self, v):
        i = len(self.tx); self.tx.append(v)
        if i >= 4 and self.tx[0] & 0xC0 == 0:
            return self.read(((self.tx[0] & 0x3F) << 16 | self.tx[1] << 8 | self.tx[2]) + i - 4)
        return 0
    def end(self):
        t = bytes(self.tx)
        if len(t) == 3:
            if t[0] == 0x4F: self.m.boot_trace.append(t); return
            self.cmds.append((t[0], t[1])); return
        if len(t) < 4 or t[0] & 0xC0 != 0x80: return
        a = (t[0] & 0x3F) << 16 | t[1] << 8 | t[2]
        for k, x in enumerate(t[3:]): self.mem[a + k] = x
        if a == 0x302054 and t[3]:
            dl = []
            while len(dl) < 2048:
                w = int.from_bytes(bytes(self.mem.get(0x300000 + 4 * len(dl) + k, 0) for k in range(4)), "little")
                dl.append(w)
                if w == 0: break
            self.dls.append((self.m.memory[self.m.symbols["BootStage"]], dl))
    def ramg(self, base, n): return bytes(self.mem.get(base + i, 0) for i in range(n))


class Machine(SpiLinkMixin):
    def __init__(self, disk, app, mouse=None, ps2=b"", ft=True):
        self.disk = disk; self.machine = z80.Z80Machine(); self.memory = self.machine.memory
        self.symbols = symbols(ROOT / "build/tsafari_z80.sym")
        for addr, path in [(0x8000, "build/tsafari_z80.bin"), (0x4000, "third_party/fat32/build/fat32.bin"),
                           (0, "third_party/fat32/build/fat32-work.bin")]:
            self.machine.set_memory_block(addr, (ROOT / path).read_bytes())
        self.machine.set_memory_block(0x3900, struct.pack("<4H", INIT, READ, WRITE, SYNC))
        for pc in (INIT, READ, WRITE, SYNC, self.symbols["Halt"]):
            self.machine.set_breakpoint(pc)
        self.machine.set_input_callback(self.port_in)
        self.machine.set_output_callback(self.port_out)
        self.rx = bytearray(); self.tx = bytearray(); self.rx_polls = 0; self.esp = None
        self.vconfig = []; self.border = []; self.writes = 0; self.boot_trace = []
        # очередь «от Z80» — как в прошивке (kInputRingBytes 16 КБ): в 4 КБ модели порция DMA 8 КБ не влезет
        self.attach_link(LinkCore(from_capacity=16384)); self.app = app; app.machine = self
        # TSAFARI.ELF при запуске разрешает порции DMA команде 79 (link_media, прошивка 0.1.121)
        self.link.core.app_media = (0x79, 0x79) if getattr(app, "media", True) else (1, 0)
        self.spi_select = 3; self.ft = Ft812(self, ft)
        self.page3 = 0x20; self.pages = {0x20: bytes(16384)}
        image = (ROOT / "build/launcher_screen.bin").read_bytes()
        for i in range(0, len(image), 16384):
            self.pages[0x40 + i // 16384] = image[i:i + 16384].ljust(16384, bytes(1))
        self.gluk_open = 0; self.gluk_reg = 0; self.ps2_mode = 0; self.ps2_queue = bytearray(ps2)
        self.fe_reads = 0; self.mouse = mouse; self.ft_miso = 0

    def port_out(self, port, value):
        try:
            if port & 255 == 0x77:
                if self.spi_select == 7 and value != 7:
                    self.ft.end()
                self.spi_select = value
                if value == 7:
                    self.ft.begin(); super().port_out(port, 3); return
            if port & 255 == 0x57 and self.spi_select == 7:
                self.ft_miso = self.ft.exchange(value); return
            if port == 0x13AF:
                self.pages[self.page3] = bytes(self.memory[0xC000:0x10000])
                self.page3 = value
                self.machine.set_memory_block(0xC000, self.pages.get(value, bytes(16384)))
                self.ports.write(port, value)               # окно #C000 — и модели DMA
            elif (port & 255) == 0xAF and port >> 8 in (0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x26, 0x28):
                super().port_out(port, value)               # регистры DMA TS-Config — и модели канала
            elif port == 0x27AF and value & 7 == 2:
                super().port_out(port, value)               # запуск DMA с SPI — модели канала (прочие — мимо)
            elif port == 0x00AF: self.vconfig.append(value)
            elif port & 255 == 0xFE: self.border.append(value & 7)
            elif port == 0xEFF7: self.gluk_open = value
            elif port == 0xDFF7: self.gluk_reg = value
            elif port == 0xBFF7:
                assert self.gluk_open == 0x80, "регистры Gluk закрыты"
                if self.gluk_reg == 0x0C and value & 1: pass        # сброс очереди: оставляем заданные байты
                if self.gluk_reg == 0xF0: self.ps2_mode = value
            elif (port & 255) == 0xAF: pass                       # прочие регистры TS-Conf
            else: super().port_out(port, value)
        except Exception as e:
            self.error = e; self.app.done = True

    def port_in(self, port):
        if port & 255 == 0x57 and self.spi_select == 7:
            v = self.ft_miso; self.ft_miso = self.ft.exchange(0xFF); return v
        if port in (0xFADF, 0xFBDF, 0xFFDF):
            return self.mouse[(0xFADF, 0xFBDF, 0xFFDF).index(port)] if self.mouse else 0xFF
        if port & 255 == 0xFE: self.fe_reads += 1
        if port == 0xBFF7:
            if self.gluk_open != 0x80 or self.gluk_reg != 0xF0 or self.ps2_mode != 2 or not self.ps2_queue: return 0
            return self.ps2_queue.pop(0)
        if port == 0x1F: return 0xFF                              # джойстика нет
        try: return super().port_in(port)
        except Exception as e:
            self.error = e; self.app.done = True; return 255

    def run(self):
        m = self.machine; m.pc = self.symbols["Start"]
        for _ in range(200000):
            m.ticks_to_stop = 1000000
            events = m.run()
            if hasattr(self, "error"): raise self.error
            if not events & BREAK: continue
            if m.pc == self.symbols["Halt"]: return
            status = 0
            if m.pc == READ:
                m.set_memory_block(m.bc, self.disk.read(m.de << 16 | m.hl))
            elif m.pc == WRITE:
                self.disk.write(m.de << 16 | m.hl, bytes(self.memory[m.bc:m.bc + 512])); self.writes += 1
            elif m.pc not in (INIT, SYNC):
                raise AssertionError(hex(m.pc))
            m.bc, m.de, m.hl = 0xA55A, 0x1234, 0x5678
            m.a, m.f = status, 64
            m.pc = int.from_bytes(self.memory[m.sp:m.sp + 2], "little"); m.sp += 2
        raise AssertionError(("timeout", hex(m.pc)))

    def screen_text(self):
        """Строки экрана диагностики как есть (байты знакомест не разбираем — только факт вывода)."""
        return bytes(self.memory[0xC000:0xD800])


class LauncherTest(unittest.TestCase):
    def test_upload_start_input_and_exit_by_f12(self):
        disk = disk_image(others=("README.TXT",))
        esp = Esp(exit_after=4)
        m = Machine(disk, esp, mouse=(0xFE, 0x12, 0x34), ps2=bytes([0xE0, 0x1F, 0xE0, 0xF0, 0x1F]))
        m.run()
        self.assertEqual(bytes(esp.data), ELF)
        self.assertTrue(esp.committed and esp.started)
        self.assertEqual(esp.query, (b"TSAFARI.ELF", len(ELF), 0x5B0555AA))
        self.assertEqual(set(esp.msel_at_start), {4}, "M_SEL = 1 до APP_START (окно и запуск)")
        self.assertEqual(m.vconfig[-1], 0, "на выходе экран снова у TS-Config")
        self.assertGreaterEqual(esp.stopped, 2, "APP_STOP на старте и на выходе")
        self.assertEqual(m.writes, 0, "на карту ничего не пишется")
        self.assertEqual(m.fe_reads, 0, "матрица ZX не читается")
        # первый пакет: мышь как есть и Win из очереди PS/2
        self.assertEqual(esp.packets[0][:4], bytes([0, 0xFE, 0x12, 0x34]))
        self.assertEqual(b"".join(p[4:] for p in esp.packets), bytes([0xE0, 0x1F, 0xE0, 0xF0, 0x1F]))
        self.assertEqual(m.border[-1], 0, "штатный выход — без красной рамки ошибки")

    def test_launch_window_first_start(self):
        """Первый запуск (path.cfg пуст): образ окна в RAM_G байт в байт, лист поиска во время поиска,
        потом лист запуска; глобус вращается (меняется кадр); M_SEL = 1 с начала."""
        esp = Esp(exit_after=3)
        m = Machine(disk_image(), esp)
        m.run()
        image = (ROOT / "build/launcher_screen.bin").read_bytes()
        self.assertEqual(m.ft.ramg(0x0E1000, len(image)), image)
        inc = (ROOT / "build/launcher_screen.inc").read_text(encoding="utf-8")
        globe_word = int(re.search(r"LS_GLOBE_WORD\s+EQU (\d+)", inc)[1]) // 4
        stages = [st for st, dl in m.ft.dls]
        self.assertIn(5, stages, "лист во время поиска (этап 5)")
        search = [dl for st, dl in m.ft.dls if st == 5]
        state_words = {dl[-3] for st, dl in m.ft.dls if len(dl) > 20}
        self.assertGreaterEqual(len(state_words), 1)
        frames = {dl[globe_word] for st, dl in m.ft.dls if len(dl) > globe_word}
        self.assertGreater(len(frames), 1, "кадр глобуса меняется")
        self.assertTrue(all(0x0E2200 <= (w & 0x3FFFFF) < 0x0E2200 + 12 * 512 for w in frames))
        self.assertTrue(any(dl != search[0] for st, dl in m.ft.dls if st >= 6), "после поиска — лист запуска")
        self.assertEqual(m.vconfig[0], 4, "M_SEL = 1 — первым делом")
        self.assertTrue(esp.started)

    def test_no_ft812_still_starts(self):
        esp = Esp(exit_after=2)
        m = Machine(disk_image(), esp, ft=False)
        m.run()
        self.assertTrue(esp.started)
        self.assertEqual(m.ft.dls, [])

    def test_cached_elf_is_not_uploaded(self):
        esp = Esp(cached=True, exit_after=2)
        Machine(disk_image(), esp).run()
        self.assertEqual(bytes(esp.data), b"")
        self.assertTrue(esp.started)

    def test_missing_elf_fails_before_esp_upload(self):
        esp = Esp()
        m = Machine(disk_image(elf=False), esp)
        m.run()
        self.assertFalse(esp.started)
        self.assertEqual(m.border[-1], 2, "ошибка — красная рамка")
        self.assertIn(0xF1, [t[2] for t in m.boot_trace], "код F1 — файл не найден")

    def test_download_saved_in_dated_folder(self):
        """Файл браузера — на карту в \\zifi\\downloads\\ГГГГ-ММ-ДД (дата по NTP) байт в байт;
        временного «имя.$$$» не остаётся; итог 77 — «записано» с датой каталога."""
        payload = bytes((i * 131 + i // 256 * 7) & 255 for i in range(50000))
        esp = DownloadEsp(payload)
        disk = disk_image()
        m = Machine(disk, esp)
        m.run()
        self.assertEqual(esp.result, (7, 0, 0, b"2026-11-04"))
        self.assertEqual(saved(disk, "2026-11-04", "ALTEREGO.ZIP"), {"ALTEREGO.ZIP": payload})
        self.assertEqual(esp.ntp_asked, 1)
        self.assertEqual(esp.acks, 1, "событие подтверждено сразу")
        self.assertGreater(len(esp.packets), 2, "мышь и клавиши пересылаются и во время записи")
        m.ft.dls and self.assertTrue(all(st <= 10 for st, dl in m.ft.dls),
                                     "после запуска браузера (этап 11) окно запуска FT812 не трогается")
        disk.assert_mirrors()

    def test_download_lost_piece_and_cyrillic_name(self):
        """Потерянный кадр F6 переспрашивается; имя по-русски (CP866) — длинное имя на карте."""
        payload = bytes(range(256)) * 9 + b"tail"
        esp = DownloadEsp(payload, name="Игра.trd".encode("cp866"), drop=(3,))
        disk = disk_image()
        Machine(disk, esp).run()
        self.assertEqual(esp.result[:3], (7, 0, 0))
        self.assertEqual(saved(disk, "2026-11-04", "Игра.trd"), {"Игра.trd": payload})

    def test_download_stream_waits_for_network(self):
        # файл идёт из сети потоком: на часть запросов «куска ещё нет» — Z80 спрашивает снова,
        # попытки не тратятся (подряд их больше DL_TRIES), файл — байт в байт
        payload = bytes((i * 29 + 3) & 255 for i in range(40000))
        esp = DownloadEsp(payload, later=list(range(1, 9)) + [50, 51, 52, 53, 120])
        disk = disk_image()
        Machine(disk, esp).run()
        self.assertEqual(esp.later_sent, 13)
        self.assertEqual(esp.result[1:3], (0, 0))
        self.assertEqual(saved(disk, "2026-11-04", "ALTEREGO.ZIP")["ALTEREGO.ZIP"], payload)

    def test_download_unknown_size_ends_by_eof(self):
        # сервер не сказал длину: E7 с #FFFFFFFF, конец — ответом [3] на месте конца файла
        payload = bytes((i * 7 + 1) & 255 for i in range(20000))
        esp = DownloadEsp(payload, unknown=True)
        disk = disk_image()
        Machine(disk, esp).run()
        self.assertEqual(esp.eof_sent, 1)
        self.assertEqual(esp.result[1:3], (0, 0))
        self.assertEqual(saved(disk, "2026-11-04", "ALTEREGO.ZIP")["ALTEREGO.ZIP"], payload)

    def test_download_refused_keeps_card_clean(self):
        """Отмена посреди файла: итог 2, под настоящим именем ничего нет (только «имя.$$$»)."""
        payload = bytes(40000)
        esp = DownloadEsp(payload, refuse_at=30)
        disk = disk_image()
        Machine(disk, esp).run()
        self.assertEqual(esp.result[:2], (7, 2))
        files = saved(disk, "2026-11-04", "ALTEREGO.ZIP")
        self.assertNotIn("ALTEREGO.ZIP", files)

    def test_download_date_without_ntp(self):
        """NTP не ответил («000…»), часы Evo сбиты — каталог 2026-01-01, как у ZiFi SPG."""
        esp = DownloadEsp(b"x" * 300, ntp=b"00000000000000")
        disk = disk_image()
        Machine(disk, esp).run()
        self.assertEqual(esp.result, (7, 0, 0, b"2026-01-01"))
        self.assertEqual(saved(disk, "2026-01-01", "ALTEREGO.ZIP"), {"ALTEREGO.ZIP": b"x" * 300})

    def test_image_cache_list_write_read_delete(self):
        """Кэш картинок (imgcache.asm): список создаёт Cache\\Images в папке программы; запись —
        файлы байт в байт без «.$$$»; список — имена и размеры; чтение — кадрами 79 байт в байт;
        удаление; чтение удалённого — ошибка карты (итог 1)."""
        a = bytes((i * 37 + 5) & 255 for i in range(5000))
        b = b"tiny" * 75
        ops = [(1, b"", None), (3, b"0K3F9QAB.IMG", a), (3, b"1ABCDEFG.IMG", b), (1, b"", None),
               (2, b"0K3F9QAB.IMG", None), (4, b"0K3F9QAB.IMG", None), (1, b"", None),
               (2, b"0K3F9QAB.IMG", None)]
        esp = CacheEsp(ops)
        disk = disk_image()
        Machine(disk, esp).run()
        r = esp.results
        self.assertEqual([x[2] for x in r], [0, 0, 0, 0, 0, 0, 0, 1], r)
        self.assertEqual(esp.acks, 8, "каждое событие подтверждено")
        self.assertEqual({n: s for n, s, *_ in r[3][4] if n.endswith(".IMG")},
                         {"0K3F9QAB.IMG": 5000, "1ABCDEFG.IMG": 300})
        self.assertEqual(r[4][4], a)
        self.assertEqual({n for n, *_ in r[6][4] if n.endswith(".IMG")}, {"1ABCDEFG.IMG"})
        self.assertEqual(cache_files(disk), {"1ABCDEFG.IMG": b})
        disk.assert_mirrors()

    def test_css_cache_in_own_folder(self):
        """Таблицы стилей (имя «.CSS») — в Cache\\CSS, картинки — в Cache\\Images: список «.CSS» —
        только таблицы, запись и чтение байт в байт, удаление — из своей папки."""
        css = b".a{color:red}" * 40
        img = bytes(range(256)) * 3
        ops = [(1, b"", None), (1, b".CSS", None), (3, b"7ABCDEFG.CSS", css), (3, b"0K3F9QAB.IMG", img),
               (1, b".CSS", None), (1, b"", None), (2, b"7ABCDEFG.CSS", None), (4, b"7ABCDEFG.CSS", None),
               (1, b".CSS", None)]
        esp = CacheEsp(ops)
        disk = disk_image()
        Machine(disk, esp).run()
        r = esp.results
        self.assertEqual([x[2] for x in r], [0] * 9, r)
        self.assertEqual({n: s for n, s, *_ in r[4][4] if not n.startswith(".")}, {"7ABCDEFG.CSS": len(css)})
        self.assertEqual({n for n, *_ in r[5][4] if not n.startswith(".")}, {"0K3F9QAB.IMG"})
        self.assertEqual(r[6][4], css)
        self.assertEqual({n for n, *_ in r[8][4] if not n.startswith(".")}, set())
        self.assertEqual(cache_files(disk), {"0K3F9QAB.IMG": img})
        self.assertEqual(cache_files(disk, "CSS"), {})
        disk.assert_mirrors()

    def test_image_cache_big_file_and_stop(self):
        """Файл больше 64 КБ — запись и чтение байт в байт (32-битные смещения); ответ «хватит»
        посреди чтения — итог 2, файл на карте цел."""
        big = bytes((i * 131 + i // 300) & 255 for i in range(70000))
        ops = [(3, b"BIGFILE0.IMG", big), (2, b"BIGFILE0.IMG", None),
               (2, b"BIGFILE0.IMG", None, 10000)]              # второе чтение — «хватит» после 10000 байт
        esp = CacheEsp(ops)
        disk = disk_image()
        Machine(disk, esp).run()
        r = esp.results
        self.assertEqual([x[2] for x in r], [0, 0, 2], r)
        self.assertEqual(r[1][4], big)
        self.assertEqual(r[2][4], big[:16384])                  # две порции DMA по 8 КБ — и «хватит»
        self.assertEqual(cache_files(disk), {"BIGFILE0.IMG": big})
        # прошивка 0.1.121, браузер разрешил порции DMA: 70000 байт — 9 порций по 8 КБ, не 69 кадров
        self.assertGreaterEqual(esp.big_frames, 8)

    def test_image_cache_read_by_frames_without_media(self):
        """Порции DMA команде 79 не разрешены (старый TSAFARI.ELF): чтение — кадрами по 1016, файл
        байт в байт."""
        big = bytes((i * 71 + i // 900) & 255 for i in range(30000))
        esp = CacheEsp([(3, b"BIGFILE2.IMG", big), (2, b"BIGFILE2.IMG", None)])
        esp.media = False
        disk = disk_image()
        Machine(disk, esp).run()
        r = esp.results
        self.assertEqual([x[2] for x in r], [0, 0], r)
        self.assertEqual(r[1][4], big)
        self.assertEqual(esp.big_frames, 0)

    def test_image_cache_write_stopped_then_again(self):
        """Запись, прерванная ответом «хватит» (браузеру срочно нужно чтение, imgcache.c), — итог 2,
        готового файла нет; та же запись снова — файл байт в байт, недописанного «.$$$» в списке нет."""
        big = bytes((i * 29 + i // 700) & 255 for i in range(70000))
        ops = [(3, b"BIGFILE1.IMG", big, 20000), (3, b"BIGFILE1.IMG", big), (1, b"", None),
               (2, b"BIGFILE1.IMG", None)]
        esp = CacheEsp(ops)
        disk = disk_image()
        Machine(disk, esp).run()
        r = esp.results
        self.assertEqual([x[2] for x in r], [2, 0, 0, 0], r)
        self.assertEqual(r[3][4], big)
        self.assertEqual([n for n, *_ in r[2][4] if "BIGFILE1" in n], ["BIGFILE1.IMG"])
        self.assertEqual(cache_files(disk), {"BIGFILE1.IMG": big})

    def test_esp_cannot_start_reports_error(self):
        esp = Esp(start_answer=0xFE)
        m = Machine(disk_image(), esp)
        m.run()
        self.assertFalse(esp.started)
        self.assertEqual(m.border[-1], 2)


if __name__ == "__main__":
    unittest.main()
