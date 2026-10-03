#!/usr/bin/env python3
"""geprobe 17 (scenes 115-119): point and spot lights, read whole where they can be.

Scene 35's 10 pixels that psprecomp gets wrong are all one step low, in the two grids
that raise something to a power (a spot of exponent 4, a specular of coefficient 8),
each where the power lands just below a byte boundary. Either the GE's power function
or what goes into it (normalised vectors, their dot product) comes out a hair higher
than psprecomp's floats; a power magnifies a small error in x, so both are suspects.
These scenes take them apart:

  115 spotcut   L.D whole. The spot cutoff is a threshold on L.D (the vertex-to-light
                direction against the spot direction), so for each geometry the probe
                searches the cutoff's 24-bit code on the PSP itself, a pass at a time,
                reading back whether each point came out lit (main.c l17_search). The
                result is the largest cutoff that still lights the point: L.D on the
                cutoff's grid. Batches: rsqrt (vertex at the origin, D = +z, light at
                (a/128, b/128, 1) 2^j, so L.D is 1/|p| itself), spotdir (L = +z, D of
                every length: does the GE normalise D?), general, and scene 35's own
                vertices and lights with D = +z.
  116 spotpow   the spot factor as a byte, floor(256 (L.D)^e), for 115's geometries at
                exponents chosen to land near a byte boundary; and scene 35's two spots
  117 diffpow   N.L: plain diffuse as a byte and powered diffuse (N = 115's D, so N.L is
                115's L.D if N is normalised the way D is)
  118 specpow   the specular, (N.H)^c, H = normalise(L + (0,0,1)); and scene 35's
                specular quadrant
  119 atten     attenuation as a byte, 1/(k0 + k1 d + k2 d^2), d = |light - vertex|

Every point goes to clip (0, 0, 0, 1) through a zero projection (w row 0 0 0 1), so the
screen offset alone puts it on its own pixel, two apart; lighting still sees the vertex
in eye space as given (view and world identity). White light and material, no ambient,
so a lit point's grey is the byte asked about. The clear is red, which no white light
can give, so an undrawn point is told from an unlit (black) one. Every input is exact in
16 significant bits (libgu sends a float's top 24 bits).

    lights17.py emit [OUT.inc]        write l17_data.inc (115's table, 116-119's streams)
    lights17.py sums DUMPDIR          the logged CRCs against these inputs
    lights17.py compare DUMPDIR [rule..]   115's codes and 116-119's bytes against a rule
    lights17.py check                 design checks: points, boundary nearness
Rules: 'cur' is psprecomp's float arithmetic (ge.c light_vertex, ge_pow), the reference
the probe's own run under psprecomp must reproduce.
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
from colour14 import XS, f32, bf

GU_POINTS = 0
GU_NORMAL_32BITF, GU_VERTEX_32BITF = 3 << 5, 3 << 7
VTYPE = GU_NORMAL_32BITF | GU_VERTEX_32BITF
GU_DIRECTIONAL, GU_POINTLIGHT, GU_SPOTLIGHT = 0, 1, 2
GU_DIFFUSE, GU_DIFFUSE_AND_SPECULAR, GU_POWERED_DIFFUSE = 2, 6, 8
CLEAR = 0xFF0000FF
PITCH, ACROSS = 2, 239

def f24(v):
    """v as a GE command float holds it: the top 24 bits (16 significant), toward zero."""
    return struct.unpack('<f', struct.pack('<I', bf(f32(v)) >> 8 << 8))[0]
def code(v): return bf(v) >> 8
def uncode(c): return struct.unpack('<f', struct.pack('<I', c << 8))[0]
def slot(i): return 1 + PITCH * (i % ACROSS), 1 + PITCH * (i // ACROSS)
def V3(x, y, z): return (f24(x), f24(y), f24(z))
def unit(v):
    l = math.sqrt(sum(c * c for c in v)); return [c / l for c in v]

CUT_LO, CUT_HI = code(1 / 32), code(16.0)       # the search's starting bracket

# =========================================================================== geometry
class Geo:
    __slots__ = ('batch', 'v', 'n', 'p', 'd')
    def __init__(s, batch, v, n, p, d): s.batch, s.v, s.n, s.p, s.d = batch, V3(*v), V3(*n), V3(*p), V3(*d)
    def words(s): return [bf(c) for c in s.v + s.n + s.p + s.d]

def rand_dir(r, near=None, max_deg=90.0):
    """A random unit vector, within max_deg of `near` when given."""
    while True:
        v = [(r() / 2**31 - 1.0) for _ in range(3)]
        l = math.sqrt(sum(c * c for c in v))
        if 0.1 < l <= 1.0:
            v = [c / l for c in v]
            if near is None or sum(a * b for a, b in zip(v, near)) >= math.cos(math.radians(max_deg)): return v

SC35 = [((-3.0, 1.7), (-2.55, 1.35, -4.5)), ((3.0, 1.7), (3.45, 1.35, -3.0)),
        ((-3.0, -1.7), (-2.55, -2.05, -4.5)), ((3.0, -1.7), (3.45, -2.05, -3.0))]
def sc35_points(q):
    (cx, cy), lp = SC35[q]
    return [((f32(cx + (c - 9.5) * 0.25), f32(cy + (r - 4.5) * 0.25), -6.0), lp)
            for r in range(10) for c in range(20)]

BATCHES = ['rsqrt', 'spotdir', 'general', 'sc35']
def geometries():
    G = []
    r = XS(0x11500001)
    # b0 rsqrt: L.D = p_z / |p| = 1/sqrt(1 + (a^2 + b^2)/16384), each p scaled by 2^j
    seen = set()
    while len(G) < 1400:
        a, b = r() % 222, r() % 222
        if a * a + b * b >= 3 * 16384 or (a, b) in seen: continue
        seen.add((a, b))
        j = (-4, -2, -1, 0, 1, 3)[r() % 6]
        sx, sy = (1 if r() & 1 else -1), (1 if r() & 1 else -1)
        p = (sx * a / 128 * 2**j, sy * b / 128 * 2**j, 2**j)
        G.append(Geo(0, (0, 0, 0), p, p, (0, 0, 1)))
    G[0] = Geo(0, (0, 0, 0), (0, 0, 1), (0, 0, 1), (0, 0, 1))          # |p| = 1 exactly
    # b1 spotdir: L = +z exactly, D of length 0.3 to 3 within 75 degrees of +z
    for k in range(400):
        j = (-2, 0, 2, 4)[k % 4]
        u = rand_dir(r, (0, 0, 1), 75.0)
        ln = 0.3 * 10 ** ((r() % 1000) / 1000)
        if k < 8: u, ln = [0, 0, 1], (0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0)[k]
        G.append(Geo(1, (0, 0, 0), (0, 0, 1), (0, 0, 2**j), [c * ln for c in u]))
    # b2 general: a vertex anywhere in [-2, 2]^3, a light 1/4 to 8 away, D within 70 degrees of L
    for k in range(1200):
        v = [(r() / 2**32 * 4 - 2) for _ in range(3)]
        u = rand_dir(r)
        dist = 2 ** ((r() % 5000) / 1000 - 2)
        p = [v[i] + u[i] * dist for i in range(3)]
        d = rand_dir(r, u, 70.0)
        G.append(Geo(2, v, u, p, d))
    # b3: scene 35's four grids, D = +z, the normal +z as scene 35 has it
    for q in range(4):
        for v, lp in sc35_points(q):
            G.append(Geo(3, v, (0, 0, 1), lp, (0, 0, 1)))
    return G

# =========================================================================== psprecomp's float arithmetic
def lit_mul(a, b): return ((2 * a + 1) * (2 * b + 1)) >> 10
def lit_byte(x):
    if not x > 0: return 0
    q = f32(x * 256.0); return 255 if q >= 255 else int(q)
def ge_pow(x, k):
    if not x > 0: return 0.0
    m, e = math.frexp(x)
    y = f32(k * f32((e - 1) + f32(2 * m - 1)))
    if not y > -126: return 0.0
    n = math.floor(y); return math.ldexp(1.0 + f32(y - n), n)
def fdot(a, b): return f32(f32(f32(a[0] * b[0]) + f32(a[1] * b[1])) + f32(a[2] * b[2]))
def fnorm(v):
    l = f32(math.sqrt(fdot(v, v)))
    return [f32(c / l) for c in v] if l > 1e-20 else list(v)

class Light:
    """Light 0 as the LGT op sets it."""
    __slots__ = ('type', 'comps', 'p', 'd', 'exp', 'cut', 'k', 'coef', 'dif', 'spec')
    def __init__(s, type, comps, p, d=(0, 0, 1), exp=1.0, cut=f24(1 / 64), k=(1.0, 0.0, 0.0), coef=1.0,
                 dif=0xFFFFFF, spec=0x000000):
        s.type, s.comps, s.p, s.d = type, comps, V3(*p), V3(*d)
        s.exp, s.cut, s.k, s.coef = f24(exp), f24(cut), V3(*k), f24(coef)
        s.dif, s.spec = dif, spec
    def key(s): return (s.type, s.comps, s.p, s.d, s.exp, s.cut, s.k, s.coef, s.dif, s.spec)
    def words(s):
        return ([s.type, s.comps] + [bf(c) for c in s.p + s.d] + [bf(s.exp), bf(s.cut)]
                + [bf(c) for c in s.k] + [bf(s.coef), s.dif, s.spec])

def cur_parts(L, v, n):
    """psprecomp's (ge.c light_vertex) factors for one light: spot x, the spot, attenuation,
    N.L and N.H, all in float32."""
    kind = 2 if L.comps == 8 else (1 if L.comps == 6 else 0)
    n = fnorm(n)
    if L.type == 0: Lv = list(L.p); att = 1.0
    else:
        Lv = [f32(L.p[i] - v[i]) for i in range(3)]
        d = f32(math.sqrt(fdot(Lv, Lv)))
        a = f32(f32(L.k[0] + f32(L.k[1] * d)) + f32(f32(L.k[2] * d) * d))
        att = f32(1.0 / a) if a != 0 else 1.0
    Lv = fnorm(Lv)
    sx = None; spot = 1.0
    if L.type == 2:
        D = fnorm(list(L.d))
        sx = fdot(Lv, D)
        spot = ge_pow(sx, L.exp) if sx >= L.cut else None
    ndl = fdot(n, Lv)
    ndh = None
    if kind == 1 and ndl >= 0:
        ndh = fdot(n, fnorm([Lv[0], Lv[1], f32(Lv[2] + 1.0)]))
    return dict(kind=kind, sx=sx, spot=spot, att=att, ndl=ndl, ndh=ndh)

def cur_byte(L, v, n):
    """The grey a white light and material give the point (red channel), psprecomp's way."""
    P = cur_parts(L, v, n)
    if P['spot'] is None: return 0
    kind = P['kind']
    dfac = P['ndl'] if P['ndl'] > 0 else 0.0
    if kind == 2 and dfac > 0: dfac = ge_pow(dfac, L.coef)
    sfac = ge_pow(P['ndh'], L.coef) if P['ndh'] is not None else 0.0
    vd, vs = lit_byte(dfac), lit_byte(sfac)
    va = 255 if P['att'] >= 1.0 else lit_byte(P['att'])
    vsp = 255 if P['spot'] >= 1.0 else lit_byte(P['spot'])
    ld, ls = L.dif & 0xFF, L.spec & 0xFF
    t = lit_mul(vd, lit_mul(ld, 255)); ts = lit_mul(vs, lit_mul(ls, 255))
    return min(255, lit_mul(vsp, lit_mul(va, t)) + lit_mul(vsp, lit_mul(va, ts)))

