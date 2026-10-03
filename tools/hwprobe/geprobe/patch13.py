#!/usr/bin/env python3
"""geprobe 13 (scenes 67-82): Bezier and spline patch positions read through depth.

Self-contained generator, mirror and readout for the patch scenes. Every scene step is
built here, item by item, with the generator order of the probe's spec (xorshift32 from
seed(scene, batch), integer half-pixel placement, first-fit-decreasing shelves), and
written to patch13_data.inc as one u32 command stream per step, which main.c's p13_run()
replays: matrices, viewport z, patch division and primitive, the item records, every
control vertex blob (float, s16, s8, indexed, morphed, skinned), the plain cal-band and
anchor points, and the 75 b9 read-back. The C side draws nothing of its own, so the
streams ARE the inputs: each step logs

    <n> items, <s> samples, control crc C, item crc I, stream crc S

and `check` compares those with this file's copy before reading any pixel.

The mirror arithmetic (ge_mul/ge_sum/ge_rcp16, the LA lerp and its rivals, de Boor
parameters) is the spec's reference (geprobe 12 / ge.c); `predict` gives every point's
depth class, depth and pixel under LA v-first with input cut (LAv).

    patch13.py emit                   write patch13_data.inc (and print each step's CRCs)
    patch13.py check <dir> [scene..]  log CRCs, then per batch: points drawn on their own
                                      predicted pixel vs expected, drops, neg/edge fates,
                                      stray pixels, depth agreement with LAv
    patch13.py points <dir> <scene>   one line per point: class, depth, pixel, the reading
<dir> is the folder holding geprobe.txt and the ge_NN_*.raw / *_depthfull.bin dumps.
"""
import collections, math, os, re, struct, sys, zlib
import numpy as np

# =========================================================================== arithmetic
import numpy as np

# ---------------------------------------------------------------- RNG, values
class R:
    def __init__(s, seed): s.x = seed & 0xFFFFFFFF; s.n = 0
    def __call__(s):
        x = s.x
        x ^= (x << 13) & 0xFFFFFFFF; x ^= x >> 17; x ^= (x << 5) & 0xFFFFFFFF
        s.x = x; s.n += 1; return x
def fb(bits): return struct.unpack('<f', struct.pack('<I', bits & 0xFFFFFFFF))[0]
def bf(f): return struct.unpack('<I', struct.pack('<f', f))[0]
def mk(s, e, m): return fb(((s & 1) << 31) | ((e + 127) << 23) | (m & 0x7FFFFF))
def mk16(s, e, m15): return mk(s, e, (m15 & 0x7FFF) << 8)
def r16(r): return 0x200 + r() % 0x7C00
def seed(scene, batch): return (int(str(scene), 16) << 24) + 1 + (batch << 8)
def sig16(v):
    """True if v has at most 16 significant bits (a GE matrix word holds it)."""
    if v == 0 or not math.isfinite(v): return v == 0
    m, e = math.frexp(abs(v)); x = m * 65536
    return x == int(x)

# --------------------------------------------------------- GE arithmetic
def ge_cut(v, bits=16, rnd=0):
    if v == 0 or not math.isfinite(v): return v
    m, e = math.frexp(v); x = math.ldexp(m, bits)
    x = math.floor(x + 0.5) if rnd else math.trunc(x)
    return math.ldexp(x, e - bits)
def ge_split(v):
    if not math.isfinite(v): return None
    if v == 0: return (0, 0)
    m, fe = math.frexp(abs(v)); s = int(m * 65536)
    return (-s if v < 0 else s, fe - 1)
def ge_mul(a, b):
    A, B = ge_split(a), ge_split(b)
    if A is None or B is None: return (0, None, a * b)
    if A[0] == 0 or B[0] == 0: return (0, -10**9, a * b)
    p = A[0] * B[0]
    q = -((-p) >> 15) if p < 0 else p >> 15
    return (q, A[1] + B[1], a * b)
def ge_sum(ts):
    if any(t[1] is None for t in ts): return sum(t[2] for t in ts)
    nz = [t for t in ts if t[0]]
    if not nz: return 0.0
    e = max(t[1] for t in nz); s = 0
    for q, te, _ in nz:
        d = e - te; m = abs(q); a = 0 if d >= 63 else m >> d
        s += -a if q < 0 else a
    if not s: return 0.0
    m = abs(s); sh = 0
    while (m >> sh) >= 65536: sh += 1
    m >>= sh
    return math.ldexp(-m if s < 0 else m, e - 15 + sh)
RCP = [131073,508,130056,500,129055,492,128072,486,127100,478,126144,470,125204,464,124275,456,123362,450,122462,444,121575,438,120701,432,119837,424,118987,418,118151,414,117324,408,116509,402,115705,396,114914,392,114131,386,113359,380,112600,376,111848,370,111108,366,110376,360,109655,356,108944,352,108240,346,107546,342,106862,338,106185,334,105518,330,104859,326,104207,322,103564,318,102928,314,102301,310,101681,306,101068,302,100464,300,99866,296,99274,292,98690,288,98114,286,97543,282,96978,278,96422,276,95870,272,95327,270,94787,266,94256,264,93728,260,93208,258,92692,254,92183,252,91681,250,91181,246,90689,244,90202,242,89718,238,89241,236,88770,234,88303,232,87839,228,87382,226,86929,224,86482,222,86038,220,85600,218,85165,216,84733,212,84308,210,83886,208,83469,206,83056,204,82647,202,82242,200,81840,198,81443,196,81050,194,80660,192,80276,192,79893,190,79514,188,79139,186,78767,184,78399,182,78034,180,77674,180,77316,178,76961,176,76609,174,76261,172,75915,170,75575,170,75235,168,74899,166,74565,164,74237,164,73909,162,73585,160,73265,160,72945,158,72629,156,72317,156,72006,154,71698,152,71394,152,71091,150,70790,148,70494,148,70198,146,69907,146,69616,144,69328,142,69043,142,68760,140,68480,140,68201,138,67926,138,67651,136,67379,134,67110,134,66842,132,66578,132,66314,130,66053,130,65794,128]
def ge_rcp16(w):
    if w == 0 or not math.isfinite(w): return math.inf
    m, e = math.frexp(abs(w)); sig = int(m * 65536) & 0x7FFF
    if sig == 0: return math.copysign(math.ldexp(1.0, 1 - e), w)
    t, l = sig >> 8, sig & 0xFF
    q = (128 * RCP[2 * t] - RCP[2 * t + 1] * l - 1) >> 8
    return math.copysign(math.ldexp(q, -15 - e), w)
