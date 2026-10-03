#!/usr/bin/env python3
"""geprobe 22 (scenes 135-137): the order a patch's triangles go out in.

Set 22 (plines21.py) found that a patch drawn as lines goes out a span at a time, the
spans a row of them at a time, each span over its own samples as the zigzag of its
strips. Whether its triangles follow the same order nothing has shown: where they do not
overlap the order cannot matter, and the probes' patches never folded over themselves.
These patches do -- folded back over themselves along u, v or both, so that a span lies
over its neighbours' other rows and columns, one piece looping over itself, and scene
23's own patch under the probes' perspective -- so that flat triangles that overlap
leave the last one's colour:
  135 ptpoints   every sample as a point: each sample's own colour (the flat colours)
  136 ptflat     as triangles, flat: each triangle its last vertex's colour, a pixel two
                 triangles share the later one's
  137 ptadd      as triangles, flat, every control 0x101010, adding: each pixel's grey
                 is 0x10 times how many triangles drew it (coverage, whatever the order)
The candidate orders (candidates) are plines21's for triangles: the whole grid's strips
in either row and column direction, or each span's own, spans u-major or v-major, each
quad's two triangles (s0 s1 s2), (s1 s2 s3) as the strips give them.

    ptris22.py emit [OUT.inc]        write c22t_data.inc
    ptris22.py sums DUMPDIR          the logged CRCs against these streams
    ptris22.py compare DUMPDIR       per patch and candidate order: pixels off in 136-137
    ptris22.py check                 design checks: overlaps, where the orders part
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
import plines21 as PL
from colour14 import XS, FMT_CV3D, GU_FLAT, GU_SMOOTH, begin

GU_POINTS, GU_TRIANGLE_STRIP = 0, 4
GU_ADD, GU_FIX = 0, 10
GREY = 0x101010
CELLS = [(c, r) for r in range(3) for c in range(4)]

def profile(n, fold, span=70.0):
    """n control offsets 0 .. span that run forward and, with fold, back over themselves
    from the middle control on (a Bezier's second piece, a spline's later spans)."""
    if not fold: return [span * k / (n - 1) for k in range(n)]
    h = (n - 1) // 2
    fwd = [span * k / h for k in range(h + 1)]
    return fwd + [span - span * k / (n - 1 - h) * 0.85 for k in range(1, n - h)]

def folded(r, cell, kind, cu, cv, du, dv, ue=0, ve=0, fu=False, fv=False, loop=False):
    """A patch in cell folded back over itself along u (fu) and/or v (fv): the folded
    half shifted 9-15 pixels across, so a span lies over its neighbour's other rows or
    columns. loop: one piece whose controls cross (0, 100, -20, 80 of 80), overlapping
    itself inside a span."""
    c, row = cell
    x0, y0 = 120 * c + 22, 90 * row + 14
    xs = [0.0, 100.0, -20.0, 80.0] if loop else profile(cu, fu)
    ys = [0.0, 70.0, -14.0, 56.0] if loop and cv == 4 else profile(cv, fv, 56.0)
    sh_u, sh_v = 9 + r() % 7, 9 + r() % 7
    hu, hv = (cu - 1) // 2, (cv - 1) // 2
    pts = []
    for j in range(cv):
        for i in range(cu):
            px = x0 + xs[i] + (sh_v if fv and j > hv else 0) + ((r() % 401) / 100 - 2)
            py = y0 + ys[j] + (sh_u if fu and i > hu else 0) + ((r() % 401) / 100 - 2)
            X, Y = PL.model_of(px, py)
            z = -5.0 + ((r() % 1000) / 1000 - 0.5) * 0.4
            pts.append((C.f32(X), C.f32(Y), C.f32(z), 0xFF000000 | (r() & 0xFFFFFF)))
    return dict(kind=kind, cu=cu, cv=cv, ue=ue, ve=ve, du=du, dv=dv, pts=pts, cell=cell)

# (kind, cu, cv, du, dv, ue, ve, fold u, fold v, loop) for the eight free cells
DESIGNS = [
    ('b', 7, 4, 4, 3, 0, 0, True, False, False),     # two pieces in u, the second back over the first
    ('b', 4, 7, 3, 4, 0, 0, False, True, False),     # the same in v
    ('b', 7, 7, 3, 3, 0, 0, True, True, False),      # four pieces over one another
    ('s', 6, 4, 4, 4, 0, 0, True, False, False),     # spline, three spans in u, folded
    ('s', 4, 6, 3, 5, 1, 2, False, True, False),     # spline folded in v, open ends
    ('s', 6, 6, 3, 3, 3, 3, True, True, False),      # spline folded both ways
    ('b', 4, 4, 8, 6, 0, 0, False, False, True),     # one piece overlapping itself
    ('s', 5, 5, 5, 4, 2, 1, True, True, False),      # spline 2 x 2 spans, folded both ways
]
def on_screen(P):
    S, _, _ = PL.screen_samples(P)
    return all(2 <= S[j][i]['x'] >> 4 <= 477 and 2 <= S[j][i]['y'] >> 4 <= 269
               for j in range(len(S)) for i in range(len(S[0])))

def box(P):
    S, _, _ = PL.screen_samples(P)
    xs = [S[j][i]['x'] >> 4 for j in range(len(S)) for i in range(len(S[0]))]
    ys = [S[j][i]['y'] >> 4 for j in range(len(S)) for i in range(len(S[0]))]
    return min(xs) - 2, min(ys) - 2, max(xs) + 2, max(ys) + 2

_P = []
def patches():
    if not _P:
        r = XS(0x13500001)
        _P.append(PL.patch23())
        free = [c for c in CELLS if c not in ((2, 1), (3, 1), (2, 2), (3, 2))]   # scene 23's patch sits there
        for cell, d in zip(free, DESIGNS):
            kind, cu, cv, du, dv, ue, ve, fu, fv, loop = d
            P = folded(r, cell, kind, cu, cv, du, dv, ue, ve, fu, fv, loop)
            if not on_screen(P): raise RuntimeError(f'cell {cell}: off screen')
            _P.append(P)
    return _P

def vtx(p): return dict(x=p[0], y=p[1], z=p[2], c=p[3])

def scene_ops(mode):
    ops = [begin(), ('MATS', PL.PERSP, PL.ID, PL.ID)]
    if mode in ('flat', 'add'): ops.append(('SHADE', GU_FLAT))
    if mode == 'add': ops.append(('BLEND', 1, GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF))
    prim = GU_POINTS if mode == 'points' else GU_TRIANGLE_STRIP
    for P in patches():
        pts = [(x, y, z, 0xFF000000 | GREY if mode == 'add' else c) for x, y, z, c in P['pts']]
        ops.append(('PATCH', 0 if P['kind'] == 'b' else 1, FMT_CV3D, P['cu'], P['cv'], P['ue'], P['ve'],
                    prim, P['du'], P['dv'], [vtx(p) for p in pts]))
    if mode == 'add': ops.append(('BLEND', 0, 0, 0, 0, 0, 0))
    ops += [('SHADE', GU_SMOOTH), ('END', 0)]
    return ops

SCENES = [
    (135, 'points', 'ptpoints', 'patch triangles: nine folded patches as points'),
    (136, 'flat', 'ptflat', 'patch triangles: the same as triangles, flat'),
    (137, 'add', 'ptadd', 'patch triangles: the same flat in one grey, added'),
]
def scene(num):
    for n, mode, name, title in SCENES:
        if n == num: return dict(num=n, name=name, title=title, ops=scene_ops(mode))
    raise KeyError(num)
def all_scenes(): return [scene(n) for n, *_ in SCENES]

# =========================================================================== the orders
def quads(i0, i1, j0, j1, jdesc=False, idesc=False):
    """Triangles ((j,i) x 3) of the strips over sample rows j0..j1, columns i0..i1."""
    out = []
    js = list(range(j0, j1))
    if jdesc: js = js[::-1]
    for j in js:
        cols = list(range(i0, i1))
        if idesc: cols = cols[::-1]
        for i in cols:
            s0, s1, s2, s3 = (j, i), (j + 1, i), (j, i + 1), (j + 1, i + 1)
            if idesc: out += [(s2, s3, s0), (s3, s0, s1)]
            else: out += [(s0, s1, s2), (s1, s2, s3)]
    return out

def candidates(nu, nv, nsu, nsv, du, dv):
    out = {}
    for jd in (False, True):
        for id_ in (False, True):
            out[f'grid{"-jdesc" if jd else ""}{"-idesc" if id_ else ""}'] = quads(0, nu - 1, 0, nv - 1, jd, id_)
    def span_ranges(ns, d, n): return [(s * d, min(s * d + d, n - 1)) for s in range(ns)]
    us, vs = span_ranges(nsu, du, nu), span_ranges(nsv, dv, nv)
    for order in ('uv', 'vu'):
        pairs = [(a, b) for b in vs for a in us] if order == 'uv' else [(a, b) for a in us for b in vs]
        tris = []
        for (ia, ib), (ja, jb) in pairs: tris += quads(ia, ib, ja, jb)
        out[f'span-{order}'] = tris
    return out

def shared(P):
    """Samples that share a pixel with another: the points scene cannot give their colours."""
    S, _, _ = PL.screen_samples(P)
    at = {}
    for j in range(len(S)):
        for i in range(len(S[0])): at.setdefault((S[j][i]['x'] >> 4, S[j][i]['y'] >> 4), []).append((j, i))
    return {q for v in at.values() if len(v) > 1 for q in v}

def predict(P, colours, tris, count=False, skip=()):
    """A predicted frame (dict pixel -> colour, or count) for one patch's flat triangles;
    a pixel whose last triangle ends on a sample in skip is left out (None)."""
    S, nsu, nsv = PL.screen_samples(P)
    px = {}
    sc = (0, 0, 479, 271)
    for t in tris:
        vs = [dict(S[j][i], rgba=colours[(j, i)], fog=255, spec=None, precise=1, z=0) for j, i in t]
        r = C.sw_tri(vs, sc, True, C.Opt('cur'))
        if r is None: continue
        X, Y, col = r[0], r[1], r[2]
        for x, y, c in zip(X, Y, col):
            q = (int(x), int(y))
            if count: px[q] = px.get(q, 0) + 1
            else: px[q] = None if t[2] in skip else int(c) & 0xFFFFFF
    return px

# =========================================================================== emission (plines21's)
def emit(path):
    out = ['/* Generated by ptris22.py emit: geprobe 22 scenes 135-137 (patch triangle order) as',
           ' * command streams for c14_run() (colour14.py\'s format, with PATCH and BLEND).  Do not',
           ' * edit; change ptris22.py and run it again. */']
    steps = []
    for k, sc in enumerate(all_scenes()):
        w, nd, nv, vc = PL.emit_scene(sc)
        crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
        steps.append((sc, len(w), nd, nv, vc, crc))
        out.append(f'\n/* scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
                   f' {len(w)} words, stream crc {crc:08X} */')
        out.append(f'static const w32 C22T_S{k}[{len(w)}] = {{')
        for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
        out.append('};')
    out.append('\nstatic const struct c14_step C22T_STEPS[] = {')
    for k, (sc, n, *_r) in enumerate(steps):
        out.append(f'    {{ {sc["num"]}, "{sc["name"]}", "{sc["title"]}", C22T_S{k}, {n} }},')
    out.append('};\n#define C22T_NSTEPS ((int)(sizeof C22T_STEPS / sizeof C22T_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text: open(path, 'w').write(text)
    return steps

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for sc, n, nd, nv, vc, crc in emit(os.path.join(td, 'c22t.inc')):
            m = re.search(rf'scene {sc["num"]}: .*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want; ok &= good
            print(f'scene {sc["num"]} {sc["name"]}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

def frame(dumpdir, num, name):
    return np.fromfile(os.path.join(dumpdir, f'ge_{num}_{name}.raw'), '<u4').reshape(272, 480) & 0xFFFFFF

def compare(dumpdir):
    Fp, Ff, Fa = (frame(dumpdir, n, nm) for n, _m, nm, _t in SCENES)
    totals = {}
    for pi, P in enumerate(patches()):
        S, nsu, nsv = PL.screen_samples(P)
        nv, nu = len(S), len(S[0])
        cols = {(j, i): int(Fp[S[j][i]['y'] >> 4, S[j][i]['x'] >> 4]) | 0xFF000000 for j in range(nv) for i in range(nu)}
        res = {}
        for name, tris in candidates(nu, nv, nsu, nsv, P['du'], P['dv']).items():
            out = []
            for F, count in ((Ff, False), (Fa, True)):
                pred = predict(P, cols, tris, count, shared(P))
                bad = 0
                for (x, y), v in pred.items():
                    if v is None: continue
                    want = min(255, 0x10 * v) * 0x010101 if count else v
                    bad += int(F[y, x]) != want
                out.append(bad)
            res[name] = out
            totals.setdefault(name, [0, 0])
            for k in range(2): totals[name][k] += out[k]
        best = min(res, key=lambda k: sum(res[k]))
        print(f'patch {pi} ({P["kind"]} {P["cu"]}x{P["cv"]}, edges {P["ue"]},{P["ve"]}, d {P["du"]}x{P["dv"]}): '
              + '  '.join(f'{k} {v}' for k, v in res.items()) + f'   best {best}')
    print('totals (flat, added):', totals)

def check():
    for pi, P in enumerate(patches()):
        S, nsu, nsv = PL.screen_samples(P)
        nv, nu = len(S), len(S[0])
        cols = {(j, i): 0xFF000000 | ((j * 37 + i * 101) * 2654435761 & 0xFFFFFF) for j in range(nv) for i in range(nu)}
        cands = candidates(nu, nv, nsu, nsv, P['du'], P['dv'])
        preds = {k: predict(P, cols, v) for k, v in cands.items()}
        cnt = predict(P, cols, cands['grid'], True)
        over = sum(1 for v in cnt.values() if v > 1)
        base = preds['span-uv']
        diff = {k: sum(1 for p in set(base) | set(v) if base.get(p) != v.get(p)) for k, v in preds.items() if k != 'span-uv'}
        print(f'patch {pi}: {P["kind"]} {P["cu"]}x{P["cv"]} d {P["du"]}x{P["dv"]} spans {nsu}x{nsv}, samples {nu}x{nv}, '
              f'{len(cnt)} pixels, {over} drawn more than once; flat pixels differing from span-uv: {diff}')

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        for sc, n, nd, nv, vc, crc in emit(argv[2] if len(argv) > 2 else os.path.join(HERE, 'c22t_data.inc')):
            print(f'scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'compare': compare(argv[2]); return 0
    if cmd == 'check': check(); return 0
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