def cur_code(g):
    """Scene 115 under psprecomp: the largest cutoff code at or below its float L.D."""
    L = Light(GU_SPOTLIGHT, GU_DIFFUSE, g.p, g.d)
    x = cur_parts(L, g.v, g.n)['sx']
    return code(x) if x > 0 else 0

# =========================================================================== the static scenes
P0 = [[0.0] * 4 for _ in range(3)] + [[0.0, 0.0, 0.0, 1.0]]
ID = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
EXPS = [0.5, 0.75, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0, 12.0, 16.0, 20.0, 24.0, 32.0, 48.0, 64.0]

def near_boundary(val, exps, x, r, n=2):
    """Up to n of exps (and random ones) where 256 x^e lands nearest a byte boundary."""
    cand = list(exps) + [f24(0.25 + (r() % 64000) / 1000) for _ in range(12)]
    scored = []
    for e in cand:
        y = 256 * x ** e
        if y < 2 or y > 254.5: continue
        f = y - math.floor(y)
        scored.append((min(f, 1 - f), e))
    scored.sort()
    return [e for _, e in scored[:n]]

class Scene:
    def __init__(s, num, name, title): s.num, s.name, s.title, s.pts = num, name, title, []
    def add(s, batch, L, v, n): s.pts.append(dict(batch=batch, L=L, v=V3(*v), n=V3(*n)))

