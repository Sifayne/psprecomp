#!/usr/bin/env python3
"""geprobe 16 (scenes 110-114): skinned and morphed vertex positions, read whole.

Scene 20's one skinned triangle with three float weights and a rotated bone is the last
3D position psprecomp gets wrong: its 404040 corner lands a sixteenth of a pixel low.
These scenes read skinned (and morphed) coordinates whole, as geprobe 12 read the vertex
path: with clip w 1, the projection's z row a power of two times one model axis, and a
viewport z scale of +-65536 with centre 0, a point's depth is that skinned coordinate's
top 16 bits (or 15 below 1/2). Each point carries its own colour, an id, and is put on
its own pixel by a screen offset (OFFSET), so its skinned x and y are free and bones may
rotate. Streams in colour14.py's format, replayed by main.c c14_run with the BONE, MORPH
and ZVIEW ops this probe adds.

The mirror predicts each point's depth under these rules for the blend, each followed by
the settled vertex path (patch13: ge_mul/ge_sum, combined matrix, 1/w, viewport):
    A    each bone's transform one GE row sum (dot4), times its weight (ge_mul), and the
         weighted terms one aligned sum (ge_sum) -- the matrix row rule, n terms
    A2   the same, the weighted terms summed one at a time
    M    the bone matrices blended first, entry by entry (one sum each), then one dot4
    C    each bone folded into the combined matrix (P V W B_i), the clip coordinates
         blended (one sum); clip w is the (affine) bones' common one
    F    float32 arithmetic, as psprecomp does today (morph weights as the 24-bit floats
         libgu sends), then the settled path; it reproduces psprecomp's every point
and, where they apply,
    Am   morphing and skinning: the sets' weights and positions blended first, then skinned
         once (the others skin each set and blend the results)
    As   8- and 16-bit weights signed (0xFF is -1/128), not unsigned
Weights: float as given (cut to 16 bits by ge_mul), u8 / 128, u16 / 32768 (unsigned).
A morph blend follows the rule too: one sum (A, M, C), chained (A2), or float32 (F).

Set 17 (fw 6.60) answered with none of these, but with
    H    one accumulator (ge_acc): each step puts it and the new term on the grid
         2^(E-15), E the larger one's own exponent, each cut toward zero, and cuts
         the sum to 16 bits. Morphing adds mw_s v_s set by set (weights morphed the
         same way), then the vertex is skinned once; skinning adds, bone by bone, the
         translation, x, y, z terms (w_i B_i[k] cut) v[k] cut
which fits every point drawn, 4171 of 4171; and a point is drawn only when |clip x|
and |clip y| are within w (the 379 others), which H's prediction includes.

    skin16.py emit [OUT.inc]        write the streams (default c16_data.inc)
    skin16.py sums DUMPDIR          every scene's SUMS line against these streams
    skin16.py compare DUMPDIR [rule..]   per scene and batch: points found, depth matches per rule
    skin16.py check                 design checks: points per scene, readout bits, separation
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
import patch13 as P13
from colour14 import XS, f32, bf

GU_POINTS = 0
GU_WEIGHT_8BIT, GU_WEIGHT_16BIT, GU_WEIGHT_32BITF = 1 << 9, 2 << 9, 3 << 9
GU_COLOR_8888, GU_VERTEX_32BITF = 7 << 2, 3 << 7
def GU_WEIGHTS(n): return ((n - 1) & 7) << 14
def GU_VERTICES(n): return ((n - 1) & 7) << 18
GU_ALWAYS = 1
RULES = ['H', 'A', 'A2', 'M', 'C', 'F', 'Am', 'As']
ID = P13.ident()

# =========================================================================== numbers
def m16(r, s, e, lo=0, hi=0x8000):
    """(-1)^s 2^e (1 + m/2^15), m in [lo, hi): a value a matrix word holds exactly."""
    return C.f32(math.ldexp(1 + (lo + r() % (hi - lo)) / 32768.0, e) * (-1 if s else 1))
def m24(r, s, e, lo=0.0, hi=1.0):
    """A float32 with a random 23-bit significand in [1+lo, 1+hi) times 2^e."""
    m = int((lo + (hi - lo) * (r() / 2**32)) * 2**23)
    return f32(math.ldexp(1 + m / 2**23, e) * (-1 if s else 1))
def bone(zrow, xrow=(1.0, 0.0, 0.0, 0.0), yrow=(0.0, 1.0, 0.0, 0.0)):
    """A bone as GE rows (x, y, z, w); w row 0 0 0 1."""
    return [list(xrow), list(yrow), list(zrow), [0.0, 0.0, 0.0, 1.0]]
def rot_z(c, s, tx=0.0, ty=0.0, zrow=(0.0, 0.0, 1.0, 0.0)):
    return bone(zrow, (c, -s, 0.0, tx), (s, c, 0.0, ty))
def mem16(B):  # column-major 4x4 for sceGuBoneMatrix (gum layout: x, y, z, w columns)
    return [bf(B[r][c]) for c in range(4) for r in range(4)]
def f24(v):
    """A float as a GE command word holds it: libgu sends a float's top 24 bits (>> 8)."""
    return struct.unpack('<f', struct.pack('<I', bf(v) >> 8 << 8))[0]
