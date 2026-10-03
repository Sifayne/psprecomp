#!/usr/bin/env python3
"""geprobe 23 (scene 143): vertex alpha, and 16-bit vertex colours in through mode.

Set 23 (morph22.py) read 16-bit vertex colours in 3D (each field widened by repeating
its top bits) and morphed colours (the accumulator, floored with the sign dropped,
clamped), but only red, green and blue: a frame's alpha byte is the stencil, so a
vertex's alpha never showed. psprecomp assumes 5650 opaque, 5551's bit 0 or 255,
4444's nibble repeated, a morphed alpha as the other channels, and through mode as 3D.
This scene reads the alpha whole with colour14's alpha-test ladder (the LADDER op:
drawn once, then 255 passes of alpha test GEQUAL r with stencil INCR, so the alpha byte
ends as the alpha), and the colour channels as they are:

  through   through-mode points in 5650, 5551, 4444 and 8888, every field random
  plain     the same in 3D (one set)
  morph     8888, 5551 and 4444 points morphed over 2 to 4 sets, the weights summing to
            one, to more, to less than zero, and a hair from a half code

3D points go through an identity projection to their own pixels (a morphed point's
first set carries its position over the first weight, the others none). Each LADDER
draws up to 100 points with one weight set.

    alpha23.py emit [OUT.inc]        write c23a_data.inc
    alpha23.py sums DUMPDIR          the logged CRCs against this stream
    alpha23.py compare DUMPDIR       channels matching psprecomp's rules, per batch
    alpha23.py check                 design checks

Set 24 (fw 6.60): the frame is psprecomp's, every channel of every point: the alphas were
as assumed.
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
import morph22 as M
from colour14 import XS, f32, bf
from lights17 import f24

GU_POINTS = 0
GU_VERTEX_32BITF, GU_TRANSFORM_2D = 3 << 7, 1 << 23
def GU_VERTICES(n): return ((n - 1) & 7) << 18
ID4 = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
CLEAR = 0x00000000

def record(fmt, fields, pos):
    """One vertex set: colour (2 or 4 bytes, aligned), float position."""
    if fmt == '8888': b = struct.pack('<I', fields[0] | fields[1] << 8 | fields[2] << 16 | fields[3] << 24)
    else: b = struct.pack('<H', M.pack16(fmt, fields)) + b'\0\0'
    return b + struct.pack('<3f', *pos)

def slot(i): return 2 + 2 * (i % 238), 2 + 2 * (i // 238)
def model_pos(px, py): return ((px + 0.5 - 240) / 240, (136 - py - 0.5) / 136, 0.0)

def weights(r, kind, k):
    if kind == 'sum1':
        ws = [(r() % 65536) / 65536 for _ in range(k - 1)]; ws = [w / max(1.0, sum(ws)) for w in ws]; ws.append(1.0 - sum(ws))
    elif kind == 'over': ws = [0.3 + (r() % 1000) / 1000 for _ in range(k)]
    elif kind == 'neg': ws = [1.0 + (r() % 1000) / 1000] + [-(r() % 1000) / 1000 for _ in range(k - 1)]
    else:
        base = (0.5, 0.25, 0.75)[r() % 3]; d = ((r() % 9) - 4) * 2.0 ** -(14 + r() % 3)
        ws = [1.0 - base - d, base + d]
    return [f24(w) for w in ws]

def batches():
    """[(batch name, mode 'through' | '3d', fmt, morph weights or None, [fields per set per point])]"""
    r = XS(0x14300001)
    out = []
    for fmt in ('5650', '5551', '4444', '8888'):
        for _ in range(2): out.append(('through', 'through', fmt, None, [[M.rfields(r, fmt)] for _ in range(100)]))
    for fmt in ('5650', '5551', '4444', '8888'):
        out.append(('plain', '3d', fmt, None, [[M.rfields(r, fmt)] for _ in range(100)]))
    for fmt in ('8888', '5551', '4444'):
        for kind in ('sum1', 'over', 'neg', 'edge'):
            for _ in range(3 if kind != 'edge' else 4):
                k = 2 if kind == 'edge' else 2 + r() % 3
                ws = weights(r, kind, k)
                if abs(ws[0]) < 0.2: ws[0] = f24(0.5)
                out.append(('morph-' + kind, '3d', fmt, ws, [[M.rfields(r, fmt) for _ in range(k)] for _ in range(40)]))
    return out

_B = None
def all_batches():
    global _B
    if _B is None: _B = batches()
    return _B

def points():
    """[(batch index, point index in it, slot)] in drawing order."""
    out = []; i = 0
    for bi, b in enumerate(all_batches()):
        for pi in range(len(b[4])): out.append((bi, pi, i)); i += 1
    return out

def scene_ops():
    ops = [('BEGIN', 3, CLEAR), ('MATS', ID4, ID4, ID4)]
    i = 0
    for name, mode, fmt, ws, pts in all_batches():
        if ws: ops.append(('MORPH', tuple(ws) + (0.0,) * (8 - len(ws))))
        vb = b''
        for sets in pts:
            px, py = slot(i); i += 1
            if mode == 'through':
                vb += record(fmt, sets[0], (px + 0.5, py + 0.5, 0.0))
            else:
                p = model_pos(px, py)
                if ws:
                    p0 = tuple(f32(c / ws[0]) for c in p)
                    vb += b''.join(record(fmt, s, p0 if k == 0 else (0.0, 0.0, 0.0)) for k, s in enumerate(sets))
                else:
                    vb += record(fmt, sets[0], tuple(f32(c) for c in p))
        vt = M.COLFMT[fmt] << 2 | GU_VERTEX_32BITF | (GU_TRANSFORM_2D if mode == 'through' else 0)
        if ws: vt |= GU_VERTICES(len(ws))
        ops.append(('LADDER', GU_POINTS, vt, len(pts), vb, 1, 255))
        if ws: ops.append(('MORPH', (1.0,) + (0.0,) * 7))
    ops.append(('END', 0))
    return ops

SCENES = [(143, 'valpha', 'vertex alpha through an alpha-test ladder, 16-bit colours in through mode')]

def emit_scene():
    OP = C.OP
    w = []; vcrc = 0; nd = 0; nv = 0
    def op(name, args):
        w.append(OP[name] << 24 | len(args)); w.extend(a & 0xFFFFFFFF for a in args)
    body = []
    for o in scene_ops():
        k = o[0]
        if k == 'BEGIN': body.append(('BEGIN', [o[1], o[2]]))
        elif k == 'END': body.append(('END', [o[1]]))
        elif k == 'MATS': body.append(('MATS', [7] + C.mem16(o[1]) + C.mem16(o[2]) + C.mem16(o[3])))
        elif k == 'MORPH': body.append(('MORPH', [bf(x) for x in o[1]]))
        elif k == 'LADDER':
            prim, vtype, cnt, vb, r0, r1 = o[1:7]
            vcrc = zlib.crc32(vb, vcrc); nd += 1; nv += cnt
            body.append(('LADDER', [prim, vtype, cnt, len(vb), r0, r1] + C.words(vb)))
        else: raise ValueError(k)
    op(*body[0])
    op('SUMS', [nd, nv, vcrc & 0xFFFFFFFF])
    for b in body[1:]: op(*b)
    return w, nd, nv, vcrc & 0xFFFFFFFF

def emit(path):
    out = ['/* Generated by alpha23.py emit: geprobe 23 scene 143 (vertex alpha) as a command',
           ' * stream for c14_run() (colour14.py\'s format).  Do not edit; change alpha23.py and',
           ' * run it again. */']
    n, name, title = SCENES[0]
    w, nd, nv, vc = emit_scene()
    crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
    out.append(f'\n/* scene {n} {name}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
               f' {len(w)} words, stream crc {crc:08X} */')
    out.append(f'static const w32 C23A_S0[{len(w)}] = {{')
    for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
    out.append('};')
    out.append('\nstatic const struct c14_step C23A_STEPS[] = {')
    out.append(f'    {{ {n}, "{name}", "{title}", C23A_S0, {len(w)} }},')
    out.append('};\n#define C23A_NSTEPS ((int)(sizeof C23A_STEPS / sizeof C23A_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text: open(path, 'w').write(text)
    return [(dict(num=n, name=name, title=title), len(w), nd, nv, vc, crc)]

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for sc, n, nd, nv, vc, crc in emit(os.path.join(td, 'c23a.inc')):
            m = re.search(rf'scene {sc["num"]}: .*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want; ok &= good
            print(f'scene {sc["num"]} {sc["name"]}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

def predict(name, fmt, ws, sets):
    """psprecomp's channels (r, g, b, a): widened fields; morphed, the accumulator floored
    with the sign dropped and clamped."""
    if not ws: return M.colour_value(fmt, sets[0])
    out = []
    for ch in range(4):
        acc = 0.0
        for mw, s in zip(ws, sets): acc = M.ge_acc(acc, M.c16(mw) * M.colour_value(fmt, s)[ch])
        out.append(min(255, math.floor(abs(acc))))
    return out

def compare(dumpdir):
    F = np.fromfile(os.path.join(dumpdir, 'ge_143_valpha.raw'), '<u4').reshape(272, 480)
    B = all_batches()
    res = {}
    for bi, pi, i in points():
        name, mode, fmt, ws, pts = B[bi]
        x, y = slot(i); c = int(F[y, x])
        got = [(c >> (8 * k)) & 0xFF for k in range(4)]
        want = predict(name, fmt, ws, pts[pi])
        key = (name, mode, fmt)
        st = res.setdefault(key, [0, 0, 0])
        st[0] += 1; st[1] += sum(got[k] == want[k] for k in range(3)); st[2] += got[3] == want[3]
    for key, (n, rgb, a) in sorted(res.items()):
        print(f'  {key[0]:12s} {key[1]:8s} {key[2]}: {n} points, rgb {rgb}/{3 * n}, alpha {a}/{n}')

def check():
    B = all_batches()
    print(f'143: {len(B)} ladders, {len(points())} points, last slot {slot(len(points()) - 1)}')
    for name, mode, fmt, ws, pts in B[:3]: print('  ', name, mode, fmt, ws, len(pts))

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        for sc, n, nd, nv, vc, crc in emit(argv[2] if len(argv) > 2 else os.path.join(HERE, 'c23a_data.inc')):
            print(f'scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'compare': compare(argv[2]); return 0
    if cmd == 'check': check(); return 0
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
