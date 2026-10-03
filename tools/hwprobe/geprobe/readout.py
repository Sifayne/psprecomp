#!/usr/bin/env python3
"""Inputs and readings of geprobe 12's readout scenes (62-66).

The scenes build every input from integer bit patterns (main.c rd_*); this is
the same generator in Python, so each point's inputs are known exactly. Each
scene's log line "N points, eye z crc C" checks the copy.

    readout.py <geprobe dir>            check the CRCs, list each scene's points
    readout.py <geprobe dir> <scene>    print slot, batch, z, w and depth per point
                                        (scenes 67-82: patch13.py's per-point listing)

A point's depth in these scenes is floor(zs * clip z / clip w) with zs a
power of two (zc 0): the quotient's top 16 bits as the GE has them.
"""
import re, struct, sys, zlib
import numpy as np

f32 = np.float32
f32 = np.float32
class R:
    def __init__(s, seed): s.x = seed
    def __call__(s):
        x = s.x
        x ^= (x << 13) & 0xFFFFFFFF; x ^= x >> 17; x ^= (x << 5) & 0xFFFFFFFF
        s.x = x; return x
def fb(bits): return struct.unpack('<f', struct.pack('<I', bits & 0xFFFFFFFF))[0]
def bf(f): return struct.unpack('<I', struct.pack('<f', float(f32(f))))[0]
def mk(s, e, m): return fb(((s & 1) << 31) | ((e + 127) << 23) | (m & 0x7FFFFF))
def mk16(s, e, m15): return mk(s, e, (m15 & 0x7FFF) << 8)
def m24(r):
    while True:
        m = r() & 0x7FFFFF
        if m: return m
def w64():
    r = R(0x64000001); return [mk(0, 1, m24(r)) for _ in range(1024)]
class Scene:
    def __init__(s): s.pts = []; s.batch = {}
    def at(s, b, z, **kw): s.pts.append(dict(batch=b, z=float(f32(z)), **kw))
    def crc(s): return zlib.crc32(b''.join(struct.pack('<I', bf(p['z'])) for p in s.pts))
def s62():
    W = w64(); S = Scene(); r = R(0x62000001)
    S.batch[0] = dict(c=1.0, zs=65536.0)
    for j in range(1, 128): w = mk(0, 0, j << 16); S.at(0, -w, w=w)
    for _ in range(4096):
        while True:
            m = (r() & 0x7FFF) << 8
            if m: break
        w = mk(0, 0, m); S.at(0, -w, w=w)
    for _ in range(2048): w = mk(0, 0, m24(r)); S.at(0, -w, w=w)
    for j in range(256): w = mk(0, 0, 0x400000 + j); S.at(0, -w, w=w)
    for j in range(256): w = mk(0, 0, 0x155500 + j); S.at(0, -w, w=w)
    for b, e in enumerate([-3, -1, 1, 2, 4, 7, 10, 13]):
        S.batch[1 + b] = dict(c=mk(0, e, 0), zs=65536.0)
        if e == 1:
            for w in W: S.at(1 + b, -w, w=w)
        else:
            for _ in range(192): w = mk(0, e, m24(r)); S.at(1 + b, -w, w=w)
    return S
