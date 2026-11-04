"""Кэши READ_AT: данные, обращения к устройству и все границы срока годности."""
import struct
import unittest

from harness import DATA, SYNC, u32
from filex_support import resource, expected, measure


class FilexCacheTests(unittest.TestCase):
    def read(self, h, chain, offset=7, length=1016):
        return measure(h, chain, offset, length)

    def raw_write(self, h, lba, data):
        h.cpu.set_memory_block(DATA, data)
        return h.call(5, hl=lba & 65535, de=lba >> 16, bc=DATA, a=len(data) // 512)

    def generation(self, h):
        return u32(h.ram, h.sym['FILEX_GENERATION'])

    def test_tick_counter_in_benchmark(self):
        """NOP и RET стоят 4+10 тактов: проверка счётчика, общего для ДО/ПОСЛЕ."""
        h, _ = resource(size=512)
        h.sym['TIMING_PROBE'] = 0x8180
        h.cpu.set_memory_block(0x8180, b'\x00\xc9')
        h.call('TIMING_PROBE')
        self.assertEqual(h.last_ticks, 14)

    def test_deep_sequential_and_backward_landmarks(self):
        """93 МБ, 32 КиБ: тёплый проход — только данные; назад — ближайшая веха."""
        h, chain = resource()
        base = 86 * 1024 * 1024 + 7
        cold = self.read(h, chain, base)
        self.assertEqual((cold['directory'], cold['fat'], cold['data']), (1, 22, 2))
        for n in range(1, 12):
            r = self.read(h, chain, base + 1016 * n)
            self.assertEqual((r['directory'], r['fat'], r['data']), (0, 0, 2))
            self.assertLess(r['ticks'], 40000)
        for index in (1001, 2601, 71, 2743, 127, 2101):
            r = self.read(h, chain, index * 32768 + 511)
            self.assertEqual(r['directory'], 0)
            self.assertLessEqual(r['fat'], 2)
            self.assertLess(r['ticks'], 170000)

    def test_sector_sizes_offsets_and_cluster_crossings(self):
        for spc in (1, 2, 4, 8, 16, 32, 64):
            with self.subTest(spc=spc):
                h, chain = resource(size=spc * 512 * 4, spc=spc)
                for offset, length in ((0, 512), (1, 1016), (511, 1016),
                                       (spc * 512 - 9, 1016), (517, 1)):
                    r = self.read(h, chain, offset, length)
                    self.assertLessEqual(r['data'], (offset % 512 + length + 511) // 512)
                    self.assertLessEqual(r['fat'], 1)

    def test_last_sector_hit_including_aligned_end(self):
        h, chain = resource(size=8192, spc=1)
        for offset, length in ((0, 512), (5, 1016), (1024, 4096)):
            self.read(h, chain, offset, length)
            last = (offset + length - 1) & ~511
            r = self.read(h, chain, last, 512)
            self.assertEqual((r['directory'], r['data']), (0, 0))
            r = self.read(h, chain, last + 17, 3)
            self.assertEqual((r['directory'], r['data']), (0, 0))

    def test_full_middle_sectors_go_directly_to_odd_user_buffer(self):
        h, chain = resource(size=32768, spc=8)
        offset, length, buffer = 3, 7000, 0xC001
        h.transfers.clear()
        self.assertEqual(h.filex(1, offset=offset, buffer=buffer, length=length),
                         ((0, True, False), length))
        self.assertEqual(bytes(h.ram[buffer:buffer + length]), expected(h, chain, offset, length))
        direct = [(lba, buf) for op, lba, buf in h.transfers
                  if op == 'read' and buf >= buffer]
        self.assertEqual(len(direct), 12)
        self.assertEqual([buf for _, buf in direct], [buffer + 509 + i * 512 for i in range(12)])

    def test_fat_sector_changes_only_at_boundary(self):
        h, chain = resource(size=300 * 512, spc=1)
        self.read(h, chain, 123 * 512, 10)  # кластер 126, ссылка в секторе 0
        r = self.read(h, chain, 123 * 512 + 510, 1016)
        self.assertEqual((r['directory'], r['fat']), (0, 0))
        r = self.read(h, chain, 125 * 512 + 510, 1016)  # ссылка кластера 128
        self.assertEqual((r['directory'], r['fat']), (0, 1))

    def test_fragmented_landmarks_are_cluster_numbers_not_lba_arithmetic(self):
        h, chain = resource(size=260 * 4096, spc=8, fragmented=True)
        self.read(h, chain, 250 * 4096 + 7)
        for index in (71, 201, 130, 249):
            r = self.read(h, chain, index * 4096 + 33)
            self.assertEqual(r['directory'], 0)
            self.assertLessEqual(r['fat'], 63)
            self.assertLess(r['ticks'], 470000)

    def test_last_position_keeps_all_32_bits_and_table_saturates(self):
        h, chain = resource(size=65540 * 512, spc=1)
        self.read(h, chain, 65535 * 512 + 511, 1016)
        r = self.read(h, chain, 65537 * 512 + 3, 512)
        self.assertLessEqual(r['fat'], 1)
        self.assertLess(r['ticks'], 60000)
        self.assertEqual(h.ram[h.sym['FILEX_POINT_COUNT']], 64)
        r = self.read(h, chain, 3073 * 512 + 7, 100)
        self.assertLessEqual(r['fat'], 1)

    def test_write_at_invalidates_and_preserves_neighbour_bytes(self):
        h, chain = resource(size=8192, spc=2)
        self.read(h, chain)
        before = expected(h, chain, 0, 8192)
        h.cpu.set_memory_block(DATA, b'changed!')
        self.assertEqual(h.filex(2, offset=1010, length=8), ((0, True, False), 8))
        self.read(h, chain, 1001)
        self.assertEqual(expected(h, chain, 0, 8192), before[:1010] + b'changed!' + before[1018:])

    def test_stream_write_invalidates_data(self):
        h, chain = resource(size=8192, spc=2)
        self.read(h, chain, 3, 100)
        h.cpu.set_memory_block(DATA, b'S' * 512)
        self.assertEqual(h.call(49, hl=DATA, b=1), (0, True, False))
        r = self.read(h, chain, 3, 100)
        self.assertEqual(r['data'], 1)
        self.assertEqual(bytes(h.ram[DATA:DATA + 100]), b'S' * 100)

    def test_append_invalidates_partial_sector_and_context(self):
        h, chain = resource(size=700, spc=4)
        self.read(h, chain, 600, 100)
        self.assertEqual(h.append(b'A' * 300), (0, True, False))
        r = self.read(h, chain, 680, 300)
        self.assertEqual(r['directory'], 1)
        self.assertEqual(bytes(h.ram[DATA + 20:DATA + 300]), b'A' * 280)

    def test_truncate_regrow_does_not_resurrect_cached_tail(self):
        h, chain = resource(size=8192, spc=2)
        self.read(h, chain, 7200, 200)
        self.assertEqual(h.filex(3, offset=700), ((0, True, False), 700))
        self.assertEqual(h.filex(1, offset=7200, length=200)[0][0], 1)
        self.assertEqual(h.filex(3, offset=8192), ((0, True, False), 8192))
        chain = h.disk.chain(3)
        r = self.read(h, chain, 7200, 200)
        self.assertGreaterEqual(r['data'], 1)
        self.assertEqual(bytes(h.ram[DATA:DATA + 200]), bytes(200))

    def test_raw_data_write_and_each_sector_generation(self):
        h, chain = resource(size=8192, spc=4)
        self.read(h, chain, 7, 100)
        generation = self.generation(h)
        lba = h.disk.data_start + h.disk.spc
        self.assertEqual(self.raw_write(h, lba, b'R' * 1024), (0, True, False))
        self.assertEqual(self.generation(h) - generation, 2)
        r = self.read(h, chain, 7, 100)
        self.assertEqual(r['data'], 1)
        self.assertEqual(bytes(h.ram[DATA:DATA + 100]), b'R' * 100)

    def test_raw_fat_write_changes_chain_with_same_file_and_directory(self):
        h, chain = resource(size=200 * 512, spc=1)
        self.read(h, chain, 150 * 512 + 7, 100)
        lba = h.disk.start + h.disk.reserved
        sector = bytearray(h.disk.read(lba))
        # Перестановка 4 и 5: размер, запись каталога и голова 3 прежние.
        for c, n in ((3, 5), (5, 4), (4, 6)):
            struct.pack_into('<I', sector, c * 4, n)
        self.assertEqual(self.raw_write(h, lba, sector), (0, True, False))
        chain[1], chain[2] = chain[2], chain[1]
        r = self.read(h, chain, 512 + 7, 100)
        self.assertEqual(r['directory'], 1)
        self.assertGreater(r['fat'], 0)

    def test_raw_directory_write_cannot_hide_behind_verified_context(self):
        h, chain = resource(size=8192)
        self.read(h, chain)
        entry = bytearray(h.disk.read(h.disk.data_start))
        entry[0] = ord('X')
        self.assertEqual(self.raw_write(h, h.disk.data_start, entry), (0, True, False))
        self.assertEqual(h.filex(1, length=10)[0][0], 0x16)

    def test_new_generation_rebuilds_previously_saved_landmark(self):
        h, chain = resource(size=200 * 512, spc=1)
        self.read(h, chain, 150 * 512 + 7, 100)
        # Меняется сама веха 128; голова, размер и запись файла прежние.
        for c, n in ((130, 132), (132, 131), (131, 133)):
            h.disk.set_fat(c, n)
        chain[128], chain[129] = chain[129], chain[128]
        h.call('FILEX_NEW_GENERATION')
        r = self.read(h, chain, 128 * 512 + 7, 100)
        self.assertEqual(r['directory'], 1)
        self.assertGreater(r['fat'], 0)

    def test_corrupt_or_root_chain_after_raw_write_is_not_cached(self):
        for c, n in ((4, 0), (4, 4), (4, 2), (2, 4)):
            with self.subTest(cluster=c, link=n):
                h, chain = resource(size=8192, spc=1)
                self.read(h, chain, 3000, 100)
                lba = h.disk.start + h.disk.reserved
                sector = bytearray(h.disk.read(lba))
                struct.pack_into('<I', sector, c * 4, n)
                self.raw_write(h, lba, sector)
                h.operations.clear()
                self.assertEqual(h.filex(1, offset=3000, length=100)[0][0], 0x20)
                self.assertFalse(any(op == 'read' and block > h.disk.data_start
                                     for op, block in h.operations))

    def test_write_and_shrink_reuse_warmed_position(self):
        for op in (2, 3):
            with self.subTest(operation=op):
                h, chain = resource()
                offset = 93_000_000 - 100
                self.read(h, chain, offset, 50)
                h.operations.clear()
                h.cpu.set_memory_block(DATA, b'W' * 8)
                self.assertEqual(h.filex(op, offset=offset, length=8),
                                 ((0, True, False), 8 if op == 2 else offset))
                before_write = []
                for operation in h.operations:
                    if operation[0] == 'write':
                        break
                    before_write.append(operation)
                fat_reads = sum(h.disk.reserved <= lba < h.disk.data_start
                                for _, lba in before_write)
                self.assertLessEqual(fat_reads, 1)
                if op == 2:
                    self.read(h, chain, offset, 8)
                    self.assertEqual(bytes(h.ram[DATA:DATA + 8]), b'W' * 8)
                else:
                    self.assertEqual(h.disk.get('resource.bin')['size'], offset)

    def test_other_file_and_same_slot_recreated(self):
        h, chain = resource(size=8192, spc=2)
        self.read(h, chain)
        self.assertEqual(h.create('other.bin'), (0, True, False))
        self.assertEqual(h.append(b'O' * 2048), (0, True, False))
        other = h.disk.chain(h.disk.get('other.bin')['cluster'])
        self.read(h, other)
        self.assertEqual(bytes(h.ram[DATA:DATA + 1016]), b'O' * 1016)
        self.assertEqual(h.find('resource.bin'), (1, False, False))
        self.read(h, chain)
        self.assertEqual(h.call(75, hl=h.name('resource.bin', b'\0')), (0, True, False))
        self.assertEqual(h.create('resource.bin'), (0, True, False))
        self.assertEqual(h.append(b'N' * 2048), (0, True, False))
        self.read(h, h.disk.chain(h.disk.get('resource.bin')['cluster']))
        self.assertEqual(bytes(h.ram[DATA:DATA + 1016]), b'N' * 1016)

    def test_mount_init_bind_and_directory_restart_expire_keys(self):
        for action in ('mount', 'mount_at', 'init', 'bind', 'directory'):
            with self.subTest(action=action):
                h, chain = resource(size=8192, spc=2)
                self.read(h, chain)
                old = self.generation(h)
                if action == 'directory':
                    self.assertIn(h.call('WDOS.NXTINI')[0], (0, 15))
                else:
                    if action == 'init':
                        self.assertEqual(h.call(1), (0, True, False))
                    if action == 'bind':
                        # Таблица остаётся в приватной копии драйвера.
                        self.assertEqual(h.call(0, hl=h.sym['BLOCK_CALLBACKS']), (0, True, False))
                        self.assertEqual(h.call(1), (0, True, False))
                    self.assertEqual(h.call(36, hl=0, de=0) if action == 'mount_at' else h.call(3),
                                     (0, True, False))
                    self.assertEqual(h.find('resource.bin'), (1, False, False))
                self.assertNotEqual(self.generation(h), old)
                self.assertEqual(self.read(h, chain)['directory'], 1)

    def test_context_offset_and_all_entry_bytes_are_part_of_key(self):
        h, chain = resource(size=8192)
        self.read(h, chain)
        context = h.sym['WDOS_EXT.APPEND_DIRECTORY_LBA']
        for delta in (0, 4, 6, 6 + 22, 6 + 28):
            with self.subTest(delta=delta):
                h.ram[context + delta] ^= 32
                self.assertNotEqual(h.filex(1, length=10)[0][0], 0)
                h.ram[context + delta] ^= 32
                self.read(h, chain)

    def test_common_buffers_can_be_used_between_cache_hits(self):
        h, chain = resource(size=8192)
        self.read(h, chain, 7, 100)
        for buffer in (0x3000, 0x3200, 0x3400):
            self.assertEqual(h.call(4, hl=0, de=0, bc=buffer, a=1), (0, True, False))
        r = self.read(h, chain, 7, 100)
        self.assertEqual((r['directory'], r['data']), (0, 0))

    def test_overwriting_shared_fat_buffer_forces_fresh_link(self):
        for buffer, sectors in ((0x3000, 1), (0x3001, 1), (0x3000, 2)):
            with self.subTest(buffer=buffer, sectors=sectors):
                h, chain = resource(size=200 * 512, spc=1)
                self.read(h, chain, 70 * 512 + 7, 100)
                self.assertEqual(h.call(4, hl=0, de=0, bc=buffer, a=sectors), (0, True, False))
                r = self.read(h, chain, 71 * 512 + 7, 100)
                self.assertEqual(r['fat'], 1)

    def test_raw_read_into_private_data_buffer_drops_tag(self):
        h, chain = resource(size=8192)
        self.read(h, chain, 7, 100)
        self.assertEqual(h.call(4, hl=0, de=0, bc=h.sym['FILEX_DATA_BUFFER'], a=1), (0, True, False))
        self.assertEqual(self.read(h, chain, 7, 100)['data'], 1)

    def test_read_failure_drops_all_caches_even_after_partial_transfer(self):
        for length in (10, 1500):
            with self.subTest(length=length):
                h, chain = resource(size=8192, spc=2)
                self.read(h, chain, 7, 100)
                fail_lba = h.disk.data_start + h.disk.spc + 3
                h.fail = lambda op, lba: op == 'read' and lba == fail_lba
                self.assertEqual(h.filex(1, offset=3 * 512 + 7, length=length)[0][0], 0x21)
                h.fail = None
                r = self.read(h, chain, 7, 100)
                self.assertEqual((r['directory'], r['data']), (1, 1))

    def test_write_error_that_landed_does_not_return_old_cached_data(self):
        h, chain = resource(size=8192, spc=2)
        self.read(h, chain, 7, 100)
        lba = h.disk.data_start + h.disk.spc
        def fail(op, block):
            if op == 'write' and block == lba:
                h.disk.write(block, b'L' * 512)
                return True
            return False
        h.fail = fail
        self.assertNotEqual(self.raw_write(h, lba, b'W' * 512)[0], 0)
        h.fail = None
        r = self.read(h, chain, 7, 100)
        self.assertEqual(r['data'], 1)
        self.assertEqual(bytes(h.ram[DATA:DATA + 100]), b'L' * 100)

    def test_sync_error_expires_cached_data(self):
        h, chain = resource(size=8192)
        self.read(h, chain, 7, 100)
        callback = h.callback
        def failed_sync():
            pc = h.cpu.pc
            callback()
            if pc == SYNC:
                h.cpu.a, h.cpu.f = 0xE1, 0
        h.callback = failed_sync
        self.assertEqual(h.call(34)[0], 0xE1)
        h.callback = callback
        r = self.read(h, chain, 7, 100)
        self.assertEqual((r['directory'], r['data']), (1, 1))

    def test_generation_wrap_does_not_revive_cached_data(self):
        h, chain = resource(size=8192)
        self.read(h, chain, 7, 100)
        h.cpu.set_memory_block(h.sym['FILEX_GENERATION'], b'\xff' * 4)
        lba = h.disk.data_start + h.disk.spc
        self.raw_write(h, lba, b'Z' * 512)
        self.assertEqual(self.generation(h), 0)
        self.assertEqual(self.read(h, chain, 7, 100)['data'], 1)
        self.assertEqual(bytes(h.ram[DATA:DATA + 100]), b'Z' * 100)


if __name__ == '__main__':
    unittest.main()