def ge_over_w(c, w): return ge_cut(c * ge_rcp16(w))
def dot4(row, v): return ge_sum([ge_mul(row[k], v[k]) for k in range(4)])
def mat_mul(a, b): return [[dot4(a[i], [b[0][j], b[1][j], b[2][j], b[3][j]]) for j in range(4)] for i in range(4)]
def ident(): return [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
def wvp(P, V=None, W=None):
    V = V or ident(); W = W or ident()
    return mat_mul(mat_mul(P, V), W)
def depth(M, x, y, z, zs, zc=0.0):
    """Screen depth of model (x,y,z) through combined M (rows = clip x,y,z,w).
    Returns (depth or None if |clip z| >= w, clip z, w)."""
    v = [x, y, z, 1.0]
    cz = dot4(M[2], v); w = dot4(M[3], v)
    if w > 0 and abs(cz) == w: return 'edge', cz, w     # |clip z| = w exactly: cal slots 472-475 decide
    if not (abs(cz) < abs(w)) or w <= 0: return None, cz, w
    nz = ge_over_w(cz, w)
    d = math.floor(ge_sum([ge_mul(zs, nz), ge_mul(zc, 1.0)]))
    return d, cz, w
def screen_xy(M, x, y, z, xs=256.0, ys=-128.0):
    v = [x, y, z, 1.0]
    cx, cy, w = dot4(M[0], v), dot4(M[1], v), dot4(M[3], v)
    nx, ny = ge_over_w(cx, w), ge_over_w(cy, w)
    return 240 + xs * nx, 136 + ys * ny

# ---------------------------------------------------------------- sampling
def Tp_floor(i, d): return (256 * i) // d if 2 * i <= d else 256 - (256 * (d - i)) // d
def Tp(i, d):
    """The GE's sample parameter (set 14): a step of 256/d in 8.6 fixed point rounded up,
    mirrored about the middle. Tp_floor, the rule the spec was written with, is a step off
    at 68 of the 1128 (d, i) pairs scenes 67-82 read."""
    q = -(-16384 // d)
    return (i * q) >> 6 if 2 * i <= d else 256 - (((d - i) * q) >> 6)
def bez_samples(c, d):
    out = []
    for pc in range((c - 1) // 3):
        for i in range(1 if pc else 0, d + 1):
            t = Tp(i, d); out.append((3 * pc, [t] * 6, pc + t / 256))
    return out
def knots(c, edge):
    spans = c - 3; kn = []
    for k in range(c + 4):
        t = k - 3
        if (edge & 1) and t < 0: t = 0
        if (edge & 2) and t > spans: t = spans
        kn.append(t)
    return kn
def deboor_params(kn, k, T):
    a = []
    for j in (1, 2, 3):
        for idx in range(3, j - 1, -1):
            i = k - 3 + idx; ul = kn[i] - kn[k]; ur = kn[i + 4 - j] - kn[k]
            den = ur - ul; dl = T - 256 * ul; dr = 256 * ur - T
            a.append(dl // den if dl <= dr else 256 - dr // den)
    return a
def spl_samples(c, d, edge):
    kn = knots(c, edge); out = []
    for sp in range(c - 3):
        for i in range(1 if sp else 0, d + 1):
            t = Tp(i, d); k = sp + 3
            out.append((k - 3, deboor_params(kn, k, t), sp + t / 256))
    return out
def greville(c, edge):
    kn = knots(c, edge)
    return [(kn[k + 1] + kn[k + 2] + kn[k + 3]) / 3 for k in range(c)]
def samples(kind, c, d, edge=0):
    return bez_samples(c, d) if kind == 'b' else spl_samples(c, d, edge)

# ---------------------------------------------------------------- lerp models
def _rnd(x, how):
    if how == 'trunc': return np.trunc(x)
    if how == 'floor': return np.floor(x)
    if how == 'near': return np.sign(x) * np.floor(np.abs(x) + 0.5)      # half away from zero
    if how == 'even': return np.round(x)                                  # half to even
    if how == 'ceil': return np.ceil(x)
    raise ValueError(how)
def make_lerp(op='trunc', res='floor', copy=False, shared=False, fixed=None, exact=False):
    def lerp(p, q, a):
        a = a[..., None]
        if exact: return ((256 - a) * p + a * q) / 256
        if fixed is not None:
            g = 2.0 ** -fixed
            P = _rnd(p / g, op); Q = _rnd(q / g, op)
            return _rnd(((256 - a) * P + a * Q) / 256, res) * g
        m = np.maximum(np.abs(p), np.abs(q))
        if shared: m = np.broadcast_to(m.max(axis=-1, keepdims=True), m.shape)
        _, e = np.frexp(m)
        sc = np.ldexp(1.0, 16 - e)
        P = _rnd(p * sc, op); Q = _rnd(q * sc, op)
        out = _rnd(((256 - a) * P + a * Q) / 256, res) / sc
        out = np.where(m == 0, 0.0, out)
        if copy: out = np.where(a == 0, p, np.where(a == 256, q, out))
        return out
    return lerp
def deboor(P, A, lerp):
    P = list(P); n = 0
    for j in (1, 2, 3):
        Q = list(P)
        for idx in range(3, j - 1, -1):
            Q[idx] = lerp(P[idx - 1], P[idx], A[n]); n += 1
        P = Q
    return P[3]
def conv(C, how):
    """controls (..,3) float64 -> GE input. 'trunc' cut16, 'near' nearest16, 'keep'."""
    if how == 'keep': return C.copy()
    m, e = np.frexp(C)
    x = np.ldexp(m, 16)
    x = np.trunc(x) if how == 'trunc' else np.sign(x) * np.floor(np.abs(x) + 0.5)
    return np.ldexp(x, e - 16)
def cut16v(X):
    m, e = np.frexp(X); return np.ldexp(np.trunc(np.ldexp(m, 16)), e - 16)
def tess(C, su, sv, lerp, order):
    """C: (cv,cu,3). returns (nv,nu,3)."""
    fu = np.array([s[0] for s in su]); au = np.array([s[1] for s in su], float)
    fv = np.array([s[0] for s in sv]); av = np.array([s[1] for s in sv], float)
    AU = [au[None, :, n] for n in range(6)]; AV = [av[:, None, n] for n in range(6)]
    if order == 'u':
        rows = [deboor([C[fv[:, None] + r, fu[None, :] + q] for q in range(4)], AU, lerp) for r in range(4)]
        return deboor(rows, AV, lerp)
    cols = [deboor([C[fv[:, None] + r, fu[None, :] + q] for r in range(4)], AV, lerp) for q in range(4)]
    return deboor(cols, AU, lerp)

LA = make_lerp()
MODELS = {
    'LAv':      (make_lerp(copy=True), 'v', 'trunc'),     # the GE's rule (set 14): a = 0/256 copy their operand
    'LAv_nocopy': (LA, 'v', 'trunc'),
    'LAu':      (make_lerp(copy=True), 'u', 'trunc'),
    'LAv_opnear': (make_lerp(op='near'), 'v', 'trunc'),
    'LAv_opeven': (make_lerp(op='even'), 'v', 'trunc'),
    'LAv_opfloor': (make_lerp(op='floor'), 'v', 'trunc'),
    'LAv_restrunc': (make_lerp(res='trunc'), 'v', 'trunc'),
    'LAv_resnear': (make_lerp(res='near'), 'v', 'trunc'),
    'LAv_copy': (make_lerp(copy=True), 'v', 'trunc'),
    'LAv_xyz':  (make_lerp(shared=True), 'v', 'trunc'),
    'LAv_innear': (LA, 'v', 'near'),
    'EXv':      (make_lerp(exact=True), 'v', 'keep'),
    'FIX17':    (make_lerp(fixed=17, op='floor'), 'v', 'trunc'),
}
def run_model(name, C, su, sv):
    lerp, order, cv = MODELS[name]
    G = tess(conv(C, cv), su, sv, lerp, order)
    if name == 'EXv': G = cut16v(G)
    return G

# =========================================================================== scenes
X0, X1, Y0, Y1, GAP = 2, 478, 15, 263, 1
PRIM_POINTS, PRIM_LINE_STRIP, PRIM_TRIANGLE_STRIP = 0, 2, 4      # pspgu.h GU_POINTS etc.
def S(d):
    if d > 48: return 192 if d <= 85 else 384
    f = 256 // d; m = 0
    while 3 * 2 ** m * f < 512: m += 1
    return 3 * 2 ** m
D1 = list(range(48, 30, -1)) + [29, 27, 25, 23, 21, 19, 17, 13, 11, 9, 7, 5, 3]
D8 = [23, 21, 19, 17, 15, 13, 11, 9]
DL = [9, 11, 13, 15, 17, 19, 21, 23]
E23 = [-24, -20, -18, -17, -16, -15, -14, -12, -10, -8, -6, -4, -3, -2, -1, 0, 1, 2, 4, 6, 8, 10, 12]
EE = [-30, -24, -20, -16, -12, -8, -4, -2, -1, 0, 1, 3, 6, 10, 12]

def P_row(a=1.0, b=0.0, cx=0.0, cy=0.0, wz=0.0, ww=1.0, xr=(1.0, 0, 0, 0), yr=(0, 1.0, 0, 0)):
    return [list(map(float, xr)), list(map(float, yr)), [cx, cy, a, b], [0.0, 0.0, wz, ww]]
# read modes -> (P, zs, allowed classes)
# item-CRC read-mode codes (FRAME 11): R_e(0) 1, N_e 2, A+ 3, A- 4, S 5, W 6, half 7, special 8, R_e(1) 9
def RD_R(e, s): return dict(P=P_row(a=(-1) ** s * 2.0 ** -(e + 1)), zs=65536.0, ok={'full'}, code=9 if s else 1, par=e)
def RD_N(e): return dict(P=P_row(a=2.0 ** -(e + 1)), zs=-65536.0, ok={'full'}, code=2, par=e)
def RD_A(k, sg): return dict(P=P_row(a=sg * 2.0 ** k), zs=65536.0, ok={'full', 'lp', 'drop', 'neg', 'edge'}, code=3 if sg > 0 else 4, par=k)
def RD_S(p): return dict(P=P_row(a=2.0 ** (p - 1), b=0.5), zs=65536.0, ok={'full', 'lp', 'drop', 'edge'}, code=5, par=p)
def RD_SPECIAL(**kw):
    d = dict(zs=65536.0, ok={'full'}, code=8, par=0); d.update(kw); return d
def rd_code(rd): return rd.get('code', 8), rd.get('par', 0)

def n3(kind, c, edge):
    if kind == 'b': return list(range(c))
    kn = knots(c, edge); return [kn[k + 1] + kn[k + 2] + kn[k + 3] for k in range(c)]
class Item:
    def __init__(s, kind, cu, cv, du, dv, Z, rd, eu=0, ev=0, Su=None, Sv=None, xyr=None,
                 tag='', mkC=None, xs=256.0, W=None, V=None, offscreen_ok=False, w_extra=0,
                 cmode='id', colgrid=None, ident='exact', fmt='f', prim=PRIM_POINTS, shade=1, extra=None, code=None):
        s.kind, s.cu, s.cv, s.du, s.dv, s.Z, s.rd = kind, cu, cv, du, dv, Z, rd
        s.eu, s.ev, s.xyr, s.tag, s.mkC, s.xs, s.W, s.V = eu, ev, xyr, tag, mkC, xs, W, V
        s.offscreen_ok = offscreen_ok
        # Emission (FRAME 2, 8): colour mode ('id' the ID colour; 'blue' 0x80+cell index in batch over
        # colgrid's low 16 bits; 'full' colgrid as is, alpha forced), identification rule for the
        # readout, vertex format ('f' float CV, 's16abs', 's16anc', 's8anc', 'idx16', 'idx8',
        # 'morphAB', 'morphA2B2', 'skin'), patch primitive and shade model, format extras.
        s.cmode, s.colgrid, s.ident, s.fmt, s.prim, s.shade, s.extra = cmode, colgrid, ident, fmt, prim, shade, extra
        s.points = prim == PRIM_POINTS        # only POINTS cells are point samples
        s.late = False                        # 75 b9 C: drawn after the read-back
        s.Su = Su or (3 if du == 1 and cu == 4 and kind == 'b' and Sv is None and False else S(du))
        s.Sv = Sv or S(dv)
        s.su = samples(kind, cu, du, eu); s.sv = samples(kind, cv, dv, ev)
        s.nu = n3(kind, cu, eu); s.nv = n3(kind, cv, ev)        # 3*g_k, integers
        s.gu = [n / 3 for n in s.nu]; s.gv = [n / 3 for n in s.nv]
        s.padu0 = 1 if (kind == 's' and not (eu & 1)) else 0
        s.padu1 = 1 if (kind == 's' and not (eu & 2)) else 0
        s.padv0 = 1 if (kind == 's' and not (ev & 1)) else 0
        s.padv1 = 1 if (kind == 's' and not (ev & 2)) else 0
        spu = (cu - 1) // 3 if kind == 'b' else cu - 3
        spv = (cv - 1) // 3 if kind == 'b' else cv - 3
        s.w = s.Su * (spu + s.padu0 + s.padu1) + 1 + w_extra
        s.h = s.Sv * (spv + s.padv0 + s.padv1) + 1
        s.n = len(s.su) * len(s.sv)
        s.code, s.par = rd_code(rd)
        if code is not None: s.code, s.par = code
    def ctrl_xy(s, bx, by):
        # integer placement (FRAME 6): S/3 is a power of two, 3*g_k an integer
        assert s.Su % 3 == 0 and s.Sv % 3 == 0
        qu, qv = s.Su // 3, s.Sv // 3
        X = [bx + 0.5 + qu * (3 * s.padu0 + n) for n in s.nu]
        Y = [by + 0.5 + qv * (3 * s.padv0 + n) for n in s.nv]
        return X, Y

def ustrip(kind, c, d, rows, rd, edge=0, **kw):
    """rows: 4 lists of c values (row-major)."""
    return Item(kind, c, 4, d, 1, rows, rd, eu=edge, ev=3, Sv=3, **kw)
def vstrip(d, cols, rd, **kw):
    # cols: 4 rows x 4 columns already (Z[j][i])
    return Item('b', 4, 4, 1, d, cols, rd, Su=3, **kw)
def cell(kind, cu, cv, du, dv, Z, rd, eu=0, ev=0, **kw):
    return Item(kind, cu, cv, du, dv, Z, rd, eu=eu, ev=ev, **kw)
def two_rows(p, q): return [list(p), list(p), list(q), list(q)]
def all_rows(p): return [list(p) for _ in range(4)]
def neg(Z): return [[-v for v in row] for row in Z]
def T_(Z): return [list(r) for r in zip(*Z)]

class Batch:
    def __init__(s, sc, b, title, cal=False, npts=None):
        s.sc, s.b, s.title, s.items, s.cal, s.npts = sc, b, title, [], cal, npts
        s.blend = False                       # 73 b5: additive blend over the whole batch
    def add(s, it, n=1):
        for _ in range(n): it.batch = s.b; s.items.append(it)
class Scene:
    def __init__(s, num, name):
        s.num, s.name, s.batches = num, name, []
    def batch(s, b, title, **kw):
        B = Batch(s.num, b, title, **kw); s.batches.append(B); return B
    def items(s): return [it for B in s.batches for it in B.items]

def cal(sc): return sc.batch(0, 'cal', cal=True, npts=480)

# ======================================================================= 67
def sc67():
    sc = Scene(67, 'patchconv'); cal(sc)
    B = sc.batch(1, 'const16'); r = R(seed(67, 1))
    for e in E23:
        for s in (0, 1):
            c = mk16(s, e, r16(r)); c2 = mk16(s, e, r16(r))
            B.add(ustrip('b', 4, 29 if s == 0 else 37, two_rows([c] * 4, [c2] * 4), RD_R(e, s)))
    B = sc.batch(2, 'const24'); r = R(seed(67, 2))
    for e in E23:
        for s in (0, 1):
            cs = []
            for _ in range(2):
                m = 0x20000 + r() % 0x7C0000
                if (m & 0xFF) == 0: m |= 1
                cs.append(mk(s, e, m))
            B.add(ustrip('b', 4, 41 if s == 0 else 33, two_rows([cs[0]] * 4, [cs[1]] * 4), RD_R(e, s)))
    B = sc.batch(3, 'carry'); r = R(seed(67, 3))
    for e in (-1, -4, -8, -16):
        for s in (0, 1):
            for d in (31, 29, 27, 25):
                halves = []
                for sig in ((1, -1, 1, -1), (-1, 1, -1, 1)):
                    m15 = r16(r); row = []
                    for k in range(4):
                        dl = 1 + (r() & 0xFE); row.append(mk(s, e, (m15 << 8) + sig[k] * dl))
                    halves.append(row)
                B.add(ustrip('b', 4, d, two_rows(*halves), RD_R(e, s)))
    B = sc.batch(4, 'edges')
    for e in (-8, -1, 0, 6, 12):
        for s in (0, 1):
            for ma, mb in ((0x000000, 0x7FFF00), (0x7FFFFF, 0x7FFF80), (0x7FFF7F, 0x000080)):
                rd = dict(RD_R(e, s)); rd['ok'] = {'full', 'drop', 'over'} if ma == 0x7FFFFF or mb == 0x7FFF80 else {'full'}
                B.add(ustrip('b', 4, 29, two_rows([mk(s, e, ma)] * 4, [mk(s, e, mb)] * 4), rd))
    B = sc.batch(5, 'width twins'); r = R(seed(67, 5))
    for i in range(20):
        d = (29, 31, 33, 35, 37, 39)[i % 6]; s = i & 1; halves = []
        for h in range(2):
            row = []
            for k in range(4):
                hi = r16(r); lo = r() & 0xFF
                row.append(((hi << 8) | lo | 1) & 0x7FFFFF)
            halves.append(row)
        for twin in (0, 1):
            rows = [[mk(s, -1, m if twin == 0 else m & 0x7FFF00) for m in hv] for hv in halves]
            B.add(ustrip('b', 4, d, two_rows(*rows), RD_R(-1, s)))
    return sc

# ======================================================================= 68
def gen68b1():
    r = R(seed(68, 1)); out = []
    for d in range(1, 49):
        p = [mk16(0, -1, r16(r)) for _ in range(4)]; q = [mk16(0, -1, r16(r)) for _ in range(4)]
        out.append((d, two_rows(p, q)))
    return out
def gen68b3():
    r = R(seed(68, 3)); out = []
    for i in range(64):
        d = D8[i % 8]; s = (i >> 3) & 1
        B = 0x2400 + r() % 0x3800
        halves = []
        for h in range(2):
            r1 = r(); k = r1 & 3; sg = (r1 >> 2) & 1; sh = 1 + (r1 >> 3) % 12
            n = 1 + r() % (1 << sh)
            m = (0x8000 + B) + (-n if sg else n)
            row = [mk16(s, -1, B)] * 4; row = list(row); row[k] = mk16(s, -1, m - 0x8000)
            halves.append(row)
        out.append((d, s, two_rows(*halves)))
    return out
def sc68():
    sc = Scene(68, 'patchlerpu'); cal(sc)
    B = sc.batch(1, 'random cubics d=1..48: P, -P(a<0), -P(zs<0)')
    for d, Z in gen68b1():
        B.add(ustrip('b', 4, d, Z, RD_R(-1, 0), tag='P'))
        B.add(ustrip('b', 4, d, neg(Z), RD_R(-1, 1), tag='-Pa'))
        B.add(ustrip('b', 4, d, neg(Z), RD_N(-1), tag='-Pzs'))
    B = sc.batch(2, 'second pattern D1: P, -P'); r = R(seed(68, 2))
    for i in range(31):
        d = D1[i]; p = [mk16(0, -1, r16(r)) for _ in range(4)]; q = [mk16(0, -1, r16(r)) for _ in range(4)]
        Z = two_rows(p, q)
        B.add(ustrip('b', 4, d, Z, RD_R(-1, 0))); B.add(ustrip('b', 4, d, neg(Z), RD_R(-1, 1)))
    B = sc.batch(3, 'base plus one control')
    for d, s, Z in gen68b3(): B.add(ustrip('b', 4, d, Z, RD_R(-1, s)))
    return sc

# ======================================================================= 69
def xyr_ex(e): return e - 12
def sc69():
    sc = Scene(69, 'patchlerpv'); cal(sc)
    B = sc.batch(1, 'V: 68 b1 P transposed')
    for d, Z in gen68b1(): B.add(vstrip(d, T_(Z), RD_R(-1, 0)))
    B = sc.batch(2, 'V: 68 b3 transposed')
    for d, s, Z in gen68b3(): B.add(vstrip(d, T_(Z), RD_R(-1, s)))
    B = sc.batch(3, 'V: one control on zero, ladder + XYR'); r = R(seed(69, 3))
    for p in range(16):
        d = DL[p % 8]; s = (p >> 2) & 1; j0 = p % 4; q = mk16(s, -4, r16(r))
        Z = [[(q if j == j0 else 0.0)] * 4 for j in range(4)]
        sg = -1 if s else 1
        for k in (3, 8, 13): B.add(vstrip(d, Z, RD_A(k, sg)))
        B.add(vstrip(d, Z, RD_A(3, sg), xyr=xyr_ex(-4)))
    return sc

# ======================================================================= 70
def gen70():
    r = R(seed(70, 15)); pats = []
    for q in range(4):
        halves = [[r16(r) for _ in range(4)] for _ in range(2)]
        pats.append(halves)
    return pats
def scaledC(fx, fy):
    def mkC(it, bx, by):
        X, Y = it.ctrl_xy(bx, by); C = np.zeros((it.cv, it.cu, 3))
        for j in range(it.cv):
            for k in range(it.cu): C[j, k] = ((X[k] - 240) / 256 * fx, (136 - Y[j]) / 128 * fy, it.Z[j][k])
        return C
    return mkC
def sc70():
    sc = Scene(70, 'patchscale'); cal(sc); pats = gen70(); dq = (29, 31, 33, 35)
    def Zof(q, s, e, scale=1.0): return two_rows(*[[mk16(s, e, m) * scale for m in h] for h in pats[q]])
    B = sc.batch(1, 'model scale sweep')
    for e in EE:
        for s in (0, 1):
            for q in range(4): B.add(ustrip('b', 4, dq[q], Zof(q, s, e), RD_R(e, s)))
    B = sc.batch(2, 'clip scale')
    for k in (-8, -4, 4, 8):
        for q in range(4):
            P = [[2.0 ** k if i == j else 0.0 for j in range(4)] for i in range(4)]
            B.add(ustrip('b', 4, dq[q], Zof(q, 0, -1), dict(P=P, zs=65536.0, ok={'full'})))
    B = sc.batch(3, 'joint xyz scale')
    for sj in (-12, -6, 6, 12):
        for q in range(4):
            P = P_row(a=2.0 ** -sj, xr=(2.0 ** -sj, 0, 0, 0), yr=(0, 2.0 ** -sj, 0, 0))
            B.add(ustrip('b', 4, dq[q], Zof(q, 0, -1, 2.0 ** sj), dict(P=P, zs=65536.0, ok={'full'}), tag=('xyzscale', sj), mkC=scaledC(2.0 ** sj, 2.0 ** sj)))
    B = sc.batch(4, 'single-axis scale')
    for ax, sj in (('x', 10), ('x', -10), ('y', 10), ('y', -10)):
        for q in range(4):
            xr = (2.0 ** -sj, 0, 0, 0) if ax == 'x' else (1, 0, 0, 0)
            yr = (0, 2.0 ** -sj, 0, 0) if ax == 'y' else (0, 1, 0, 0)
            B.add(ustrip('b', 4, dq[q], Zof(q, 0, -1), dict(P=P_row(xr=xr, yr=yr), zs=65536.0, ok={'full'}), tag=('axscale', ax, sj), mkC=scaledC(2.0 ** sj if ax == 'x' else 1.0, 2.0 ** sj if ax == 'y' else 1.0)))
    B = sc.batch(5, 'XYR twins of b1')
    for e in (-24, -16, -10, -6, -3):
        for s in (0, 1):
            for q in (0, 1): B.add(ustrip('b', 4, dq[q], Zof(q, s, e), RD_R(e, s), xyr=xyr_ex(e)))
    return sc

# ======================================================================= 71
def nearzero_pattern(r, d):
    while True:
        ist = 1 + r() % (d - 1); T = Tp(ist, d)
        Bk = [math.comb(3, k) * T ** k * (256 - T) ** (3 - k) for k in range(4)]
        m = []; sg = []
        for k in range(4):
            m.append(0x8000 + (r() & 0x7FFF)); sg.append(r() & 1)
        km = max(range(4), key=lambda k: (Bk[k], -k))
        So = sum(Bk[k] * (-1 if sg[k] else 1) * m[k] for k in range(4) if k != km)
        mm = (abs(So) + Bk[km] // 2) // Bk[km]
        if 0x8000 <= mm <= 0xFFFF:
            m[km] = mm; sg[km] = 0 if So < 0 else 1
            return [mk16(sg[k], -1, m[k] - 0x8000) for k in range(4)], ist
def sc71():
    sc = Scene(71, 'patchalign'); cal(sc)
    B = sc.batch(1, 'one control on zero: A3 A8 A13 + XYR A3'); r = R(seed(71, 1))
    for p in range(16):
        d = DL[p % 8]; s = (p >> 2) & 1; k0 = p % 4; q = mk16(s, -4, r16(r))
        Z = all_rows([q if k == k0 else 0.0 for k in range(4)]); sg = -1 if s else 1
        for k in (3, 8, 13): B.add(ustrip('b', 4, d, Z, RD_A(k, sg)))
        B.add(ustrip('b', 4, d, Z, RD_A(3, sg), xyr=xyr_ex(-4)))
    B = sc.batch(2, 'two binades: A0 A5 A10'); r = R(seed(71, 2))
    for p in range(20):
        d = (10, 14, 18, 22)[p % 4]; s = (p >> 1) & 1; e2 = (4, 6, 9, 13)[p % 4]
        b1 = mk16(s, -1, r16(r)); s1 = mk16(s, -e2, r16(r)); b2 = mk16(s, -1, r16(r)); s2 = mk16(s, -e2, r16(r))
        row = [b1, s1, b2, s2] if p % 2 == 0 else [s1, b1, s2, b2]
        sg = -1 if s else 1
        for k in (0, 5, 10): B.add(ustrip('b', 4, d, all_rows(row), RD_A(k, sg)))
    def five(B, d, Z):
        for k in (0, 4):
            for sg in (1, -1): B.add(ustrip('b', 4, d, Z, RD_A(k, sg)))
        B.add(ustrip('b', 4, d, Z, RD_S(0)))
    B = sc.batch(3, 'mixed signs one binade'); r = R(seed(71, 3))
    for i in range(8):
        d = (12, 16, 20, 24, 28, 32, 36, 40)[i % 8]; row = []
        for k in range(4):
            x = r(); row.append(mk16(x & 1, -1, r16(r)))
        five(B, d, all_rows(row))
    B = sc.batch(4, 'mixed signs and binades'); r = R(seed(71, 4))
    for i in range(8):
        d = (13, 17, 21, 25, 29, 33, 37, 41)[i % 8]; row = []
        for k in range(4):
            x = r(); row.append(mk16(x & 1, -1 if (x >> 1) & 1 else -5, r16(r)))
        five(B, d, all_rows(row))
    B = sc.batch(5, 'exponent edges'); r = R(seed(71, 5))
    V = [0.5, -0.5, 1 - 2 ** -16, -(1 - 2 ** -16), 0.25, -0.25, 0.5 - 2 ** -17, -(0.5 - 2 ** -17), 0.0, 0.75, -0.75, 2 ** -16, -2 ** -16]
    for i in range(8):
        d = 37 if i % 2 == 0 else 23
        row = [V[r() % 13] for _ in range(4)]
        for k in (0, 2):
            for sg in (1, -1): B.add(ustrip('b', 4, d, all_rows(row), RD_A(k, sg)))
    B = sc.batch(6, 'large middle rows'); r = R(seed(71, 6))
    for i in range(12):
        s = i & 1
        p = [mk16(s, -10, r16(r)) for _ in range(4)]; p2 = [mk16(s, -10, r16(r)) for _ in range(4)]
        if i < 6:
            b1 = [mk16(s, -1, r16(r)) for _ in range(4)]; b2 = [mk16(s, -1, r16(r)) for _ in range(4)]
            Z = [p, b1, b2, p2]
        else: Z = two_rows(p, p2)
        B.add(ustrip('b', 4, 37, Z, RD_R(-10, s)))
    B = sc.batch(7, 'near-zero windows'); r = R(seed(71, 7))
    for p in range(8):
        d = (8, 12, 16, 20, 24, 28, 32, 36)[p % 8]
        row, ist = nearzero_pattern(r, d)
        for k in (0, 8, 16):
            for sg in (1, -1): B.add(ustrip('b', 4, d, all_rows(row), RD_A(k, sg)))
    B = sc.batch(8, 'operand ties'); r = R(seed(71, 8))
    for p in range(24):
        g = (1, 2, 3, 5)[p % 4]; s = (p >> 2) & 1; d = (9, 13, 17, 21)[(p >> 1) % 4]
        mask = (1 << g) - 1; half = 1 << (g - 1)
        def tie(): return mk16(s, -1 - g, (r16(r) & ~mask) | half)
        t0 = tie(); b1 = mk16(s, -1, r16(r)); b2 = mk16(s, -1, r16(r)); t3 = tie()
        sg = -1 if s else 1
        for k in (0, g): B.add(ustrip('b', 4, d, all_rows([t0, b1, b2, t3]), RD_A(k, sg)))
    return sc

# ======================================================================= 72
SZ = [(3, 5), (6, 7), (9, 10), (12, 13), (16, 16), (20, 15), (5, 11), (7, 5)]
def rev(Z): return [list(reversed(row)) for row in Z]
def sc72():
    sc = Scene(72, 'patch2d'); cal(sc)
    B = sc.batch(1, 'G GT GR -G'); r = R(seed(72, 1))
    for du, dv in SZ:
        G = [[0.0] * 4 for _ in range(4)]; Cc = [[0] * 4 for _ in range(4)]
        for j in range(4):
            for k in range(4): G[j][k] = mk16(0, -1, r16(r)); Cc[j][k] = r() & 0xFFFF
        bl = dict(cmode='blue', ident='blue')
        B.add(cell('b', 4, 4, du, dv, G, RD_R(-1, 0), tag='G', colgrid=Cc, **bl))
        B.add(cell('b', 4, 4, dv, du, T_(G), RD_R(-1, 0), tag='GT', colgrid=T_(Cc), **bl))
        B.add(cell('b', 4, 4, du, dv, rev(G), RD_R(-1, 0), tag='GR', colgrid=rev(Cc), **bl))
        B.add(cell('b', 4, 4, du, dv, neg(G), RD_R(-1, 1), tag='-G', colgrid=Cc, **bl))
    B = sc.batch(2, 'multi-piece'); r = R(seed(72, 2))
    for (cu, cv, du, dv) in ((7, 7, 6, 6), (7, 4, 8, 5)):
        G = [[0.0] * cu for _ in range(cv)]; Cc = [[0] * cu for _ in range(cv)]
        for j in range(cv):
            for k in range(cu): G[j][k] = mk16(0, -1, r16(r)); Cc[j][k] = r() & 0xFFFF
        bl = dict(cmode='blue', ident='blue')
        B.add(cell('b', cu, cv, du, dv, G, RD_R(-1, 0), colgrid=Cc, **bl))
        B.add(cell('b', cv, cu, dv, du, T_(G), RD_R(-1, 0), colgrid=T_(Cc), **bl))
        B.add(cell('b', cu, cv, du, dv, rev(G), RD_R(-1, 0), colgrid=rev(Cc), **bl))
        B.add(cell('b', cu, cv, du, dv, neg(G), RD_R(-1, 1), colgrid=Cc, **bl))
    B = sc.batch(3, 'one-hot 2D: A3 A8 A13 + XYR'); r = R(seed(72, 3))
    for idx, ((du, dv), (jr, kc)) in enumerate(zip([(11, 9), (9, 10), (7, 11), (10, 7)], [(0, 1), (1, 0), (1, 2), (2, 1)])):
        s = idx & 1; q = mk16(s, -4, r16(r)); sg = -1 if s else 1
        G = [[q if (j == jr and k == kc) else 0.0 for k in range(4)] for j in range(4)]
        for (Z, a, b) in ((G, du, dv), (T_(G), dv, du)):
            for k in (3, 8, 13): B.add(cell('b', 4, 4, a, b, Z, RD_A(k, sg)))
            B.add(cell('b', 4, 4, a, b, Z, RD_A(3, sg), xyr=xyr_ex(-4)))
    B = sc.batch(4, 'contrast 1D'); r = R(seed(72, 4))
    for i in range(20):
        d = D1[(5 * i) % 31]; p = []; q = []
        for k in range(4):
            x = r() & 0x3FF
            if k % 2 == 0: p.append(mk16(0, -1, 0x40 + x)); q.append(mk16(0, -1, 0x7FBF - x))
            else: p.append(mk16(0, -1, 0x7FBF - x)); q.append(mk16(0, -1, 0x40 + x))
        B.add(ustrip('b', 4, d, two_rows(p, q), RD_R(-1, 0)))
    B = sc.batch(5, 'signed patterns, sign-split'); r = R(seed(72, 5))
    for pat in range(2):
        G = [[0.0] * 4 for _ in range(4)]
        for j in range(4):
            for k in range(4):
                sgn = (k & 1) if pat == 0 else ((j + k) & 1)
                G[j][k] = mk16(sgn, -1, r16(r))
        for (Z, a, b) in ((G, 10, 9), (T_(G), 9, 10)):
            for k in (0, 4):
                for sg in (1, -1): B.add(cell('b', 4, 4, a, b, Z, RD_A(k, sg)))
    B = sc.batch(6, 'order contrast: rows in binades -1,-5,-9,-13'); r = R(seed(72, 6))
    for du, dv in ((9, 10), (10, 11), (7, 11)):
        G = [[mk16(0, -1 - 4 * j, r16(r)) for k in range(4)] for j in range(4)]
        for (Z, a, b) in ((G, du, dv), (T_(G), dv, du)):
            for k in (0, 6, 12): B.add(cell('b', 4, 4, a, b, Z, RD_A(k, 1)))
    return sc

# ======================================================================= 73
DU = [2, 3, 5, 7, 9, 11, 4, 6, 8, 10]
def sc73():
    sc = Scene(73, 'patchspline'); cal(sc)
    B = sc.batch(1, 'u-splines'); r = R(seed(73, 1)); n = 0
    for c in (4, 5, 6, 7, 8):
        for edge in range(4):
            for k in range(2):
                du = DU[n % 10]; pads = (0 if edge & 1 else 1) + (0 if edge & 2 else 1)
                if S(du) * (c - 3 + pads) + 1 > 476: du = DU[n % 5]
                s = n & 1
                p = [mk16(s, -1, r16(r)) for _ in range(c)]; q = [mk16(s, -1, r16(r)) for _ in range(c)]
                B.add(ustrip('s', c, du, two_rows(p, q), RD_R(-1, s), edge=edge)); n += 1
    B = sc.batch(2, 'v-splines'); r = R(seed(73, 2))
    for c in (5, 7):
        for edge in range(4):
            dv = (3, 5, 7, 10)[edge]
            p = [mk16(0, -1, r16(r)) for _ in range(c)]; q = [mk16(0, -1, r16(r)) for _ in range(c)]
            Z = [[p[j], p[j], q[j], q[j]] for j in range(c)]
            B.add(Item('s', 4, c, 1, dv, Z, RD_R(-1, 0), eu=3, ev=edge, Su=3))
    B = sc.batch(3, '2D splines + transpose'); r = R(seed(73, 3))
    for (cu, cv, eu, ev) in ((5, 5, 0, 0), (6, 6, 3, 3), (5, 6, 1, 2), (6, 5, 2, 1), (7, 5, 0, 3)):
        for (du, dv) in ((3, 4),):
            G = [[0.0] * cu for _ in range(cv)]; Cc = [[0] * cu for _ in range(cv)]
            for j in range(cv):
                for k in range(cu): G[j][k] = mk16(0, -1, r16(r)); Cc[j][k] = r() & 0xFFFF
            bl = dict(cmode='blue', ident='blue')
            B.add(cell('s', cu, cv, du, dv, G, RD_R(-1, 0), eu=eu, ev=ev, colgrid=Cc, **bl))
            B.add(cell('s', cv, cu, dv, du, T_(G), RD_R(-1, 0), eu=ev, ev=eu, colgrid=T_(Cc), **bl))
    B = sc.batch(4, 'far A/B'); r = R(seed(73, 4))
    for part in range(3):
        for sd in range(6):
            if part == 0: cu, kind, far = 7, 's', (4, 5, 6)
            elif part == 1: cu, kind, far = 7, 'b', (4, 5, 6)
            else: cu, kind, far = 4, 'b', (1, 2, 3)
            rows = 4
            A = [[mk16(0, -8, r16(r)) for k in range(cu)] for j in range(rows)]
            Bv = [[mk16(0, -1, r16(r)) for k in far] for j in range(rows)]
            Bm = [list(row) for row in A]
            for j in range(rows):
                for t, k in enumerate(far): Bm[j][k] = Bv[j][t]
            rd = dict(RD_R(-8, 0)); rd['ok'] = {'full', 'drop', 'lp'}
            if part < 2:
                A = [A[0], A[0], A[2], A[2]]; Bm = [Bm[0], Bm[0], Bm[2], Bm[2]]
                B.add(ustrip(kind, cu, 8, A, rd)); B.add(ustrip(kind, cu, 8, Bm, rd))
            else:
                B.add(cell('b', 4, 4, 8, 8, A, rd)); B.add(cell('b', 4, 4, 8, 8, Bm, rd))
    B = sc.batch(5, 'span boundary, additive'); r = R(seed(73, 5)); B.blend = True
    for p in range(24):
        kind = 'b' if p & 1 == 0 else 's'; after = (p >> 1) & 1; s = (p >> 2) & 1
        big = (4,) if after else ((2,) if kind == 'b' else (0,))
        row = []
        for k in range(7):
            m = r16(r)
            if kind == 'b' and k == 3: m |= 1
            row.append(mk16(s, 0 if k in big else -1, m))
        rd = dict(RD_A(0, -1 if s else 1)); rd['ok'] = {'full', 'drop', 'neg'}
        col = 0xFF000000 | 0x20 << 16 | (p + 1) << 8 | 0x20
        B.add(ustrip(kind, 7, 8, all_rows(row), rd, edge=0, tag=('span', kind, after),
                     cmode='full', colgrid=[[col] * 7 for _ in range(4)], ident='add'))
    return sc

# ======================================================================= 74
def sc74():
    sc = Scene(74, 'patchspace'); cal(sc)
    def canc(r):
        mT = 0x2000 | (r() & 0x1FFF)
        Ds = [[528 + r() % 240 for _ in range(4)] for _ in range(2)]
        return mT, Ds
    def Ttr(t): return [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, t], [0, 0, 0, 1.0]]
    B = sc.batch(1, 'cancellation A1 A3 +/-'); r = R(seed(74, 1))
    for i in range(20):
        plus = i < 10; d = D1[11 + i]; mT, Ds = canc(r); T = mk16(0, 5, mT)
        sz = 1 if plus else 0
        Z = two_rows(*[[mk16(sz, 5, mT - D) for D in h] for h in Ds])
        zs = 65536.0 if plus else -65536.0
        B.add(ustrip('b', 4, d, Z, RD_SPECIAL(P=P_row(), W=Ttr(T if plus else -T), zs=zs), tag=('A1', i)))
        B.add(ustrip('b', 4, d, Z, RD_SPECIAL(P=P_row(b=T if plus else -T), zs=zs), tag=('A3', i)))
    B = sc.batch(2, 'zs halving'); r = R(seed(74, 2))
    for i in range(12):
        d = D1[(3 * i) % 31]
        if i < 6:
            mT, Ds = canc(r); T = mk16(0, 5, mT)
            Z = two_rows(*[[mk16(1, 5, mT - D) for D in h] for h in Ds]); W = Ttr(T)
        else:
            Z = two_rows([mk16(0, -1, r16(r)) for _ in range(4)], [mk16(0, -1, r16(r)) for _ in range(4)]); W = None
        B.add(ustrip('b', 4, d, Z, RD_SPECIAL(P=P_row(), W=W), tag=('half', i, 0)))
        B.add(ustrip('b', 4, d, Z, RD_SPECIAL(P=P_row(), W=W, zs=32768.0, ok={'full', 'lp'}, code=7), tag=('half', i, 1)))
        B.add(ustrip('b', 4, d, Z, RD_SPECIAL(P=P_row(), W=W, zs=32768.0, zc=16384.0, ok={'full'}), tag=('half', i, 2)))
    B = sc.batch(3, 'shear A4, rotation A5'); r = R(seed(74, 3))
    for i in range(24):
        d = D1[(3 * i + 1) % 31]
        if i < 12:
            rx = mk16(0, 4, r() & 0x3FFF); Ds = [[528 + r() % 240 for _ in range(4)] for _ in range(2)]
            def mkC(it, bx, by, rx=rx, Ds=Ds):
                X, Y = it.ctrl_xy(bx, by)
                C = np.zeros((4, 4, 3))
                for j in range(4):
                    for k in range(4):
                        x = (X[k] - 240) / 256 + 2; D = Ds[j // 2][k]      # x in [1.07, 2.93]: exponent(rx x) >= 4
                        C[j, k] = (x, (136 - Y[j]) / 128, ge_cut(-rx * x + D / 1024))
                return C
            W = [[1, 0, 0, 0], [0, 1, 0, 0], [rx, 0, 1, 0], [0, 0, 0, 1.0]]
            B.add(ustrip('b', 4, d, [[0.0] * 4] * 4, RD_SPECIAL(P=P_row(xr=(1, 0, 0, -2)), W=W), mkC=mkC, tag='A4'))
        else:
            mT, Ds = canc(r); T = mk16(0, 5, mT)
            def mkC(it, bx, by, mT=mT, Ds=Ds):
                X, Y = it.ctrl_xy(bx, by)
                C = np.zeros((4, 4, 3))
                for j in range(4):
                    for k in range(4):
                        C[j, k] = (mk16(0, 5, mT - Ds[j // 2][k]), (136 - Y[j]) / 128, (X[k] - 240) / 256)
                return C
            # world: x' = z, y' = y, z' = -x + T  (rows of world as clip-style rows)
            W = [[0, 0, 1, 0], [0, 1, 0, 0], [-1, 0, 0, T], [0, 0, 0, 1.0]]
            B.add(ustrip('b', 4, d, [[0.0] * 4] * 4, dict(P=P_row(), W=W, zs=65536.0, ok={'full'}), mkC=mkC, tag='A5'))
    B = sc.batch(4, 'axis routing z/x/y'); r = R(seed(74, 4))
    for i in range(12):
        d = D1[(5 * i) % 31]
        p = [mk16(0, -1, r16(r)) for _ in range(4)]; q = [mk16(0, -1, r16(r)) for _ in range(4)]
        Z = two_rows(p, q)
        for route in 'zxy':
            def mkC(it, bx, by, Z=Z, route=route):
                X, Y = it.ctrl_xy(bx, by); C = np.zeros((4, 4, 3))
                for j in range(4):
                    for k in range(4):
                        x, y, z = (X[k] - 240) / 256, (136 - Y[j]) / 128, Z[j][k]
                        C[j, k] = (x, y, z) if route == 'z' else ((z, y, x) if route == 'x' else (x, z, y))
                return C
            if route == 'z': P = P_row()
            elif route == 'x': P = [[0, 0, 1.0, 0], [0, 1.0, 0, 0], [1.0, 0, 0, 0], [0, 0, 0, 1.0]]
            else: P = [[1.0, 0, 0, 0], [0, 0, 1.0, 0], [0, 1.0, 0, 0], [0, 0, 0, 1.0]]
            B.add(ustrip('b', 4, d, Z, dict(P=P, zs=65536.0, ok={'full'}), mkC=mkC, tag=('route', route)))
    B = sc.batch(5, 'w routing + twin'); r = R(seed(74, 5))
    for i in range(8):
        d = D1[(11 * i) % 31]
        Pk = two_rows([mk16(0, 0, 1 + r() % 255) for _ in range(4)], [mk16(0, 0, 1 + r() % 255) for _ in range(4)])
        for twin in (0, 1):
            def mkC(it, bx, by, Pk=Pk):
                X, Y = it.ctrl_xy(bx, by); C = np.zeros((4, 4, 3))
                for j in range(4):
                    for k in range(4):
                        C[j, k] = ((X[k] - X[0]) / 256, (Y[0] - Y[j]) / 128, Pk[j][k])
                return C
            def mkP(it, bx, by, twin=twin):
                X, Y = it.ctrl_xy(bx, by); tx = (X[0] - 240) / 256; ty = (136 - Y[0]) / 128
                if twin == 0: return [[1.0, 0, tx, 0], [0, 1.0, ty, 0], [0, 0, 0, 0.5], [0, 0, 1.0, 0]]
                return [[1.0, 0, 0, tx], [0, 1.0, 0, ty], [0, 0, 0.5, 0], [0, 0, 0, 1.0]]
            B.add(ustrip('b', 4, d, Pk, dict(P=None, mkP=mkP, zs=131072.0 if twin == 0 else 65536.0, ok={'full'}), mkC=mkC, tag=('wroute', twin)))
    B = sc.batch(6, 'random row in W, V, P'); r = R(seed(74, 6))
    for i in range(12):
        d = D1[(7 * i) % 31]
        Z = two_rows([mk16(0, -2, 0x200 + r() % 0x3E00) for _ in range(4)], [mk16(0, -2, 0x200 + r() % 0x3E00) for _ in range(4)])
        x = r(); rx = mk16(x & 1, -7, r16(r)); s_ = mk16(0, 0, r() & 0xFFF); T = mk16(0, -1, 0x0F00 + (r() & 0x3FF))
        row = [[1, 0, 0, 0], [0, 1, 0, 0], [rx, 0, s_, T], [0, 0, 0, 1.0]]
        def mkC(it, bx, by, Z=Z):
            X, Y = it.ctrl_xy(bx, by); C = np.zeros((4, 4, 3))
            for j in range(4):
                for k in range(4): C[j, k] = ((X[k] - X[0]) / 256, (Y[0] - Y[j]) / 128, Z[j][k])
            return C
        for where in 'WVP':
            def mkP(it, bx, by, where=where, row=row):
                X, Y = it.ctrl_xy(bx, by); tx = (X[0] - 240) / 256; ty = (136 - Y[0]) / 128
                zr = row[2] if where == 'P' else [0, 0, 1.0, 0]
                return [[1.0, 0, 0, tx], [0, 1.0, 0, ty], list(zr), [0, 0, 0, 1.0]]
            rd = RD_SPECIAL(P=None, mkP=mkP)
            if where == 'W': rd['W'] = row
            if where == 'V': rd['V'] = row
            B.add(ustrip('b', 4, d, Z, rd, mkC=mkC, tag=('wvp', where, i)))
    return sc

# ======================================================================= 75
def persp_words():
    f = 1 / math.tan(math.radians(30)); asp = 480 / 272; n, fa = 1.0, 100.0
    return [ge_cut(v) for v in (f / asp, f, (fa + n) / (n - fa), 2 * fa * n / (n - fa))]
def sc75():
    sc = Scene(75, 'patchvalid'); cal(sc)
    B = sc.batch(1, 'perspective strips'); r = R(seed(75, 1))
    c = mk16(0, 0, 0x7F00)
    for i in range(16):
        ms = []
        for h in range(2):
            ms.append([(0x0400 + r() % 0x1C00) if k % 2 == 0 else (0x6000 + r() % 0x1C00) for k in range(4)])
        def mkC(it, bx, by, ms=ms):
            X, Y = it.ctrl_xy(bx, by); C = np.zeros((4, 4, 3))
            for j in range(4):
                for k in range(4):
                    w = mk16(0, 1, ms[j // 2][k])
                    C[j, k] = (ge_cut((X[k] - 240) / 256 * w), ge_cut((136 - Y[j]) / 128 * w), w)
            return C
        B.add(ustrip('b', 4, 16, [[0.0] * 4] * 4, dict(P=[[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 0, c], [0, 0, 1.0, 0]], zs=65536.0, ok={'full'}), mkC=mkC, Su=96))
    B = sc.batch(2, 'w cancellation'); r = R(seed(75, 2))
    for i in range(8):
        d = (29, 31, 33, 35, 37, 39, 41, 13)[i]; mT = 0x2000 | (r() & 0x1FFF); T = mk16(0, 3, mT)
        Ms = [[mT + 4097 + r() % 255 for _ in range(4)] for _ in range(2)]
        def mkC(it, bx, by, Ms=Ms):
            X, Y = it.ctrl_xy(bx, by); C = np.zeros((4, 4, 3))
            for j in range(4):
                for k in range(4): C[j, k] = ((X[k] - X[0]) / 256, (Y[0] - Y[j]) / 128, mk16(1, 3, Ms[j // 2][k]))
            return C
        def mkP(it, bx, by):
            X, Y = it.ctrl_xy(bx, by); tx = (X[0] - 240) / 256; ty = (136 - Y[0]) / 128
            return [[1.0, 0, -tx, 0], [0, 1.0, -ty, 0], [0, 0, 0, 0.5], [0, 0, -1.0, 0]]
        W = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, T], [0, 0, 0, 1.0]]
        B.add(ustrip('b', 4, d, [[0.0] * 4] * 4, dict(P=None, mkP=mkP, W=W, zs=131072.0, ok={'full'}), mkC=mkC))
    B = sc.batch(3, 'near-equal'); r = R(seed(75, 3))
    for i in range(16):
        halves = []
        for h in range(2):
            cm = 0x2000 + (r() & 0x3FFF)
            halves.append([mk16(0, -1, cm + ((r() & 0x1FF) - 256)) for _ in range(4)])
        B.add(ustrip('b', 4, D1[2 * i], two_rows(*halves), RD_R(-1, 0)))
    B = sc.batch(4, 'non-pow2 coefficient'); r = R(seed(75, 4))
    for i in range(16):
        a = mk16(0, 0, 0x2EC0 + (r() & 0xFF))
        Z = two_rows([mk16(0, -1, 0x200 + r() % 0x3500) for _ in range(4)], [mk16(0, -1, 0x200 + r() % 0x3500) for _ in range(4)])
        B.add(ustrip('b', 4, 37, Z, dict(P=P_row(a=a), zs=65536.0, ok={'full'})))
    B = sc.batch(5, 'parameter ramps')
    kinds = [('b', 4, 0), ('b', 7, 0), ('b', 10, 0)] + [('s', c, e) for c in (4, 5, 7) for e in range(4)]
    for kind, c, e in kinds:
        for d in ((7, 13) if kind == 'b' else (7,)):
            spans = (c - 1) // 3 if kind == 'b' else c - 3
            m = 5 if spans <= 2 else 6
            g = [k / 3 for k in range(c)] if kind == 'b' else greville(c, e)
            n_ = [round(3 * x) for x in g]
            nmin = min(n_)
            p = [0.5 + 2.0 ** -m * (nk - nmin + 4) for nk in n_]
            top = max(n_) - nmin
            q = [0.5 + 2.0 ** -m * (top - (nk - nmin) + 4) for nk in n_]
            B.add(ustrip(kind, c, d, two_rows(p, q), RD_R(-1, 0), edge=e))
    kx, ky, pa, pb = persp_words()
    B = sc.batch(6, 'perspective cells'); r = R(seed(75, 6))
    for i in range(24):
        zz = [[mk16(1, 2, 8192 + (r() & 511)) for k in range(4)] for j in range(4)]
        def mkC(it, bx, by, zz=zz):
            X, Y = it.ctrl_xy(bx, by); C = np.zeros((4, 4, 3))
            for j in range(4):
                for k in range(4):
                    w = -zz[j][k]
                    C[j, k] = (ge_cut((X[k] - 240) / 256 * w / kx), ge_cut((136 - Y[j]) / 128 * w / ky), zz[j][k])
            return C
        P = [[kx, 0, 0, 0], [0, ky, 0, 0], [0, 0, pa, pb], [0, 0, -1.0, 0]]
        B.add(cell('b', 4, 4, 8, 8, [[0.0] * 4] * 4, dict(P=P, zs=65536.0, ok={'full'}), mkC=mkC))
    B = sc.batch(7, 'rotated world + view'); r = R(seed(75, 7))
    cR = mk16(0, -1, 0x5DB3); sR = 0.5
    for i in range(12):
        zz = [[mk16(1, 2, 8192 + (r() & 511)) for k in range(4)] for j in range(4)]
        def mkC(it, bx, by, zz=zz):
            X, Y = it.ctrl_xy(bx, by); C = np.zeros((4, 4, 3))
            for j in range(4):
                for k in range(4):
                    w = -zz[j][k]; ex = (X[k] - 240) / 256 * w / kx; ey = (136 - Y[j]) / 128 * w / ky
                    wz = zz[j][k] + 5.0           # world z = eye z + 5 (view translate -5)
                    # world = R(y): x' = c x + s z, z' = -s x + c z  -> invert
                    mx = cR * ex - sR * wz; mz = sR * ex + cR * wz
                    C[j, k] = (ge_cut(mx), ge_cut(ey), ge_cut(mz))
            return C
        P = [[kx, 0, 0, 0], [0, ky, 0, 0], [0, 0, pa, pb], [0, 0, -1.0, 0]]
        W = [[cR, 0, sR, 0], [0, 1, 0, 0], [-sR, 0, cR, 0], [0, 0, 0, 1.0]]
        V = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, -5.0], [0, 0, 0, 1.0]]
        B.add(cell('b', 4, 4, 8, 8, [[0.0] * 4] * 4, dict(P=P, W=W, V=V, zs=65536.0, ok={'full'}), mkC=mkC, tag='rot'))
    B = sc.batch(8, 'topology'); r = R(seed(75, 8))
    for (cu, cv, du, dv, kind) in ((4, 4, 2, 2, 'b'), (4, 4, 3, 2, 'b'), (4, 4, 4, 4, 'b'), (7, 4, 2, 1, 'b'), (7, 7, 2, 2, 'b'), (5, 5, 2, 2, 's3')):
        G = [[mk16(0, -1, r16(r)) for k in range(cu)] for j in range(cv)]
        Cc = [[0xFF000000 | 0x48 << 16 | ((255 * j) // (cv - 1)) << 8 | (255 * k) // (cu - 1) for k in range(cu)] for j in range(cv)]
        for prim in range(5):
            k_ = 's' if kind == 's3' else kind; ee = 3 if kind == 's3' else 0
            pr = (PRIM_POINTS, PRIM_TRIANGLE_STRIP, PRIM_TRIANGLE_STRIP, PRIM_LINE_STRIP, PRIM_LINE_STRIP)[prim]
            B.add(cell(k_, cu, cv, du, dv, G, RD_R(-1, 0), eu=ee, ev=ee, Su=12, Sv=12, tag=('topo', prim),
                       cmode='full', colgrid=Cc, ident='b48', prim=pr, shade=0 if prim in (2, 4) else 1))
    B = sc.batch(9, 'triangle colour planes'); r = R(seed(75, 9))
    for (du, dv) in ((4, 4), (3, 5), (5, 3)):
        G = [[0.5] * 4 for _ in range(4)]
        ncol = 16 if (du, dv) == (4, 4) else 4
        cols = [r() & 0xFFFFFF for _ in range(ncol)]
        if ncol == 16: Cc = [[0xFF000000 | cols[4 * j + k] for k in range(4)] for j in range(4)]
        elif du == 3: Cc = [[0xFF000000 | cols[k] for k in range(4)] for j in range(4)]      # u-only
        else: Cc = [[0xFF000000 | cols[j] for k in range(4)] for j in range(4)]              # v-only
        for tw in range(3):
            it = cell('b', 4, 4, du, dv, G, RD_R(-1, 0), Su=48, Sv=48, tag=('tri', tw), cmode='full', colgrid=Cc,
                      ident='box', prim=PRIM_POINTS if tw == 0 else PRIM_TRIANGLE_STRIP)
            if tw: it.n = 0; it.nopts = True        # B (patch triangles) and C (plain triangles): pixels, not point samples
            if tw == 2: it.late = True
            B.add(it)
    return sc

# ======================================================================= 76-82
def sc76():
    sc = Scene(76, 'patchrange'); cal(sc)
    EX = [13, 14, 15, 16, 17, 20, 24, 31, 32, 40, 64, 100, -40, -60, -100, -120, -125]
    B = sc.batch(1, 'extreme constants'); r = R(seed(76, 1)); consts = []
    for e in EX:
        for s in (0, 1):
            c = mk16(s, e, r16(r)); c2 = mk16(s, e, r16(r)); consts.append((e, s, c, c2))
            B.add(ustrip('b', 4, 29, two_rows([c] * 4, [c2] * 4), RD_R(e, s)))
    B = sc.batch(2, 'extreme random'); pats = gen70()
    for e in (-60, -40, 14, 16, 20, 32, 64, 100):
        for s in (0, 1):
            B.add(ustrip('b', 4, 29, two_rows(*[[mk16(s, e, m) for m in h] for h in pats[0]]), RD_R(e, s)))
    B = sc.batch(3, 'companions')
    for e, s, c, c2 in consts:
        rd = dict(P=P_row(a=(-1) ** s * 2.0 ** -(e + 8), b=0.5), zs=65536.0, ok={'full', 'lp'})
        B.add(ustrip('b', 4, 29, two_rows([c] * 4, [c2] * 4), rd))
    B = sc.batch(4, 'huge and tiny x'); r = R(seed(76, 4))
    for ex in (12, 16, 20, 30, -20, -40):
        for q in range(4):
            s = q & 1; Z = two_rows([mk16(s, -8, r16(r)) for _ in range(4)], [mk16(s, -8, r16(r)) for _ in range(4)])
            def mkC(it, bx, by, Z=Z, ex=ex):
                X, Y = it.ctrl_xy(bx, by); C = np.zeros((4, 4, 3))
                for j in range(4):
                    for k in range(4): C[j, k] = ((X[k] - 240) / 256 * 2.0 ** ex, (136 - Y[j]) / 128, Z[j][k])
                return C
            P = P_row(a=(-1) ** s * 2.0 ** 7, xr=(2.0 ** -ex, 0, 0, 0))
            B.add(ustrip('b', 4, 29, Z, dict(P=P, zs=65536.0, ok={'full'}), mkC=mkC))
    B = sc.batch(5, 'clip scale +/-16')
    for k in (-16, 16):
        for q in range(4):
            P = [[2.0 ** k if i == j else 0.0 for j in range(4)] for i in range(4)]
            B.add(ustrip('b', 4, 29, two_rows(*[[mk16(0, -1, m) for m in h] for h in pats[q]]), dict(P=P, zs=65536.0, ok={'full'})))
    return sc
def anchorC(it, bx, by):
    X, Y = it.ctrl_xy(bx, by); C = np.zeros((it.cv, it.cu, 3))
    for j in range(it.cv):
        for k in range(it.cu): C[j, k] = ((X[k] - X[0]) / 256, (Y[0] - Y[j]) / 128, it.Z[j][k])
    return C
def anchorP(it, bx, by):
    X, Y = it.ctrl_xy(bx, by); tx = (X[0] - 240) / 256; ty = (136 - Y[0]) / 128
    return [[1.0, 0, 0, tx], [0, 1.0, 0, ty], [0, 0, 1.0, 0], [0, 0, 0, 1.0]]
def sc77():
    sc = Scene(77, 'patchfmt'); cal(sc)
    B = sc.batch(1, 's16 vs float'); r = R(seed(77, 1))
    for i in range(24):
        Z = two_rows([(16896 + r() % 15360) / 32768 for _ in range(4)], [(16896 + r() % 15360) / 32768 for _ in range(4)])
        for t in range(2): B.add(ustrip('b', 4, D1[i % 31], Z, RD_R(-1, 0), fmt=('s16abs', 'f')[t], tag=('fmt', ('s16', 'float')[t])))
    B = sc.batch(2, 's8 vs s16 vs float'); r = R(seed(77, 2))
    for i in range(16):
        Z = two_rows([(64 + r() % 63) / 128 for _ in range(4)], [(64 + r() % 63) / 128 for _ in range(4)])
        for t in range(3):
            rd = dict(RD_R(-1, 0)); rd['P'] = None; rd['mkP'] = anchorP
            B.add(ustrip('b', 4, (3, 5, 7, 9, 11, 13, 17, 21)[i % 8], Z, rd, mkC=anchorC, tag=('fmt', ('s8', 's16', 'float')[t]),
                         fmt=('s8anc', 's16anc', 'f')[t], code=(8, 0)))
    B = sc.batch(3, 's16 negative'); r = R(seed(77, 3))
    for i in range(12):
        Z = two_rows([-(16896 + r() % 15360) / 32768 for _ in range(4)], [-(16896 + r() % 15360) / 32768 for _ in range(4)])
        for t in range(2): B.add(ustrip('b', 4, D1[(5 * i) % 31], Z, RD_R(-1, 1), fmt=('s16abs', 'f')[t], tag=('fmt', ('s16', 'float')[t])))
    B = sc.batch(4, 'index16 reversed'); r = R(seed(77, 4))
    for i in range(12):
        Z = two_rows([mk16(0, -1, r16(r)) for _ in range(4)], [mk16(0, -1, r16(r)) for _ in range(4)])
        for t in range(2): B.add(ustrip('b', 4, D1[(7 * i) % 31], Z, RD_R(-1, 0), fmt=('idx16', 'f')[t], tag=('fmt', ('index16', 'plain')[t])))
    B = sc.batch(5, 'index8 permuted'); r = R(seed(77, 5))
    for i in range(8):
        Z = two_rows([mk16(0, -1, r16(r)) for _ in range(4)], [mk16(0, -1, r16(r)) for _ in range(4)])
        perm = list(range(16))
        for n in range(15, -1, -1):
            j = r() % (n + 1); perm[n], perm[j] = perm[j], perm[n]
        for t in range(2): B.add(ustrip('b', 4, D1[(11 * i) % 31], Z, RD_R(-1, 0), fmt=('idx8', 'f')[t], extra=perm, tag=('fmt', ('index8', 'plain')[t])))
    B = sc.batch(6, 'anchors', npts=1088)
    B.anchor = (241, 21)
    return sc
def sc78():
    sc = Scene(78, 'patchmorph'); cal(sc)
    B = sc.batch(1, 'morph cancellation'); r = R(seed(78, 1))
    for i in range(16):
        d = D1[i % 31]; mT = 0x2000 | (r() & 0x1FFF)
        Ds = [[528 + r() % 240 for _ in range(4)] for _ in range(2)]
        dvals = two_rows(*[[D / 1024 for D in h] for h in Ds])
        T = mk16(0, 5, mT)
        A = two_rows(*[[mk16(1, 5, mT - D) for D in h] for h in Ds])
        A2 = two_rows(*[[D / 1024 + (D & 255) / 65536 for D in h] for h in Ds])
        B2 = two_rows(*[[D / 1024 - (D & 255) / 65536 for D in h] for h in Ds])
        ex = ((A, [[T] * 4 for _ in range(4)], (1.0, 1.0)), None, (A2, B2, (0.5, 0.5)))
        for t in range(3): B.add(ustrip('b', 4, d, dvals, RD_R(-1, 0), tag=('morph', t), ident='blue1',
                                        fmt=('morphAB', 'f', 'morphA2B2')[t], extra=ex[t]))
    B = sc.batch(2, 'skin cancellation'); r = R(seed(78, 2))
    for i in range(16):
        d = D1[(3 * i) % 31]; mT = 0x2000 | (r() & 0x1FFF)
        Ds = [[528 + r() % 240 for _ in range(4)] for _ in range(2)]
        T = mk16(0, 5, mT)
        if i < 8: vz = two_rows(*[[mk16(1, 5, mT - D) for D in h] for h in Ds]); bone = T
        else: vz = two_rows(*[[D / 1024 for D in h] for h in Ds]); bone = 0.0
        B.add(ustrip('b', 4, d, two_rows(*[[D / 1024 for D in h] for h in Ds]), RD_R(-1, 0), fmt='skin', extra=(vz, bone),
                     code=(8, 0) if i < 8 else None, tag=('skin', i < 8)))
    return sc
def sc79():
    sc = Scene(79, 'patchbig48'); cal(sc)
    B = sc.batch(1, '48x48 G GT'); r = R(seed(79, 1))
    G = [[mk16(0, -1, r16(r)) for k in range(4)] for j in range(4)]
    B.add(cell('b', 4, 4, 48, 48, G, RD_R(-1, 0))); B.add(cell('b', 4, 4, 48, 48, T_(G), RD_R(-1, 0)))
    return sc
DD = [49, 50, 51, 52, 53, 56, 63, 64, 65, 66, 67, 72, 84, 85, 86, 96, 100, 112, 127, 128]
def sc80():
    sc = Scene(80, 'patchdiv'); cal(sc)
    B = sc.batch(1, 'd 49-128: P, -P, P2'); r = R(seed(80, 1))
    for d in DD:
        Z = two_rows([mk16(0, -1, r16(r)) for _ in range(4)], [mk16(0, -1, r16(r)) for _ in range(4)])
        Z2 = two_rows([mk16(0, -1, r16(r)) for _ in range(4)], [mk16(0, -1, r16(r)) for _ in range(4)])
        B.add(ustrip('b', 4, d, Z, RD_R(-1, 0))); B.add(ustrip('b', 4, d, neg(Z), RD_R(-1, 1))); B.add(ustrip('b', 4, d, Z2, RD_R(-1, 0)))
    return sc
DM = [129, 130, 144, 160, 170, 172, 192, 193, 200, 224, 240, 252, 253, 254, 255]
def sc81():
    sc = Scene(81, 'patchdivmax'); cal(sc)
    B = sc.batch(1, 'd 129-255 P (F2, 2 copies)'); r = R(seed(81, 1))
    zz = {}
    for d in DM:
        Z = two_rows([mk16(0, -1, r16(r)) for _ in range(4)], [mk16(0, -1, r16(r)) for _ in range(4)]); zz[d] = Z
        for cp in range(2): B.add(ustrip('b', 4, d, Z, RD_R(-1, 0), Su=768, xs=1024.0, offscreen_ok=True, tag=('F2', cp)))
    B = sc.batch(2, 'd 129-255 -P')
    for d in (255, 254, 253, 252, 193, 170):
        for cp in range(2): B.add(ustrip('b', 4, d, neg(zz[d]), RD_R(-1, 1), Su=768, xs=1024.0, offscreen_ok=True, tag=('F2', cp)))
    return sc
def sc82():
    sc = Scene(82, 'patchbig64'); cal(sc)
    B = sc.batch(1, '64x64 G GT'); r = R(seed(82, 1))
    G = [[mk16(0, -1, r16(r)) for k in range(4)] for j in range(4)]
    B.add(cell('b', 4, 4, 64, 64, G, RD_R(-1, 0), Su=192, Sv=192)); B.add(cell('b', 4, 4, 64, 64, T_(G), RD_R(-1, 0), Su=192, Sv=192))
    return sc
def sc82b():
    sc = Scene(82, 'patchrows'); cal(sc)
    B = sc.batch(2, 'long spline rows (step 2)'); r = R(seed(82, 2))
    for (c, du, Su) in ((10, 37, 96), (10, 36, 96), (16, 20, 48)):
        p = [mk16(0, -1, r16(r)) for _ in range(c)]; q = [mk16(0, -1, r16(r)) for _ in range(c)]
        for cp in range(2): B.add(ustrip('s', c, du, two_rows(p, q), RD_R(-1, 0), Su=Su, offscreen_ok=True, tag=('long', cp)))
    return sc

ALL = [sc67, sc68, sc69, sc70, sc71, sc72, sc73, sc74, sc75, sc76, sc77, sc78, sc79, sc80, sc81, sc82, sc82b]

# =========================================================================== layout
# FRAME 7: first-fit-decreasing shelves; 79, 81, 82 and the long rows have fixed places.
def pack_ffd(boxes):
    order = sorted(range(len(boxes)), key=lambda i: (-boxes[i][1], -boxes[i][0], i))
    shelves = []; ytop = Y0; pos = [None] * len(boxes)
    for i in order:
        w, h = boxes[i]
        for sh in shelves:
            if h <= sh[1] and sh[2] + w <= X1:
                pos[i] = (sh[2], sh[0]); sh[2] += w + GAP; break
        else:
            if ytop + h > Y1: return None, ytop
            shelves.append([ytop, h, X0 + w + GAP]); pos[i] = (X0, ytop); ytop += h + GAP
    return pos, ytop

def place(sc):
    """(items, [(bx, by)], ytop, anchor block position or None)."""
    its = sc.items()
    if sc.num == 81:                       # F2: one copy per shelf
        y = Y0; pos = []
        for it in its:
            pos.append((0, y) if it.tag[1] == 0 else (-289, y)); y += it.h + GAP
        return its, pos, y, None
    if sc.name == 'patchrows':
        y = Y0; pos = []
        for it in its:
            pos.append((2, y) if it.tag[1] == 0 else (478 - it.w, y)); y += it.h + GAP
        return its, pos, y, None
    if sc.num in (79, 82):
        return its, [(2, Y0), (196, Y0)], Y0 + its[0].h, None
    boxes = [(it.w, it.h) for it in its]
    extra = [B.anchor for B in sc.batches if hasattr(B, 'anchor')]
    pos, ytop = pack_ffd(boxes + extra)
    if pos is None: raise RuntimeError(f'scene {sc.num} does not pack (y {ytop})')
    return its, pos[:len(its)], ytop, (pos[len(its)] if extra else None)

def mats(it, bx, by):
    """The item's projection, view and world as row lists (clip = M (x, y, z, 1)), XYR applied."""
    rd = it.rd
    P = rd['P'] if rd.get('P') is not None else rd['mkP'](it, bx, by)
    P = [list(map(float, r)) for r in P]
    W = rd.get('W'); V = rd.get('V')
    if it.xyr is not None:
        X, Y = it.ctrl_xy(bx, by); xa = (X[0] - 240) / 256; ya = (136 - Y[0]) / 128
        P[0] = [2.0 ** -it.xyr, 0.0, 0.0, xa]; P[1] = [0.0, 2.0 ** -it.xyr, 0.0, ya]
    V = [list(map(float, r)) for r in V] if V else ident()
    W = [list(map(float, r)) for r in W] if W else ident()
    return P, V, W

def controls(it, bx, by):
    """(cv, cu, 3) model positions in double: what the GE takes in (the mirror's controls)."""
    if it.mkC: return it.mkC(it, bx, by)
    X, Y = it.ctrl_xy(bx, by)
    C = np.zeros((it.cv, it.cu, 3))
    for j in range(it.cv):
        for k in range(it.cu):
            x, y = (X[k] - 240) / 256, (136 - Y[j]) / 128
            if it.xs == 1024.0: x = (X[k] - 240) / 1024
            C[j, k] = (x, y, it.Z[j][k])
    if it.xyr is not None:
        C[..., 0] = (C[..., 0] - C[0, 0, 0]) * 2.0 ** it.xyr
        C[..., 1] = (C[..., 1] - C[0, 0, 1]) * 2.0 ** it.xyr
    return C

def classify(d):
    if d is None: return 'drop'
    if d == 'edge': return 'edge'
    if d < 0: return 'neg'
    if d < 32768: return 'lp'
    if d < 65536: return 'full'
    return 'over'

OFF_X, OFF_Y = 2048 - 240, 2048 - 136          # scene_begin's sceGuOffset
def screen_px(M, x, y, z, xs=256.0, ys=-128.0):
    """The pixel a point lands on: centre 2048 plus scale times clip over w as one GE sum
    (ge.c ge_screen_axis), less the offset, floored. None if w is not usable."""
    v = [x, y, z, 1.0]
    cx, cy, w = dot4(M[0], v), dot4(M[1], v), dot4(M[3], v)
    if not (w > 0 or w < 0) or not math.isfinite(w): return None
    nx, ny = ge_over_w(cx, w), ge_over_w(cy, w)
    if not (math.isfinite(nx) and math.isfinite(ny)): return None
    X = ge_sum([ge_mul(xs, nx), ge_mul(2048.0, 1.0)]) - OFF_X
    Y = ge_sum([ge_mul(ys, ny), ge_mul(2048.0, 1.0)]) - OFF_Y
    return math.floor(X), math.floor(Y)

# =========================================================================== cal band (FRAME 10)
KX, KY, PA, PB = fb(0x3F7B4300), fb(0x3FDDB300), fb(0xBF829500), fb(0xC0014A00)
PERSP_P = [[KX, 0.0, 0.0, 0.0], [0.0, KY, 0.0, 0.0], [0.0, 0.0, PA, PB], [0.0, 0.0, -1.0, 0.0]]
def f32(v): return float(np.float32(v))
def f32mul(a, b): return float(np.float32(a) * np.float32(b))
def cut16(v):                       # rd_cut16: toward zero to 16 significant bits, then float
    r = ge_cut(v); assert f32(r) == r; return r
def ndcx(i): return (2 * (i % 240) + 0.5 - 240.0) / 256.0
def ndcy(i): return (136.0 - (10 + 2 * (i // 240)) - 0.5) / 128.0

def _two(e, s): return [('R', e, s)]
CAL = {
    67: [('R', e, s) for e in (-24, -16, -1, 0, 12) for s in (0, 1)],
    68: [('R', -1, 0), ('R', -1, 1), ('N', -1)],
    69: [('R', -1, 0), ('R', -1, 1), ('A', 3, 1), ('A', 3, -1), ('A', 8, 1), ('A', 13, 1)],
    70: [('R', -30, 0), ('R', -24, 0), ('R', -16, 0), ('R', -1, 0), ('R', -1, 1), ('R', 12, 0), ('R', 12, 1)],
    71: [('A', 0, 1), ('A', 0, -1), ('A', 3, 1), ('A', 4, 1), ('A', 4, -1), ('A', 8, 1), ('A', 13, 1), ('A', 16, 1),
         ('S', 0), ('R', -10, 0), ('R', -10, 1)],
    72: [('R', -1, 0), ('R', -1, 1), ('A', 0, 1), ('A', 0, -1), ('A', 3, 1), ('A', 4, 1), ('A', 4, -1), ('A', 6, 1), ('A', 12, 1)],
    73: [('R', -1, 0), ('R', -1, 1), ('R', -8, 0), ('A', 0, 1), ('A', 0, -1)],
    74: [('R', -1, 0), ('N', -1), ('HALF',), ('ZC',), ('W',)],
    75: [('R', -1, 0), ('W',), ('C',), ('PERSP',), ('NP',)],
    76: [('R', e, s) for e in (100, -125, 64, -60) for s in (0, 1)] + [('COMP', 100), ('COMP', -125)],
}
for _n in (77, 78, 79, 80, 81, 82): CAL[_n] = [('R', -1, 0), ('R', -1, 1)]

def _rvals(s, e, t, r):
    if t == 0: return mk16(s, e, 0)
    if t == 1: return mk16(s, e, 0x7FFF)
    if t == 2: return mk(s, e, 0x7FFFFF)
    if t == 3: return mk(s, e, 0x000080)
    return mk16(s, e, r16(r))

def cal_band(num, xdiv=1.0):
    """480 slots: (i, P, zs, zc, x, y, z, runkey) in slot order, drawing from seed(num, 0) in order."""
    r = R(seed(num, 0)); modes = CAL[num]; m = len(modes); per = 432 // m; out = []
    for mi, mode in enumerate(modes):
        lo = mi * per; hi = 432 if mi == m - 1 else lo + per; st = {}
        for i in range(lo, hi):
            t = i - lo; x, y = ndcx(i) / xdiv, ndcy(i); zc = 0.0
            kind = mode[0]
            if kind == 'R':
                e, s = mode[1], mode[2]; P = P_row(a=(-1) ** s * 2.0 ** -(e + 1)); zs = 65536.0; z = _rvals(s, e, t, r)
            elif kind == 'N':
                e = mode[1]; P = P_row(a=2.0 ** -(e + 1)); zs = -65536.0; z = _rvals(1, e, t, r)
            elif kind in ('HALF', 'ZC'):
                P = P_row(a=1.0); zs = 32768.0; zc = 16384.0 if kind == 'ZC' else 0.0; z = _rvals(0, -1, t, r)
            elif kind == 'A':
                k, sg = mode[1], mode[2]; P = P_row(a=sg * 2.0 ** k); zs = 65536.0
                if t == 0: v = mk16(0, -k - 1, 0x7FFF)
                elif t == 1: v = mk16(0, -k - 1, 0)
                elif t == 2: v = mk16(0, -k - 17, r16(r))
                elif t == 3: v = mk16(0, -k, 0)
                else: v = mk16(0, -k - 1 - ((t - 4) & 7), r16(r))
                z = sg * v
            elif kind == 'S':
                p = mode[1]; P = P_row(a=2.0 ** (p - 1), b=0.5); zs = 65536.0
                if t == 0: z = mk16(0, -p - 1, 0x7FFF)
                elif t == 1: z = -mk16(0, -p - 1, 0x7FFF)
                elif t == 2: z = mk16(0, -p - 17, 0)
                elif t == 3: z = -mk16(0, -p - 17, 0)
                else:
                    xx = r(); z = mk16(xx & 1, -p - 1 - ((xx >> 1) & 3), r16(r))
            elif kind == 'W':
                P = P_row(a=0.0, b=0.5, wz=1.0, ww=0.0); zs = 131072.0
                w = 1.0 if t == 0 else mk16(0, 0, r() & 0x7FF)
                x, y, z = f32mul(x, w), f32mul(y, w), w
                assert f32(ndcx(i) / xdiv * w) == x       # exact: <= 22 significant bits
            elif kind == 'C':
                c = mk16(0, 0, 0x7F00); P = P_row(a=0.0, b=c, wz=1.0, ww=0.0); zs = 65536.0
                w = mk16(0, 1, 0x400 + r() % 0x7800); x, y, z = f32mul(x, w), f32mul(y, w), w
            elif kind == 'PERSP':
                P = PERSP_P; zs = 65536.0
                zz = mk16(1, 2, 8192 + (r() & 511)); w = -zz
                x = cut16((x * w) / KX); y = cut16((y * w) / KY); z = zz
            elif kind == 'NP':
                if 'a' not in st: st['a'] = mk16(0, 0, 0x2EC0 + (r() & 0xFF))
                P = P_row(a=st['a']); zs = 65536.0; z = mk16(0, -1, 0x200 + r() % 0x3500)
            elif kind == 'COMP':
                e = mode[1]; P = P_row(a=2.0 ** -(e + 8), b=0.5); zs = 65536.0; z = mk16(0, e, r16(r))
            else: raise ValueError(mode)
            out.append((i, P, zs, zc, x, y, z, ('m', mi)))
    # 432..479: projection a = 1, b = 0, w = 1
    P1 = P_row(a=1.0)
    def put(i, z, zs): out.append((i, P1, zs, 0.0, ndcx(i) / xdiv, ndcy(i), z, ('X', zs)))
    i = 432
    for zs in (65536.0, -65536.0):
        put(i, 1.5, zs); put(i + 1, mk16(0, 1, r16(r)), zs); put(i + 2, -1.5, zs); put(i + 3, -mk16(0, 1, r16(r)), zs); i += 4
    for t in range(16): put(440 + t, -mk16(0, -1 - (t & 3), r16(r)), 65536.0)
    for t in range(16): put(456 + t, mk16(0, -1 - (t & 3), r16(r)), -65536.0)
    put(472, 1.0, 65536.0); put(473, -1.0, 65536.0); put(474, 1.0, -65536.0); put(475, -1.0, -65536.0)
    put(476, mk(0, -1, 0x7FFFFF), 65536.0); put(477, mk(0, -1, 0x7FFF80), 65536.0)
    put(478, -mk(0, -1, 0x7FFFFF), -65536.0); put(479, mk(0, -1, 0x7FFF7F), 65536.0)
    assert [s[0] for s in out] == list(range(480))
    return out

def cal_colour(i): return 0xFF000000 | (i & 0xFF) | ((i >> 8) & 0xFF) << 8 | 0x40 << 16

# =========================================================================== streams
# One u32 command stream per scene step, replayed by main.c p13_run(). A command is a header
# word (op << 24 | number of argument words) and its arguments. Floats travel as their bits.
OPS = ['NOP', 'BEGIN', 'MATS', 'ZVIEW', 'DIVIDE', 'PATCHPRIM', 'SHADE', 'BLEND', 'MORPH', 'BONE',
       'ITEM', 'BEZIER', 'SPLINE', 'DRAW', 'BATCH', 'CALDONE', 'B9', 'END']
OP = {n: i for i, n in enumerate(OPS)}

# pspgu.h vertex type bits
GU_COLOR_8888, GU_VERTEX_8BIT, GU_VERTEX_16BIT, GU_VERTEX_32BITF = 7 << 2, 1 << 7, 2 << 7, 3 << 7
GU_WEIGHT_32BITF, GU_INDEX_8BIT, GU_INDEX_16BIT = 3 << 9, 1 << 11, 2 << 11
def GU_VERTICES(n): return ((n - 1) & 7) << 18
def GU_WEIGHTS(n): return ((n - 1) & 7) << 14
FMT_CV3D = GU_COLOR_8888 | GU_VERTEX_32BITF            # GU_TRANSFORM_3D is 0
VTYPE = {'f': FMT_CV3D, 's16abs': GU_COLOR_8888 | GU_VERTEX_16BIT, 's16anc': GU_COLOR_8888 | GU_VERTEX_16BIT,
         's8anc': GU_COLOR_8888 | GU_VERTEX_8BIT, 'idx16': FMT_CV3D | GU_INDEX_16BIT, 'idx8': FMT_CV3D | GU_INDEX_8BIT,
         'morphAB': FMT_CV3D | GU_VERTICES(2), 'morphA2B2': FMT_CV3D | GU_VERTICES(2),
         'skin': FMT_CV3D | GU_WEIGHTS(1) | GU_WEIGHT_32BITF}
GU_ADD, GU_FIX = 0, 10

STEPS = [  # (scene, builder, dump name, step title)
    (67, sc67, 'patchconv', 'patch control conversion, constants and 24-bit controls'),
    (68, sc68, 'patchlerpu', 'random cubics along u, every division 1-48, and their mirrors'),
    (69, sc69, 'patchlerpv', 'the same cubics along v, and one control on zero'),
    (70, sc70, 'patchscale', 'one set of cubics at 15 scales, in model and clip space'),
    (71, sc71, 'patchalign', 'mixed magnitudes and signs inside a lerp: ladders, sign-split reads, ties'),
    (72, sc72, 'patch2d', '2D Bezier patches: transposes, reversals, negations, order contrast, colour order'),
    (73, sc73, 'patchspline', 'splines in every edge mode, 1D and 2D, exponent reach, span boundaries'),
    (74, sc74, 'patchspace', 'where the GE tessellates: cancellation, screen-space halving, axis and w routing, random rows'),
    (75, sc75, 'patchvalid', 'perspective, w cancellation, near-equal, a 16-bit coefficient, ramps, end-to-end, triangle colour planes'),
    (76, sc76, 'patchrange', 'extreme magnitudes (risky, own step)'),
    (77, sc77, 'patchfmt', '16-bit and 8-bit controls, indexed patches (risky, own step)'),
    (78, sc78, 'patchmorph', 'morphed and skinned patches (risky, own step)'),
    (79, sc79, 'patchbig48', '48 x 48 patches (risky, own step)'),
    (80, sc80, 'patchdiv', 'divisions 49 to 128 (risky, own step)'),
    (81, sc81, 'patchdivmax', 'divisions 129 to 255 (risky, own step)'),
    (82, sc82, 'patchbig64', '64 x 64 patches (riskiest, step 1 of 2)'),
    (82, sc82b, 'patchrows', 'rows of more than 256 samples (riskiest, step 2 of 2)'),
]

def u32(f): return bf(f) if isinstance(f, float) else f & 0xFFFFFFFF
def exact32(v):
    v = float(v); assert f32(v) == v and math.isfinite(v), v; return v
def mem16(M):
    """A 4x4 row list as ScePspFMatrix4 memory: column-major, row r of column c at [4c + r]."""
    words = [exact32(M[r][c]) for c in range(4) for r in range(4)]
    for w in words: assert sig16(w), (w, M)
    return words
def words_of(b):
    b = b + b'\0' * (-len(b) % 4)
    return list(struct.unpack('<%dI' % (len(b) // 4), b))

class Built:
    """A scene step with its items placed, IDs assigned and the cal band generated."""
    def __init__(s, num, fn, name, title):
        s.num, s.name, s.title = num, name, title
        s.sc = fn(); s.its, s.pos, s.ytop, s.apos = place(s.sc)
        cnt = collections.Counter()
        for n, it in enumerate(s.its):
            it.id = n + 1; it.bidx = cnt[it.batch]; cnt[it.batch] += 1
        s.xs = 1024.0 if num == 81 else 256.0
        s.vpw = 2048 if num == 81 else 512
        s.cal = cal_band(num, 4.0 if num == 81 else 1.0)
        s.timing = 1 if num >= 76 else 0
        s.anchors = s.make_anchors() if num == 77 else []

    def col(s, it, j, k):
        if it.cmode == 'id': return 0xFF000000 | (0x40 + it.batch) << 16 | it.id
        if it.cmode == 'blue': return 0xFF000000 | (0x80 + it.bidx) << 16 | it.colgrid[j][k]
        return 0xFF000000 | it.colgrid[j][k]

    def vblob(s, it, C):
        """(vtype, vertex bytes, index bytes) exactly as handed to the GE."""
        cu, cv = it.cu, it.cv; f = it.fmt
        def cvb(col, x, y, z): return struct.pack('<Ifff', col, exact32(x), exact32(y), exact32(z))
        def ival(v, scale, lim):
            q = v * scale; assert q == int(q) and abs(q) <= lim, (v, scale); return int(q)
        out = []
        idx = b''
        for j in range(cv):
            for k in range(cu):
                x, y, z = C[j, k]; c = s.col(it, j, k)
                if f in ('f', 'idx16', 'idx8'): out.append(cvb(c, x, y, z))
                elif f in ('s16abs', 's16anc'):
                    out.append(struct.pack('<Ihhhh', c, ival(x, 32768, 32767), ival(y, 32768, 32767), ival(z, 32768, 32767), 0))
                elif f == 's8anc':
                    out.append(struct.pack('<Ibbbb', c, ival(x, 128, 127), ival(y, 128, 127), ival(z, 128, 127), 0))
                elif f == 'morphAB':
                    A, Bz, _ = it.extra
                    out.append(cvb(c, x, y, A[j][k]) + cvb(0, 0.0, 0.0, Bz[j][k]))
                elif f == 'morphA2B2':
                    A2, B2, _ = it.extra
                    out.append(cvb(c, x, y, A2[j][k]) + cvb(c, x, y, B2[j][k]))
                elif f == 'skin':
                    vz, _ = it.extra
                    out.append(struct.pack('<fIfff', 1.0, c, exact32(x), exact32(y), exact32(vz[j][k])))
                else: raise ValueError(f)
        if f == 'idx16':        # stored reversed, indices 15..0
            out = out[::-1]; idx = struct.pack('<16H', *[15 - m for m in range(16)])
        elif f == 'idx8':       # control m stored at perm[m] and indexed there
            perm = it.extra; st = [None] * 16
            for m in range(16): st[perm[m]] = out[m]
            out = st; idx = struct.pack('<16B', *perm)
        return VTYPE[f], b''.join(out), idx

    def record(s, it, bx, by):
        return struct.pack('<BBBBBBBBhhhH', 0 if it.kind == 'b' else 1, it.cu, it.cv, it.eu | it.ev << 2, it.du, it.dv,
                           it.batch, it.code, it.par, 2 * bx, 2 * by, it.id)

    def make_anchors(s):
        """77 b6: (n, P, zs, vtype, colour, (x, y, z) as stored ints, model (x, y, z), draw group)."""
        Xb, Yb = s.apos; out = []
        def items_of(b, tagfmt): return [it for it in s.its if it.batch == b and it.tag[1] == tagfmt]
        z16 = []
        for it in items_of(1, 's16') + items_of(2, 's16') + items_of(3, 's16'):
            for j in range(4):
                for k in range(4): z16.append((it.batch, int(it.Z[j][k] * 32768)))
        z8 = [int(it.Z[j][k] * 128) for it in items_of(2, 's8') for j in range(4) for k in range(4)]
        assert len(z16) == 832 and len(z8) == 256
        Xa, Ya = Xb + 0.5, Yb + 0.5
        Panc = [[1.0, 0, 0, (Xa - 240) / 256], [0, 1.0, 0, (136 - Ya) / 128], [0, 0, 1.0, 0], [0, 0, 0, 1.0]]
        for n in range(1088):
            col, row = n % 120, n // 120; X = Xb + 0.5 + 2 * col; Y = Yb + 0.5 + 2 * row
            c = 0xFF000000 | 0x46 << 16 | (1000 + n // 256)
            if n < 832:
                b, zq = z16[n]; xq, yq = int(64 * (2 * X - 480)), int(128 * (272 - 2 * Y))
                P = P_row(a=-1.0 if b == 3 else 1.0)
                out.append((n, P, 65536.0, GU_COLOR_8888 | GU_VERTEX_16BIT, c, (xq, yq, zq),
                            (xq / 32768, yq / 32768, zq / 32768), 1 if b == 3 else 0))
            else:
                zq = z8[n - 832]; xq, yq = col, -2 * row
                out.append((n, Panc, 65536.0, GU_COLOR_8888 | GU_VERTEX_8BIT, c, (xq, yq, zq),
                            (xq / 128, yq / 128, zq / 128), 2))
        return out

class Emitter:
    def __init__(e):
        e.w = []; e.cur = {}; e.ctrl_crc = 0; e.item_crc = 0; e.items = 0; e.samples = 0
    def op(e, name, args=()):
        args = [u32(a) for a in args]
        assert len(args) < (1 << 24)
        e.w.append(OP[name] << 24 | len(args)); e.w.extend(args)
    def mats(e, P, V, W, zs, zc):
        mask = 0; data = []
        for bit, key, M in ((1, 'P', P), (2, 'V', V), (4, 'W', W)):
            m = mem16(M)
            if e.cur.get(key) != m: mask |= bit; data += m; e.cur[key] = m
        if mask:
            e.op('MATS', [mask] + data)
        if mask or e.cur.get('z') != (zs, zc):
            e.op('ZVIEW', [exact32(zs), exact32(zc)]); e.cur['z'] = (zs, zc)
    def state(e, name, val, args):
        if e.cur.get(name) != val: e.op(name, args); e.cur[name] = val

def emit(B):
    """The stream for one Built scene step; also fills B.ctrl_crc/item_crc/items/samples/stream_crc."""
    E = Emitter()
    E.op('BEGIN', [B.vpw, 256, B.timing])
    E.cur.update(PATCHPRIM=PRIM_POINTS, SHADE=1, MORPH=(1.0, 0.0), BLEND=0)
    I4 = ident()
    E.op('BATCH', [0])
    runs = []
    for sl in B.cal:
        if runs and runs[-1][0] == sl[7]: runs[-1][1].append(sl)
        else: runs.append((sl[7], [sl]))
    for _, sls in runs:
        _, P, zs, zc = sls[0][:4]
        E.mats(P, I4, I4, zs, zc)
        blob = b''.join(struct.pack('<Ifff', cal_colour(i), exact32(x), exact32(y), exact32(z)) for i, _, _, _, x, y, z, _ in sls)
        E.op('DRAW', [PRIM_POINTS, FMT_CV3D, len(sls), len(blob), 0] + words_of(blob))
    E.op('CALDONE')
    batches = sorted({it.batch for it in B.its} | {Bt.b for Bt in B.sc.batches if Bt.npts and not Bt.cal})
    for b in batches:
        Bt = [x for x in B.sc.batches if x.b == b][0]
        E.op('BATCH', [b])
        if Bt.blend: E.op('BLEND', [1, GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF]); E.cur['BLEND'] = 1
        late = []
        for it, (bx, by) in zip(B.its, B.pos):
            if it.batch != b: continue
            P, V, W = mats(it, bx, by)
            E.mats(P, V, W, it.rd['zs'], it.rd.get('zc', 0.0))
            E.state('PATCHPRIM', it.prim, [it.prim]); E.state('SHADE', it.shade, [it.shade])
            if it.fmt.startswith('morph'): wts = it.extra[2]
            else: wts = (1.0, 0.0)
            E.state('MORPH', wts, list(wts) + [0.0] * 6)
            if it.fmt == 'skin':
                T = it.extra[1]
                bone = [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, T], [0, 0, 0, 1.0]]
                E.state('BONE', T, [0] + mem16(bone))
            rec = B.record(it, bx, by)
            E.op('ITEM', words_of(rec) + [it.n])
            E.item_crc = zlib.crc32(rec, E.item_crc); E.items += 1; E.samples += it.n
            if it.late: late.append((it, bx, by)); continue
            vt, vb, ib = B.vblob(it, controls(it, bx, by))
            E.ctrl_crc = zlib.crc32(vb + ib, E.ctrl_crc)
            E.op('DIVIDE', [it.du, it.dv])
            blob = words_of(vb) + words_of(ib)
            if it.kind == 'b': E.op('BEZIER', [vt, it.cu, it.cv, len(vb), len(ib)] + blob)
            else: E.op('SPLINE', [vt, it.cu, it.cv, it.eu, it.ev, len(vb), len(ib)] + blob)
        if Bt.blend: E.op('BLEND', [0, GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF]); E.cur['BLEND'] = 0
        if Bt.npts and B.anchors:           # 77 b6: three plain point draws
            for g in range(3):
                pts = [a for a in B.anchors if a[7] == g]
                E.mats(pts[0][1], I4, I4, 65536.0, 0.0)
                if g < 2: blob = b''.join(struct.pack('<Ihhhh', a[4], *a[5], 0) for a in pts)
                else: blob = b''.join(struct.pack('<Ibbbb', a[4], *a[5], 0) for a in pts)
                E.op('DRAW', [PRIM_POINTS, pts[0][3], len(pts), len(blob), 1] + words_of(blob))
                E.samples += len(pts)
        if late:                             # 75 b9: read A back, then draw C
            data = [len(late)]
            for it, bx, by in late:
                A = [x for x in B.its if x.batch == b and x.tag == ('tri', 0) and x.du == it.du and x.dv == it.dv][0]
                bxA, byA = B.pos[B.its.index(A)]
                X0, Y0_ = A.ctrl_xy(bxA, byA)[0][0], A.ctrl_xy(bxA, byA)[1][0]
                nu, nv = it.du + 1, it.dv + 1
                data += [nu, nv]
                for j in range(nv):
                    for i in range(nu):
                        X = X0 + 48 * Tp(i, it.du) / 256; Y = Y0_ + 48 * Tp(j, it.dv) / 256
                        px, py = math.floor(X), math.floor(Y)
                        XC, YC = X + (bx - bxA), Y + (by - byA)
                        data += [px | py << 16, exact32((XC - 240) / 256), exact32((136 - YC) / 128), 0.5]
            E.op('B9', data)
    E.op('END')
    B.words = E.w; B.ctrl_crc, B.item_crc, B.nitems, B.nsamples = E.ctrl_crc, E.item_crc, E.items, E.samples
    B.stream_crc = zlib.crc32(struct.pack('<%dI' % len(E.w), *E.w))
    return E.w

def build_all(which=None):
    out = []
    for num, fn, name, title in STEPS:
        if which and num not in which: continue
        B = Built(num, fn, name, title); emit(B); out.append(B)
    return out

def write_inc(path, Bs):
    """Write the include only when its text changes, so make (main.o depends on it) does not rebuild
    for an identical emit."""
    import io
    f = io.StringIO()
    _write_inc(f, Bs)
    text = f.getvalue()
    try:
        with open(path) as g:
            if g.read() == text: return False
    except OSError:
        pass
    with open(path, 'w') as g: g.write(text)
    return True


def _write_inc(f, Bs):
    f.write('/* Generated by patch13.py emit: geprobe 13 scenes 67-82 as command streams.\n'
            ' * Do not edit; change patch13.py and run it again. */\n')
    for i, n in enumerate(OPS): f.write(f'#define P13_{n} {i}\n')
    for k, B in enumerate(Bs):
        f.write(f'\n/* scene {B.num} {B.name}: {B.nitems} items, {B.nsamples} samples, {len(B.words)} words,'
                f' stream crc {B.stream_crc:08X} */\nstatic const w32 P13_S{k}[{len(B.words)}] = {{\n')
        ws = B.words
        for i in range(0, len(ws), 8):
            f.write(' ' + ' '.join(f'0x{w:08X}u,' for w in ws[i:i + 8]) + '\n')
        f.write('};\n')
    f.write('\nstatic const struct p13_step { int scene; const char *name; const char *title; const w32 *s; int n; } P13_STEPS[] = {\n')
    for k, B in enumerate(Bs):
        f.write(f'    {{ {B.num}, "{B.name}", "{B.title}", P13_S{k}, {len(B.words)} }},\n')
    f.write('};\n#define P13_NSTEPS ((int)(sizeof P13_STEPS / sizeof P13_STEPS[0]))\n')

# =========================================================================== prediction
def predict(B, model='LAv'):
    """Every point the step draws: dict(kind, batch, item, i, j, cls, d, px, ident, col, n).
    Positions and depths through the GE arithmetic of ge13 (LAv: LA v-first, input cut)."""
    out = []
    for i, P, zs, zc, x, y, z, _ in B.cal:
        M = wvp(P)
        d, _, _ = depth(M, x, y, z, zs, zc)
        out.append(dict(kind='cal', batch=0, item=None, i=i, j=0, cls=classify(d), d=d,
                        px=screen_px(M, x, y, z, B.xs), ident='exact', col=cal_colour(i)))
    for n, (it, (bx, by)) in enumerate(zip(B.its, B.pos)):
        if not it.points or getattr(it, 'nopts', False): continue
        C = controls(it, bx, by)
        P, V, W = mats(it, bx, by); M = wvp(P, V, W)
        G = run_model(model, C, it.su, it.sv)
        nv, nu = G.shape[:2]
        col = s_col = B.col(it, 0, 0)
        for j in range(nv):
            for i in range(nu):
                x, y, z = G[j, i]
                d, _, _ = depth(M, x, y, z, it.rd['zs'], it.rd.get('zc', 0.0))
                out.append(dict(kind='item', batch=it.batch, item=n, i=i, j=j, cls=classify(d), d=d,
                                px=screen_px(M, x, y, z, B.xs), ident=it.ident, col=col, offok=it.offscreen_ok,
                                past256=max(i, j) >= 256))
    for a in B.anchors:
        M = wvp(a[1]); x, y, z = a[6]
        d, _, _ = depth(M, x, y, z, a[2])
        out.append(dict(kind='anchor', batch=6, item=None, i=a[0], j=0, cls=classify(d), d=d,
                        px=screen_px(M, x, y, z, B.xs), ident='exact', col=a[4]))
    return out

# =========================================================================== reading a run
def depth_full(path):
    """A `_depthfull.bin` dump (512-pixel stride through the plain VRAM address) in screen
    order: render.c depth_addr's permutation (as readout.py)."""
    raw = np.fromfile(path, dtype='<u2')
    y, x = np.mgrid[0:272, 0:480]
    l = 0x88000 + (y * 512 + x) * 2
    mid = (l >> 5) & 0x1F
    rot = ((mid << 1) | (mid >> 4)) & 0x1F
    p = ((l & ~(0x1F << 5)) | (rot << 5)) ^ 0x2040
    return raw[(p - 0x88000) // 2]

def matches(ident, pix, col):
    """Does lit pixel `pix` belong to a sample coloured `col` under identification rule `ident`?"""
    if pix & 0xFFFFFF == 0: return False                       # the clear colour (alpha aside)
    if ident == 'exact': return (pix & 0xFFFFFF) == (col & 0xFFFFFF)
    if ident == 'blue': return ((pix >> 16) & 0xFF) == ((col >> 16) & 0xFF)
    if ident == 'blue1': return abs(((pix >> 16) & 0xFF) - ((col >> 16) & 0xFF)) <= 1
    if ident == 'b48': return ((pix >> 16) & 0xFF) == 0x48
    if ident == 'box': return True
    if ident == 'add':
        r, g, b = pix & 0xFF, (pix >> 8) & 0xFF, (pix >> 16) & 0xFF; k = (col >> 8) & 0xFF
        return (r, g, b) in ((0x20, k, 0x20), (0x40, 2 * k, 0x40))
    raise ValueError(ident)

def log_steps(log):
    """{(scene, name): text of that step's log} from geprobe.txt."""
    out = {}
    parts = re.split(r'\n(?=\S.*?scene (\d\d): )', '\n' + log)
    for p in parts:
        m = re.search(r'scene (\d\d): (.*)', p)
        if not m: continue
        out.setdefault(int(m.group(1)), []).append(p)
    return out

def check(base, which=None, verbose=False):
    log = open(os.path.join(base, 'geprobe.txt'), errors='replace').read()
    steps = log_steps(log)
    total = collections.Counter(); allok = True
    for B in build_all(which):
        txts = steps.get(B.num, [])
        txt = ''
        for t in txts:
            if f'_{B.name}' in t or (B.title.split(' (')[0] in t): txt = t
        m = re.search(r'(\d+) items, (\d+) samples, control crc ([0-9A-F]{8}), item crc ([0-9A-F]{8}), stream crc ([0-9A-F]{8})', txt)
        want = (B.nitems, B.nsamples, B.ctrl_crc, B.item_crc, B.stream_crc)
        got = tuple(int(g, 16) if i >= 2 else int(g) for i, g in enumerate(m.groups())) if m else None
        crcok = got == want
        allok &= crcok
        print(f'== scene {B.num} {B.name}: {B.nitems} items, {B.nsamples} samples, control crc {B.ctrl_crc:08X}, '
              f'item crc {B.item_crc:08X}, stream crc {B.stream_crc:08X}: log {"MATCHES" if crcok else ("DIFFERS " + str(got))}')
        for l in re.findall(r'.*(?:GE \d+ us|headroom|b9 miss|GE still busy).*', txt): print('   log:', l.strip())
        cpath = os.path.join(base, f'ge_{B.num:02d}_{B.name}.raw'); dpath = os.path.join(base, f'ge_{B.num:02d}_{B.name}_depthfull.bin')
        if not (os.path.exists(cpath) and os.path.exists(dpath)):
            print('   no dumps'); allok = False; continue
        col = np.fromfile(cpath, dtype='<u4').reshape(272, 480); dep = depth_full(dpath)
        pred = predict(B)
        stat = collections.defaultdict(collections.Counter); miss = collections.defaultdict(list)
        claimed = set()
        for p in pred:
            key = (p['batch'], p['kind'])
            s = stat[key]; s['n'] += 1
            px = p['px']
            on = px is not None and 0 <= px[0] < 480 and 0 <= px[1] < 272
            c = p['cls']
            if not on:
                s['offscreen'] += 1
                if not p.get('offok'): s['OFFSCREEN_UNEXPECTED'] += 1
                continue
            pix = int(col[px[1], px[0]]); lit = matches(p['ident'], pix, p['col'])
            if lit: claimed.add(px)
            if c in ('full', 'lp', 'over'):
                s['expected'] += 1
                if lit:
                    s['drawn'] += 1
                    if p['ident'] in ('exact', 'add') and c != 'over':
                        s['depth_ok'] += int(dep[px[1], px[0]]) == p['d']
                        s['depth_n'] += 1
                elif p.get('past256'):           # 82 step 2: whether such samples exist is the reading
                    s['absent_past_256'] += 1
                else:
                    s['MISSING'] += 1; miss[key].append((p['item'], p['i'], p['j'], px, f'{pix:08X}'))
            elif c == 'drop':
                s['drop'] += 1
                if lit: s['DROP_LIT'] += 1
            else:
                s[c] += 1; s[c + '_lit' if lit else c + '_absent'] += 1
        # lit pixels in an item's box that no prediction claims
        for n, (it, (bx, by)) in enumerate(zip(B.its, B.pos)):
            if not it.points or getattr(it, 'nopts', False):
                x0, x1, y0, y1 = max(bx, 0), min(bx + it.w, 480), max(by, 0), min(by + it.h, 272)
                lit = int(((col[y0:y1, x0:x1] & 0xFFFFFF) != 0).sum()) if x1 > x0 and y1 > y0 else 0
                stat[(it.batch, 'cells')]['cells'] += 1; stat[(it.batch, 'cells')]['lit_px'] += lit
                continue
            c0 = B.col(it, 0, 0)
            for y in range(max(by, 0), min(by + it.h, 272)):
                for x in range(max(bx, 0), min(bx + it.w, 480)):
                    if (x, y) not in claimed and matches(it.ident, int(col[y, x]), c0):
                        stat[(it.batch, 'item')]['EXTRA'] += 1
        # 75 b9: patch triangles (B) against plain triangles from the read-back colours (C), pixel by pixel
        for it, (bx, by) in zip(B.its, B.pos):
            if it.tag == ('tri', 2):
                Bi = [x for x in B.its if x.batch == it.batch and x.tag == ('tri', 1) and x.du == it.du][0]
                bxB, byB = B.pos[B.its.index(Bi)]
                cb = col[byB:byB + it.h, bxB:bxB + it.w] & 0xFFFFFF; cc = col[by:by + it.h, bx:bx + it.w] & 0xFFFFFF
                s = stat[(it.batch, 'B vs C')]
                s['px'] += int(((cb != 0) | (cc != 0)).sum()); s['differ'] += int((cb != cc).sum())
                s['differ_interior'] += int((cb[1:-1, 1:-1] != cc[1:-1, 1:-1]).sum())
                s['C_black'] += int(((cb != 0) & (cc == 0)).sum())
        for key in sorted(stat):
            s = stat[key]
            bad = s['MISSING'] or s['DROP_LIT'] or s['EXTRA'] or s['OFFSCREEN_UNEXPECTED']
            allok &= not bad
            for k in ('n', 'expected', 'drawn', 'MISSING', 'absent_past_256', 'drop', 'DROP_LIT', 'EXTRA', 'offscreen', 'neg', 'edge', 'depth_ok', 'depth_n'):
                total[k] += s[k]
            print(f'   b{key[0]} {key[1]:6s} ' + ' '.join(f'{k}={v}' for k, v in s.items()))
            for mm in miss[key][:8 if not verbose else 10 ** 9]: print('      missing item/i/j/px/pixel', mm)
    print('TOTAL', dict(total), 'ALL OK' if allok else 'PROBLEMS')
    return allok

def points(base, num):
    """Every point of a scene: its inputs' class, predicted depth and pixel, and the run's reading."""
    Bs = [B for B in build_all([num])]
    for B in Bs:
        cpath = os.path.join(base, f'ge_{B.num:02d}_{B.name}.raw')
        col = np.fromfile(cpath, dtype='<u4').reshape(272, 480) if base and os.path.exists(cpath) else None
        dep = depth_full(cpath.replace('.raw', '_depthfull.bin')) if col is not None else None
        last = None
        for p in predict(B):
            if p['kind'] == 'item' and p['item'] != last:      # the item's inputs, once
                last = p['item']; it = B.its[last]; bx, by = B.pos[last]
                P, V, W = mats(it, bx, by); C = controls(it, bx, by)
                print(f"# item {last} id {it.id} batch {it.batch} {'Bezier' if it.kind == 'b' else 'spline'} "
                      f"{it.cu}x{it.cv} edges {it.eu},{it.ev} div {it.du},{it.dv} box ({bx},{by}) {it.w}x{it.h} "
                      f"code {it.code} par {it.par} fmt {it.fmt} tag {it.tag!r} zs {it.rd['zs']} zc {it.rd.get('zc', 0.0)}")
                for nm, Mx in (('P', P), ('V', V), ('W', W)):
                    print(f'#   {nm} rows', ' | '.join(' '.join(f'{bf(v):08X}' for v in r) for r in Mx))
                for j in range(it.cv):
                    print(f'#   row {j}:', ' '.join('(%08X %08X %08X %08X)' % (B.col(it, j, k), bf(C[j, k, 0]), bf(C[j, k, 1]), bf(C[j, k, 2]))
                                                for k in range(it.cu)))
            px = p['px']; got = ''
            if col is not None and px and 0 <= px[0] < 480 and 0 <= px[1] < 272:
                got = f'{int(col[px[1], px[0]]):08X} {int(dep[px[1], px[0]])}'
            print(B.name, p['kind'], p['batch'], p['item'], p['i'], p['j'], p['cls'], p['d'], px, got)

def main(argv):
    if len(argv) < 2 or argv[1] not in ('emit', 'check', 'points'):
        print(__doc__); return 2
    if argv[1] == 'emit':
        here = os.path.dirname(os.path.abspath(__file__))
        Bs = build_all()
        changed = write_inc(os.path.join(here, 'patch13_data.inc'), Bs)
        print('patch13_data.inc', 'written' if changed else 'unchanged (left as is)')
        for B in Bs:
            print(f'scene {B.num} {B.name}: {B.nitems} items, {B.nsamples} samples, control crc {B.ctrl_crc:08X}, '
                  f'item crc {B.item_crc:08X}, stream crc {B.stream_crc:08X}, {len(B.words)} words')
        print('total words', sum(len(B.words) for B in Bs))
        return 0
    if argv[1] == 'check':
        which = [int(a) for a in argv[3:] if not a.startswith('-')] or None
        return 0 if check(argv[2], which, '-v' in argv) else 1
    if argv[1] == 'points':
        points(argv[2], int(argv[3])); return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