def s63():
    S = Scene(); r = R(0x63000001); b = 0
    S.batch[b] = dict(cx=0, cy=0, a=1.0, c=0.0, zs=65536.0)
    for _ in range(1024): S.at(b, mk(0, -1, r()))
    b += 1
    for de in [-2, -9, -16, -21]:
        S.batch[b] = dict(cx=0, cy=0, a=1.0, c=-1.0, zs=mk(0, 15 - de, 0))
        lo = 1 << (23 + de)
        for _ in range(256): S.at(b, mk(0, 0, lo | (r() & (lo - 1))))
        b += 1
    for q in range(8):
        S.batch[b] = dict(cx=0, cy=0, a=mk16(0, 0, r() & 0x1FFF), c=0.0, zs=65536.0)
        for j in range(256):
            x = r(); S.at(b, mk(0, -1, ((x & 0x3FFF) << 8) if j < 128 else (x & 0x3FFFFF)))
        b += 1
    for q in range(12):
        g = [4, 7, 11, 15, 17, 19][q >> 1]
        S.batch[b] = dict(cx=0, cy=0, a=1.0, c=mk16(0, -1, 0x2000 | (r() & 0x1FFF)), zs=65536.0)
        for _ in range(128): S.at(b, mk(q & 1, -g, r()))
        b += 1
    S.batch[b] = dict(cx=0, cy=0, a=1.0, c=mk16(0, -2, 0x6000 | (r() & 0x1FFF)), zs=65536.0)
    for _ in range(128): S.at(b, mk(0, -4, r()))
    b += 1
    mb = (0x4000 | (r() & 0x3FFF)) << 8
    S.batch[b] = dict(cx=0, cy=0, a=1.0, c=mk(0, 0, mb), zs=1048576.0)
    for _ in range(128): S.at(b, mk(1, 0, mb - ((1 << 18) | (r() & 0x3FFFF))))
    b += 1
    for q, (ex, ey, ea, ez) in enumerate([(-6, -8, 0, -5), (-4, -10, -2, -4), (-13, -5, 0, -6), (-7, -7, 1, -7)]):
        x = r(); cx = mk16(x & 1, ex, x >> 1)
        x = r(); cy = mk16(x & 1, ey, x >> 1)
        x = r(); a = mk16(0, ea, x)
        x = r(); c = mk16(0, -1, 0x1000 + (x & 0x1FFF))
        S.batch[b] = dict(cx=cx, cy=cy, a=a, c=c, zs=65536.0)
        for _ in range(512): S.at(b, mk(0, ez, r()))
        b += 1
    return S
def s64():
    W = w64(); S = Scene(); r = R(0x64000002)
    for q in range(4):
        S.batch[q] = dict(c=mk16(0, 0, 0x4000 | (r() & 0x3FFF)), zs=65536.0)
        for w in W: S.at(q, -w, w=w)
    return S
def s65():
    S = Scene(); r = R(0x65000001)
    def F(x): return float(f32(x))
    a = mk16(0, 0, r() & 0x1FFF); T = mk16(0, 5, 0x2000 | (r() & 0x1FFF))
    S.batch[0] = dict(a=a, b=0.0, v=1.0, u=0.0, rx=0.0, s=1.0, t=T, zs=65536.0)
    for _ in range(512): d = mk(0, -1, r() & 0x3FFFC0); S.at(0, -(f32(T) - f32(d)))
    a = mk16(0, 0, r() & 0x1FFF); T = mk16(0, 5, 0x2000 | (r() & 0x1FFF)); U = mk16(1, 4, r() & 0x3FFF)
    S.batch[1] = dict(a=a, b=0.0, v=1.0, u=U, rx=0.0, s=1.0, t=T, zs=65536.0)
    for _ in range(512): d = mk(0, -1, r() & 0x3FFFE0); S.at(1, -((f32(T) + f32(U)) - f32(d)))
    a = mk16(0, 0, r() & 0xFFF); v = mk16(0, -1, r() & 0xFFF); s = mk16(0, 0, r() & 0xFFF)
    S.batch[2] = dict(a=a, b=0.0, v=v, u=0.0, rx=0.0, s=s, t=0.0, zs=65536.0)
    for _ in range(512): S.at(2, mk(0, 0, r() & 0x1FFFFF))
    a = mk16(0, 0, r() & 0xFFF); x = r(); rx = mk16(x & 1, -5, x >> 1)
    S.batch[3] = dict(a=a, b=0.0, v=1.0, u=0.0, rx=rx, s=1.0, t=0.0, zs=65536.0)
    for _ in range(512): S.at(3, mk(0, -1, r() & 0x1FFFFF))
    a = mk16(0, 0, r() & 0x3FF); T = mk16(0, 3, r() & 0xFFF)
    S.batch[4] = dict(a=a, b=-T, v=1.0, u=0.0, rx=0.0, s=1.0, t=T, zs=65536.0)
    for _ in range(512): S.at(4, mk(0, -1, r() & 0x1FFFFF))
    a = mk16(0, 2, r() & 0x1FFF); T = mk16(0, -2, r())
    S.batch[5] = dict(a=a, b=0.0, v=1.0, u=0.0, rx=0.0, s=1.0, t=T, zs=65536.0)
    for _ in range(512): d = mk16(0, -3, r() & 0x3FFF); S.at(5, -(f32(T) - f32(d)))
    return S
