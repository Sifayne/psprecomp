#!/usr/bin/env python3
"""geprobe 15 (scenes 99-109): line colours.  Generator, mirror and checker.

In the pattern of colour14.py, on its stream format and its Mirror (vertex path, fog,
lighting, alpha ladder, depth): the scene builders below make every line of every scene,
`emit` writes them as command streams (c15_data.inc) that main.c replays with
colour14's c14_run(), and the mirror draws them the way psprecomp's
psp_render_walk_line does (src/render.c), under the current rule ('cur') or a rival.

What the earlier dumps already say (set 15, geprobe 14): a flat-shaded line takes its
second vertex's colour (scene 75's 1494 line pixels, where psprecomp interpolates and
matches 747), and a line with |dx| == |dy| is y-major (scene 22's 1376, where x-major
leaves 45 a step off). These scenes confirm both and measure what nothing has drawn yet:
colour along lines in every direction, the extra start pixel, strips' joint pixels,
fog, the secondary colour, alpha, 3D and perspective, and depth.

A rule is 'cur', 'H' (y-major ties, flat from the second vertex) or either followed by
',key=value' changes:
    tie    x | y                 which axis is major when |dx| == |dy|
    flat   interp | a | b        a flat-shaded line's colour (interp: shading ignored)
    start  extrap | clamp        the pixel before the first (start in its diamond): the
                                 plane at its centre, or the start vertex's own value
    fog    lerp | plane          fog along the line: psprecomp's rounded float lerp, or a
                                 plane like colour's (gradient by area_rcp, floored)
    spec   consta | plane | none the secondary colour: the first vertex's, constant (what
                                 psprecomp does since its points add theirs), interpolated
                                 as a plane, or not added on lines
    grad   rcp | exact           colour gradient by area_rcp's reciprocal, or exact
    edge   above | minor         on a pixel diamond's edge, inside means above the centre
                                 (geprobe 6's shallow lines), or on the minor axis's
                                 negative side (above for shallow lines, left for steep)
e.g. 'H,spec=plane'. 'cur' is psprecomp before set 16; 'GE' is what set 16 measured
(y-major ties, flat from the second vertex, fog and secondary planes, minor-side edges),
which matches every pixel of scenes 99-109 and which psprecomp draws now.

    lines15.py emit [OUT.inc]          write the command streams (default c15_data.inc)
    lines15.py sums DUMPDIR            every scene's SUMS line in geprobe.txt against these streams
    lines15.py predict OUTDIR [rule]   predicted ge_NN_name.raw (and 109's _depthfull.bin)
    lines15.py compare DUMPDIR [scene..] [rule..]   px off per scene under each rule
    lines15.py check                   design checks: separation between rules, overlap
    lines15.py selfcheck DUMPDIR       model-free twins on a dump (107 == 99, 108 vs 99)
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
from colour14 import (XS, f32, bf, chan, area_rcp, P16, begin, ID, VP3, cv3d, model_xy, mat_persp,
                      FMT_CV2D, FMT_CV3D, FMT_TCNV3D, GU_FLAT, GU_SMOOTH, GU_ALWAYS, GU_POINTS)

GU_LINES, GU_LINE_STRIP = 1, 2

# =========================================================================== rules
DEFAULTS = dict(tie='x', flat='interp', start='extrap', fog='lerp', spec='consta', grad='rcp', edge='above')
PRESETS = {'cur': {}, 'H': dict(tie='y', flat='b'),
           # what set 16 (geprobe 15, fw 6.60) measured, and psprecomp draws since: every scene exact
           'GE': dict(tie='y', flat='b', fog='plane', spec='plane', edge='minor')}
def parse_rule(rule):
    parts = rule.split(',')
    o = dict(DEFAULTS); o.update(PRESETS[parts[0]])
    for p in parts[1:]:
        k, v = p.split('=')
        assert k in DEFAULTS, k
        o[k] = v
    return o
RIVALS = ['GE', 'cur', 'H', 'H,tie=x', 'H,flat=a', 'H,flat=interp', 'H,start=clamp', 'H,fog=plane',
          'H,spec=plane', 'H,spec=none', 'H,grad=exact']

def plane_chan(acc):
    v = acc >> 14
    return np.clip(v, 0, 255)

def walk(a, b, opt):
    """psprecomp's psp_render_walk_line on mirror vertices a, b (x, y in 1/16, rgba,
    fog, spec, z): (X, Y, col, fog, Z, k) arrays, k the major index (-1: the pixel
    before the first)."""
    dx, dy = b['x'] - a['x'], b['y'] - a['y']
    ax_, ay_ = abs(dx), abs(dy)
    if max(ax_, ay_) < 16: return None
    xmajor = ax_ > ay_ if opt['tie'] == 'y' else ax_ >= ay_
    Ma, ma = (a['x'], a['y']) if xmajor else (a['y'], a['x'])
    dM, dm = (dx, dy) if xmajor else (dy, dx)
    adM = abs(dM); sm = -1 if dM < 0 else 1
    Mb = Ma + dM
    M0 = (Ma - 8 + 15) // 16 if sm > 0 else (Ma - 8) // 16
    Mend = (Mb - 8 + 15) // 16 if sm > 0 else (Mb - 8) // 16
    n = Mend - M0 if sm > 0 else M0 - Mend
    c0 = 16 * M0 + 8
    pm = ma * adM + dm * sm * (c0 - Ma); dmk = 16 * dm; smd = 16 * adM
    first, last = 0, n - 1
    mb = ma + dm
    for end, k in ((0, n - 1), (1, -1)):
        Mp = M0 + sm * k; mp = (pm + dmk * k) // smd
        pM, pmin = (Ma, ma) if end else (Mb, mb)
        dMaj = pM - (16 * Mp + 8); dMin = pmin - (16 * mp + 8)
        ddy = dMin if (xmajor or opt['edge'] == 'minor') else dMaj
        s = abs(dMaj) + abs(dMin)
        if s < 8 or (s == 8 and ddy < 0):
            if end: first = -1
            else: last = n - 2
    if last < first: return None
    k = np.arange(first, last + 1, dtype=np.int64)
    dist16 = sm * (c0 - Ma) + 16 * k
    Mp = M0 + sm * k; mp = (pm + dmk * k) // smd
    X, Y = (Mp, mp) if xmajor else (mp, Mp)
    lq, lsh = area_rcp(adM)
    dd = np.where(k < 0, 0, dist16) if opt['start'] == 'clamp' else dist16
    def lin(va, vb):
        if opt['grad'] == 'exact':
            return plane_chan((va * 16384 * adM + (vb - va) * 16384 * dd) // adM)
        g = ((vb - va) * 16384 * lq) >> lsh
        return plane_chan(va * 16384 + g * dd)
    if opt['flat'] in ('a', 'b') and a.get('flatshade'):
        col = np.full(len(k), (a if opt['flat'] == 'a' else b)['rgba'], np.int64)
    else:
        col = np.zeros(len(k), np.int64)
        for c in range(4): col |= lin(chan(a['rgba'], c), chan(b['rgba'], c)) << (8 * c)
    # fog
    if opt['fog'] == 'plane':
        fog = lin(a['fog'], b['fog'])
    else:
        t = np.array([f32(float(d) / float(adM)) for d in dist16], np.float64)
        fog = np.array([int(f32(f32(f32(1.0 - tt) * a['fog']) + f32(tt * b['fog'])) + 0.5) for tt in t], np.int64)
    # depth
    Z = None
    if a['z'] is not None and b['z'] is not None:
        za, zb = a['z'], b['z']
        zg = ((zb - za) * 16384 * lq) >> lsh
        Z = (za * 16384 + zg * dist16) // 16384
    return X, Y, col, fog, Z, k

# =========================================================================== the mirror
class LineMirror(C.Mirror):
    def __init__(m, rule='cur'):
        super().__init__(C.Opt('cur'))
        m.lopt = parse_rule(rule)
    def line(m, a, b):
        a = dict(a, flatshade=m.flat); b = dict(b, flatshade=m.flat)
        if m.flat and m.lopt['flat'] == 'interp': pass
        r = walk(a, b, m.lopt)
        if r is None: return
        X, Y, col, fog, Z, k = r
        ok = (X >= 0) & (X < 480) & (Y >= 0) & (Y < 272)
        X, Y, col, fog, k = X[ok], Y[ok], col[ok], fog[ok], k[ok]
        if Z is not None: Z = Z[ok]
        if m.tex: col = col & 0xFF000000            # black texel, REPLACE on RGB
        sp = m.lopt['spec']
        if sp != 'none' and (a['spec'] is not None or b['spec'] is not None):
            if sp == 'consta':
                s = a['spec'] or 0
                add = [np.full(len(X), chan(s, c), np.int64) for c in range(3)]
            else:
                ra = dict(a, rgba=a['spec'] or 0, flatshade=False); rb = dict(b, rgba=b['spec'] or 0, flatshade=False)
                sc = walk(ra, rb, dict(m.lopt, flat='interp'))[2][ok]
                add = [(sc >> (8 * c)) & 255 for c in range(3)]
            for c in range(3):
                v = np.minimum(((col >> (8 * c)) & 255) + add[c], 255)
                col = (col & ~(0xFF << (8 * c))) | (v << (8 * c))
        tid = m.tid; m.tid += 1
        m.shade(X, Y, col, fog, tid, Z if m.depth else None)
    def point(m, v):
        """A point as psprecomp draws it since geprobe 14: its secondary colour added after
        the texture function (render.c sw_point_sample), which colour14's mirror predates."""
        x, y = v['x'] >> 4, v['y'] >> 4
        col = v['rgba'] & 0xFF000000 if m.tex else v['rgba']
        if v['spec'] is not None:
            for c in range(3):
                s = min(chan(col, c) + chan(v['spec'], c), 255)
                col = (col & ~(0xFF << (8 * c))) | (s << (8 * c))
        Z = np.array([v['z']], np.int64) if v['z'] is not None else None
        m.shade(np.array([x]), np.array([y]), np.array([col], np.int64), np.array([v['fog']]), -2, Z)
    def draw(m, prim, vtype, verts, idx):
        if prim in (GU_LINES, GU_LINE_STRIP):
            vv = [m.vertex(v, vtype) for v in verts]
            seq = [vv[i] for i in idx] if idx is not None else vv
            step = 2 if prim == GU_LINES else 1
            for i in range(0, len(seq) - 1, step): m.line(seq[i], seq[i + 1])
        else:
            super().draw(prim, vtype, verts, idx)