def wval(fmt, raw, signed=False):
    if fmt == 'u8': return (raw - 256 if signed and raw > 127 else raw) / 128.0
    if fmt == 'u16': return (raw - 65536 if signed and raw > 32767 else raw) / 32768.0
    return raw

# =========================================================================== the arithmetic
def c16(v): return P13.ge_cut(v)
def ge_acc(acc, t):
    """One step of the skinning and morphing accumulator (set 17)."""
    if t == 0: return acc
    if acc == 0: return c16(t)
    E = max(math.frexp(abs(acc))[1], math.frexp(abs(t))[1]) - 1
    u = math.ldexp(1.0, E - 15)
    return c16(math.trunc(acc / u) * u + math.trunc(t / u) * u)
H_ORDER = (3, 0, 1, 2)

def skin_pos(rule, bones, ws, v):
    """Skinned model position (x, y, z) under rule H, A, A2, M or F."""
    v4 = [v[0], v[1], v[2], 1.0]
    if rule == 'H':
        out = []
        for c in range(3):
            acc = 0.0
            for B, w in zip(bones, ws):
                for k in H_ORDER: acc = ge_acc(acc, c16(c16(c16(w) * c16(B[c][k])) * c16(v4[k])))
            out.append(acc)
        return out
    if rule == 'F':
        out = []
        for c in range(3):
            acc = f32(0.0)
            for B, w in zip(bones, ws):
                t = f32(0.0)
                for k in range(4): t = f32(t + f32(f32(B[c][k]) * f32(v4[k])))
                acc = f32(acc + f32(f32(w) * t))
            out.append(acc)
        return out
    if rule == 'M':
        Bb = [[P13.ge_sum([P13.ge_mul(w, B[c][k]) for B, w in zip(bones, ws)]) for k in range(4)] for c in range(3)]
        return [P13.dot4(Bb[c], v4) for c in range(3)]
    out = []
    for c in range(3):
        terms = [P13.ge_mul(w, P13.dot4(B[c], v4)) for B, w in zip(bones, ws)]
        if rule == 'A': out.append(P13.ge_sum(terms))
        else:
            s = 0.0
            for i, t in enumerate(terms):
                s = P13.ge_sum([P13.ge_mul(1.0, s), t]) if i else P13.ge_sum([t])
            out.append(s)
    return out

