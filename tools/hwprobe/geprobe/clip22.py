#!/usr/bin/env python3
"""geprobe 22 (scene 141): points, lines and triangles past the clip volume.

Set 17 found that a point is drawn only when |clip x| and |clip y| are within w (every
point drawn at most 0.989 w, every one missing at least 1.005 w), with depth clamping
off; lines were never posed, nor anything with depth clamping on (GU_CLIP_PLANES), where
psprecomp draws points wherever they fall and clips lines only at the near plane. This
scene asks, in 120 cells of 32 x 34 pixels, each with its own viewport (10 pixels a
unit, centred in the cell), its own scissor (the cell), clip w 1 (or 4 in a few cells):
  points      x/w or y/w = 0.95 ... 1.05 on all four sides, including 1 exactly and the
              floats either side of it; z/w a hair either side of -1 and 1
  lines       from the middle to an end at 0.99 ... 150 w out in x, y or both; both
              ends out on one side; across the volume with both ends out; an end past
              z/w = +-1, and one behind the eye
  triangles   one, two or all three corners out by 1.01 ... 150 w; a corner past z
each cell drawn once with depth clamping off (the first 60 cells) and once on (the last
60). Coverage reads off the frame cell by cell.

The rules (pixels, per cell):
  cur     psprecomp: points culled past x/y with clamping off; lines and triangles
          drawn whole whatever x/y (guard band), culled whole past z with clamping off,
          and with it on clipped at the near plane
  cullxy  lines culled whole, as points are, when an end is past x/y
  clipxy  lines cut at the x/y planes

    clip22.py emit [OUT.inc]        write c22c_data.inc
    clip22.py sums DUMPDIR          the logged CRCs against this stream
    clip22.py compare DUMPDIR       per cell: drawn pixels against the rules
    clip22.py check                 design checks

Set 23 (fw 6.60) differs from 'cur' in nine cells: lines beyond x = w at both ends are not
drawn (clamping off or on), and with clamping on points are culled past x, y and z = w as
with it off. psprecomp now does so (ge.c emit_point_line) and matches every cell.
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
import patch13 as P13
import lines15 as L15
from colour14 import XS, f32, bf, FMT_CV3D, GU_TRIANGLES

GU_POINTS, GU_LINES = 0, 1
CW, CH = 32, 34
COLS, ROWS = 15, 8
SCALE = 10.0
WHITE = 0xFFFFFFFF
ID4 = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
def P_w(k): return [[k, 0, 0, 0], [0, k, 0, 0], [0, 0, k, 0], [0, 0, 0, k]]

def cell(k):
    """Cell k's scissor (x0, y0, x1, y1) and centre pixel."""
    i, j = k % COLS, k // COLS
    x0, y0 = CW * i, CH * j
    return (x0, y0, x0 + CW - 1, y0 + CH - 1), (x0 + CW // 2, y0 + CH // 2)

def nx(v): return f32(v)
ONE_UP, ONE_DN = struct.unpack('<f', struct.pack('<I', 0x3F800001))[0], struct.unpack('<f', struct.pack('<I', 0x3F7FFFFF))[0]
NEAR1 = [0.95, 0.99, 0.999, ONE_DN, 1.0, ONE_UP, 1.001, 1.01, 1.05]

def items():
    """[(kind, prim, [(x, y, z) clip coordinates with w 1], wscale, note)] for 60 cells."""
    out = []
    # points: 4 cells, one per side, nine values each in its own row (or column), and 2 for z
    for side in range(4):
        pts = []
        for n, v in enumerate(NEAR1):
            off = (n - 4) * 0.25
            if side == 0: pts.append((v, off, 0.0))
            elif side == 1: pts.append((-v, off, 0.0))
            elif side == 2: pts.append((off, v, 0.0))
            else: pts.append((off, -v, 0.0))
        out.append(('points', GU_POINTS, pts, 1.0, f'side {side}'))
    for zs in (1, -1):
        pts = [(-1.0 + n * 0.25, 0.3 * zs, zs * z) for n, z in enumerate([0.99, ONE_DN, 1.0, ONE_UP, 1.0001, 1.001, 1.01, 1.1, 1.5])]
        out.append(('points', GU_POINTS, pts, 1.0, f'z {zs:+d}'))
    # lines from inside to an end out in x, y or both
    ends = [0.99, 1.0, 1.01, 1.05, 1.2, 1.5, 2.0, 3.0, 10.0, 150.0]
    dirs = [(1, 0.13), (-1, -0.21), (0.17, 1), (-0.09, -1), (1, 1)]
    for n, e in enumerate(ends):
        d = dirs[n % len(dirs)]
        m = max(abs(d[0]), abs(d[1]))
        out.append(('line', GU_LINES, [(-0.31, 0.27, 0.0), (d[0] / m * e, d[1] / m * e, 0.0)], 1.0, f'to {e}'))
    for n, e in enumerate([1.01, 1.5, 10.0, 150.0]):
        d = dirs[(n + 2) % len(dirs)]
        m = max(abs(d[0]), abs(d[1]))
        out.append(('line', GU_LINES, [(0.21, -0.33, 0.0), (d[0] / m * e, d[1] / m * e, 0.0)], 1.0, f'to {e}'))
    out.append(('line', GU_LINES, [(1.2, -0.5, 0.0), (1.8, 0.6, 0.0)], 1.0, 'both out, one side'))
    out.append(('line', GU_LINES, [(-1.5, -0.4, 0.0), (1.5, 0.5, 0.0)], 1.0, 'across, both out'))
    out.append(('line', GU_LINES, [(-0.3, -3.0, 0.0), (0.4, 3.0, 0.0)], 1.0, 'across in y'))
    out.append(('line', GU_LINES, [(-20.0, -0.2, 0.0), (20.0, 0.3, 0.0)], 1.0, 'across, far'))
    out.append(('line', GU_LINES, [(-1.3, -1.3, 0.0), (1.3, 1.2, 0.0)], 1.0, 'across a corner'))
    out.append(('line', GU_LINES, [(1.02, -0.6, 0.0), (1.02, 0.7, 0.0)], 1.0, 'just out, along x = 1.02'))
    out.append(('line', GU_LINES, [(0.98, -0.6, 0.0), (0.98, 0.7, 0.0)], 1.0, 'just in, along x = 0.98'))
    for z in (1.01, 1.5, -1.01, -1.5, -3.0):
        out.append(('line', GU_LINES, [(-0.5, -0.2, 0.0), (0.6, 0.3, z)], 1.0, f'an end at z {z}'))
    out.append(('line', GU_LINES, [(-0.5, 0.2, -1.2), (0.6, -0.3, 1.2)], 1.0, 'both ends past z'))
    out.append(('line', GU_LINES, [(-0.5, 0.4, 0.0), (1.5, -0.3, 0.0)], 4.0, 'w 4, to 1.5'))
    out.append(('line', GU_LINES, [(-0.4, -0.4, 0.0), (0.98, 0.5, 0.0)], 4.0, 'w 4, to 0.98'))
    # triangles
    tri_out = [1.01, 1.5, 3.0, 10.0, 150.0]
    for e in tri_out:
        out.append(('tri', GU_TRIANGLES, [(-0.6, -0.5, 0.0), (0.5, -0.6, 0.0), (e * 0.3, e, 0.0)], 1.0, f'a corner to y {e}'))
    for e in (1.2, 10.0, 150.0):
        out.append(('tri', GU_TRIANGLES, [(-0.6, -0.4, 0.0), (e, -0.2, 0.0), (0.2, e, 0.0)], 1.0, f'two corners to {e}'))
    for e in (1.5, 10.0, 150.0):
        out.append(('tri', GU_TRIANGLES, [(-e, -e, 0.0), (e * 1.2, -e * 0.3, 0.0), (-e * 0.2, e, 0.0)], 1.0, f'all three out {e}'))
    for z in (1.01, -1.01, -3.0):
        out.append(('tri', GU_TRIANGLES, [(-0.6, -0.5, 0.0), (0.5, -0.6, 0.0), (0.1, 0.7, z)], 1.0, f'a corner at z {z}'))
    out.append(('tri', GU_TRIANGLES, [(-0.6, -0.5, 0.0), (0.5, -0.6, 0.0), (1.4, 1.3, 0.0)], 4.0, 'w 4, to 1.4'))
    while len(out) < 60:
        r = len(out)
        e = 1.0 + (r % 7) * 0.37
        out.append(('line', GU_LINES, [(-0.2, 0.1 * (r % 5), 0.0), (e * (1 if r & 1 else -1), 0.7 - 0.1 * (r % 9), 0.0)], 1.0, f'to x {e:.2f}'))
    assert len(out) == 60, len(out)
    return out

def all_items():
    """120 (cell, clamp, item)."""
    its = items()
    return [(k, 0, its[k]) for k in range(60)] + [(60 + k, 1, its[k]) for k in range(60)]

def scene_ops():
    ops = [('BEGIN', 3, 0xFF000000)]
    for k, clamp, (kind, prim, pts, ws, note) in all_items():
        sc, (cx, cy) = cell(k)
        ops.append(('CLAMP', clamp))
        ops.append(('MATS', P_w(ws), ID4, ID4))
        ops.append(('VIEWPORT', 2048, 2048, int(2 * SCALE), int(2 * SCALE)))
        ops.append(('OFFSET', 2048 - cx, 2048 - cy))
        ops.append(('SCISSOR',) + sc)
        verts = [dict(x=f32(x), y=f32(y), z=f32(z), c=WHITE) for x, y, z in pts]
        ops.append(('DRAW', prim, FMT_CV3D, verts, None))
    ops += [('CLAMP', 0), ('SCISSOR', 0, 0, 479, 271), ('END', 0)]
    return ops

SCENES = [(141, 'clipxy', 'points, lines and triangles past x, y and z, clamping off and on')]

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
    out = ['/* Generated by clip22.py emit: geprobe 22 scene 141 (past the clip volume) as a',
           ' * command stream for c14_run() (colour14.py\'s format, with CLAMP).  Do not edit;',
           ' * change clip22.py and run it again. */']
    n, name, title = SCENES[0]
    w, nd, nv, vc = emit_scene()
    crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
    out.append(f'\n/* scene {n} {name}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
               f' {len(w)} words, stream crc {crc:08X} */')
    out.append(f'static const w32 C22C_S0[{len(w)}] = {{')
    for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
    out.append('};')
    out.append('\nstatic const struct c14_step C22C_STEPS[] = {')
    out.append(f'    {{ {n}, "{name}", "{title}", C22C_S0, {len(w)} }},')
    out.append('};\n#define C22C_NSTEPS ((int)(sizeof C22C_STEPS / sizeof C22C_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text: open(path, 'w').write(text)
    return [(dict(num=n, name=name, title=title), len(w), nd, nv, vc, crc)]

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for sc, n, nd, nv, vc, crc in emit(os.path.join(td, 'c22c.inc')):
            m = re.search(rf'scene {sc["num"]}: .*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want; ok &= good
            print(f'scene {sc["num"]} {sc["name"]}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

# =========================================================================== a mirror of the screen positions
def screen(k, x, y, z, ws):
    """Clip (ws x, ws y, ws z, ws) through 1/w and cell k's viewport: (x16, y16, inside z)."""
    sc, (cx, cy) = cell(k)
    cxw, cyw, w = ws * x, ws * y, ws
    nxv, nyv = P13.ge_over_w(cxw, w), P13.ge_over_w(cyw, w)
    def axis(n, s, off): return math.floor((P13.ge_sum([P13.ge_mul(s, n), P13.ge_mul(2048.0, 1.0)]) - off) * 16)
    return axis(nxv, SCALE, 2048 - cx), axis(nyv, -SCALE, 2048 - cy), abs(z) <= 1.0

def predict(k, clamp, item, rule='cur'):
    """The set of pixels drawn in cell k under rule (a rough mirror: exact positions,
    psprecomp's walks)."""
    kind, prim, pts, ws, note = item
    sc, _ = cell(k)
    out = set()
    S = [screen(k, x, y, z, ws) for x, y, z in pts]
    def clip(p, i): return P13.dot4(P_w(ws)[i], [p[0], p[1], p[2], 1.0])      # the GE's clip coordinate
    def inxy(p): return abs(clip(p, 0)) <= clip(p, 3) and abs(clip(p, 1)) <= clip(p, 3)
    if kind == 'points':
        for (x, y, z), s in zip(pts, S):
            if not clamp and (not inxy((x, y, z)) or abs(z) > 1.0): continue
            out.add((s[0] >> 4, s[1] >> 4))
    elif kind == 'line':
        if not clamp and any(abs(z) > 1.0 for _, _, z in pts): return out
        if clamp and all(z < -1.0 for _, _, z in pts): return out
        if rule == 'cullxy' and not all(inxy(p) for p in pts): return out
        a, b = (dict(x=s[0], y=s[1], rgba=WHITE, fog=255, spec=None, z=None) for s in S)
        r = L15.walk(a, b, LOPT)
        if r is not None:
            for x, y in zip(r[0], r[1]): out.add((int(x), int(y)))
    else:
        if not clamp and any(abs(z) > 1.0 for _, _, z in pts): return out
        V = [(s[0], s[1]) for s in S]
        a, b, c = V
        if (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) < 0: V = [a, c, b]
        X, Y = C.tri_raster(V, sc)
        out |= set(zip(map(int, X), map(int, Y)))
    return {p for p in out if sc[0] <= p[0] <= sc[2] and sc[1] <= p[1] <= sc[3]}

LOPT = L15.LineMirror('GE').lopt
def frame(dumpdir):
    return np.fromfile(os.path.join(dumpdir, 'ge_141_clipxy.raw'), '<u4').reshape(272, 480) & 0xFFFFFF

def compare(dumpdir):
    F = frame(dumpdir)
    for k, clamp, item in all_items():
        sc, _ = cell(k)
        hw = {(x, y) for y in range(sc[1], sc[3] + 1) for x in range(sc[0], sc[2] + 1) if F[y, x]}
        res = []
        for rule in ('cur', 'cullxy'):
            p = predict(k, clamp, item, rule)
            res.append(f'{rule} {len(hw ^ p)} off')
        print(f'cell {k:3d} clamp {clamp} {item[0]:6s} {item[4]:28s}: {len(hw)} pixels drawn; ' + ', '.join(res))

def check():
    n = 0
    for k, clamp, item in all_items():
        a, b = predict(k, clamp, item, 'cur'), predict(k, clamp, item, 'cullxy')
        n += a != b
    print(f'141: {len(all_items())} cells, {n} where cur and cullxy part')
    for k, clamp, item in all_items()[:60]:
        print(f'  cell {k:3d} {item[0]:6s} {item[4]:28s}: cur {len(predict(k, 0, item))} px clamp off, '
              f'{len(predict(k, 1, item))} on')

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        for sc, n, nd, nv, vc, crc in emit(argv[2] if len(argv) > 2 else os.path.join(HERE, 'c22c_data.inc')):
            print(f'scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'compare': compare(argv[2]); return 0
    if cmd == 'check': check(); return 0
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
