#!/usr/bin/env python3
"""geprobe 21 (scenes 130-133): the order a patch's lines go out in.

A patch drawn as lines (PATCHPRIMITIVE 1) goes out, per pair of sample rows, as the GE's
own strip would: (0,j) (0,j+1) (1,j) (1,j+1) ..., a zigzag of each column's rung and a
diagonal to the next (geprobe 2 scene 22). Which strip comes first, which way each runs,
and whether the GE emits the whole grid or each span's own, decide which of two segments
that share pixels near a sample is drawn last: scene 23's one pixel off is such a pixel.
These scenes draw the same nine patches four ways:
  130 plpoints   as points: every sample's own colour, where the mirror puts it
  131 plsmooth   as lines, smooth: what games see
  132 plflat     as lines, flat: each segment one colour, its second vertex's, so a pixel
                 two segments share names the last and which way it ran
  133 pladd      as lines, flat, every control point 0x202020 and additive blending:
                 each pixel's grey is 0x20 times how many segments drew it
One patch is scene 23's own (the 5 x 4 spline, open/open in u, at 8 x 8 divisions, under
the probes' perspective); the others are Bezier and spline patches of 4 to 7 control
points a side, every edge mode, divisions 1 to 8 and unequal, the controls jittered so
lines cross. The streams are colour14's format with new PATCH and BLEND ops.

    plines21.py emit [OUT.inc]        write c21p_data.inc
    plines21.py sums DUMPDIR          the logged CRCs against these streams
    plines21.py compare DUMPDIR       per patch and candidate order: pixels off in 131-133
    plines21.py check                 design checks
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
import patch13 as P13
import lines15 as L15
from colour14 import XS, FMT_CV3D, GU_FLAT, GU_SMOOTH, begin

GU_POINTS, GU_LINE_STRIP = 0, 2
GU_ADD, GU_FIX = 0, 10
PERSP = [[0.981491089, 0, 0, 0], [0, 1.73202515, 0, 0], [0, 0, -1.02017212, -2.0201416], [0, 0, -1, 0]]
ID = P13.ident()
SCALE = 47.1                                   # pixels per model unit at z = -5 (both axes)
CELLS = [(c, r) for r in range(3) for c in range(4)]

def model_of(px, py):
    """Model x, y that land near screen (px, py) at z = -5."""
    return (px - 240) / SCALE, (136 - py) / SCALE

def patch23():
    """Scene 23's line patch, exactly as main.c scene_spline draws it."""
    nu, nv, cx, cy, s = 5, 4, 2.4, -1.2, 0.5
    pts = []
    for j in range(nv):
        for i in range(nu):
            x = C.f32(cx + (i - (nu - 1) / 2.0) * s); y = C.f32(cy + (j - (nv - 1) / 2.0) * s)
            z = C.f32(-5.0 + (0.6 if (i + j) & 1 else -0.6) + (1.2 if (i in (1, 2) and j in (1, 2)) else 0.0))
            c = 0xFF000000 | (i * 255 // (nu - 1)) | (j * 255 // (nv - 1)) << 8 | (255 if (i + j) & 1 else 64) << 16
            pts.append((x, y, z, c))
    return dict(kind='s', cu=5, cv=4, ue=3, ve=0, du=8, dv=8, pts=pts, cell=None)

def fits(P):
    """Every sample on screen with a margin, and no two within 2 pixels of each other."""
    S, _, _ = screen_samples(P)
    pts = [(S[j][i]['x'] >> 4, S[j][i]['y'] >> 4) for j in range(len(S)) for i in range(len(S[0]))]
    if any(not (2 <= x <= 477 and 2 <= y <= 269) for x, y in pts): return False
    for a in range(len(pts)):
        for b in range(a + 1, len(pts)):
            if abs(pts[a][0] - pts[b][0]) < 2 and abs(pts[a][1] - pts[b][1]) < 2: return False
    return True

def random_patch(r, cell):
    for _ in range(500):
        P = _random_patch(r, cell)
        if fits(P): return P
    raise RuntimeError(f'cell {cell}: no patch fits')

def _random_patch(r, cell):
    kind = 'b' if (cell[0] + cell[1]) % 2 == 0 else 's'
    if kind == 'b': cu, cv = (4, 7)[r() % 2], (4, 7)[r() % 2]
    else: cu, cv = 4 + r() % 4, 4 + r() % 3
    ue, ve = (r() % 4, r() % 4) if kind == 's' else (0, 0)
    du, dv = 1 + r() % 8, 1 + r() % 8
    c, row = cell
    x0, y0 = 120 * c + 18, 90 * row + 16
    w, h = 80, 56
    pts = []
    for j in range(cv):
        for i in range(cu):
            px = x0 + w * i / (cu - 1) + (r() % 25) - 12
            py = y0 + h * j / (cv - 1) + (r() % 19) - 9
            X, Y = model_of(px, py)
            z = -5.0 + ((r() % 1000) / 1000 - 0.5) * 1.2
            col = 0xFF000000 | (r() & 0xFFFFFF)
            pts.append((C.f32(X), C.f32(Y), C.f32(z), col))
    return dict(kind=kind, cu=cu, cv=cv, ue=ue, ve=ve, du=du, dv=dv, pts=pts, cell=cell)

_P = []
def patches():
    if not _P:
        r = XS(0x13000001)
        _P.append(patch23())
        def box(P):
            S, _, _ = screen_samples(P)
            xs = [S[j][i]['x'] >> 4 for j in range(len(S)) for i in range(len(S[0]))]
            ys = [S[j][i]['y'] >> 4 for j in range(len(S)) for i in range(len(S[0]))]
            return min(xs) - 3, min(ys) - 3, max(xs) + 3, max(ys) + 3
        boxes = [box(_P[0])]
        for cell in CELLS:
            if cell in ((2, 1), (3, 1), (2, 2), (3, 2)): continue      # scene 23's patch sits there
            for _ in range(200):
                P = random_patch(r, cell); b = box(P)
                if all(b[2] < o[0] or o[2] < b[0] or b[3] < o[1] or o[3] < b[1] for o in boxes): break
            else: raise RuntimeError(f'cell {cell}: overlaps')
            _P.append(P); boxes.append(b)
    return _P

def vtx(p): return dict(x=p[0], y=p[1], z=p[2], c=p[3])

def scene_ops(mode):
    ops = [begin(), ('MATS', PERSP, ID, ID)]
    if mode in ('flat', 'add'): ops.append(('SHADE', GU_FLAT))
    if mode == 'add': ops.append(('BLEND', 1, GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF))
    prim = GU_POINTS if mode == 'points' else GU_LINE_STRIP
    for P in patches():
        pts = [(x, y, z, 0xFF202020 if mode == 'add' else c) for x, y, z, c in P['pts']]
        ops.append(('PATCH', 0 if P['kind'] == 'b' else 1, FMT_CV3D, P['cu'], P['cv'], P['ue'], P['ve'],
                    prim, P['du'], P['dv'], [vtx(p) for p in pts]))
    if mode == 'add': ops.append(('BLEND', 0, 0, 0, 0, 0, 0))
    ops += [('SHADE', GU_SMOOTH), ('END', 0)]
    return ops

SCENES = [
    (130, 'points', 'plpoints', 'patch lines: nine patches as points'),
    (131, 'smooth', 'plsmooth', 'patch lines: the same as lines, smooth'),
    (132, 'flat', 'plflat', 'patch lines: the same as lines, flat'),
    (133, 'add', 'pladd', 'patch lines: the same flat in one grey, added'),
]
def scene(num):
    for n, mode, name, title in SCENES:
        if n == num: return dict(num=n, name=name, title=title, ops=scene_ops(mode))
    raise KeyError(num)
def all_scenes(): return [scene(n) for n, *_ in SCENES]

# =========================================================================== emission (colour14's, plus PATCH and BLEND)
def emit_scene(sc):
    OP = C.OP
    w = []; vcrc = 0; nd = 0; nv = 0
    def op(name, args):
        w.append(OP[name] << 24 | len(args)); w.extend(a & 0xFFFFFFFF for a in args)
    body = []
    for o in sc['ops']:
        k = o[0]
        if k == 'PATCH':
            kind, vtype, cu, cv, ue, ve, prim, du, dv, verts = o[1:]
            vb = C.vbytes(vtype, verts)
            vcrc = zlib.crc32(vb, vcrc); nd += 1; nv += cu * cv
            body.append(('PATCH', [kind, vtype, cu, cv, ue, ve, prim, du, dv, len(vb)] + C.words(vb)))
        elif k == 'BLEND': body.append(('BLEND', list(o[1:7])))
        elif k == 'BEGIN': body.append(('BEGIN', [o[1], o[2]]))
        elif k == 'END': body.append(('END', [o[1]]))
        elif k == 'SHADE': body.append(('SHADE', [o[1]]))
        elif k == 'MATS': body.append(('MATS', [7] + C.mem16(o[1]) + C.mem16(o[2]) + C.mem16(o[3])))
        else: raise ValueError(k)
    op(*body[0])
    op('SUMS', [nd, nv, vcrc & 0xFFFFFFFF])
    for b in body[1:]: op(*b)
    return w, nd, nv, vcrc & 0xFFFFFFFF

def emit(path):
    out = ['/* Generated by plines21.py emit: geprobe 21 scenes 130-133 (patch line order) as',
           ' * command streams for c14_run() (colour14.py\'s format, with PATCH and BLEND).  Do not',
           ' * edit; change plines21.py and run it again. */']
    steps = []
    for k, sc in enumerate(all_scenes()):
        w, nd, nv, vc = emit_scene(sc)
        crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
        steps.append((sc, len(w), nd, nv, vc, crc))
        out.append(f'\n/* scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
                   f' {len(w)} words, stream crc {crc:08X} */')
        out.append(f'static const w32 C21P_S{k}[{len(w)}] = {{')
        for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
        out.append('};')
    out.append('\nstatic const struct c14_step C21P_STEPS[] = {')
    for k, (sc, n, *_r) in enumerate(steps):
        out.append(f'    {{ {sc["num"]}, "{sc["name"]}", "{sc["title"]}", C21P_S{k}, {n} }},')
    out.append('};\n#define C21P_NSTEPS ((int)(sizeof C21P_STEPS / sizeof C21P_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text: open(path, 'w').write(text)
    return steps

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for sc, n, nd, nv, vc, crc in emit(os.path.join(td, 'c21p.inc')):
            m = re.search(rf'scene {sc["num"]}: patch lines.*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want; ok &= good
            print(f'scene {sc["num"]} {sc["name"]}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

# =========================================================================== the mirror
def grid(P):
    """The patch's samples: model positions (nv, nu, 3) by the GE's tessellation (patch13
    'LAv'), and the sample index ranges of its spans in u and v."""
    Ca = np.array([[P['pts'][j * P['cu'] + i][:3] for i in range(P['cu'])] for j in range(P['cv'])], float)
    su = P13.samples(P['kind'], P['cu'], P['du'], P['ue']); sv = P13.samples(P['kind'], P['cv'], P['dv'], P['ve'])
    G = P13.run_model('LAv', Ca, su, sv)
    nsu = (P['cu'] - 1) // 3 if P['kind'] == 'b' else P['cu'] - 3
    nsv = (P['cv'] - 1) // 3 if P['kind'] == 'b' else P['cv'] - 3
    return G, nsu, nsv

M_WVP = P13.wvp(PERSP, ID, ID)
def ge_screen_axis(n, scale, centre, off):
    """ge.c ge_screen_axis: the viewport's sum, less the offset, in sixteenths, floored."""
    v = P13.ge_sum([P13.ge_mul(scale, n), P13.ge_mul(centre, 1.0)])
    return math.floor((v - off) * 16)

def to_screen(x, y, z):
    """A model point through the probes' perspective, as the GE places a vertex (ge_clip,
    ge_over_w, ge_screen_axis with sceGuViewport(2048, 2048, 480, 272) and its offset)."""
    p = [x, y, z, 1.0]
    cx, cy, w = P13.dot4(M_WVP[0], p), P13.dot4(M_WVP[1], p), P13.dot4(M_WVP[3], p)
    nx, ny = P13.ge_over_w(cx, w), P13.ge_over_w(cy, w)
    return dict(x=ge_screen_axis(nx, 240.0, 2048.0, 1808.0), y=ge_screen_axis(ny, -136.0, 2048.0, 1912.0),
                rgba=0, fog=255, spec=None, precise=1, z=None)

def screen_samples(P, mirror=None):
    G, nsu, nsv = grid(P)
    nv, nu = G.shape[:2]
    S = [[to_screen(float(G[j, i, 0]), float(G[j, i, 1]), float(G[j, i, 2])) for i in range(nu)] for j in range(nv)]
    return S, nsu, nsv

def mirror_for():
    m = L15.LineMirror('GE')
    m.reset(0xFF000000)
    m.P, m.V, m.W = PERSP, ID, ID
    return m

def strips(i0, i1, j0, j1, jdesc=False, idesc=False):
    """Segments ((j,i) -> (j,i)) of the zigzag strips over sample rows j0..j1, columns i0..i1."""
    segs = []
    js = range(j0, j1)
    if jdesc: js = reversed(list(js))
    for j in js:
        cols = list(range(i0, i1 + 1))
        if idesc: cols = cols[::-1]
        seq = []
        for i in cols: seq += [(j, i), (j + 1, i)]
        segs += [(seq[k], seq[k + 1]) for k in range(len(seq) - 1)]
    return segs

def candidates(nu, nv, nsu, nsv, du, dv):
    out = {}
    for jd in (False, True):
        for id_ in (False, True):
            out[f'grid{"-jdesc" if jd else ""}{"-idesc" if id_ else ""}'] = strips(0, nu - 1, 0, nv - 1, jd, id_)
    # each span's own grid, spans u-major or v-major
    def span_ranges(ns, d): return [(s * d, s * d + d) for s in range(ns)]
    for order in ('uv', 'vu'):
        segs = []
        us, vs = span_ranges(nsu, du), span_ranges(nsv, dv)
        pairs = [(a, b) for b in vs for a in us] if order == 'uv' else [(a, b) for a in us for b in vs]
        for (ia, ib), (ja, jb) in pairs: segs += strips(ia, ib, ja, jb)
        out[f'span-{order}'] = segs
    return out

def predict(P, colours, segs, flat=False, count=False):
    """A predicted frame (dict pixel -> colour or count) for one patch's segments."""
    m = mirror_for()
    S, nsu, nsv = screen_samples(P, m)
    m.flat = flat
    px = {}
    for (ja, ia), (jb, ib) in segs:
        a = dict(S[ja][ia]); b = dict(S[jb][ib])
        a['rgba'] = colours[(ja, ia)]; b['rgba'] = colours[(jb, ib)]
        a = dict(a, flatshade=flat); b = dict(b, flatshade=flat)
        r = L15.walk(a, b, m.lopt)
        if r is None: continue
        X, Y, col, fog, Z, k = r
        for x, y, c in zip(X, Y, col):
            if 0 <= x < 480 and 0 <= y < 272:
                if count: px[(int(x), int(y))] = px.get((int(x), int(y)), 0) + 1
                else: px[(int(x), int(y))] = int(c) & 0xFFFFFF
    return px

def frame(dumpdir, num, name):
    return np.fromfile(os.path.join(dumpdir, f'ge_{num}_{name}.raw'), '<u4').reshape(272, 480) & 0xFFFFFF

def compare(dumpdir):
    Fp, Fs, Ff, Fa = (frame(dumpdir, n, nm) for n, _m, nm, _t in SCENES)
    totals = {}
    for pi, P in enumerate(patches()):
        m = mirror_for()
        S, nsu, nsv = screen_samples(P, m)
        nv, nu = len(S), len(S[0])
        cols = {(j, i): int(Fp[S[j][i]['y'] >> 4, S[j][i]['x'] >> 4]) | 0xFF000000 for j in range(nv) for i in range(nu)}
        res = {}
        for name, segs in candidates(nu, nv, nsu, nsv, P['du'], P['dv']).items():
            out = []
            for F, flat, count in ((Fs, False, False), (Ff, True, False), (Fa, True, True)):
                pred = predict(P, cols, segs, flat, count)
                bad = 0
                for (x, y), v in pred.items():
                    got = int(F[y, x])
                    want = min(255, 0x20 * v) * 0x010101 if count else v
                    bad += got != want
                out.append(bad)
            res[name] = out
            totals.setdefault(name, [0, 0, 0])
            for k in range(3): totals[name][k] += out[k]
        best = min(res, key=lambda k: sum(res[k]))
        print(f'patch {pi} ({P["kind"]} {P["cu"]}x{P["cv"]}, edges {P["ue"]},{P["ve"]}, d {P["du"]}x{P["dv"]}): '
              + '  '.join(f'{k} {v}' for k, v in res.items()) + f'   best {best}')
    print('totals (smooth, flat, added):', {k: v for k, v in totals.items()})

def check():
    m = mirror_for()
    for pi, P in enumerate(patches()):
        S, nsu, nsv = screen_samples(P, m)
        xs = [S[j][i]['x'] / 16 for j in range(len(S)) for i in range(len(S[0]))]
        ys = [S[j][i]['y'] / 16 for j in range(len(S)) for i in range(len(S[0]))]
        cands = candidates(len(S[0]), len(S), nsu, nsv, P['du'], P['dv'])
        # how many pixels do the candidates disagree on (flat)
        cols = {(j, i): 0xFF000000 | (j * 37 + i * 101) * 2654435761 & 0xFFFFFF for j in range(len(S)) for i in range(len(S[0]))}
        preds = {k: predict(P, cols, v, flat=True) for k, v in cands.items()}
        base = preds['grid']
        diff = {k: sum(1 for p in set(base) | set(v) if base.get(p) != v.get(p)) for k, v in preds.items() if k != 'grid'}
        print(f'patch {pi}: {P["kind"]} {P["cu"]}x{P["cv"]} d {P["du"]}x{P["dv"]}, samples {len(S[0])}x{len(S)}, '
              f'x {min(xs):.0f}-{max(xs):.0f} y {min(ys):.0f}-{max(ys):.0f}; flat pixels differing from grid order: {diff}')

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        for sc, n, nd, nv, vc, crc in emit(argv[2] if len(argv) > 2 else os.path.join(HERE, 'c21p_data.inc')):
            print(f'scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'compare': compare(argv[2]); return 0
    if cmd == 'check': check(); return 0
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