def predict_depth(rule, pt):
    """A point's depth: its (morphed and) skinned position through P V W, w 1, ZVIEW."""
    P, V, W, zs = pt['P'], pt['V'], pt['W'], pt['zs']
    M = P13.wvp(P, V, W)
    sets = pt['sets']                       # [(morph weight, vertex (x,y,z), weights or None)]
    if rule == 'C' and pt['bones']:
        assert len(sets) == 1
        _, v, ws = sets[0]
        v4 = [v[0], v[1], v[2], 1.0]
        Mb = [P13.mat_mul(M, B) for B in pt['bones']]
        cz = P13.ge_sum([P13.ge_mul(w, P13.dot4(MB[2], v4)) for MB, w in zip(Mb, ws)])
        w_ = P13.dot4(Mb[0][3], v4)       # affine bones: every P V W B_i has M's w row
        if not (abs(cz) < abs(w_)) or w_ <= 0: return None
        nz = P13.ge_over_w(cz, w_)
        return math.floor(P13.ge_sum([P13.ge_mul(zs, nz), P13.ge_mul(0.0, 1.0)]))
    sr = 'A' if rule in ('C', 'Am', 'As') else rule      # the skinning rule under it
    def blend(vals, mws):
        if rule == 'H':
            acc = 0.0
            for mw, x in zip(mws, vals): acc = ge_acc(acc, c16(f24(mw)) * c16(x))
            return acc
        if rule == 'F':
            acc = f32(0.0)
            for mw, x in zip(mws, vals): acc = f32(acc + f32(f24(mw) * f32(x)))
            return acc
        ts = [P13.ge_mul(mw, x) for mw, x in zip(mws, vals)]
        if rule != 'A2': return P13.ge_sum(ts)
        acc = P13.ge_sum(ts[:1])
        for t in ts[1:]: acc = P13.ge_sum([P13.ge_mul(1.0, acc), t])
        return acc
    if len(sets) == 1:
        _, v, ws = sets[0]
        pos = skin_pos(sr, pt['bones'], ws, v) if ws else list(v)
    elif rule in ('Am', 'H') and sets[0][2]:   # morph the weights and the position, then skin once
        mws = [mw for mw, _, _ in sets]
        v = [blend([s[1][c] for s in sets], mws) for c in range(3)]
        ws = [blend([s[2][i] for s in sets], mws) for i in range(len(sets[0][2]))]
        pos = skin_pos(sr, pt['bones'], ws, v)
    else:      # morph: blend the sets' (skinned) positions by the morph weights
        ps = [skin_pos(sr, pt['bones'], ws, v) if ws else list(v) for _, v, ws in sets]
        mws = [mw for mw, _, _ in sets]
        pos = [blend([p[c] for p in ps], mws) for c in range(3)]
    if rule == 'H':      # a point outside the clip volume's x and y planes is not drawn
        v4 = [pos[0], pos[1], pos[2], 1.0]
        cw = P13.dot4(M[3], v4)
        if not (abs(P13.dot4(M[0], v4)) <= cw and abs(P13.dot4(M[1], v4)) <= cw): return None
    d = P13.depth(M, pos[0], pos[1], pos[2], zs, 0.0)[0]
    return d if isinstance(d, int) else None

# =========================================================================== scenes
def P_read(axis, k):
    """Projection: clip x = x, clip y = y, clip z = 2^k (model axis), clip w = 1."""
    P = [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 1.0]]
    P[2][axis] = math.ldexp(1.0, k)
    return P

def binade_k(v):
    """k so that 2^k |v| is in [1/2, 1)."""
    m, e = math.frexp(abs(v)); return -e

class Scene:
    def __init__(s, num, name, title):
        s.num, s.name, s.title, s.pts, s.batch = num, name, title, [], {}
    def add(s, b, bones, sets, P, zs, fmt, V=None, W=None):
        s.pts.append(dict(batch=b, bones=bones, sets=sets, P=P, V=V or ID, W=W or ID, zs=zs, fmt=fmt))

def pick_read(est, axis):
    """Projection and zs that read est (a skinned coordinate) in [1/2, 1) of one sign."""
    k = binade_k(est); return P_read(axis, k), (65536.0 if est > 0 else -65536.0)

def norm_weights(r, n, kind='float'):
    raw = [0.2 + (r() % 1000) / 1000 for _ in range(n)]
    tot = sum(raw); ws = [x / tot for x in raw]
    if kind == 'float24': return [f32(w) for w in ws]
    if kind == 'float16': return [C.f32(P13.ge_cut(w)) for w in ws]
    return ws

