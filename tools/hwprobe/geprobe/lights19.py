#!/usr/bin/env python3
"""geprobe 19 (scenes 121-124): lighting under real world and view matrices.

Sets 18 and 19 settled lighting's own arithmetic (lights17.py rule 'ge', ge.c) with
the world and view matrices identity. What reaches it under real ones is open: the
eye-space vertex, normal, light position and spot direction. psprecomp forms them in
float32 (world = W v, eye = V world; the normal through W's and V's 3x3; the light by V)
and cuts them to 16 bits on the way in. These scenes tell that from the rivals:
    cur     psprecomp's float32 transforms, then the 'ge' lighting
    seq     V (W v) in the GE's arithmetic, each row one ge_sum (patch13 dot4), each
            result cut to 16 bits; the normal V3 (W3 n); the light V p; D V3 D
    comb    (V W) v, the matrices multiplied first as the clip path does (P V) W
    world   lighting in world space: W v, W3 n, the light and D as given
Large translations put the vertex and the light far from the eye but near each other,
so L = light - vertex keeps few bits and the rivals part by many codes; a world matrix
whose 3x3 has large entries that cancel does the same for positions and normals.

  121 eyesearch  L.D read whole through the spot cutoff search, as scene 115, with W and
                 V loaded per group of points (main.c l19_search). Batches: world (W
                 rotated, scaled, translated 2^0..2^10), view (V likewise), both,
                 dir (V a rotation, small coordinates: D's transform), cancel.
  122 eyenorm    plain diffuse bytes, directional light, normals through cancelling and
                 non-uniform world matrices and rotated views
  123 eyespec    specular bytes under rotated views: is H's (0,0,1) the eye's?
  124 eyeatt     attenuation bytes with the vertex and light far out

Every point goes to clip (0, 0, 0, 1) through a zero projection as in 115-119 (the
combined P V W keeps w's row), so the offset alone places it.

    lights19.py emit [OUT.inc]        write l19_data.inc
    lights19.py sums DUMPDIR          the logged CRCs against these inputs
    lights19.py compare DUMPDIR [rule..]   121's codes and 122-124's bytes per rule
    lights19.py check                 design checks: points, how often the rules part
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
import patch13 as P13
import lights17 as L17
from colour14 import XS, f32, bf
from lights17 import f24, code, Light, GU_SPOTLIGHT, GU_POINTLIGHT, GU_DIRECTIONAL, GU_DIFFUSE, \
    GU_DIFFUSE_AND_SPECULAR, GU_POINTS, VTYPE, CLEAR, slot, P0

RULES = ['cur', 'seq', 'comb', 'world']

# =========================================================================== matrices
def rot(r):
    """A random rotation (row-major 3x3) from a random unit quaternion."""
    while True:
        q = [(r() / 2**31 - 1.0) for _ in range(4)]
        n = math.sqrt(sum(c * c for c in q))
        if 0.1 < n <= 1: break
    w, x, y, z = (c / n for c in q)
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
            [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
            [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]
def mm3(a, b): return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
def affine(M3, t):
    """A 4x4 (row-major, bottom row 0 0 0 1) from a 3x3 and a translation, entries f24."""
    return [[f24(M3[i][0]), f24(M3[i][1]), f24(M3[i][2]), f24(t[i])] for i in range(3)] + [[0.0, 0.0, 0.0, 1.0]]
ID4 = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
def inv3(M):
    a = np.array([[M[i][j] for j in range(3)] for i in range(3)]); return np.linalg.inv(a).tolist()
def apply(M, v, w=1.0): return [sum(M[i][j] * v[j] for j in range(3)) + M[i][3] * w for i in range(3)]
def rdir(r):
    while True:
        v = [(r() / 2**31 - 1.0) for _ in range(3)]; l = math.sqrt(sum(c * c for c in v))
        if 0.1 < l <= 1: return [c / l for c in v]
def near(r, u, deg):
    c = math.cos(math.radians(deg))
    while True:
        d = rdir(r)
        if sum(a * b for a, b in zip(d, u)) >= c: return d
def tvec(r, k): return [c * 2.0 ** k for c in rdir(r)]

# =========================================================================== the eye-space inputs, by rule
def m4x3_f32(M, v, w=True):
    """psprecomp's mul_4x3 / mul_3x3 in float32: ((m0 x + m1 y) + m2 z) + t."""
    out = []
    for i in range(3):
        a = f32(f32(f32(f32(M[i][0] * v[0]) + f32(M[i][1] * v[1])) + f32(M[i][2] * v[2])))
        out.append(f32(a + M[i][3]) if w else a)
    return out