def sc116(G):
    S = Scene(116, 'spotpow', 'spot factor as a byte: (L.D)^e, 115\'s geometries')
    r = XS(0x11600001)
    for gi, g in enumerate(G):
        if g.batch not in (0, 2): continue
        x = f32(g.p[2] / math.sqrt(sum(c * c for c in g.p))) if g.batch == 0 else \
            sum(a * b for a, b in zip(unit([g.p[i] - g.v[i] for i in range(3)]), unit(g.d)))
        for e in near_boundary(None, EXPS, x, r):
            S.add(g.batch, Light(GU_SPOTLIGHT, GU_DIFFUSE, g.p, g.d, exp=e), g.v, g.n)
    for q, (e, cut) in ((1, (4.0, 0.9)), (3, (1.5, 0.5))):       # scene 35's spots, as they were
        for v, lp in sc35_points(q):
            k = (1.0, 0.0, 0.0) if q == 1 else (0.5, 0.3, 0.1)
            S.add(3, Light(GU_SPOTLIGHT, GU_DIFFUSE, lp, (0, 0, 1), exp=e, cut=cut, k=k), v, (0, 0, 1))
    return S

def sc117(G):
    S = Scene(117, 'diffpow', 'N.L: plain diffuse and powered diffuse as bytes')
    r = XS(0x11700001)
    for g in G:
        if g.batch != 2: continue
        S.add(0, Light(GU_POINTLIGHT, GU_DIFFUSE, g.p), g.v, g.d)            # N = D: plain N.L
        x = sum(a * b for a, b in zip(unit([g.p[i] - g.v[i] for i in range(3)]), unit(g.d)))
        for e in near_boundary(None, EXPS, x, r):
            S.add(1, Light(GU_POINTLIGHT, GU_POWERED_DIFFUSE, g.p, coef=e), g.v, g.d)
    for g in G:                                                              # rsqrt geometries, N = +z
        if g.batch == 0: S.add(2, Light(GU_POINTLIGHT, GU_DIFFUSE, g.p), g.v, (0, 0, 1))
    return S