def s66():
    S = Scene(); r = R(0x66000001)
    S.batch[0] = dict(zs=65536.0); S.batch[1] = dict(zs=-65536.0)
    for e in range(2, 7):
        for _ in range(192 if e < 6 else 256):
            w = mk(0, e, r() if e < 6 else r() & 0x3FFFFF); S.at(0, -w, w=w)
    for _ in range(1024):
        w = mk(0, 0, 0x20000 + (r() & 0x1FFFFF)); S.at(1, -w, w=w)
    return S
SCENES = {62: (s62, 'rcpread'), 63: (s63, 'dotread'), 64: (s64, 'mulread'), 65: (s65, 'matread'), 66: (s66, 'perspread')}


def slot_xy(i):
    """Slot i's ndc x and y (exact): two pixels apart, 240 a row from row 10."""
    return (2 * (i % 240) + 0.5 - 240.0) / 256.0, (136.0 - (10 + 2 * (i // 240)) - 0.5) / 128.0


def depth_full(path):
    """A `_depthfull.bin` dump (512-pixel stride through the plain VRAM
    address) put back in screen order: render.c depth_addr's permutation."""
    raw = np.fromfile(path, dtype='<u2')
    y, x = np.mgrid[0:272, 0:480]
    l = 0x88000 + (y * 512 + x) * 2
    mid = (l >> 5) & 0x1F
    rot = ((mid << 1) | (mid >> 4)) & 0x1F
    p = ((l & ~(0x1F << 5)) | (rot << 5)) ^ 0x2040
    return raw[(p - 0x88000) // 2]


def read(base, sc, name):
    """{slot: (depth, batch)} for every point the scene drew."""
    col = np.fromfile(f'{base}/ge_{sc}_{name}.raw', dtype='<u4').reshape(272, 480) & 0xFFFFFF
    dep = depth_full(f'{base}/ge_{sc}_{name}_depthfull.bin')
    out = {}
    for y, x in zip(*np.nonzero(col)):
        c = int(col[y, x])
        out[(c & 0xFF) | ((c >> 8) & 0xFF) << 8] = (int(dep[y, x]), (c >> 16) - 0x40)
    return out


def main():
    base = sys.argv[1]
    if len(sys.argv) > 2 and int(sys.argv[2]) >= 99:     # geprobe 15's line-colour scenes
        import lines15
        lines15.sums(base)
        lines15.compare(base, [int(sys.argv[2])], None)
        return
    if len(sys.argv) > 2 and int(sys.argv[2]) >= 83:     # geprobe 14's colour-plane scenes
        import colour14
        colour14.sums(base)
        colour14.compare(base, [int(sys.argv[2])], None)
        return
    if len(sys.argv) > 2 and int(sys.argv[2]) >= 67:     # geprobe 13's patch scenes
        import patch13
        patch13.points(base, int(sys.argv[2]))
        return
    log = open(f'{base}/geprobe.txt').read()
    want = [int(sys.argv[2])] if len(sys.argv) > 2 else list(SCENES)
    for sc in want:
        fn, name = SCENES[sc]
        S = fn()
        m = re.search(rf'scene {sc}:.*?\n\s+(\d+) points, eye z crc ([0-9A-F]+)', log)
        ok = m and int(m.group(1)) == len(S.pts) and int(m.group(2), 16) == S.crc()
        got = read(base, sc, name)
        print(f'scene {sc} {name}: {len(S.pts)} points, crc {"matches" if ok else "DIFFERS"}, '
              f'{len(got)} drawn')
        if len(sys.argv) > 2:
            for i, p in enumerate(S.pts):
                d = got.get(i, (None, None))[0]
                print(i, p['batch'], repr(p['z']), repr(p.get('w')), d)


if __name__ == '__main__':
    main()