def m4x3_ge(M, v, w=True):
    return [P13.dot4(M[i], [v[0], v[1], v[2], 1.0 if w else 0.0]) for i in range(3)]

def eye_inputs(rule, W, V, v, n, Lt):
    """(vertex, normal, light position, spot direction) in the space lighting runs in."""
    if rule == 'cur':
        e = m4x3_f32(V, m4x3_f32(W, v)); ne = m4x3_f32(V, m4x3_f32(W, n, False), False)
        lp = m4x3_f32(V, Lt.p, Lt.type != GU_DIRECTIONAL); ld = m4x3_f32(V, Lt.d, False)
    elif rule == 'seq':
        e = m4x3_ge(V, m4x3_ge(W, v)); ne = m4x3_ge(V, m4x3_ge(W, n, False), False)
        lp = m4x3_ge(V, Lt.p, Lt.type != GU_DIRECTIONAL); ld = m4x3_ge(V, Lt.d, False)
    elif rule == 'comb':
        VW = P13.mat_mul(V, W)
        e = m4x3_ge(VW, v); ne = m4x3_ge(VW, n, False)
        lp = m4x3_ge(V, Lt.p, Lt.type != GU_DIRECTIONAL); ld = m4x3_ge(V, Lt.d, False)
    elif rule == 'world':
        e = m4x3_ge(W, v); ne = m4x3_ge(W, n, False); lp = list(Lt.p); ld = list(Lt.d)
    else: raise ValueError(rule)
    return e, ne, lp, ld

class EyeLight:
    """A Light with its position and direction replaced by eye-space ones."""
    def __init__(s, Lt, p, d):
        for k in Light.__slots__: setattr(s, k, getattr(Lt, k))
        s.p, s.d = tuple(p), tuple(d)

def x_of(rule, W, V, g):
    """Scene 121: L.D under a rule (the lights17 'ge' arithmetic on the eye inputs)."""
    Lt = Light(GU_SPOTLIGHT, GU_DIFFUSE, g['p'], g['d'])
    e, ne, lp, ld = eye_inputs(rule, W, V, g['v'], g['n'], Lt)
    return L17.ge_parts(EyeLight(Lt, lp, ld), e, ne)['sx']
def code_of(rule, W, V, g):
    x = x_of(rule, W, V, g); return code(x) if x and x > 0 else 0
def byte_of(rule, W, V, pt):
    Lt = pt['L']
    e, ne, lp, ld = eye_inputs(rule, W, V, pt['v'], pt['n'], Lt)
    return L17.ge_byte(EyeLight(Lt, lp, ld), e, ne)

# =========================================================================== scene 121's geometries
BATCHES = ['world', 'view', 'both', 'dir', 'cancel']
PER = 8          # points per matrix pair
def V3(v): return tuple(f24(c) for c in v)

def group(r, batch, W, V, M3world, centre_world, spread=1.0):
    """PER geometries under W and V: a vertex near `centre` (model space), a light 0.5 to 4
    from it in world space, D within 55 degrees of the vertex-to-light direction, the
    normal along that direction in model space."""
    out = []
    Winv = inv3(M3world)
    for _ in range(PER):
        v = V3([centre_world[i] + (r() / 2**31 - 1.0) * spread for i in range(3)])
        w = apply(W, v)
        u = rdir(r); dist = 0.5 * 8 ** ((r() % 1000) / 1000)
        p = V3([w[i] + u[i] * dist for i in range(3)])
        uu = [p[i] - w[i] for i in range(3)]; l = math.sqrt(sum(c * c for c in uu)); uu = [c / l for c in uu]
        d = V3(near(r, uu, 55.0))
        n = V3([sum(Winv[i][j] * uu[j] for j in range(3)) for i in range(3)])
        out.append(dict(batch=batch, v=v, n=n, p=p, d=d))
    return out