def render(sc, rule='cur'):
    m = LineMirror(rule)
    m.run(sc)
    return m

# =========================================================================== scene builders
_cache = {}
def ends(rng, n=4):
    """Two ARGB colours whose channels differ by at least 64 each (alpha 255 unless n == 4)."""
    out = [0, 0]
    for c in range(n):
        while True:
            p, q = rng() & 255, rng() & 255
            if abs(p - q) >= 64: break
        out[0] |= p << (8 * c); out[1] |= q << (8 * c)
    if n < 4: out = [o | 0xFF000000 for o in out]
    return out

def geo99():
    """96 lines, one per 3.75 degrees from 1.1, 24 to 30 px long, each centred in a 40 x 34
    cell (12 across, 8 down), ends on random sixteenths; none exactly 45 degrees."""
    if '99' not in _cache:
        r = XS(0x99000001); out = []
        for i in range(96):
            cx, cy = 40 * (i % 12) + 20, 34 * (i // 12) + 17
            th = math.radians(1.1 + 3.75 * i); L = 24 + (r() % 97) / 16
            ox, oy = (r() % 16) / 16, (r() % 16) / 16
            ax, ay = P16(cx + ox - L / 2 * math.cos(th)), P16(cy + oy - L / 2 * math.sin(th))
            bx, by = P16(cx + ox + L / 2 * math.cos(th)), P16(cy + oy + L / 2 * math.sin(th))
            if abs(bx - ax) == abs(by - ay): bx += 1
            out.append(((ax, ay), (bx, by), ends(r, 3)))
        _cache['99'] = out
    return _cache['99']
def v2(x16, y16, c, z=0.0): return dict(x=x16 / 16.0, y=y16 / 16.0, z=z, c=c)
def lines_draw(L, vtype=FMT_CV2D, prim=GU_LINES):
    verts = []
    for (a, b, cs) in L:
        verts += [v2(a[0], a[1], cs[0]), v2(b[0], b[1], cs[1])]
    return ('DRAW', prim, vtype, verts, None)
def sc99(): return [begin(), lines_draw(geo99()), ('END', 0)]

def geo100():
    """|dx| == |dy| = 10 px in the four diagonal directions at 64 start offsets (fx, fy in
    sixteenths), then |dy| a sixteenth either side: 384 lines in 15 x 15 cells."""
    if '100' not in _cache:
        r = XS(0x10000001); out = []
        cells = 0
        def cell():
            nonlocal cells
            x, y = 15 * (cells % 32), 15 * (cells // 32); cells += 1
            return x, y
        for d in range(4):
            sx, sy = (1, 1, -1, -1)[d], (1, -1, 1, -1)[d]
            for j in range(64):
                fx, fy = (j % 8) * 2 + (j // 8) % 2, (j // 8) * 2 + (j % 2)
                x, y = cell()
                ax = (x + 2 + (10 if sx < 0 else 0)) * 16 + fx; ay = (y + 2 + (10 if sy < 0 else 0)) * 16 + fy
                out.append(((ax, ay), (ax + sx * 160, ay + sy * 160), ends(r, 3)))
        for d in range(4):
            sx, sy = (1, 1, -1, -1)[d], (1, -1, 1, -1)[d]
            for j in range(32):
                e = 1 if j < 16 else -1; fx, fy = (j % 16), (5 * j) % 16
                x, y = cell()
                ax = (x + 2 + (10 if sx < 0 else 0)) * 16 + fx; ay = (y + 2 + (10 if sy < 0 else 0)) * 16 + fy
                out.append(((ax, ay), (ax + sx * 160, ay + sy * (160 + e)), ends(r, 3)))
        _cache['100'] = out
    return _cache['100']
def sc100(): return [begin(), lines_draw(geo100()), ('END', 0)]

def geo101():
    """Short lines in 8 directions (shallow and steep, every sign) and two lengths, each
    starting just past a pixel centre in its direction of travel -- 1, 3, 5 or 7
    sixteenths along the major axis and -5, -2, +2 or +5 across it -- so the start lies
    in (or, at the corners, just outside) that pixel's diamond and the pixel before the
    first is drawn or not: 256 lines in 15 x 15 cells. The pixel before the first, its
    colour, and the end pixel are what count."""
    if '101' not in _cache:
        r = XS(0x10100001); out = []; n = 0
        for (lM, lm) in ((72, 24), (40, 8)):
            for d in range(8):
                steep = d >= 4; sx = 1 if d % 2 == 0 else -1; sy = 1 if (d // 2) % 2 == 0 else -1
                ddx, ddy = (lm, lM) if steep else (lM, lm)
                for j in range(16):
                    maj, mnr = (1, 3, 5, 7)[j % 4], (-5, -2, 2, 5)[j // 4]
                    fx, fy = ((mnr, maj * sy) if steep else (maj * sx, mnr))
                    x, y = 15 * (n % 32), 15 * (n // 32); n += 1
                    ax, ay = (x + 7) * 16 + 8 + fx, (y + 7) * 16 + 8 + fy
                    out.append(((ax, ay), (ax + sx * ddx, ay + sy * ddy), ends(r, 3)))
        _cache['101'] = out
    return _cache['101']
def sc101(): return [begin(), lines_draw(geo101()), ('END', 0)]

def strips(seed, rows, y0, amp, n_strips=4, segs=6, dx=20):
    """Zigzag strips: per row, n_strips strips of segs segments, dx px apart in x and
    alternating amp px in y, every vertex on a random sixteenth with its own colour."""
    r = XS(seed); out = []
    for row in range(rows):
        for s in range(n_strips):
            pts = []
            for i in range(segs + 1):
                x = 120 * s + 2 + dx * i; y = y0 + 34 * row + (4 if i % 2 == 0 else 4 + amp[(row + s) % len(amp)])
                pts.append(((x * 16 + r() % 16), (y * 16 + r() % 16), ends(r, 3)[0]))
            out.append(pts)
    return out
def strip_draws(S, vtype=FMT_CV2D, prim=GU_LINE_STRIP):
    ops = []
    for pts in S:
        ops.append(('DRAW', prim, vtype, [v2(x, y, c) for x, y, c in pts], None))
    return ops
def verts3d(L):
    vs = []
    for (a, b, cs) in L: vs += [cv3d(a[0], a[1], -0.5, cs[0]), cv3d(b[0], b[1], -0.5, cs[1])]
    return vs

def sc102():
    """Flat shading: rows 0-1 through-mode GU_LINES (99's first 24 lines), row 2 through
    strips, rows 4-5 the same lines in 3D, row 6 the strips in 3D; each vertex its own
    colour, so a line reads which vertex it takes."""
    L = geo99()[:24]
    S = strips(0x10200001, 1, 68, [12, 20, 6, 16])
    ops = [begin(), ('SHADE', GU_FLAT), lines_draw(L)] + strip_draws(S)
    L3 = [((a[0], a[1] + 136 * 16), (b[0], b[1] + 136 * 16), cs) for a, b, cs in L]
    S3 = [[(x, y + 136 * 16, c) for x, y, c in pts] for pts in S]
    ops += [('MATS', ID, ID, ID), VP3, ('DRAW', GU_LINES, FMT_CV3D, verts3d(L3), None)]
    for pts in S3:
        ops.append(('DRAW', GU_LINE_STRIP, FMT_CV3D, [cv3d(x, y, -0.5, c) for x, y, c in pts], None))
    return ops + [('SHADE', GU_SMOOTH), ('END', 0)]

def sc103():
    """Smooth strips at many joint angles: 8 rows of 4 zigzag strips, amplitudes 0 to 28
    px, so each joint pixel shows which segment drew it last."""
    S = strips(0x10300001, 8, 0, [0, 3, 7, 12, 18, 24, 28, 1])
    return [begin()] + strip_draws(S) + [('END', 0)]

def fog104():
    if '104' not in _cache:
        r = XS(0x10400001); out = []
        for _ in geo99():
            while True:
                p, q = 16 + r() % 224, 16 + r() % 224
                if abs(p - q) >= 64: break
            out.append((p, q))
        _cache['104'] = out
    return _cache['104']
def sc104():
    """99's lines in 3D (identity, scale 256), white, with fog: each end's fog byte chosen
    mid-step (eye z), fog colour black, so R = G = B is the fogged white; each end also
    drawn as a point in its cell's top-left corner."""
    verts = []; pts = []
    for i, ((a, b, _), (fa, fb)) in enumerate(zip(geo99(), fog104())):
        verts += [cv3d(a[0], a[1], C.fog_z(fa), 0xFFFFFFFF), cv3d(b[0], b[1], C.fog_z(fb), 0xFFFFFFFF)]
        cx, cy = 40 * (i % 12), 34 * (i // 12)
        for j, F in enumerate((fa, fb)):
            pts.append(cv3d((cx + 1 + 2 * j) * 16 + 8, (cy + 1) * 16 + 8, C.fog_z(F), 0xFFFFFFFF))
    return [begin(), ('MATS', ID, ID, ID), VP3, ('FOG', 1, 0.0, 1.0, 0x000000),
            ('DRAW', GU_LINES, FMT_CV3D, verts, None), ('DRAW', GU_POINTS, FMT_CV3D, pts, None),
            ('FOG', 0, 0.0, 1.0, 0), ('END', 0)]

N105 = [(0.0, 0.0, 1.0), (0.6, 0.0, 0.8), (0.0, -0.6, 0.8), (0.48, 0.36, 0.8)]
def sc105():
    """Lit lines with separate specular, as scene 95's triangles: row 0 primary and
    secondary, row 1 secondary alone (black REPLACE texture), row 2 primary alone; 24
    lines a row (99's first 24, in 40 x 34 cells, two rows of 12), every end also as a
    point in a row under them."""
    ops = [begin(), ('MATS', ID, ID, ID), VP3]
    r = XS(0x10500001); L = geo99()[:24]
    verts = []
    for i, (a, b, _) in enumerate(L):
        for j, (x, y) in enumerate((a, b)):
            c = 0xFF000000 | ((r() & 0x7F7F7F))
            d = cv3d(x, y, -0.5, c); d['n'] = N105[(2 * i + j) % 4]; d['u'] = d['v'] = 0.0
            verts.append(d)
    for row, (mode, tex) in enumerate(((1, 0), (1, 1), (2, 0))):
        ops += [('OFFSET', 2048 - 240, 2048 - 136 - 90 * row), ('LIGHT', mode, C.COEF95), ('TEX', tex),
                ('DRAW', GU_LINES, FMT_TCNV3D, verts, None)]
        for t, v in enumerate(verts):
            sx, sy = v['x'] * 256 + 240, 136 - v['y'] * 256        # the vertex's screen position at offset 0
            tx, ty = 4 + 4 * t, 70
            ddx, ddy = tx - math.floor(sx), ty - math.floor(sy)
            ops += [('OFFSET', 2048 - 240 - ddx, 2048 - 136 - 90 * row - ddy),
                    ('DRAW', GU_POINTS, FMT_TCNV3D, [v], None)]
    ops += [('TEX', 0), ('LIGHT', 0, 0.0), ('OFFSET', 2048 - 240, 2048 - 136), ('END', 0)]
    return ops

def sc106():
    """99's lines with alpha ends of their own, the alpha plane read into the stencil by
    255 alpha-test passes (colour14's LADDER)."""
    r = XS(0x10600001); L = []
    for (a, b, cs) in geo99():
        al = ends(r, 1)
        L.append((a, b, [(cs[0] & 0xFFFFFF) | ((al[0] & 255) << 24), (cs[1] & 0xFFFFFF) | ((al[1] & 255) << 24)]))
    d = lines_draw(L)
    return [begin(), ('LADDER', d[1], d[2], d[3], 1, 255), ('END', 0)]

def sc107():
    """99 through an identity 3D transform: the same pixels, if 3D lines are through-mode
    lines at the projected ends."""
    return [begin(), ('MATS', ID, ID, ID), VP3, ('DRAW', GU_LINES, FMT_CV3D, verts3d(geo99()), None), ('END', 0)]

def sc108():
    """99 with clip w 1, 2 or 4 at the ends (screen positions unchanged): equal to 99 if
    colour runs straight in screen space along a line, as across a triangle."""
    verts = []
    for i, (a, b, cs) in enumerate(geo99()):
        wa, wb = ((1, 2), (2, 1), (1, 4), (4, 1))[i % 4]
        verts += [cv3d(a[0], a[1], -float(wa), cs[0], wa), cv3d(b[0], b[1], -float(wb), cs[1], wb)]
    return [begin(), ('MATS', mat_persp(), ID, ID), VP3, ('DRAW', GU_LINES, FMT_CV3D, verts, None), ('END', 0)]

def sc109():
    """99 in through mode with depth ends from 2000 to 63000, depth test ALWAYS with writes
    on, and the whole depth buffer dumped: depth along lines in every direction."""
    r = XS(0x10900001); verts = []
    for (a, b, cs) in geo99():
        za, zb = 2000 + r() % 61000, 2000 + r() % 61000
        verts += [v2(a[0], a[1], cs[0], float(za)), v2(b[0], b[1], cs[1], float(zb))]
    return [begin(), ('DEPTH', 1, GU_ALWAYS), ('DRAW', GU_LINES, FMT_CV2D, verts, None), ('END', 1)]

SCENES = [
    (99, sc99, 'lineoct', 'line colours: 96 directions, random sixteenths'),
    (100, sc100, 'linetie', 'line colours: 45-degree lines at 64 offsets, and a sixteenth either side'),
    (101, sc101, 'linestart', 'line colours: short lines in 8 directions, starts swept around the pixel'),
    (102, sc102, 'lineflat', 'line colours: flat-shaded lines and strips, through mode and 3D'),
    (103, sc103, 'linestrip', 'line colours: smooth strips, joints at many angles'),
    (104, sc104, 'linefog', 'line colours: fog along 3D lines, and each end as a point'),
    (105, sc105, 'linelit', 'line colours: lit lines, primary and secondary, with end points'),
    (106, sc106, 'linealpha', 'line colours: alpha along lines read by 255 alpha-test passes'),
    (107, sc107, 'line3d', 'line colours: scene 99 through an identity 3D transform'),
    (108, sc108, 'lineperspw', 'line colours: scene 99 with clip w 1, 2 and 4 at the ends'),
    (109, sc109, 'linedepth', 'line colours: scene 99 with depth ends, and the full depth dump'),
]
def scene(num):
    for n, fn, name, title in SCENES:
        if n == num: return dict(num=n, name=name, title=title, ops=fn())
    raise KeyError(num)
def all_scenes(): return [scene(n) for n, *_ in SCENES]

# =========================================================================== emit, sums
def emit(path):
    out = ['/* Generated by lines15.py emit: geprobe 15 scenes 99-109 (line colours) as command',
           ' * streams for c14_run() (colour14.py\'s format).  Do not edit; change lines15.py and',
           ' * run it again. */']
    steps = []
    for k, sc in enumerate(all_scenes()):
        w, nd, nv, vc = C.emit_scene(sc)
        crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
        steps.append((sc, len(w), nd, nv, vc, crc))
        out.append(f'\n/* scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
                   f' {len(w)} words, stream crc {crc:08X} */')
        out.append(f'static const w32 C15_S{k}[{len(w)}] = {{')
        for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
        out.append('};')
    out.append('\nstatic const struct c14_step C15_STEPS[] = {')
    for k, (sc, n, *_r) in enumerate(steps):
        out.append(f'    {{ {sc["num"]}, "{sc["name"]}", "{sc["title"]}", C15_S{k}, {n} }},')
    out.append('};\n#define C15_NSTEPS ((int)(sizeof C15_STEPS / sizeof C15_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text:
        open(path, 'w').write(text)
    return steps

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for sc, n, nd, nv, vc, crc in emit(os.path.join(td, 'c15.inc')):
            m = re.search(rf'scene {sc["num"]}: line colours.*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want
            ok &= good
            print(f'scene {sc["num"]} {sc["name"]}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

# =========================================================================== predict, compare, check
def read_frame(path):
    return np.fromfile(path, '<u4').reshape(272, 480).astype(np.int64)
def read_depth(path):
    sys.path.insert(0, HERE)
    import readout
    return readout.depth_full(path).astype(np.int64)

def predict(outdir, rule='cur'):
    os.makedirs(outdir, exist_ok=True)
    for sc in all_scenes():
        F = render(sc, rule).F
        F.px.astype('<u4').tofile(os.path.join(outdir, f'ge_{sc["num"]:02d}_{sc["name"]}.raw'))

def scene_diff(sc, frame_px, rule, depth=None):
    m = render(sc, rule)
    P = m.F.px
    mask = (m.F.cover > 0)
    d = ((P & 0xFFFFFF) != (frame_px & 0xFFFFFF)) & mask
    if sc['num'] == 106:                         # the ladder: alpha byte is the plane
        d = ((P & 0xFFFFFFFF) != (frame_px & 0xFFFFFFFF)) & mask
    extra = ((frame_px & 0xFFFFFF) != 0) & ~mask & ((m.F.px & 0xFFFFFF) == 0)
    res = int(d.sum()) + int(extra.sum())
    if depth is not None and sc['num'] == 109:
        res_d = int(((m.F.depth != depth) & mask).sum())
        return res, res_d
    return res, None

def compare(dumpdir, nums=None, rules=None):
    for sc in all_scenes():
        if nums and sc['num'] not in nums: continue
        f = os.path.join(dumpdir, f'ge_{sc["num"]:02d}_{sc["name"]}.raw')
        if not os.path.exists(f): print(sc['num'], sc['name'], 'missing'); continue
        px = read_frame(f)
        dep = None
        if sc['num'] == 109:
            dpath = os.path.join(dumpdir, f'ge_109_{sc["name"]}_depthfull.bin')
            if os.path.exists(dpath): dep = read_depth(dpath)
        out = []
        for r in rules or RIVALS:
            n, nd = scene_diff(sc, px, r, dep)
            out.append(f'{r}:{n}' + (f'/depth {nd}' if nd is not None else ''))
        print(f'{sc["num"]} {sc["name"]}: ' + '  '.join(out))

def check():
    """Design checks: how many pixels each rival changes against 'cur' and against 'H',
    per scene, and pixels written by more than one line where that matters."""
    for sc in all_scenes():
        base = {r: render(sc, r).F for r in ('cur', 'H')}
        over = int((base['cur'].cover > 1).sum())
        row = []
        for r in RIVALS:
            F = render(sc, r).F
            row.append(f'{r}: cur {int(((F.px ^ base["cur"].px) & 0xFFFFFFFF != 0).sum())} H {int(((F.px ^ base["H"].px) & 0xFFFFFFFF != 0).sum())}')
        print(f'{sc["num"]} {sc["name"]}: covered {int((base["cur"].cover > 0).sum())}, overdrawn {over}')
        print('   ' + ' | '.join(row))

def selfcheck(dumpdir):
    def fr(n):
        name = [s for s in SCENES if s[0] == n][0][2]
        return read_frame(os.path.join(dumpdir, f'ge_{n:02d}_{name}.raw')) & 0xFFFFFF
    a, b, c = fr(99), fr(107), fr(108)
    print('107 == 99 (rgb px differing):', int((a != b).sum()))
    print('108 vs 99 (rgb px differing):', int((a != c).sum()))
    return int((a != b).sum()) == 0

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        path = argv[2] if len(argv) > 2 else os.path.join(HERE, 'c15_data.inc')
        for sc, n, nd, nv, vc, crc in emit(path):
            print(f'scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'predict': predict(argv[2], argv[3] if len(argv) > 3 else 'cur'); return 0
    if cmd == 'compare':
        nums = [int(a) for a in argv[3:] if a.isdigit()] or None
        rules = [a for a in argv[3:] if not a.isdigit()] or None
        compare(argv[2], nums, rules); return 0
    if cmd == 'check': check(); return 0
    if cmd == 'selfcheck': return 0 if selfcheck(argv[2]) else 1
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