def rand_bone_z(r, n_mix=True):
    """A bone whose z row maps a vertex in the probes' range into roughly [1/2, 1)."""
    rx = m16(r, r() & 1, -6) if n_mix else 0.0
    ry = m16(r, r() & 1, -6) if n_mix else 0.0
    s = m16(r, 0, -1, 0, 0x2000)                 # [0.5, 0.625)
    t = m16(r, 0, -2, 0, 0x1800)                 # [0.25, 0.30)
    return [rx, ry, s, t]

def vert(r):
    return (m24(r, r() & 1, -2), m24(r, r() & 1, -2), m24(r, 0, -1, 0.0, 0.8))   # x, y in +-[1/4,1/2), z in [1/2, 0.9)

def sc110():
    """Float weights: 1 to 8 bones, random z rows mixing x, y and z, weights near a sum of
    1 (float32 and 16-bit), the skinned z read whole; then x and y read through bones
    with random rotations; then scene 20's own bones and weights around its corners."""
    S = Scene(110, 'skinfloat', 'skinning: float weights, 1 to 8 bones, x, y and z read whole')
    r = XS(0x11000001)
    for b, n in enumerate((1, 2, 3, 4, 5, 8)):
        bones = [bone(rand_bone_z(r)) for _ in range(n)]
        for j in range(160):
            ws = norm_weights(r, n, 'float24' if j % 2 else 'float16') if n > 1 else [m24(r, 0, -1, 0.5, 1.0)]
            v = vert(r)
            est = skin_pos('F', bones, ws, v)[2]
            P, zs = pick_read(est, 2)
            S.add(b, bones, [(1.0, v, ws)], P, zs, 'float')
    for b, axis in ((6, 0), (7, 1)):
        bones = []
        for i in range(3):
            th = (r() % 3600) / 10.0 * math.pi / 180
            c, s_ = P13.ge_cut(math.cos(th)), P13.ge_cut(math.sin(th))
            bones.append(rot_z(c, s_, m16(r, r() & 1, -3), m16(r, r() & 1, -3), rand_bone_z(r)))
        for j in range(160):
            ws = norm_weights(r, 3, 'float24')
            v = vert(r)
            est = skin_pos('F', bones, ws, v)[axis]
            if abs(est) < 2**-6: continue
            P, zs = pick_read(est, axis)
            S.add(b, bones, [(1.0, v, ws)], P, zs, 'float')
    # scene 20: bones identity, translate (1.5, 0.8), rotate 90; weights 0.2, 0.3, 0.5
    B20 = [bone([0, 0, 1.0, 0]), bone([0, 0, 1.0, 0], (1.0, 0, 0, 1.5), (0, 1.0, 0, 0.8)),
           bone([0, 0, 1.0, 0], (0, -1.0, 0, 0), (1.0, 0, 0, 0))]
    W20 = [f32(0.2), f32(0.3), f32(0.5)]
    corners = [(1.5, 1.2, -5.0), (2.3, 1.2, -5.0), (1.9, 1.9, -5.0)]
    for j in range(120):
        cx, cy, cz = corners[j % 3]
        v = (f32(cx + ((j // 3) % 8 - 4) / 64), f32(cy + ((j // 24)) / 64), f32(cz))
        for axis in (0, 1, 2):
            est = skin_pos('F', B20, W20, v)[axis]
            if abs(est) < 2**-6: continue
            P, zs = pick_read(est, axis)
            S.add(8, B20, [(1.0, v, W20)], P, zs, 'float')
    return S

def sc111():
    """u8 (x/128) and u16 (x/32768) weights, 2 to 8 bones, including 0x80/0x8000 (one),
    0xFF/0xFFFF (just under two) and 0; skinned z read whole."""
    S = Scene(111, 'skinfmt', 'skinning: 8-bit and 16-bit weights, extremes included')
    r = XS(0x11100001)
    b = 0
    for fmt, unit, top in (('u8', 128, 255), ('u16', 32768, 65535)):
        for n in (2, 3, 4, 8):
            bones = [bone(rand_bone_z(r)) for _ in range(n)]
            for j in range(100):
                if j < 8:       # extremes on bone 0, the rest zero, or one and one
                    raws = [[unit] + [0] * (n - 1), [top] + [0] * (n - 1), [0] * (n - 1) + [unit],
                            [unit // 2] * 2 + [0] * (n - 2), [top, 0] + [0] * (n - 2), [1] + [unit - 1] + [0] * (n - 2),
                            [unit // n] * n, [top // n] * n][j]
                else:
                    tot = unit; raws = []
                    for i in range(n - 1):
                        x = r() % (tot // 2 + 1); raws.append(x); tot -= x
                    raws.append(tot)
                ws = [wval(fmt, x) for x in raws]
                if sum(ws) == 0: continue
                v = vert(r)
                est = skin_pos('F', bones, ws, v)[2]
                P, zs = pick_read(est, 2)
                S.add(b, bones, [(1.0, v, raws)], P, zs, fmt)
            b += 1
    return S

def sc112():
    """Float weights that do not sum to 1: up to 2, negative, tiny; bones of very
    different sizes (2^-8 to 2^8), so the weighted terms' exponents differ widely; each
    point read at its own binade."""
    S = Scene(112, 'skinodd', 'skinning: weights off one, negative and tiny; bones of every size')
    r = XS(0x11200001)
    for b in range(6):
        n = (2, 3, 4, 2, 3, 4)[b]
        bones = []
        for i in range(n):
            e = (-8, -4, 0, 4, 8)[(r() % 5)] if b >= 3 else 0
            zr = rand_bone_z(r)
            bones.append(bone([math.ldexp(x, e) for x in zr]))
        for j in range(150):
            ws = [m16(r, (r() & 3) == 0, -(r() % 12), 0, 0x8000) for _ in range(n)]
            v = vert(r)
            est = skin_pos('F', bones, ws, v)[2]
            if abs(est) < 2**-20 or abs(est) > 2**20: continue
            P, zs = pick_read(est, 2)
            S.add(b, bones, [(1.0, v, ws)], P, zs, 'float')
    return S

def sc113():
    """Skinning under non-trivial world and view z rows: whether the GE blends skinned
    model positions and then transforms (A), or folds each bone into the combined matrix
    and blends clip positions (C)."""
    S = Scene(113, 'skinworld', 'skinning: under world and view matrices, blend before or after')
    r = XS(0x11300001)
    for b in range(4):
        n = 3
        bones = [bone(rand_bone_z(r)) for _ in range(n)]
        sw, tw = m16(r, 0, 0, 0, 0x4000), m16(r, 1, -1, 0, 0x4000)    # world z: [1, 1.5) z - [1/2, 3/4)
        W = [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, sw, tw], [0, 0, 0, 1.0]]
        V = ID if b < 2 else [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, m16(r, 0, -1, 0, 0x8000), m16(r, 0, -3)], [0, 0, 0, 1.0]]
        for j in range(150):
            ws = norm_weights(r, n, 'float24')
            v = vert(r)
            s = skin_pos('F', bones, ws, v)
            ez = V[2][2] * (W[2][2] * s[2] + W[2][3]) + V[2][3]
            if abs(ez) < 2**-6: continue
            P, zs = pick_read(ez, 2)
            S.add(b, bones, [(1.0, v, ws)], P, zs, 'float', V=V, W=W)
    return S

def sc114():
    """Morphing: 2 to 4 vertex sets blended by the morph weights (float32 and 16-bit),
    the morphed z read whole; then 2 sets each with 2 float weights (morph and skin)."""
    S = Scene(114, 'skinmorph', 'morphing: 2 to 4 vertex sets, and morphing with skinning')
    r = XS(0x11400001)
    for b, n in enumerate((2, 3, 4)):
        for j in range(160):
            mws = norm_weights(r, n, 'float24' if j % 2 else 'float16')
            sets = [(mw, vert(r), None) for mw in mws]
            est = sum(mw * v[2] for mw, v, _ in sets)
            P, zs = pick_read(est, 2)
            S.add(b, [], sets, P, zs, 'float')
    bones = [bone(rand_bone_z(r)) for _ in range(2)]
    for j in range(160):
        mws = norm_weights(r, 2, 'float24')
        sets = [(mw, vert(r), norm_weights(r, 2, 'float24')) for mw in mws]
        est = sum(mw * skin_pos('F', bones, ws, v)[2] for mw, v, ws in sets)
        P, zs = pick_read(est, 2)
        S.add(3, bones, sets, P, zs, 'float')
    return S

SCENES = [sc110, sc111, sc112, sc113, sc114]
_built = {}
def scenes():
    if not _built:
        for fn in SCENES:
            S = fn()
            # within a batch, points that share a projection and z scale together: fewer MATS
            S.pts.sort(key=lambda pt: (pt['batch'], str(pt['P']), pt['zs']))
            _built[S.num] = S
    return [_built[k] for k in sorted(_built)]

# =========================================================================== layout, vertex bytes, ops
PITCH = 3
def slot_px(i):
    """Slot i's target pixel: 3 px apart, 158 across, from (2, 2)."""
    return 2 + PITCH * (i % 158), 2 + PITCH * (i // 158)
def colour_id(i):
    """Point i's colour: i + 1 seven bits a channel, each held as 2k + 1, so that a morph
    blend that loses or gains a little in a channel (weights summing to just under or
    over one, truncated or rounded) still reads back as the same id."""
    k = i + 1
    return 0xFF000000 | sum((((k >> (7 * c)) & 0x7F) * 2 + 1) << (8 * c) for c in range(3))
def id_of(rgb):
    return sum((((rgb >> (8 * c)) & 0xFF) >> 1) << (7 * c) for c in range(3))

def vbytes(fmt, nw, sets, cid):
    """One vertex record: per morph set, weights (u8 / u16 / float), colour, position."""
    b = b''
    for _, v, ws in sets:
        rec = b''
        if nw:
            if fmt == 'u8': rec += bytes(ws)
            elif fmt == 'u16': rec += struct.pack(f'<{nw}H', *ws)
            else: rec += struct.pack(f'<{nw}f', *ws)
            rec += b'\0' * (-len(rec) % 4)
        rec += struct.pack('<I', cid) + struct.pack('<fff', *v)
        b += rec
    return b

def vtype_of(fmt, nw, nsets):
    vt = GU_COLOR_8888 | GU_VERTEX_32BITF | GU_VERTICES(nsets)
    if nw:
        vt |= GU_WEIGHTS(nw) | {'u8': GU_WEIGHT_8BIT, 'u16': GU_WEIGHT_16BIT, 'float': GU_WEIGHT_32BITF}[fmt]
    return vt

def est_screen(pt):
    """Where the point lands on screen at the standard offset (float estimate)."""
    sets = pt['sets']
    if len(sets) == 1 and sets[0][2] is not None and pt['bones']:
        ws = [wval(pt['fmt'], x) for x in sets[0][2]] if pt['fmt'] != 'float' else sets[0][2]
        p = skin_pos('F', pt['bones'], ws, sets[0][1])
    elif len(sets) == 1:
        p = list(sets[0][1])
    else:
        p = [0.0, 0.0, 0.0]
        for mw, v, ws in sets:
            if ws is not None:
                q = skin_pos('F', pt['bones'], ws, v)
            else:
                q = v
            for c in range(3): p[c] += mw * q[c]
    return 240 + 240 * p[0], 136 - 136 * p[1]

def scene_ops(S):
    ops = [('BEGIN', 3, 0xFF000000), ('DEPTH', 1, GU_ALWAYS)]
    cur = {}
    def state(key, val, op):
        if cur.get(key) != val: ops.append(op); cur[key] = val
    for i, pt in enumerate(S.pts):
        tx, ty = slot_px(i)
        sx, sy = est_screen(pt)
        dx, dy = tx - math.floor(sx), ty - math.floor(sy)
        state('mats', (str(pt['P']), str(pt['V']), str(pt['W'])), ('MATS', pt['P'], pt['V'], pt['W']))
        state('zs', pt['zs'], ('ZVIEW', pt['zs'], 0.0))
        for k, B in enumerate(pt['bones']):
            state(f'bone{k}', str(B), ('BONE', k, B))
        mws = [mw for mw, _, _ in pt['sets']]
        if len(mws) > 1: state('morph', tuple(mws), ('MORPH', tuple(mws) + (0.0,) * (8 - len(mws))))
        state('off', (dx, dy), ('OFFSET', 2048 - 240 - dx, 2048 - 136 - dy))
        nw = len(pt['sets'][0][2]) if pt['sets'][0][2] is not None else 0
        ops.append(('RAW', GU_POINTS, vtype_of(pt['fmt'], nw, len(pt['sets'])), 1,
                    vbytes(pt['fmt'], nw, pt['sets'], colour_id(i))))
    ops += [('OFFSET', 2048 - 240, 2048 - 136), ('END', 1)]
    return ops

def emit_scene(S):
    OP = C.OP
    w = []; vcrc = 0; ndraw = 0; nvert = 0
    def op(name, args):
        w.append(OP[name] << 24 | len(args)); w.extend(a & 0xFFFFFFFF for a in args)
    body = []
    for o in scene_ops(S):
        k = o[0]
        if k == 'BEGIN': body.append(('BEGIN', [o[1], o[2]]))
        elif k == 'END': body.append(('END', [o[1]]))
        elif k == 'DEPTH': body.append(('DEPTH', [o[1], o[2]]))
        elif k == 'MATS': body.append(('MATS', [7] + C.mem16(o[1]) + C.mem16(o[2]) + C.mem16(o[3])))
        elif k == 'ZVIEW': body.append(('ZVIEW', [bf(o[1]), bf(o[2])]))
        elif k == 'BONE': body.append(('BONE', [o[1]] + mem16(o[2])))
        elif k == 'MORPH': body.append(('MORPH', [bf(x) for x in o[1]]))
        elif k == 'OFFSET': body.append(('OFFSET', [o[1], o[2]]))
        elif k == 'RAW':
            prim, vtype, cnt, vb = o[1:5]
            vcrc = zlib.crc32(vb, vcrc); ndraw += 1; nvert += cnt
            body.append(('DRAW', [prim, vtype, cnt, len(vb), 0] + C.words(vb)))
        else: raise ValueError(k)
    op(*body[0])
    op('SUMS', [ndraw, nvert, vcrc & 0xFFFFFFFF])
    for b in body[1:]: op(*b)
    return w, ndraw, nvert, vcrc & 0xFFFFFFFF

def emit(path):
    out = ['/* Generated by skin16.py emit: geprobe 16 scenes 110-114 (skinning and morphing) as',
           ' * command streams for c14_run() (colour14.py\'s format).  Do not edit; change skin16.py',
           ' * and run it again. */']
    steps = []
    for k, S in enumerate(scenes()):
        w, nd, nv, vc = emit_scene(S)
        crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
        steps.append((S, len(w), nd, nv, vc, crc))
        out.append(f'\n/* scene {S.num} {S.name}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
                   f' {len(w)} words, stream crc {crc:08X} */')
        out.append(f'static const w32 C16_S{k}[{len(w)}] = {{')
        for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
        out.append('};')
    out.append('\nstatic const struct c14_step C16_STEPS[] = {')
    for k, (S, n, *_r) in enumerate(steps):
        out.append(f'    {{ {S.num}, "{S.name}", "{S.title}", C16_S{k}, {n} }},')
    out.append('};\n#define C16_NSTEPS ((int)(sizeof C16_STEPS / sizeof C16_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text:
        open(path, 'w').write(text)
    return steps

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for S, n, nd, nv, vc, crc in emit(os.path.join(td, 'c16.inc')):
            m = re.search(rf'scene {S.num}: (?:skinning|morphing).*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want
            ok &= good
            print(f'scene {S.num} {S.name}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

# =========================================================================== reading a dump
def find_points(dumpdir, S):
    """{point index: (x, y, depth)} found by its id colour (id_of) within 2 px of its slot."""
    sys.path.insert(0, HERE)
    import readout
    col = np.fromfile(os.path.join(dumpdir, f'ge_{S.num}_{S.name}.raw'), '<u4').reshape(272, 480) & 0xFFFFFF
    dep = readout.depth_full(os.path.join(dumpdir, f'ge_{S.num}_{S.name}_depthfull.bin'))
    ids = np.zeros_like(col)
    for c in range(3): ids |= ((col >> (8 * c)) & 0xFF) >> 1 << (7 * c)
    ids[col == 0] = 0
    out = {}
    for i in range(len(S.pts)):
        tx, ty = slot_px(i); want = i + 1
        y0, y1, x0, x1 = max(ty - 2, 0), min(ty + 3, 272), max(tx - 2, 0), min(tx + 3, 480)
        hit = np.argwhere(ids[y0:y1, x0:x1] == want)
        if len(hit): y, x = hit[0][0] + y0, hit[0][1] + x0; out[i] = (int(x), int(y), int(dep[y, x]))
    return out

def point_ws(pt, ws, signed=False):
    return [wval(pt['fmt'], x, signed) for x in ws] if pt['fmt'] != 'float' else ws

def predicted(pt, rule):
    sets = [(mw, v, point_ws(pt, ws, rule == 'As') if ws is not None else None) for mw, v, ws in pt['sets']]
    q = dict(pt, sets=sets)
    if rule == 'C' and (len(sets) > 1 or not pt['bones']): rule = 'A'
    return predict_depth(rule, q)

def compare(dumpdir, rules=None):
    rules = rules or RULES
    for S in scenes():
        found = find_points(dumpdir, S)
        by = {}
        for i, pt in enumerate(S.pts):
            b = pt['batch']; st = by.setdefault(b, {'n': 0, 'found': 0, **{r: 0 for r in rules}})
            st['n'] += 1
            if i not in found:
                if 'H' in rules: st['H'] += predicted(pt, 'H') is None
                continue
            st['found'] += 1
            for r in rules: st[r] += predicted(pt, r) == found[i][2]
        print(f'{S.num} {S.name}:')
        for b, st in sorted(by.items()):
            print(f'   b{b}: {st["found"]}/{st["n"]} found; depth matches ' + ' '.join(f'{r}:{st[r]}' for r in rules)
                  + ('  (H counts a point it predicts absent and is)' if 'H' in rules and st['found'] < st['n'] else ''))

def check():
    for S in scenes():
        by = {}
        for i, pt in enumerate(S.pts):
            st = by.setdefault(pt['batch'], {'n': 0, 'none': 0, 'lt32768': 0, **{f'{r}!=A': 0 for r in RULES if r != 'A'}})
            st['n'] += 1
            dA = predicted(pt, 'A')
            if dA is None: st['none'] += 1; continue
            if dA < 32768: st['lt32768'] += 1
            for r in RULES:
                if r != 'A': st[f'{r}!=A'] += predicted(pt, r) != dA
        tx, ty = slot_px(len(S.pts) - 1)
        print(f'{S.num} {S.name}: {len(S.pts)} points, last slot ({tx},{ty})')
        for b, st in sorted(by.items()): print('   b%d' % b, st)

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        path = argv[2] if len(argv) > 2 else os.path.join(HERE, 'c16_data.inc')
        for S, n, nd, nv, vc, crc in emit(path):
            print(f'scene {S.num} {S.name}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'compare': compare(argv[2], argv[3:] or None); return 0
    if cmd == 'check': check(); return 0
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