def search_set():
    r = XS(0x12100001)
    mats, G = [], []
    def add(batch, W, V, M3, centre, spread=1.0, groups=1):
        for _ in range(groups):
            mats.append((W, V))
            for g in group(r, batch, W, V, M3, centre, spread):
                g['m'] = len(mats) - 1; G.append(g)
    for k in range(75):                                       # b0 world: V identity
        s = 2 ** ((r() % 2000) / 1000 - 1); R = rot(r); M3 = [[c * s for c in row] for row in R]
        add(0, affine(M3, tvec(r, (0, 2, 4, 6, 8, 10)[k % 6])), ID4, M3, [0, 0, 0])
    for k in range(75):                                       # b1 view: W identity
        V = affine(rot(r), tvec(r, (0, 2, 4, 6, 8, 10)[k % 6]))
        add(1, ID4, V, [[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0, 0, 0])
    for k in range(75):                                       # b2 both
        s = 2 ** ((r() % 2000) / 1000 - 1); R = rot(r); M3 = [[c * s for c in row] for row in R]
        W = affine(M3, tvec(r, (0, 3, 6, 9)[k % 4])); V = affine(rot(r), tvec(r, (0, 3, 6, 9)[(k // 4) % 4]))
        add(2, W, V, M3, [0, 0, 0])
    for k in range(38):                                       # b3 dir: V a rotation, small coordinates
        add(3, ID4, affine(rot(r), [0, 0, 0]), [[1, 0, 0], [0, 1, 0], [0, 0, 1]], [0, 0, 0])
    for k in range(50):                                       # b4 cancel: W3 = R1 diag(2^m, 2^-m, 1) R2
        m = (2, 3, 4, 5, 6)[k % 5]; R1, R2 = rot(r), rot(r)
        M3 = mm3(mm3(R1, [[2.0 ** m, 0, 0], [0, 2.0 ** -m, 0], [0, 0, 1]]), R2)
        ax = [R2[1][j] for j in range(3)]                     # model direction W shrinks most
        add(4, affine(M3, tvec(r, 2)), ID4, M3, [c * 2.0 ** (m / 2) for c in ax], spread=0.25)
    return mats, G

# =========================================================================== scenes 122-124
class Scene:
    def __init__(s, num, name, title): s.num, s.name, s.title, s.pts, s.mats = num, name, title, [], []
    def mat(s, W, V): s.mats.append((W, V)); return len(s.mats) - 1
    def add(s, batch, m, Lt, v, n): s.pts.append(dict(batch=batch, m=m, L=Lt, v=V3(v), n=V3(n)))

def cancelling(r, m):
    """R1 diag(2^m, 2^-m, 1) R2 and its parts: entries near 2^m that cancel on the axis
    it shrinks."""
    R1, R2 = rot(r), rot(r)
    return mm3(mm3(R1, [[2.0 ** m, 0, 0], [0, 2.0 ** -m, 0], [0, 0, 1]]), R2), R1, R2

def sc122():
    S = Scene(122, 'eyenorm', 'lit under real matrices: diffuse, normals through cancelling matrices')
    r = XS(0x12200001)
    for k in range(250):
        b = k % 3; m = (2, 3, 4, 5, 6)[(k // 3) % 5]
        A, R1, R2 = cancelling(r, m)
        I3 = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
        if b == 0:   W, V, Wm, Vm = affine(A, [0, 0, 0]), ID4, A, I3            # cancelling world
        elif b == 1:                                                              # and a rotated view
            Vr = rot(r); W, V, Wm, Vm = affine(A, [0, 0, 0]), affine(Vr, [0, 0, 0]), A, Vr
        else:        W, V, Wm, Vm = ID4, affine(A, [0, 0, 0]), I3, A            # a cancelling view
        mi = S.mat(W, V)
        R2t = [[R2[j][i] for j in range(3)] for i in range(3)]
        for _ in range(8):
            # in A's own frame the normal is (2^-2m a, 1, 2^-m c): every product near 2^m, the
            # result near 2^-m (r1, 1, r3) -- a direction anywhere, from heavy cancellation
            a_, c_ = (r() / 2**31 - 1.0), (r() / 2**31 - 1.0)
            loc = [2.0 ** (-2 * m) * a_, 1.0, 2.0 ** -m * c_]
            nA = [sum(R2t[i][j] * loc[j] for j in range(3)) for i in range(3)]   # model-space normal
            if b == 2:      # the view cancels: the normal reaches it through W = I
                n = nA
                ne = [2.0 ** -m * x for x in apply(affine(R1, [0, 0, 0]), [a_, 1.0, c_])]
                de = near(r, unit(ne), 60.0); Vinv = inv3(Vm)                   # wanted in eye space
                ld_world = unit([sum(Vinv[i][j] * de[j] for j in range(3)) for i in range(3)])
            else:
                n = nA
                nw = apply(affine(R1, [0, 0, 0]), [a_, 1.0, c_])                 # world direction (scaled)
                ld_world = near(r, unit(nw), 60.0)
            S.add(b, mi, Light(GU_DIRECTIONAL, GU_DIFFUSE, V3(ld_world)), [0, 0, 0], n)
    return S

def unit(v):
    l = math.sqrt(sum(c * c for c in v)); return [c / l for c in v]

def sc123():
    S = Scene(123, 'eyespec', 'lit under real matrices: specular under rotated views')
    r = XS(0x12300001)
    for k in range(200):
        V = affine(rot(r), tvec(r, (0, 3, 6, 8, 10)[k % 5]) if k % 2 else [0, 0, 0])
        W = ID4 if k % 4 < 2 else affine([[c * 2 ** ((r() % 1000) / 1000 - 0.5) for c in row] for row in rot(r)], [0, 0, 0])
        mi = S.mat(W, V)
        Vr = [row[:3] for row in V[:3]]; Vt = [[Vr[j][i] for j in range(3)] for i in range(3)]   # V^-1 rotation
        W3 = [row[:3] for row in W[:3]]; Winv = inv3(W3)
        for _ in range(8):
            v = [(r() / 2**31 - 1.0) for _ in range(3)]
            w = apply(W, v)
            u = rdir(r); dist = 0.5 * 8 ** ((r() % 1000) / 1000)
            p = [w[i] + u[i] * dist for i in range(3)]
            ue = [sum(Vr[i][j] * u[j] for j in range(3)) for i in range(3)]          # L in eye space
            if ue[2] < -0.3: ue = [-c for c in ue]; p = [w[i] - u[i] * dist for i in range(3)]
            He = [ue[0], ue[1], ue[2] + 1]; hl = math.sqrt(sum(c * c for c in He)); He = [c / hl for c in He]
            ne = near(r, He, 25.0)                                                    # N near H, eye space
            nw = [sum(Vt[i][j] * ne[j] for j in range(3)) for i in range(3)]
            n = [sum(Winv[i][j] * nw[j] for j in range(3)) for i in range(3)]
            coef = (1.0, 4.0, 8.0, 16.0)[r() % 4]
            S.add(k % 2, mi, Light(GU_POINTLIGHT, GU_DIFFUSE_AND_SPECULAR, V3(p), coef=coef, dif=0, spec=0xFFFFFF), v, n)
    return S

def sc124():
    S = Scene(124, 'eyeatt', 'lit under real matrices: attenuation with the vertex and light far out')
    r = XS(0x12400001)
    for k in range(150):
        kk = (0, 3, 6, 9)[k % 4]
        W = affine([[c * 2 ** ((r() % 2000) / 1000 - 1) for c in row] for row in rot(r)], tvec(r, kk))
        V = affine(rot(r), tvec(r, (0, 3, 6, 9)[(k // 4) % 4])) if k % 2 else ID4
        mi = S.mat(W, V)
        W3 = [row[:3] for row in W[:3]]; Winv = inv3(W3)
        for _ in range(8):
            v = [(r() / 2**31 - 1.0) for _ in range(3)]
            w = apply(W, v)
            u = rdir(r); dist = 2 ** ((r() % 5000) / 1000 - 2)
            p = [w[i] + u[i] * dist for i in range(3)]
            n = [sum(Winv[i][j] * u[j] for j in range(3)) for i in range(3)]
            target = 2 ** -((r() % 6000) / 1000)
            kk3 = [(1 / target, 0, 0), (0, 1 / (target * dist), 0), (0, 0, 1 / (target * dist * dist))][r() % 3]
            S.add(k % 2, mi, Light(GU_POINTLIGHT, GU_DIFFUSE, V3(p), k=kk3), v, n)
    return S

_SRCH, _S = [], {}
def search():
    if not _SRCH: _SRCH.extend(search_set())
    return _SRCH
def scenes():
    if not _S:
        for fn in (sc122, sc123, sc124):
            S = fn(); _S[S.num] = S
    return [_S[k] for k in sorted(_S)]

# =========================================================================== emission
def mem16(M): return [bf(M[r][c]) for c in range(4) for r in range(4)]

def scene_ops(S):
    ops = [('BEGIN', 3, CLEAR), ('LMODE', 1)]
    lastm, lastL = None, None
    for i, pt in enumerate(S.pts):
        if pt['m'] != lastm:
            W, V = S.mats[pt['m']]; ops.append(('MATS', P0, V, W)); lastm = pt['m']
        x, y = slot(i)
        ops.append(('OFFSET', 2048 - x, 2048 - y))
        if pt['L'].key() != lastL: ops.append(('LGT', pt['L'].words())); lastL = pt['L'].key()
        ops.append(('RAW', GU_POINTS, VTYPE, 1, L17.vbytes(pt['v'], pt['n'])))
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

def search_words():
    mats, G = search()
    mw = [x for W, V in mats for x in mem16(W) + mem16(V)]
    gw = [x for g in G for x in [g['m']] + [bf(c) for c in g['v'] + g['n'] + g['p'] + g['d']]]
    return mw, gw
def search_crc():
    mw, gw = search_words()
    return zlib.crc32(struct.pack(f'<{len(gw)}I', *gw), zlib.crc32(struct.pack(f'<{len(mw)}I', *mw))) & 0xFFFFFFFF

def emit(path):
    mats, G = search(); mw, gw = search_words()
    out = ['/* Generated by lights19.py emit: geprobe 19 scenes 121-124 (lighting under real',
           ' * matrices). L19_MAT: per matrix pair, world then view as sceGuSetMatrix takes them',
           ' * (16 float bits each, column-major); L19_GEO: per geometry, its pair, then vertex,',
           ' * normal, light position and spot direction as float bits. C19_S*: 122-124\'s',
           ' * streams for c14_run(). Do not edit; change lights19.py and run it again. */',
           f'\n#define L19_NMAT {len(mats)}', f'#define L19_NGEO {len(G)}', f'/* input crc {search_crc():08X} */',
           f'static const w32 L19_MAT[{len(mats)}][32] = {{']
    for i in range(0, len(mw), 32): out.append('    { ' + ', '.join(f'0x{x:08X}u' for x in mw[i:i + 32]) + ' },')
    out.append('};')
    out.append(f'static const w32 L19_GEO[{len(G)}][13] = {{')
    for i in range(0, len(gw), 13): out.append('    { ' + ', '.join(f'0x{x:08X}u' for x in gw[i:i + 13]) + ' },')
    out.append('};')
    steps = []
    for k, S in enumerate(scenes()):
        w, nd, nv, vc = emit_scene(S)
        crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
        steps.append((S, len(w), nd, nv, vc, crc))
        out.append(f'\n/* scene {S.num} {S.name}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
                   f' {len(w)} words, stream crc {crc:08X} */')
        out.append(f'static const w32 C19_S{k}[{len(w)}] = {{')
        for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
        out.append('};')
    out.append('\nstatic const struct c14_step C19_STEPS[] = {')
    for k, (S, n, *_r) in enumerate(steps):
        out.append(f'    {{ {S.num}, "{S.name}", "{S.title}", C19_S{k}, {n} }},')
    out.append('};\n#define C19_NSTEPS ((int)(sizeof C19_STEPS / sizeof C19_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text: open(path, 'w').write(text)
    return steps

# =========================================================================== reading a dump
def read_search(dumpdir):
    raw = np.fromfile(os.path.join(dumpdir, 'ge_121_eyesearch.bin'), '<u4').reshape(-1, 2)
    return [(int(a) & 0xFFFFFF, int(a) >> 24, int(b)) for a, b in raw]

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    m = re.search(r'scene 121: .*?\n\s+(\d+) geometries, (\d+) matrix pairs, input crc ([0-9A-F]+)', log)
    mats, G = search()
    want = (str(len(G)), str(len(mats)), f'{search_crc():08X}')
    good = bool(m) and m.groups() == want; ok &= good
    print('scene 121 eyesearch: ' + ('log MATCHES' if good else f'log DIFFERS: {m.groups() if m else "no line"} against {want}'))
    with tempfile.TemporaryDirectory() as td:
        for S, n, nd, nv, vc, crc in emit(os.path.join(td, 'l19.inc')):
            m = re.search(rf'scene {S.num}: .*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want; ok &= good
            print(f'scene {S.num} {S.name}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

def compare(dumpdir, rules=None):
    rules = rules or RULES
    mats, G = search(); R = read_search(dumpdir)
    print('121 eyesearch:')
    for b, name in enumerate(BATCHES):
        idx = [i for i, g in enumerate(G) if g['batch'] == b]
        fl = sum(1 for i in idx if R[i][1])
        line = f'   b{b} {name}: {len(idx)} geometries, {fl} flagged; code matches'
        for rule in rules:
            line += f' {rule}:{sum(R[i][0] == code_of(rule, *mats[G[i]["m"]], G[i]) for i in idx)}'
        print(line)
    for S in scenes():
        got = L17.read_scene(dumpdir, S)
        by = {}
        for i, pt in enumerate(S.pts):
            st = by.setdefault(pt['batch'], {'n': 0, 'absent': 0, **{r: 0 for r in rules}})
            st['n'] += 1
            if got[i] < 0: st['absent'] += 1; continue
            for rule in rules: st[rule] += byte_of(rule, *S.mats[pt['m']], pt) == got[i]
        print(f'{S.num} {S.name}:')
        for b, st in sorted(by.items()): print(f'   b{b}:', st)

def check():
    mats, G = search()
    print(f'121: {len(G)} geometries, {len(mats)} matrix pairs; last slot {slot(len(G) - 1)}')
    for b, name in enumerate(BATCHES):
        idx = [i for i, g in enumerate(G) if g['batch'] == b]
        xs = {r: [x_of(r, *mats[G[i]['m']], G[i]) for i in idx] for r in RULES}
        lo = min(min(v for v in xs[r] if v is not None) for r in RULES)
        part = {f'{r}!=cur': sum(code(a or 0) != code(b_ or 0) for a, b_ in zip(xs[r], xs['cur'])) for r in RULES[1:]}
        part['seq!=comb'] = sum(code(a or 0) != code(b_ or 0) for a, b_ in zip(xs['seq'], xs['comb']))
        print(f'   b{b} {name}: {len(idx)}, smallest L.D {lo:.3f}', part)
    for S in scenes():
        by = {}
        for pt in S.pts:
            st = by.setdefault(pt['batch'], {'n': 0, 'cur0': 0, **{f'{r}!=cur': 0 for r in RULES[1:]}, 'seq!=comb': 0})
            st['n'] += 1
            bs = {r: byte_of(r, *S.mats[pt['m']], pt) for r in RULES}
            st['cur0'] += bs['cur'] == 0
            for r in RULES[1:]: st[f'{r}!=cur'] += bs[r] != bs['cur']
            st['seq!=comb'] += bs['seq'] != bs['comb']
        print(f'{S.num} {S.name}: {len(S.pts)} points, {len(S.mats)} matrix pairs, last slot {slot(len(S.pts) - 1)}')
        for b, st in sorted(by.items()): print(f'   b{b}:', st)

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        steps = emit(argv[2] if len(argv) > 2 else os.path.join(HERE, 'l19_data.inc'))
        mats, G = search()
        print(f'scene 121 eyesearch: {len(G)} geometries, {len(mats)} matrix pairs, input crc {search_crc():08X}')
        for S, n, nd, nv, vc, crc in steps:
            print(f'scene {S.num} {S.name}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'compare': compare(argv[2], argv[3:] or None); return 0
    if cmd == 'check': check(); return 0
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
