"""Разреженный ресурсный файл: настоящая FAT, данные зависят от LBA и байта."""
import struct
from harness import Disk, Driver, DATA


class ResourceDisk(Disk):
    def read(self, lba):
        if lba >= self.data_start + self.spc and lba not in self.blocks:
            assert lba < self.start + self.total
            return bytes(((lba * 17 + (lba >> 8) + i * 29 + (i >> 8)) & 255)
                         for i in range(512))
        return super().read(lba)


def resource(size=93_000_000, spc=64, build=None, fragmented=False):
    count = (size + spc * 512 - 1) // (spc * 512)
    chain = [3 + i * (137 if fragmented else 1) for i in range(count)]
    disk = ResourceDisk(spc=spc, clusters=max(65536, chain[-1] + 1))
    assert chain[-1] < disk.clusters + 2
    for c, n in zip(chain, chain[1:] + [0x0FFFFFFF]):
        disk.set_fat(c, n)
    entry = bytearray(512)
    entry[:11] = b'RESOURCEBIN'
    entry[11] = 32
    struct.pack_into('<H', entry, 26, 3)
    struct.pack_into('<I', entry, 28, size)
    disk.write(disk.data_start, entry)
    h = Driver(disk, build=build)
    assert h.find('resource.bin') == (1, False, False)
    h.operations.clear()
    return h, chain


def expected(h, chain, offset, length):
    result = bytearray()
    pos = offset
    while pos < offset + length:
        ci, within = divmod(pos, h.disk.spc * 512)
        lba = h.disk.data_start + (chain[ci] - 2) * h.disk.spc + within // 512
        skip = within % 512
        take = min(512 - skip, offset + length - pos)
        result.extend(h.disk.read(lba)[skip:skip + take])
        pos += take
    return bytes(result)


def measure(h, chain, offset, length=1016):
    h.operations.clear()
    assert h.filex(1, offset=offset, length=length) == ((0, True, False), length)
    assert bytes(h.ram[DATA:DATA + length]) == expected(h, chain, offset, length)
    reads = [lba for op, lba in h.operations if op == 'read']
    fat = sum(h.disk.start + h.disk.reserved <= lba < h.disk.data_start for lba in reads)
    directory = reads.count(h.disk.data_start)
    return dict(offset=offset, length=length, directory=directory, fat=fat,
                data=len(reads) - fat - directory, ticks=h.last_ticks)
