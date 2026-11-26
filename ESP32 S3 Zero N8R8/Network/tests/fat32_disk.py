"""Образ SD-карты FAT32 для машинных тестов Z80: класс Disk и разбор символов sjasmplus.

Скопировано без изменений (кроме этой шапки и импортов, которые здесь не нужны) из проверок
драйвера FAT32 — FAT32-Driver-ZX-Evolution, tests/harness.py, коммит e72839070fcdc8eb0b6dbae2b9f6ff52b0a2a22d
(через копию в DOOM VDAC4, third_party/fat32/tests/harness.py, 23.11.2026). Автор — Andrew Lazarev,
лицензия MIT: ../shared/fat32/LICENSE. Сам драйвер — ../shared/fat32.

Disk — том FAT32 с настоящими MBR, BPB и FSInfo; хранит только записанные секторы, поэтому
большой «разреженный» том занимает мало памяти. Им пользуется test_esp_app_spg_data_z80.py.
"""
from pathlib import Path
import re
import struct


def symbols(path):
    return {m[1]: int(m[2], 16) for line in Path(path).read_text().splitlines()
            if (m := re.match(r'^([^:]+):\s+EQU\s+(0x[0-9A-Fa-f]+)$', line))}


def u32(data, offset=0):
    return struct.unpack_from('<I', data, offset)[0]