def sc118(G):
    S = Scene(118, 'specpow', 'specular as a byte: (N.H)^c, H = normalise(L + (0,0,1))')
    r = XS(0x11800001)
    for g in G:
        if g.batch != 2: continue
        Lu = unit([g.p[i] - g.v[i] for i in range(3)])
        if Lu[2] < -0.5: continue
        H = unit([Lu[0], Lu[1], Lu[2] + 1])
        n = rand_dir(r, H, 35.0)
        x = sum(a * b for a, b in zip(n, H))
        for e in near_boundary(None, EXPS, x, r):
            S.add(0, Light(GU_POINTLIGHT, GU_DIFFUSE_AND_SPECULAR, g.p, coef=e, dif=0, spec=0xFFFFFF), g.v, n)
    for g in G:                                                              # rsqrt geometries, N = +z, c = 1 and 8
        if g.batch == 0:
            for c in (1.0, 8.0):
                S.add(1, Light(GU_POINTLIGHT, GU_DIFFUSE_AND_SPECULAR, g.p, coef=c, dif=0, spec=0xFFFFFF), g.v, (0, 0, 1))
    for v, lp in sc35_points(2):                                             # scene 35's specular grid
        S.add(2, Light(GU_POINTLIGHT, GU_DIFFUSE_AND_SPECULAR, lp, coef=8.0, dif=0, spec=0xFFFFFF), v, (0, 0, 1))
    return S

def sc119(G):
    S = Scene(119, 'atten', 'attenuation as a byte: 1/(k0 + k1 d + k2 d^2)')
    r = XS(0x11900001)
    for k in range(1600):
        v = [(r() / 2**32 * 2 - 1) for _ in range(3)]
        u = rand_dir(r)
        dist = 2 ** ((r() % 6000) / 1000 - 3)                              # 1/8 to 8
        p = [v[i] + u[i] * dist for i in range(3)]
        b = k % 4
        target = 2 ** -((r() % 7000) / 1000)                                # attenuation 1/128 to 1
        if b == 0: kk = (1 / target, 0, 0)
        elif b == 1: kk = (0, 1 / (target * dist), 0)
        elif b == 2: kk = (0, 0, 1 / (target * dist * dist))
        else:
            w = [r() % 1000 + 1 for _ in range(3)]; t = sum(w)
            kk = (w[0] / t / target, w[1] / t / (target * dist), w[2] / t / (target * dist * dist))
        S.add(b, Light(GU_POINTLIGHT, GU_DIFFUSE, p, k=kk), v, u)
    for v, lp in sc35_points(0):                                             # scene 35's attenuated point light
        S.add(4, Light(GU_POINTLIGHT, GU_DIFFUSE, lp, k=(0.5, 0.3, 0.1)), v, (0, 0, 1))
    return S

