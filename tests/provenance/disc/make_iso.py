#!/usr/bin/env python3
"""Build a small bootable ISO 9660 image whose every data sector names itself.

Project-authored from ECMA-119 (ISO 9660): a primary volume descriptor, the
terminator, both path tables, a root directory holding two files, one
subdirectory and the PSP_GAME tree that makes the image bootable, and files
whose 32-bit words are (sector << 16) | word_index so a raw sector read or a
file read identifies exactly which sector it came from. The probe itself is
the image's EBOOT.BIN, so the executable runs it the way it runs a game: from
its own disc. The fixed entries are recorded in LAYOUT; the boot tree's sizes
depend on the probe build and are read back from the image by derive.py.
"""
import argparse
import struct
from pathlib import Path

SECTOR = 2048
ROOT = Path(__file__).resolve().parent
# name, parent directory, first sector, size in bytes, flags (2 = directory)
LAYOUT = {
    'root':     ('\x00',      None,       20, SECTOR, 2),
    'SUB':      ('SUB',       'root',     21, SECTOR, 2),
    'ALPHA':    ('ALPHA.BIN', 'root',     22, 8 * SECTOR, 0),
    'BETA':     ('BETA.TXT',  'root',     30, 100, 0),
    'GAMMA':    ('GAMMA.BIN', 'SUB',      31, 3 * SECTOR + 7, 0),
    'PSP_GAME': ('PSP_GAME',  'root',     35, SECTOR, 2),
    'PARAM':    ('PARAM.SFO', 'PSP_GAME', 36, None, 0),
    'SYSDIR':   ('SYSDIR',    'PSP_GAME', 37, SECTOR, 2),
    'EBOOT':    ('EBOOT.BIN', 'SYSDIR',   38, None, 0),
}
PVD, TERMINATOR, PATH_L, PATH_M = 16, 17, 18, 19
DATE7 = bytes([126, 9, 19, 12, 0, 0, 0])            # 2026-09-19 12:00:00, GMT
DATE17 = b'2026091912000000\x00'


def both16(v): return struct.pack('<H', v) + struct.pack('>H', v)
def both32(v): return struct.pack('<I', v) + struct.pack('>I', v)


def record(name, sector, size, flags):
    ident = name.encode('latin-1')
    if flags == 0:
        ident += b';1'
    body = bytes([0]) + both32(sector) + both32(size) + DATE7 + bytes([flags, 0, 0]) + both16(1) + bytes([len(ident)]) + ident
    if len(ident) % 2 == 0:
        body += b'\x00'
    return bytes([len(body) + 1]) + body


def directory(entries, self_sector, parent_sector):
    data = record('\x00', self_sector, SECTOR, 2) + record('\x01', parent_sector, SECTOR, 2)
    for name, sector, size, flags in entries:
        data += record(name, sector, size, flags)
    return data.ljust(SECTOR, b'\x00')


def path_table(little):
    order = '<' if little else '>'
    out = b''
    for ident, sector, parent in (('\x00', 20, 1), ('SUB', 21, 1), ('PSP_GAME', 35, 1), ('SYSDIR', 37, 3)):
        raw = ident.encode('latin-1')
        out += bytes([len(raw), 0]) + struct.pack(order + 'I', sector) + struct.pack(order + 'H', parent) + raw
        if len(raw) % 2:
            out += b'\x00'
    return out


def content(sector, size):
    words = b''.join(struct.pack('<I', (s << 16) | j) for s in range(sector, sector + (size + SECTOR - 1) // SECTOR)
                     for j in range(SECTOR // 4))
    return words[:size]


def param_sfo():
    """The smallest PARAM.SFO the loader needs: a bootable UMD game entry."""
    entries = [('BOOTABLE', 0x0404, struct.pack('<I', 1), 4),
               ('CATEGORY', 0x0204, b'UG\x00\x00', 3),
               ('DISC_ID', 0x0204, b'PROB00001\x00'.ljust(16, b'\x00'), 10),
               ('DISC_VERSION', 0x0204, b'1.00\x00'.ljust(8, b'\x00'), 5),
               ('PSP_SYSTEM_VER', 0x0204, b'6.60\x00'.ljust(8, b'\x00'), 5),
               ('TITLE', 0x0204, b'Disc probe\x00'.ljust(128, b'\x00'), 11)]
    keys = b''
    data = b''
    table = b''
    for name, fmt, value, used in entries:
        table += struct.pack('<HHIII', len(keys), fmt, used, len(value), len(data))
        keys += name.encode('ascii') + b'\x00'
        data += value
    while len(keys) % 4:
        keys += b'\x00'
    head = struct.pack('<4sIIII', b'\x00PSF', 0x0101, 20 + len(table), 20 + len(table) + len(keys), len(entries))
    return head + table + keys + data


def total_sectors(image):
    """The volume space size the image's own descriptor declares."""
    return struct.unpack_from('<I', image, PVD * SECTOR + 80)[0]


def build(eboot):
    sfo = param_sfo()
    eboot_sector = LAYOUT['EBOOT'][2]
    total = eboot_sector + (len(eboot) + SECTOR - 1) // SECTOR
    image = bytearray(total * SECTOR)
    def put(sector, data): image[sector * SECTOR:sector * SECTOR + len(data)] = data
    ptable = path_table(True)
    root = record('\x00', 20, SECTOR, 2)
    pvd = (bytes([1]) + b'CD001' + bytes([1, 0]) + b'PSPRECOMP PROBE'.ljust(32) + b'PROBE'.ljust(32)
           + bytes(8) + both32(total) + bytes(32) + both16(1) + both16(1) + both16(SECTOR)
           + both32(len(ptable)) + struct.pack('<I', PATH_L) + bytes(4) + struct.pack('>I', PATH_M) + bytes(4)
           + root + b' ' * 128 + b' ' * 128 + b' ' * 128 + b' ' * 128 + b' ' * 37 + b' ' * 37 + b' ' * 37
           + DATE17 + DATE17 + DATE17 + DATE17 + bytes([1, 0]))
    put(PVD, pvd)
    put(TERMINATOR, bytes([255]) + b'CD001' + bytes([1]))
    put(PATH_L, ptable)
    put(PATH_M, path_table(False))
    put(20, directory([('SUB', 21, SECTOR, 2), ('ALPHA.BIN', 22, 8 * SECTOR, 0), ('BETA.TXT', 30, 100, 0),
                       ('PSP_GAME', 35, SECTOR, 2)], 20, 20))
    put(21, directory([('GAMMA.BIN', 31, 3 * SECTOR + 7, 0)], 21, 20))
    put(35, directory([('PARAM.SFO', 36, len(sfo), 0), ('SYSDIR', 37, SECTOR, 2)], 35, 20))
    put(36, sfo)
    put(37, directory([('EBOOT.BIN', eboot_sector, len(eboot), 0)], 37, 35))
    put(eboot_sector, eboot)
    for key in ('ALPHA', 'BETA', 'GAMMA'):
        _, _, sector, size, _ = LAYOUT[key]
        put(sector, content(sector, size))
    assert total_sectors(image) == total
    return bytes(image)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('eboot', type=Path, help='the probe ELF, placed as PSP_GAME/SYSDIR/EBOOT.BIN')
    parser.add_argument('-o', '--output', type=Path, default=ROOT / 'probe.iso')
    args = parser.parse_args()
    data = build(args.eboot.read_bytes())
    args.output.write_bytes(data)
    print(f'wrote {len(data)} bytes, {total_sectors(data)} sectors, to {args.output}')
