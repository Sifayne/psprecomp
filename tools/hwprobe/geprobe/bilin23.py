#!/usr/bin/env python3
"""geprobe 23 (scene 144): bilinear filtering at negative texture coordinates.

Set 23 (morph22.py) found a nearest texel to be the coordinate cut toward zero to a
sixteenth of a texel and then floored, so -61.004 texels reads texel -61. Bilinear
filtering quantises to sixteenths too (render.c sample_bilinear: floor((u - 1/2) 16)
and the weight its last four bits, from gpu/filtering's positive coordinates); at a
negative coordinate the cut toward zero may come first, a sixteenth off. Scene 144
draws 3D points textured with a 64 x 64 texture whose red is 255 on odd columns and
green 255 on odd rows (main.c c23_texb: linear, repeating, replacing), so a point's
red is the weight of its odd column and green that of its odd row, each a count of
sixteenths through the blend. Coordinates run over -120 .. 120 texels at every 1/64
of a texel, u and v independent; every point goes to clip (0, 0, 0, 1) through a zero
projection and its offset places it, as in 115-140.

The rules (texel_rgb):
  cur       psprecomp: f = floor((t - 1/2) 16), texel f >> 4, weight (f & 15) / 16
  cut       the coordinate cut toward zero to a sixteenth first, as nearest: f = trunc(16 t) - 8
  cutafter  f = trunc((t - 1/2) 16), toward zero after the half texel

    bilin23.py emit [OUT.inc]        write c23b_data.inc
    bilin23.py sums DUMPDIR          the logged CRCs against this stream
    bilin23.py compare DUMPDIR       readings matching each rule, by sign and fraction
    bilin23.py check                 design checks: where the rules part
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
import patch13 as P13
from colour14 import XS, f32, bf
from lights17 import GU_POINTS, P0, PITCH, ACROSS

CLEAR = 0xFF0000FF
ID4 = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
VTYPE = 3 | (7 << 2) | (3 << 7)            # float uv, 8888 colour, float position
SIZE = 64
def slot(i): return 1 + PITCH * (i % ACROSS), 1 + PITCH * (i // ACROSS)

INTS = [-120, -97, -64, -33, -18, -9, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 8, 17, 32, 63, 96, 119,
        -71, -50, -41, -26, -13, -7, -6, 6, 7, 12, 25, 38, 51, 70, 88, 101, 110]

def points():
    """[(u texels, v texels)]: every 1/64 fraction against every integer part, u and v
    paired so that each sign, parity and fraction meets the others."""
    out = []
    for n, I in enumerate(INTS):
        for k in range(64):
            J = INTS[(n * 7 + k * 3 + 5) % len(INTS)]
            kv = (k * 37 + n * 11 + 5) % 64
            out.append((I + k / 64, J + kv / 64))
    return out

def texels(t):
    """The coordinate the texture unit sees, in texels: the GE's u s + o (s 1, o 0) is u
    cut to 16 bits, times the size."""
    return P13.ge_sum([P13.ge_mul(t / SIZE, 1.0), P13.ge_mul(0.0, 1.0)]) * SIZE

def quant(rule, t):
    if rule == 'cur': return math.floor(f32(f32(f32(t) - 0.5) * 16.0) + 1e-3)
    if rule == 'cut': return math.trunc(t * 16) - 8
    if rule == 'cutafter': return math.trunc((t - 0.5) * 16)
    raise ValueError(rule)

def odd_weight(rule, t):
    """The weight (0..16 sixteenths) bilinear gives the odd texel along one axis."""
    f = quant(rule, texels(t))
    u0, a = f >> 4, f & 15
    return a if u0 % 2 == 0 else 16 - a

def channel(w16):
    """render.c sample_bilinear's blend of 0 and 255 at weight w16/16 on that texel (the
    other axis's weights sum to one), truncated."""
    a = f32(w16 / 16.0)
    return int(f32(f32(1.0 - a) * 0.0) + f32(a * 255.0))

def texel_rgb(rule, p):
    return channel(odd_weight(rule, p[0])), channel(odd_weight(rule, p[1]))
RULES = ['cur', 'cut', 'cutafter']