_G, _S = [], {}
def geo():
    if not _G: _G.extend(geometries())
    return _G
def scenes():
    if not _S:
        for fn in (sc116, sc117, sc118, sc119):
            S = fn(geo()); _S[S.num] = S
    return [_S[k] for k in sorted(_S)]

# =========================================================================== emission
def vbytes(v, n): return struct.pack('<6f', *(n + v))

def scene_ops(S):
    ops = [('BEGIN', 3, CLEAR), ('MATS', P0, ID, ID), ('LMODE', 1)]
    last = None
    for i, pt in enumerate(S.pts):
        x, y = slot(i)
        ops.append(('OFFSET', 2048 - x, 2048 - y))
        if pt['L'].key() != last: ops.append(('LGT', pt['L'].words())); last = pt['L'].key()
        ops.append(('RAW', GU_POINTS, VTYPE, 1, vbytes(pt['v'], pt['n'])))
    ops += [('LMODE', 0), ('OFFSET', 2048 - 240, 2048 - 136), ('END', 0)]
    return ops

def emit_scene(S):
    OP = C.OP
    w = []; vcrc = 0; nd = 0; nv = 0
    def op(name, args):
        w.append(OP[name] << 24 | len(args)); w.extend(a & 0xFFFFFFFF for a in args)
    body = []
    for o in scene_ops(S):
        k = o[0]
        if k == 'BEGIN': body.append(('BEGIN', [o[1], o[2]]))
        elif k == 'END': body.append(('END', [o[1]]))
        elif k == 'MATS': body.append(('MATS', [7] + C.mem16(o[1]) + C.mem16(o[2]) + C.mem16(o[3])))
        elif k == 'LMODE': body.append(('LMODE', [o[1]]))
        elif k == 'LGT': body.append(('LGT', o[1]))
        elif k == 'OFFSET': body.append(('OFFSET', [o[1], o[2]]))
        elif k == 'RAW':
            prim, vtype, cnt, vb = o[1:5]
            vcrc = zlib.crc32(vb, vcrc); nd += 1; nv += cnt
            body.append(('DRAW', [prim, vtype, cnt, len(vb), 0] + C.words(vb)))
        else: raise ValueError(k)
    op(*body[0])
    op('SUMS', [nd, nv, vcrc & 0xFFFFFFFF])
    for b in body[1:]: op(*b)
    return w, nd, nv, vcrc & 0xFFFFFFFF

def geo_crc(G):
    return zlib.crc32(struct.pack(f'<{12 * len(G)}I', *[x for g in G for x in g.words()])) & 0xFFFFFFFF

