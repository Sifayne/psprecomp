#!/usr/bin/env python3
"""Derive disc filesystem and raw-device facts from our recorded disc probe.

The image is the one make_iso.py built around the probe (its LAYOUT names
every fixed sector; the boot tree's size is read from the image), so each
recorded word can be checked against what the image holds. The probe ran as
a game booted from that image. Every rule below is checked against recorded
output only; no emulator source or runtime implementation is an input.
"""
import struct
import sys
from pathlib import Path

root = Path(__file__).resolve().parent
sys.path.insert(0, str(root))
from make_iso import LAYOUT, SECTOR, content, total_sectors  # noqa: E402

rows = {}
for line in (root / 'observed.txt').read_text().splitlines():
    words = [int(w, 16) for w in line.split()]
    assert (words[0], words[1]) not in rows, ('duplicate record', words[:2])
    rows[(words[0], words[1])] = words[2:]
assert rows[(9, 0)] == [0], 'the run completed'
image = (root / 'probe.iso').read_bytes()
TOTAL_SECTORS = total_sectors(image)
assert len(image) == TOTAL_SECTORS * SECTOR
FILL, STAT_FILL = 0xeeeeeeee, 0xcdcdcdcd
def word(sector, index): return struct.unpack_from('<I', image, sector * SECTOR + 4 * index)[0]
def file_words(key, offset, count):
    _, _, sector, size, _ = LAYOUT[key]
    data = content(sector, size)[offset:offset + 4 * count]
    return list(struct.unpack('<%dI' % (len(data) // 4), data[:len(data) // 4 * 4]))

# --- opens: which names exist -------------------------------------------------
opened = {case: rows[(0, case)][0] for case in range(15)}
NOENT = opened[3]                                        # disc0:/NOPE.BIN
assert NOENT >> 16 == 0x8001 and opened[3] == NOENT
assert opened[0] == opened[1] == opened[2] == 1          # the three files
assert opened[4] == opened[10] == opened[11] == 1        # the root and a directory open, with or without a slash
assert opened[13] == 1                                   # disc0:ALPHA.BIN, no slash
assert opened[6] == 1 and opened[14] == 1                # umd1: is the device; umd1:/ALPHA.BIN is the filesystem
assert opened[5] == opened[7] == opened[12] == 1         # and so is umd0:, for a program booted from the disc
assert opened[8] == NOENT                                # disc0:/alpha.bin: names are compared exactly
assert opened[9] == NOENT                                # disc0:/ALPHA.BIN;1: the version is not part of the name

# --- stat blocks --------------------------------------------------------------
def stat(case): return rows[(1, case)]
alpha, gamma, beta, nope, disc_root, sub, sub_slash, device = (stat(c) for c in (0, 1, 2, 3, 4, 10, 11, 6))
assert nope[0] == NOENT and all(w == STAT_FILL for w in nope[1:])   # a failed stat writes nothing
FILE_MODE, FILE_ATTR = alpha[1], alpha[2]
DIR_MODE, DIR_ATTR = sub[1], sub[2]
assert FILE_MODE >> 12 == 2 and DIR_MODE >> 12 == 1 and (FILE_MODE ^ DIR_MODE) == 0x3000
assert (FILE_MODE & 0o777) == (DIR_MODE & 0o777)
TIME = alpha[5:9]
PRIVATE_FILL = alpha[18]
for entry, key in ((alpha, 'ALPHA'), (gamma, 'GAMMA'), (beta, 'BETA'), (sub, 'SUB'), (sub_slash, 'SUB')):
    _, _, sector, size, flags = LAYOUT[key]
    assert entry[0] == 0 and entry[3] == size and entry[4] == 0, key
    assert (entry[1], entry[2]) == ((DIR_MODE, DIR_ATTR) if flags else (FILE_MODE, FILE_ATTR)), key
    assert entry[5:9] == entry[9:13] == entry[13:17] == TIME, key       # ctime, atime, mtime alike
    assert entry[17] == sector, key                                     # st_private[0] is the start sector
    assert entry[18:23] == [PRIVATE_FILL] * 5, key
assert TIME[0] & 0xffff == 1900 and TIME[0] >> 16 == 1 and TIME[1:] == [0, 0, 0]   # not the record's 2026 date
assert disc_root[:5] == [0, DIR_MODE, DIR_ATTR, 0, 0] and disc_root[17] == 0        # the root: no size, no sector
assert device[:5] == [0, FILE_MODE, FILE_ATTR, TOTAL_SECTORS, 0] and device[17] == 0   # umd1:: a file sized in sectors
assert device[5:9] == TIME and device[18:23] == [PRIVATE_FILL] * 5
assert stat(5) == device                                 # umd0: reports the same block

# --- file reads and seeks -----------------------------------------------------
def read(case): return rows[(2, case)]
def seek(case): return rows[(3, case)]
assert read(0) == [16] + file_words('ALPHA', 0, 4)
assert seek(0) == [SECTOR, 0] and read(1) == [8] + file_words('ALPHA', SECTOR, 2)   # byte SECTOR is the next sector's first word
assert seek(1) == [SECTOR + 4, 0] and read(2) == [4] + file_words('ALPHA', SECTOR + 4, 1)
assert seek(2) == [8 * SECTOR, 0] and read(3) == [0, FILL]                         # SEEK_END is the file's size; nothing past it
assert seek(3) == [8 * SECTOR - 8] and read(4) == [8] + file_words('ALPHA', 8 * SECTOR - 8, 2)
assert seek(4) == [7]
unaligned = content(22, 8 * SECTOR)[7:15]
assert read(5) == [8] + list(struct.unpack('<2I', unaligned))                      # byte-granular positions
assert read(6) == [100] + file_words('BETA', 0, 3)                                 # 128 asked of 100
assert read(7) == [3 * SECTOR + 7] + file_words('GAMMA', 0, 2) and read(8) == [0, FILL]
BADF = read(9)[0]
assert BADF >> 16 == 0x8002 and read(10) == [BADF]                                 # a closed and a bad descriptor
assert read(11) == [16, word(21, 0)] and seek(5) == [SECTOR, 0] and seek(6) == [0]   # a directory reads its own records

# --- the raw device -----------------------------------------------------------
def raw(case): return rows[(4, case)]
for base in (32, 48, 64):                                # umd0:, umd1:, and umd0: again once the drive is activated
    assert raw(base) == [1]
    assert raw(base + 1) == [22, 0] and read(base + 2) == [8, word(22, 0), word(22, 1), word(22, 2)]   # positions are sectors
    assert raw(base + 3) == [30, 0]                      # a read of 8 advanced 8 sectors: counts are sectors too
    assert raw(base + 4) == [30] and read(base + 5) == [4, word(30, 0), word(30, 1)]
    assert raw(base + 6) == [0, 0] and read(base + 7) == [4, word(0, 0), word(0, 1)]
    assert raw(base + 8) == [100, 0]                     # a seek past the image is not clamped
    assert read(base + 9) == [4, FILL]                   # and a read there answers the count, writing nothing
    assert raw(base + 10) == [TOTAL_SECTORS, 0]          # SEEK_END is the image size in sectors

# --- asynchronous operations --------------------------------------------------
def a(case): return rows[(5, case)]
assert a(0) == [0] and a(1) == [0, 4096, 0]              # a queued read; its result is the byte count
assert a(2) == [1, word(22, 0)]                          # at least one poll found it running (1)
NO_ASYNC = a(3)[0]
assert NO_ASYNC >> 16 == 0x8002 and a(3) == a(4) == a(5) == [NO_ASYNC, 0xffffffff, 0xffffffff]   # nothing pending: poll, wait, GetAsyncStat
assert a(6) == [0]
BUSY = a(7)[0]
assert BUSY >> 16 == 0x8002 and BUSY not in (NO_ASYNC, BADF)   # a second operation before the first is collected
assert a(8) == [0, 2048, 0] and a(9) == [NO_ASYNC, 0xffffffff, 0xffffffff]   # the refused one never ran
assert a(10) == [0] and a(11) == [0, 4096, 0]            # an async seek's result is the position
assert a(12) == [4, word(24, 0)]                         # and it moved the descriptor
assert a(13) == [0] and a(14) == [0, 0, 0] and a(15) == [BADF, 0xffffffff, 0xffffffff]   # close, collected, gone
assert a(16) == [1] and a(17) == [0, 1, 0]               # an async open's result is its descriptor
assert a(18) == [1] and a(19) == [0, NOENT, 0xffffffff]  # a failed one still has a descriptor; the error, sign-extended
assert a(21) == [BADF] and a(22) == [BADF, 0xffffffff, 0xffffffff]   # collecting the error released it
assert a(20) == [BADF, 0xffffffff, 0xffffffff]

# --- directory listings -------------------------------------------------------
def name(words): return b''.join(struct.pack('<I', w) for w in words).split(b'\0')[0].decode()
def entry(words): return (name(words[5:8]), words[1], words[2], words[3], words[4], words[8])
def listing(case, expected):
    assert rows[(6, case)] == [1]
    for n, key in enumerate(expected):
        ident, _, sector, size, flags = LAYOUT[key]
        mode, attr = (DIR_MODE, DIR_ATTR) if flags else (FILE_MODE, FILE_ATTR)
        assert rows[(7, case * 16 + n)][0] == 1
        assert entry(rows[(7, case * 16 + n)]) == (ident, mode, attr, size, sector, TIME[0]), key
    end = rows[(7, case * 16 + len(expected))]
    assert end[0] == 0 and end[5] == (STAT_FILL & 0xffffff00)      # the end writes only the name's first byte
    assert all(w == STAT_FILL for w in end[1:5] + end[6:])
    assert rows[(6, case + 8)] == [0]
listing(0, ['SUB', 'ALPHA', 'BETA', 'PSP_GAME'])         # stored order; no "." or ".." entries
listing(1, ['GAMMA'])
assert rows[(6, 2)] == [NOENT]
listing(3, [])                                           # a file opens as an empty listing

# --- drive activation ---------------------------------------------------------
def act(case): return rows[(8, case)]
ACTIVE, INACTIVE = act(0)[0], act(5)[0]                  # the drive state before and after deactivation
assert act(1) == [0] and act(2) == [0] and act(3) == [ACTIVE] and act(4) == [0]
assert ACTIVE != INACTIVE and act(6) == [0]
assert all(act(16 + case) == [opened[case]] for case in opened)   # activation changes no name's answer
assert all(act(48 + case) == [opened[case]] for case in opened)   # nor does deactivation
assert act(8) == [1]                                     # umd0: after activating again; act(7) is that wait's result

lines = ['/* Generated by tests/provenance/disc/derive.py from our probe stdout. */',
         f'#define IO_DISC_SECTOR {SECTOR}',
         f'#define IO_DISC_FILE_MODE 0x{FILE_MODE:x}',
         f'#define IO_DISC_DIR_MODE 0x{DIR_MODE:x}',
         f'#define IO_DISC_FILE_ATTR 0x{FILE_ATTR:x}',
         f'#define IO_DISC_DIR_ATTR 0x{DIR_ATTR:x}',
         '/* Every disc entry reports the same times: year 1900, month 1, the rest zero. */',
         f'#define IO_DISC_TIME_WORD0 0x{TIME[0]:08x}',
         f'#define IO_DISC_PRIVATE_FILL 0x{PRIVATE_FILL:08x}',
         f'#define IO_ERROR_NOENT 0x{NOENT:08x}',
         f'#define IO_ERROR_BADF 0x{BADF:08x}',
         f'#define IO_ERROR_NO_ASYNC 0x{NO_ASYNC:08x}',
         f'#define IO_ERROR_ASYNC_BUSY 0x{BUSY:08x}']
(root.parents[2] / 'src/hle/io_observed.h').write_text('\n'.join(lines) + '\n')
print('\n'.join(lines[1:]))
print(f'{len(rows)} records checked')