def scene_ops():
    ops = [('BEGIN', 3, CLEAR), ('MATS', P0, ID4, ID4), ('TEXB', 1, 1.0, 1.0, 0.0, 0.0)]
    for i, (u, v) in enumerate(points()):
        x, y = slot(i)
        ops.append(('OFFSET', 2048 - x, 2048 - y))
        vb = struct.pack('<2f', f32(u / SIZE), f32(v / SIZE)) + struct.pack('<I', 0xFFFFFFFF) + struct.pack('<3f', 0.0, 0.0, 0.0)
        ops.append(('RAW', GU_POINTS, VTYPE, 1, vb))
    ops += [('TEXB', 0, 1.0, 1.0, 0.0, 0.0), ('OFFSET', 2048 - 240, 2048 - 136), ('END', 0)]
    return ops

SCENES = [(144, 'bilneg', 'bilinear filtering at negative texture coordinates')]

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
        elif k == 'OFFSET': body.append(('OFFSET', [o[1], o[2]]))
        elif k == 'TEXB': body.append(('TEXB', [o[1]] + [bf(f32(x)) for x in o[2:6]]))
        elif k == 'RAW':
            prim, vtype, cnt, vb = o[1:5]
            vcrc = zlib.crc32(vb, vcrc); nd += 1; nv += cnt
            body.append(('DRAW', [prim, vtype, cnt, len(vb), 0] + C.words(vb)))
        else: raise ValueError(k)
    op(*body[0])
    op('SUMS', [nd, nv, vcrc & 0xFFFFFFFF])
    for b in body[1:]: op(*b)
    return w, nd, nv, vcrc & 0xFFFFFFFF

def emit(path):
    out = ['/* Generated by bilin23.py emit: geprobe 23 scene 144 (bilinear at negative',
           ' * coordinates) as a command stream for c14_run() (colour14.py\'s format, with TEXB).',
           ' * Do not edit; change bilin23.py and run it again. */']
    n, name, title = SCENES[0]
    w, nd, nv, vc = emit_scene()
    crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
    out.append(f'\n/* scene {n} {name}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
               f' {len(w)} words, stream crc {crc:08X} */')
    out.append(f'static const w32 C23B_S0[{len(w)}] = {{')
    for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
    out.append('};')
    out.append('\nstatic const struct c14_step C23B_STEPS[] = {')
    out.append(f'    {{ {n}, "{name}", "{title}", C23B_S0, {len(w)} }},')
    out.append('};\n#define C23B_NSTEPS ((int)(sizeof C23B_STEPS / sizeof C23B_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text: open(path, 'w').write(text)
    return [(dict(num=n, name=name, title=title), len(w), nd, nv, vc, crc)]

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for sc, n, nd, nv, vc, crc in emit(os.path.join(td, 'c23b.inc')):
            m = re.search(rf'scene {sc["num"]}: .*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want; ok &= good
            print(f'scene {sc["num"]} {sc["name"]}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

def compare(dumpdir):
    F = np.fromfile(os.path.join(dumpdir, 'ge_144_bilneg.raw'), '<u4').reshape(272, 480)
    res = {}
    for i, p in enumerate(points()):
        x, y = slot(i); c = int(F[y, x])
        got = (c & 0xFF, (c >> 8) & 0xFF)
        for a in (0, 1):
            key = 'negative' if p[a] < 0 else 'positive'
            st = res.setdefault(key, {r: 0 for r in RULES + ['n']})
            st['n'] += 1
            for r in RULES: st[r] += texel_rgb(r, p)[a] == got[a]
    for key, st in res.items(): print(f'  {key}: {st["n"]} readings; ' + ', '.join(f'{r} {st[r]}' for r in RULES))

def check():
    pts = points()
    print(f'144: {len(pts)} points, last slot {slot(len(pts) - 1)}')
    for r in RULES[1:]:
        d = sum(texel_rgb(r, p)[a] != texel_rgb('cur', p)[a] for p in pts for a in (0, 1))
        dn = sum(texel_rgb(r, p)[a] != texel_rgb('cur', p)[a] for p in pts for a in (0, 1) if p[a] < 0)
        print(f'  {r} parts from cur on {d} readings ({dn} negative)')

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        for sc, n, nd, nv, vc, crc in emit(argv[2] if len(argv) > 2 else os.path.join(HERE, 'c23b_data.inc')):
            print(f'scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'compare': compare(argv[2]); return 0
    if cmd == 'check': check(); return 0
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
