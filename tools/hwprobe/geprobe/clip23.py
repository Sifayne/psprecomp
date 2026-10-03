#!/usr/bin/env python3
"""geprobe 23 (scene 142): past the clip volume, the cases set 23 left.

Set 23 (clip22.py) found that a line is not drawn when both its ends lie beyond the
same x or y plane, with depth clamping off or on, and is drawn whole with an end
inside or across the volume; that points are culled past x, y and z = w with
clamping on too. Not posed: a line whose ends lie beyond different planes (one past
x = w, the other past y = w), which may pass through a corner of the volume or miss
it; triangles wholly beyond one plane, or beyond several without touching the volume;
ends past z = w at both ends, or past -w, with clamping on; ends on a plane exactly;
and clip w other than 1 at the two ends. Scene 142 asks them in clip22's 120 cells
(10 pixels a unit, the cell's scissor), each item drawn with clamping off (cells 0-59)
and on (60-119).

The rivals the frames are read against (compare lists each cell's pixels beside
psprecomp's own frame, which draws by the first):
  cur      psprecomp: a line culled when both ends lie beyond the same x or y plane;
           triangles drawn whatever x and y; with clamping off anything past z culled
  outcode  a line or triangle culled when all its corners lie beyond one plane (the same
           as cur for lines; for triangles new)
  anyout   a line culled when both ends lie outside the x/y square, whichever planes

    clip23.py emit [OUT.inc]        write c23c_data.inc
    clip23.py sums DUMPDIR          the logged CRCs against this stream
    clip23.py compare DUMPDIR [PCDIR]   per cell: pixels drawn, and against psprecomp's frame
    clip23.py check                 design checks
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
import patch13 as P13
import lines15 as L15
import clip22 as K
from colour14 import f32, bf, FMT_CV3D, GU_TRIANGLES
from clip22 import cell, CW, CH, COLS, ROWS, SCALE, WHITE, ID4, P_w, ONE_UP, ONE_DN

GU_POINTS, GU_LINES = 0, 1
PERSP = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 0, 0], [0, 0, -1, 0]]     # clip (X, Y, 0, -Z)

def L(a, b, note, P=None): return ('line', GU_LINES, [a, b], P, note)
def T(a, b, c, note, P=None): return ('tri', GU_TRIANGLES, [a, b, c], P, note)

def items():
    """60 items: (kind, prim, model corners, projection or None for w 1, note)."""
    z = 0.0
    out = [
        # lines whose ends lie beyond different planes
        L((1.5, 0.3, z), (0.3, 1.5, z), 'x+ to y+, through the corner'),
        L((1.5, 0.8, z), (0.8, 1.5, z), 'x+ to y+, past the corner (x+y 2.3)'),
        L((1.05, 0.98, z), (0.98, 1.05, z), 'x+ to y+, a hair past it (x+y 2.03)'),
        L((1.1, 0.0, z), (0.0, 1.1, z), 'x+ to y+, x+y 1.1'),
        L((1.2, -0.5, z), (-0.5, 1.2, z), 'x+ to y+, through the middle'),
        L((2.0, 0.5, z), (-0.5, -2.0, z), 'x+ to y-'),
        L((1.6, 0.9, z), (0.9, -1.6, z), 'x+ to y-, past the corner'),
        L((-1.5, 0.8, z), (0.8, 1.5, z), 'x- to y+, past the corner'),
        L((-1.4, -0.2, z), (0.3, -1.3, z), 'x- to y-, through'),
        L((-1.3, -0.9, z), (-0.9, -1.3, z), 'x- to y-, past the corner'),
        L((1.5, 1.5, z), (-1.5, -1.5, z), 'corner to corner, through'),
        L((1.5, 1.5, z), (0.5, 1.8, z), 'corner region to y+'),
        L((1.3, 1.2, z), (1.6, 1.4, z), 'both beyond x+ and y+'),
        L((3.0, 0.2, z), (0.2, 3.0, z), 'far x+ to far y+, through'),
        L((8.0, 3.0, z), (3.0, 8.0, z), 'far x+ to far y+, past'),
        # ends on a plane
        L((1.0, -0.5, z), (1.0, 0.5, z), 'both on x = w'),
        L((ONE_UP, -0.5, z), (ONE_UP, 0.5, z), 'both a float past x = w'),
        L((1.0, -0.5, z), (1.3, 0.5, z), 'one on x = w, one past'),
        L((-1.0, -0.6, z), (-1.0, 0.6, z), 'both on x = -w'),
        L((-0.6, 1.0, z), (0.6, 1.0, z), 'both on y = w'),
        # w other than 1: perspective, clip (X, Y, 0, -Z)
        L((2.2, 0.0, -2.0), (0.5, 0.2, -1.0), 'persp: x/w 1.1 to inside', PERSP),
        L((2.2, 0.0, -2.0), (1.2, 0.3, -1.0), 'persp: x/w 1.1 and 1.2', PERSP),
        L((2.2, 0.0, -2.0), (0.95, 0.3, -1.0), 'persp: x 2.2 > w 2, x 0.95 < w 1', PERSP),
        L((1.9, 0.0, -2.0), (1.2, 0.3, -1.0), 'persp: x 1.9 < w 2, x 1.2 > w 1', PERSP),
        # triangles wholly beyond planes
        T((1.2, -0.5, z), (1.6, 0.4, z), (1.3, 0.8, z), 'all beyond x+'),
        T((-1.2, -0.5, z), (-1.6, 0.4, z), (-1.3, 0.8, z), 'all beyond x-'),
        T((-0.5, 1.2, z), (0.4, 1.6, z), (0.8, 1.3, z), 'all beyond y+'),
        T((-0.5, -1.2, z), (0.4, -1.6, z), (0.8, -1.3, z), 'all beyond y-'),
        T((1.5, 0.6, z), (0.6, 1.5, z), (1.6, 1.6, z), 'x+ and y+, missing the corner'),
        T((1.5, 0.2, z), (0.2, 1.5, z), (1.6, 1.6, z), 'x+ and y+, over the corner'),
        T((1.4, -0.3, z), (1.5, 0.9, z), (0.7, 1.4, z), 'two x+, one y+, missing'),
        T((3.0, -0.5, z), (0.5, 3.0, z), (4.0, 4.0, z), 'far, missing (x+y >= 2.5)'),
        T((1.0, -0.5, z), (1.0, 0.5, z), (1.4, 0.0, z), 'two on x = w, one past'),
        T((1.01, -1.5, z), (1.01, 1.5, z), (2.0, 0.0, z), 'all just past x = w'),
        T((2.0, 0.0, -2.0), (2.4, 0.5, -2.0), (1.3, 0.2, -1.0), 'persp: all past x = w', PERSP),
        T((1.8, 0.0, -2.0), (2.4, 0.5, -2.0), (1.3, 0.2, -1.0), 'persp: one x/w 0.9', PERSP),
        # z planes (clip z = model z, w 1): both ends or all corners past one
        L((-0.5, -0.2, 1.2), (0.6, 0.3, 1.5), 'both ends past z = w'),
        L((-0.5, -0.2, -1.2), (0.6, 0.3, -1.5), 'both ends past z = -w'),
        L((-0.5, -0.2, 1.2), (0.6, 0.3, -1.5), 'one past z = w, one past -w'),
        L((-0.5, -0.2, 1.0), (0.6, 0.3, 1.0), 'both on z = w'),
        L((-0.5, -0.2, -1.0), (0.6, 0.3, -1.0), 'both on z = -w'),
        L((-0.5, -0.2, 1.2), (0.6, 0.3, 0.5), 'one past z = w'),
        T((-0.6, -0.5, 1.2), (0.5, -0.6, 1.3), (0.1, 0.7, 1.5), 'all past z = w'),
        T((-0.6, -0.5, -1.2), (0.5, -0.6, -1.3), (0.1, 0.7, -1.5), 'all past z = -w'),
        T((-0.6, -0.5, 1.2), (0.5, -0.6, -1.3), (0.1, 0.7, 0.0), 'one past each'),
        T((-0.6, -0.5, 1.2), (0.5, -0.6, 1.3), (0.1, 0.7, 0.5), 'two past z = w'),
        # points on and past the planes, in both senses
        ('points', GU_POINTS, [(1.0, -0.6, z), (-1.0, -0.4, z), (-0.6, 1.0, z), (-0.4, -1.0, z),
                               (ONE_UP, 0.2, z), (-ONE_UP, 0.4, z), (0.2, ONE_UP, z), (0.4, -ONE_UP, z),
                               (0.0, 0.0, 1.0), (0.3, 0.0, -1.0), (0.6, 0.0, 1.0001), (-0.6, 0.3, -1.0001)],
         None, 'points on and a float past every plane'),
    ]
    k = 0
    while len(out) < 60:                 # more x+/y+ lines either side of x + y = 2
        s = 1.9 + 0.05 * (k % 9)
        a = 1.02 + 0.1 * (k % 4)
        out.append(L((a, s - a, z), (s - a, a, z), f'x+ to y+, x+y {s:.2f}'))
        k += 1
    return out

def all_items():
    its = items()
    return [(k, 0, its[k]) for k in range(60)] + [(60 + k, 1, its[k]) for k in range(60)]

def scene_ops():
    ops = [('BEGIN', 3, 0xFF000000)]
    for k, clamp, (kind, prim, pts, P, note) in all_items():
        sc, (cx, cy) = cell(k)
        ops.append(('CLAMP', clamp))
        ops.append(('MATS', P or ID4, ID4, ID4))
        ops.append(('VIEWPORT', 2048, 2048, int(2 * SCALE), int(2 * SCALE)))
        ops.append(('OFFSET', 2048 - cx, 2048 - cy))
        ops.append(('SCISSOR',) + sc)
        verts = [dict(x=f32(x), y=f32(y), z=f32(z_), c=WHITE) for x, y, z_ in pts]
        ops.append(('DRAW', prim, FMT_CV3D, verts, None))
    ops += [('CLAMP', 0), ('SCISSOR', 0, 0, 479, 271), ('END', 0)]
    return ops

SCENES = [(142, 'clipmore', 'past the clip volume: ends beyond different planes, z both ways')]

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
        elif k == 'CLAMP': body.append(('CLAMP', [o[1]]))
        elif k == 'MATS': body.append(('MATS', [7] + C.mem16(o[1]) + C.mem16(o[2]) + C.mem16(o[3])))
        elif k == 'VIEWPORT': body.append(('VIEWPORT', list(o[1:5])))
        elif k == 'OFFSET': body.append(('OFFSET', [o[1], o[2]]))
        elif k == 'SCISSOR': body.append(('SCISSOR', list(o[1:5])))
        elif k == 'DRAW':
            prim, vtype, verts = o[1], o[2], o[3]
            vb = C.vbytes(vtype, verts)
            vcrc = zlib.crc32(vb, vcrc); nd += 1; nv += len(verts)
            body.append(('DRAW', [prim, vtype, len(verts), len(vb), 0] + C.words(vb)))
        else: raise ValueError(k)
    op(*body[0])
    op('SUMS', [nd, nv, vcrc & 0xFFFFFFFF])
    for b in body[1:]: op(*b)
    return w, nd, nv, vcrc & 0xFFFFFFFF

def emit(path):
    out = ['/* Generated by clip23.py emit: geprobe 23 scene 142 (past the clip volume) as a',
           ' * command stream for c14_run() (colour14.py\'s format).  Do not edit; change',
           ' * clip23.py and run it again. */']
    n, name, title = SCENES[0]
    w, nd, nv, vc = emit_scene()
    crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
    out.append(f'\n/* scene {n} {name}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
               f' {len(w)} words, stream crc {crc:08X} */')
    out.append(f'static const w32 C23C_S0[{len(w)}] = {{')
    for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
    out.append('};')
    out.append('\nstatic const struct c14_step C23C_STEPS[] = {')
    out.append(f'    {{ {n}, "{name}", "{title}", C23C_S0, {len(w)} }},')
    out.append('};\n#define C23C_NSTEPS ((int)(sizeof C23C_STEPS / sizeof C23C_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text: open(path, 'w').write(text)
    return [(dict(num=n, name=name, title=title), len(w), nd, nv, vc, crc)]

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for sc, n, nd, nv, vc, crc in emit(os.path.join(td, 'c23c.inc')):
            m = re.search(rf'scene {sc["num"]}: .*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want; ok &= good
            print(f'scene {sc["num"]} {sc["name"]}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

def frame(dumpdir):
    return np.fromfile(os.path.join(dumpdir, 'ge_142_clipmore.raw'), '<u4').reshape(272, 480) & 0xFFFFFF

def drawn(F, k):
    sc, _ = cell(k)
    return F[sc[1]:sc[3] + 1, sc[0]:sc[2] + 1] != 0

def compare(dumpdir, pcdir=None):
    H = frame(dumpdir)
    P = frame(pcdir) if pcdir else None
    for k, clamp, item in all_items():
        h = drawn(H, k)
        line = f'cell {k:3d} clamp {clamp} {item[0]:6s} {item[4]:40s}: {int(h.sum()):4d} pixels'
        if P is not None:
            p = drawn(P, k)
            line += f'; psprecomp {int(p.sum()):4d}, {int((h != p).sum())} differ'
        print(line)

def check():
    its = items()
    print(f'142: {len(its)} items x 2 clamping states, {len(all_items())} cells')
    for k, it in enumerate(its): print(f'  {k:2d} {it[0]:6s} {it[4]}')

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        for sc, n, nd, nv, vc, crc in emit(argv[2] if len(argv) > 2 else os.path.join(HERE, 'c23c_data.inc')):
            print(f'scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'compare': compare(argv[2], argv[3] if len(argv) > 3 else None); return 0
    if cmd == 'check': check(); return 0
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