class Disk:
    """Независимый разбор FAT и тестовый том FAT32 с настоящими MBR/BPB/FSInfo."""
    def __init__(self, spc=1, partition=0, clusters=65536, extended=0, extended_skip=0,
                 fats=2, ext_flags=0):
        self.spc, self.start, self.clusters = spc, partition, clusters
        self.fats = fats
        # BPB_ExtFlags: бит 7 отключает зеркалирование, младшая тетрада —
        # номер активной копии FAT. Стенд обязан читать ту же копию.
        self.active = (ext_flags & 0x0F) if ext_flags & 0x80 else 0
        self.step = extended + 16
        self.ebr = partition - extended - self.step*extended_skip if extended else 0
        self.reserved = 32
        self.fat_sectors = (clusters + 2 + 127) // 128
        self.total = self.reserved + fats*self.fat_sectors + clusters*spc
        self.data_start = partition + self.reserved + fats*self.fat_sectors
        # Храним только записанные секторы: большие и полные тома требуют мало ОЗУ.
        self.blocks = {}
        bpb = bytearray(512)
        bpb[:11] = b'\xEB\x58\x90F32TEST '
        struct.pack_into('<HBHBHHBHHHII', bpb, 11,
                         512, spc, self.reserved, fats, 0, 0, 0xF8, 0, 63, 255,
                         partition, self.total)
        struct.pack_into('<IHHIHH', bpb, 36, self.fat_sectors, ext_flags, 0, 2, 1, 6)
        bpb[64], bpb[66] = 0x80, 0x29
        struct.pack_into('<I', bpb, 67, 0x76543210)
        bpb[71:82], bpb[82:90], bpb[510:512] = b'TEST VOLUME', b'FAT32   ', b'\x55\xAA'
        self.write(partition, bpb)
        self.write(partition+6, bpb)
        info = bytearray(512)
        struct.pack_into('<I', info, 0, 0x41615252)
        struct.pack_into('<III', info, 484, 0x61417272, clusters-1, 3)
        info[510:512] = b'\x55\xAA'
        self.write(partition+1, info)
        self.write(partition+7, info)
        for cluster, value in [(0, 0x0FFFFFF8), (1, 0x0FFFFFFF), (2, 0x0FFFFFFF)]:
            self.set_fat(cluster, value)
        if partition:
            mbr = bytearray(512)
            mbr[510:512] = b'\x55\xAA'
            if extended:
                # Цепочка EBR: extended — отступ от каждого EBR до его тома,
                # extended_skip — сколько логических дисков идёт перед нужным.
                # У «пустышек» нет годного BPB, поэтому драйвер обязан пройти
                # по ссылке дальше, а не остановиться на первом же томе.
                assert self.ebr > 0, 'цепочка EBR не помещается перед томом'
                mbr[450] = 0x0F
                struct.pack_into('<II', mbr, 454, self.ebr,
                                 self.total + partition - self.ebr)
                for n in range(extended_skip + 1):
                    last = n == extended_skip
                    rec = bytearray(512)
                    rec[510:512] = b'\x55\xAA'
                    rec[450] = 0x0C
                    struct.pack_into('<II', rec, 454, extended,
                                     self.total if last else 16)
                    if not last:
                        # Ссылка на следующий EBR — от начала первого EBR.
                        rec[450+16] = 0x05
                        struct.pack_into('<II', rec, 454+16,
                                         self.step*(n+1), self.step + 16)
                    self.write(self.ebr + self.step*n, rec)
            else:
                mbr[450] = 0x0C
                struct.pack_into('<II', mbr, 454, partition, self.total)
            self.write(0, mbr)

    def read(self, lba):
        assert 0 <= lba < self.start + self.total, ('LBA outside media', lba)
        return self.blocks.get(lba, bytes(512))

    def write(self, lba, data):
        assert len(data) == 512
        assert 0 <= lba < self.start + self.total
        self.blocks[lba] = bytes(data)

    def set_fat(self, cluster, value):
        for copy in range(self.fats):
            lba = self.start + self.reserved + copy*self.fat_sectors + cluster//128
            sector = bytearray(self.read(lba))
            struct.pack_into('<I', sector, (cluster % 128)*4, value)
            self.write(lba, sector)

    def fat(self, cluster, copy=None):
        copy = self.active if copy is None else copy
        sector = self.read(self.start+self.reserved+copy*self.fat_sectors+cluster//128)
        return u32(sector, cluster%128*4) & 0x0FFFFFFF

    def chain(self, first):
        found = []
        while first:
            assert 2 <= first < self.clusters+2, ('invalid cluster', first)
            assert first not in found, ('FAT cycle', first)
            found.append(first)
            first = self.fat(first)
            if first >= 0x0FFFFFF8:
                return found
        assert not found, 'allocated chain ends in FREE'
        return []

    def cluster_data(self, cluster):
        lba = self.data_start + (cluster-2)*self.spc
        return b''.join(self.read(lba+i) for i in range(self.spc))

    def entries(self, directory=2):
        raw = b''.join(self.cluster_data(c) for c in self.chain(directory))
        pending, result = {}, []
        positions = (1,3,5,7,9,14,16,18,20,22,24,28,30)
        for i in range(0, len(raw), 32):
            e = raw[i:i+32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                pending = {}
                continue
            if e[11] == 15:
                pending[e[0] & 31] = e
                continue
            if pending:
                name = b''.join(pending[n][p:p+2] for n in sorted(pending)
                                for p in positions).decode('utf-16-le').split('\0')[0].rstrip('\uffff')
                checksum = 0
                for x in e[:11]:
                    checksum = (((checksum & 1) << 7) + (checksum >> 1) + x) & 255
                assert all(record[13] == checksum for record in pending.values())
            else:
                stem, ext = e[:8].decode('cp866').rstrip(), e[8:11].decode('cp866').rstrip()
                name = stem + ('.'+ext if ext else '')
            pending = {}
            result.append(dict(name=name, short=e[:11], attr=e[11], size=u32(e,28),
                               cluster=struct.unpack_from('<H',e,26)[0] | struct.unpack_from('<H',e,20)[0]<<16,
                               raw=e))
        return result

    def get(self, name, directory=2):
        return next(e for e in self.entries(directory) if e['name'].casefold() == name.casefold())

    def contents(self, name, directory=2):
        e = self.get(name, directory)
        data = b''.join(self.cluster_data(c) for c in self.chain(e['cluster']))
        assert len(data) >= e['size']
        return data[:e['size']]

    def assert_mirrors(self):
        assert not self.active, 'том без зеркалирования: копии FAT равны быть не должны'
        for copy in range(1, self.fats):
            for i in range(self.fat_sectors):
                assert self.read(self.start+self.reserved+i) ==                     self.read(self.start+self.reserved+copy*self.fat_sectors+i)