def emit(path):
    G = geo()
    out = ['/* Generated by lights17.py emit: geprobe 17 scenes 115-119 (point and spot lights).',
           ' * L17_GEO is scene 115\'s table (vertex, normal, light position, spot direction, as',
           ' * float bits); C17_S* are 116-119\'s command streams for c14_run(). Do not edit; change',
           ' * lights17.py and run it again. */',
           f'\n#define L17_NGEO {len(G)}',
           f'#define L17_CUT_LO 0x{CUT_LO:06X}u     /* 1/32: every geometry lit */',
           f'#define L17_CUT_HI 0x{CUT_HI:06X}u     /* 16: none */',
           f'/* geometry crc {geo_crc(G):08X} */',
           f'static const w32 L17_GEO[{len(G)}][12] = {{']
    for g in G: out.append('    { ' + ', '.join(f'0x{x:08X}u' for x in g.words()) + ' },')
    out.append('};')
    steps = []
    for k, S in enumerate(scenes()):
        w, nd, nv, vc = emit_scene(S)
        crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
        steps.append((S, len(w), nd, nv, vc, crc))
        out.append(f'\n/* scene {S.num} {S.name}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
                   f' {len(w)} words, stream crc {crc:08X} */')
        out.append(f'static const w32 C17_S{k}[{len(w)}] = {{')
        for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
        out.append('};')
    out.append('\nstatic const struct c14_step C17_STEPS[] = {')
    for k, (S, n, *_r) in enumerate(steps):
        out.append(f'    {{ {S.num}, "{S.name}", "{S.title}", C17_S{k}, {n} }},')
    out.append('};\n#define C17_NSTEPS ((int)(sizeof C17_STEPS / sizeof C17_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text:
        open(path, 'w').write(text)
    return steps

# =========================================================================== reading a dump
def read_search(dumpdir):
    """Scene 115's results: per geometry (cutoff code, flags, lit byte)."""
    raw = np.fromfile(os.path.join(dumpdir, 'ge_115_spotcut.bin'), '<u4').reshape(-1, 2)
    return [(int(a) & 0xFFFFFF, int(a) >> 24, int(b)) for a, b in raw]

def read_scene(dumpdir, S):
    """Each point's grey (green channel), or -1 where the clear (red) shows."""
    F = np.fromfile(os.path.join(dumpdir, f'ge_{S.num}_{S.name}.raw'), '<u4').reshape(272, 480) & 0xFFFFFF
    out = []
    for i in range(len(S.pts)):
        x, y = slot(i); c = int(F[y, x])
        out.append(-1 if c == 0x0000FF else (c >> 8) & 0xFF)
    return out

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    m = re.search(r'scene 115: .*?\n\s+(\d+) geometries, input crc ([0-9A-F]+)', log)
    want = (str(len(geo())), f'{geo_crc(geo()):08X}')
    good = bool(m) and m.groups() == want; ok &= good
    print('scene 115 spotcut: ' + ('log MATCHES' if good else f'log DIFFERS: {m.groups() if m else "no line"} against {want}'))
    with tempfile.TemporaryDirectory() as td:
        for S, n, nd, nv, vc, crc in emit(os.path.join(td, 'l17.inc')):
            m = re.search(rf'scene {S.num}: .*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want; ok &= good
            print(f'scene {S.num} {S.name}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

def compare(dumpdir, rules=None):
    rules = rules or ['cur']
    G = geo(); R = read_search(dumpdir)
    print('115 spotcut:')
    for b, name in enumerate(BATCHES):
        idx = [i for i, g in enumerate(G) if g.batch == b]
        fl = sum(1 for i in idx if R[i][1])
        line = f'   b{b} {name}: {len(idx)} geometries, {fl} flagged; code matches'
        for rule in rules:
            if rule == 'cur': line += f' cur:{sum(R[i][0] == cur_code(G[i]) for i in idx)}'
        print(line)
    for S in scenes():
        got = read_scene(dumpdir, S)
        by = {}
        for i, pt in enumerate(S.pts):
            st = by.setdefault(pt['batch'], {'n': 0, 'absent': 0, **{r: 0 for r in rules}})
            st['n'] += 1
            if got[i] < 0: st['absent'] += 1; continue
            for rule in rules:
                if rule == 'cur': st[rule] += cur_byte(pt['L'], pt['v'], pt['n']) == got[i]
        print(f'{S.num} {S.name}:')
        for b, st in sorted(by.items()): print(f'   b{b}:', st)

def check():
    G = geo()
    print('115:', len(G), 'geometries;', {BATCHES[b]: sum(g.batch == b for g in G) for b in range(4)})
    xs = [cur_parts(Light(GU_SPOTLIGHT, GU_DIFFUSE, g.p, g.d), g.v, g.n)['sx'] for g in G]
    print('   L.D (cur) min %.4f max %.4f; below 1/16: %d; over 16: %d' %
          (min(xs), max(xs), sum(x < 1 / 16 for x in xs), sum(x > 16 for x in xs)))
    for S in scenes():
        near = 0; by = {}
        for pt in S.pts:
            P = cur_parts(pt['L'], pt['v'], pt['n'])
            by[pt['batch']] = by.get(pt['batch'], 0) + 1
        last = slot(len(S.pts) - 1)
        print(f'{S.num} {S.name}: {len(S.pts)} points {by}, last slot {last}')

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        steps = emit(argv[2] if len(argv) > 2 else os.path.join(HERE, 'l17_data.inc'))
        print(f'scene 115 spotcut: {len(geo())} geometries, input crc {geo_crc(geo()):08X}')
        for S, n, nd, nv, vc, crc in steps:
            print(f'scene {S.num} {S.name}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'compare': compare(argv[2], argv[3:] or None); return 0
    if cmd == 'check': check(); return 0
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
