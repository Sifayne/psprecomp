#!/usr/bin/env python3
"""geprobe 14 (scenes 83-98): triangle colour planes.  Generator, mirror and checker.

One file, in the pattern of patch13.py:
  * the scene builders make every primitive of every scene (exact 1/16-px corners,
    colours chosen by a seeded xorshift32 search, scissor windows, state);
  * `emit` turns them into one u32 command stream per scene (c14_data.inc) that the
    probe's c14_run() replays, logging the stream CRC before it draws;
  * the mirror renders the same primitives the way psprecomp's sw_tri does
    (src/render.c: coverage, area_rcp, floored gradients, 1/16384 accumulators,
    anchors, fog / secondary / depth planes, flat shading), under the current rule
    ('cur' = leftmost corner among those inside the scissor) or any rival
    ('z' = H1, the depth-plane corner; and the alternatives in RULES), under H1's
    arithmetic or a rival's (ARITH, after '+': 'z+trunc'); a rule may end in ':nofan' or
    ':fan' (through-mode fans dropped as psprecomp's sw_draw did until geprobe 19's runs,
    or drawn; the default drops them only under 'cur', psprecomp as it was);
  * `verify` checks the port against psprecomp's own frames (miner's triangle log +
    run), `check` prints the design checks, `predict DIR` writes predicted frames,
    `compare`, `decode` and `selfcheck` read a hardware dump.

    colour14.py sums DUMPDIR                every scene's SUMS line in geprobe.txt against these streams
    colour14.py verify [all | NAME.raw..]   port == psprecomp (default scenes 39, 46, 47); needs
                                            COLOUR14_TRILOG_DIR (an instrumented psprecomp's run/tri.txt)
    colour14.py check [scene..]             design checks (on screen, overlap, info, separation)
    colour14.py emit [OUT.inc]              write the command streams
    colour14.py predict OUTDIR [rule]       predicted frames (and 97's _depthfull.bin) under a rule
    colour14.py cmp DIR_A DIR_B             two dump folders frame by frame (32 bits, depth)
    colour14.py compare DUMPDIR [scene..] [rule..]   per scene / per rule pixel agreement of a dump
                                            (default: the anchor rules, then H1 under each arithmetic rival)
    colour14.py decode DUMPDIR [scene..] [-v] [--alpha=256-a|255-a|2a] [--rule=RULE]
                                            per triangle and channel: which anchor fits; per scene which
                                            arithmetic (start, gradient, clamp, secondary sum) the dump follows
    colour14.py selfcheck DUMPDIR           the model-free hardware-vs-hardware checks
    colour14.py selftest TMPDIR             decode on frames with a known answer

    A rule is RULE[+ARITH][:fan|:nofan], e.g. z, cur, z:nofan, zins, z+trunc, bias1 (= z+bias1).
"""
import sys, os, re, math, struct, zlib, json
import numpy as np

GE_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, GE_DIR)
import patch13 as P13            # ge_mul/ge_sum/dot4/ge_over_w/wvp: settled 3D arithmetic (geprobe 12)

W_, H_ = 480, 272
HERE = os.path.dirname(os.path.abspath(__file__))

# =========================================================================== GE constants (pspgu.h)
GU_POINTS, GU_TRIANGLES, GU_TRIANGLE_STRIP, GU_TRIANGLE_FAN = 0, 3, 4, 5
GU_TEXTURE_32BITF, GU_COLOR_8888, GU_NORMAL_32BITF, GU_VERTEX_32BITF = 3, 7 << 2, 3 << 5, 3 << 7
GU_INDEX_16BIT, GU_TRANSFORM_2D = 2 << 11, 1 << 23
FMT_CV2D = GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D
FMT_CV3D = GU_COLOR_8888 | GU_VERTEX_32BITF
FMT_TCNV3D = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_NORMAL_32BITF | GU_VERTEX_32BITF
GU_FLAT, GU_SMOOTH = 0, 1
GU_ALWAYS, GU_EQUAL, GU_GEQUAL = 1, 2, 7
GU_KEEP, GU_INCR = 0, 4
GU_PSM_8888 = 3

# =========================================================================== xorshift32 (as patch13 / main.c rd_rand)
class XS:
    def __init__(s, seed):
        s.x = seed & 0xFFFFFFFF or 0x2545F491
    def __call__(s):
        x = s.x
        x ^= (x << 13) & 0xFFFFFFFF; x ^= x >> 17; x ^= (x << 5) & 0xFFFFFFFF
        s.x = x; return x
def seed14(scene, item, chan):
    """xorshift32 seed of one colour search: scene, item (triangle or shape) and channel
    (0 R, 1 G, 2 B, 3 A, 4 fog)."""
    return ((scene << 24) | ((item & 0xFFFF) << 8) | (chan << 4) | 1) & 0xFFFFFFFF

def f32(v): return struct.unpack('<f', struct.pack('<f', v))[0]
def bf(v): return struct.unpack('<I', struct.pack('<f', v))[0]

# =========================================================================== sw_tri, ported
def area_rcp(area):
    """render.c area_rcp: 1/area as q / 2^sh from the 256-entry table with a linear step."""
    L = int(area).bit_length()
    sh = 16 + L - 1
    m = area >> (L - 17) if L > 17 else area << (17 - L)
    h, l = m >> 8, m & 0xFF
    R = (1 << 27) // h
    S = (1 << 24) // (h * h)
    return (R - ((l * S + 31) >> 5)) >> 3, sh

def top_left(dx, dy): return (dy == 0 and dx > 0) or dy < 0

def chan(c, i): return (c >> (8 * i)) & 0xFF

# Anchor rules.  'cur' is psprecomp today; 'z' is H1 (the depth plane's corner over all
# three); the rest are the rivals the scenes must separate:
#   zins / zinsfull / zscreen10 / zregion10: H1's choice made only among the corners
#       "inside" (fallback: all three when none is), inside meaning
#       zins       the scissor box on the pixel coordinate's low ten bits (render.c inside[])
#       zinsfull   the scissor box on the whole pixel coordinate
#       zscreen10  the screen (0..479, 0..271) on the low ten bits
#       zregion10  the drawing region, (0,0) to the scissor end (this PSPSDK's sceGuScissor
#                  sends REGION1 = 0, REGION2 = scissor end), on the low ten bits
#   zlower: x ties go to the lower corner;  zpix: corners compared by pixel column (x >> 4)
#   zswapT / zswapB / zswapTB: the level TOP pair, the level BOTTOM pair, or both, taken the
#       other way round from render.c (H1: the left one of a level top pair is the top, the
#       left one of a level bottom pair is the bottom)
# and the arithmetic rivals below (ARITH), on top of any anchor rule: 'z+bias1'.
RULES = ['cur', 'z', 'zins', 'zinsfull', 'zscreen10', 'zregion10', 'left', 'right', 'top', 'mid', 'bot',
         'first', 'second', 'last', 'zlower', 'zswapT', 'zswapB', 'zswapTB', 'zpix']
# Arithmetic rivals (H1d start, H1e gradient and clamp, H1g secondary), each written
# RULE+ARITH (e.g. 'z+trunc'; a bare name means z+name):
#   start:     centre0 (pixel centre +0, not +8/16), snap (anchor moved to its pixel's
#              centre), bias1 / bias16 / biashalf (start +1, +16, +8192 in 1/16384)
#   gradient:  trunc (toward zero), round, exact (n*16384 // area, no area_rcp),
#              fine (four more fraction bits), gclamp (saturated at 255 a pixel)
#   value:     wrap (mod 256, not clamped)
#   secondary: sumcorners (one plane of primary + secondary corner values, not the sum
#              of two floored planes; changes only primary-plus-secondary triangles)
START_ARITH = ['centre0', 'snap', 'bias1', 'bias16', 'biashalf']
ARITH = ['trunc', 'round', 'exact', 'fine', 'centre0', 'bias1', 'bias16', 'biashalf', 'snap', 'wrap', 'gclamp',
         'sumcorners']
PLANE_ARITH = [a for a in ARITH if a != 'sumcorners']      # the ones that change a single plane
INSIDE = {'cur': 'scissor10', 'zins': 'scissor10', 'zinsfull': 'scissorfull', 'zscreen10': 'screen10',
          'zregion10': 'region10'}

def parse_rule(s, arith=None):
    """'RULE[+ARITH][:fan|:nofan]' (a bare ARITH name means z+ARITH) -> (rule, arith, pc):
    pc True / False / None for ':nofan' / ':fan' / the default."""
    pc = None
    if ':' in s:
        s, f = s.split(':', 1)
        if f not in ('fan', 'nofan'): raise ValueError(f'{f}: the suffix is :fan or :nofan')
        pc = f == 'nofan'
    if '+' in s: s, arith = s.split('+', 1)
    elif s in ARITH: s, arith = 'z', s
    if s not in RULES: raise ValueError(f'{s}: rules are {", ".join(RULES)}')
    if arith is not None and arith not in ARITH: raise ValueError(f'{arith}: arithmetic rivals are {", ".join(ARITH)}')
    return s, arith, pc

def inside(v, kind, sc, precise):
    """Is corner v = (x16, y16) inside, for one of the INSIDE kinds?  Transformed (precise)
    corners always are (render.c)."""
    if precise: return True
    px, py = v[0] >> 4, v[1] >> 4
    wx, wy = px & 1023, py & 1023
    if kind == 'scissor10': return sc[0] <= wx <= sc[2] and sc[1] <= wy <= sc[3]
    if kind == 'scissorfull': return sc[0] <= px <= sc[2] and sc[1] <= py <= sc[3]
    if kind == 'screen10': return wx <= 479 and wy <= 271
    if kind == 'region10': return wx <= sc[2] and wy <= sc[3]
    raise ValueError(kind)

def roles(vs, top='left', bot='left'):
    """render.c's sort (top='left', bot='left'): (top, mid, bottom) by y then x, so the left
    one of a level top pair is the top, and of a level bottom pair the left one is the
    bottom.  'right' takes that pair the other way round.  Returns order, z_from_right."""
    o = sorted(range(3), key=lambda k: (vs[k][1], vs[k][0] if top == 'left' else -vs[k][0], k))
    if vs[o[1]][1] == vs[o[2]][1]:
        l, r = (o[1], o[2]) if vs[o[1]][0] <= vs[o[2]][0] else (o[2], o[1])
        o = [o[0], r, l] if bot == 'left' else [o[0], l, r]
    t, m, b = vs[o[0]], vs[o[1]], vs[o[2]]
    fr = (b[0] - t[0]) * (m[1] - t[1]) - (b[1] - t[1]) * (m[0] - t[0]) >= 0
    return o, fr

def pick(vs, right, allowed, lower=False, pix=False):
    X = (lambda v: v[0] >> 4) if pix else (lambda v: v[0])
    k0 = -1
    for k in range(3):
        if not allowed[k]: continue
        if k0 < 0 or (X(vs[k]) > X(vs[k0]) if right else X(vs[k]) < X(vs[k0])) or \
           (X(vs[k]) == X(vs[k0]) and (vs[k][1] > vs[k0][1] if lower else vs[k][1] < vs[k0][1])):
            k0 = k
    return k0

def anchor(vs, sub, rule, sc, precise):
    """vs: the three corners (x16, y16) after the winding swap; sub[k]: submission index of
    vs[k]; returns the anchor's index into vs."""
    allk = [1, 1, 1]
    o, fr = roles(vs)
    if rule in INSIDE:
        ins = [inside(vs[k], INSIDE[rule], sc, precise[k]) for k in range(3)]
        al = ins if any(ins) else allk
        return pick(vs, False if rule == 'cur' else fr, al)
    if rule == 'z': return pick(vs, fr, allk)
    if rule == 'zlower': return pick(vs, fr, allk, lower=True)
    if rule == 'zpix': return pick(vs, fr, allk, pix=True)
    if rule in ('zswapT', 'zswapB', 'zswapTB'):
        _, fr2 = roles(vs, 'right' if rule in ('zswapT', 'zswapTB') else 'left',
                       'right' if rule in ('zswapB', 'zswapTB') else 'left')
        return pick(vs, fr2, allk)
    if rule == 'left': return pick(vs, False, allk)
    if rule == 'right': return pick(vs, True, allk)
    if rule in ('top', 'mid', 'bot'): return o[('top', 'mid', 'bot').index(rule)]
    if rule in ('first', 'second', 'last'): return sub.index(('first', 'second', 'last').index(rule))
    raise ValueError(rule)

def depth_rule(rule):
    """The depth plane's corner under a colour rule.  psprecomp today restricts zk0 to the
    scissor-inside corners as well ('cur' -> zins); H1 and the miner's patch use all three;
    the inside variants and the tie variants move depth with colour (one shared corner);
    the colour-only rivals (left, top, first, ...) leave depth at H1."""
    if rule == 'cur': return 'zins'
    if rule in INSIDE or rule in ('zlower', 'zpix', 'zswapT', 'zswapB', 'zswapTB'): return rule
    return 'z'

def grad(n, rq, rsh, area, mode):
    """Gradient in 1/16384 channel per 1/16 px (render.c: floor(n*rq >> (rsh-14)))."""
    s = rsh - 14
    if mode == 'trunc':
        p = n * rq; return -((-p) >> s) if p < 0 else p >> s
    if mode == 'round': return (n * rq + (1 << (s - 1))) >> s
    if mode == 'exact': return (n << 14) // area
    if mode == 'gclamp':
        g = (n * rq) >> s; lim = 255 * 16384 // 16
        return max(-lim, min(lim, g))
    return (n * rq) >> s

class Opt:
    def __init__(s, rule='cur', arith=None):
        s.rule = rule; s.arith = arith

def tri_raster(V, sc):
    """Coverage of one triangle, render.c sw_tri: V = 3 (x16, y16) already wound (area > 0).
    Returns X, Y (pixel arrays)."""
    a, b, c = V
    xs = [p[0] for p in V]; ys = [p[1] for p in V]
    minx = max(min(xs) >> 4, sc[0]); maxx = min((max(xs) + 15) >> 4, sc[2])
    miny = max(min(ys) >> 4, sc[1]); maxy = min((max(ys) + 15) >> 4, sc[3])
    if minx > maxx or miny > maxy: return np.zeros(0, np.int64), np.zeros(0, np.int64)
    X, Y = np.meshgrid(np.arange(minx, maxx + 1, dtype=np.int64), np.arange(miny, maxy + 1, dtype=np.int64))
    X = X.ravel(); Y = Y.ravel()
    px = X * 16 + 8; py = Y * 16 + 8
    ok = np.ones(X.shape, bool)
    for p, q in ((b, c), (c, a), (a, b)):
        dx, dy = q[0] - p[0], q[1] - p[1]
        w = dx * (py - q[1]) - dy * (px - q[0])
        ok &= (w + (0 if top_left(dx, dy) else -1)) >= 0
    return X[ok], Y[ok]

def grads(V, cv, rq, rsh, area, arith=None):
    """A plane's (gx, gy) in 1/16384 channel per 1/16 px (H1's floor unless arith says
    otherwise; 'fine' is not of this form and gives H1's)."""
    a, b, c = V
    c0, c1, c2 = cv
    nx = (c1 - c0) * (c[1] - a[1]) - (c2 - c0) * (b[1] - a[1])
    ny = (c2 - c0) * (b[0] - a[0]) - (c1 - c0) * (c[0] - a[0])
    return grad(nx, rq, rsh, area, arith), grad(ny, rq, rsh, area, arith)

def plane(V, cv, k, X, Y, rq, rsh, area, arith=None, hi=255, clamp=True):
    """One plane's floored, clamped value at pixels (X, Y), started from corner k (hi: the
    clamp, 255 for colour and fog, 65535 for depth; clamp=False: the floored value before
    the clamp or wrap)."""
    a, b, c = V
    c0, c1, c2 = cv
    nx = (c1 - c0) * (c[1] - a[1]) - (c2 - c0) * (b[1] - a[1])
    ny = (c2 - c0) * (b[0] - a[0]) - (c1 - c0) * (c[0] - a[0])
    if arith == 'fine':          # four more fraction bits: 1/16384 of a step per pixel
        s = rsh - 18
        gx, gy = (nx * rq) >> s, (ny * rq) >> s
        acc = cv[k] * 262144 + gx * (X * 16 + 8 - V[k][0]) + gy * (Y * 16 + 8 - V[k][1])
        v = acc >> 18
    else:
        gx = grad(nx, rq, rsh, area, arith); gy = grad(ny, rq, rsh, area, arith)
        cx = 0 if arith == 'centre0' else 8
        xk, yk = V[k]
        if arith == 'snap': xk, yk = (xk >> 4) * 16 + 8, (yk >> 4) * 16 + 8
        acc = cv[k] * 16384 + gx * (X * 16 + cx - xk) + gy * (Y * 16 + cx - yk)
        if arith == 'bias1': acc = acc + 1
        if arith == 'bias16': acc = acc + 16
        if arith == 'biashalf': acc = acc + 8192
        v = acc >> 14
    if not clamp: return v
    if arith == 'wrap': return v & hi
    return np.clip(v, 0, hi)

def sw_tri(vtx, sc, flat, opt, depth=False):
    """vtx: 3 dicts (x, y in 1/16, rgba, fog, spec (None or u32), precise, z), submission
    order.  Returns X, Y, col (u32, pre-fog, secondary added), fog (int array) and, with
    depth, the depth plane (render.c: integer corner depths, its own corner)."""
    last_rgba = vtx[2]['rgba']
    V = [(v['x'], v['y']) for v in vtx]
    sub = [0, 1, 2]
    a, b, c = V
    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    if area == 0: return None
    vs = list(vtx)
    if area < 0:
        V = [V[0], V[2], V[1]]; sub = [0, 2, 1]; vs = [vtx[0], vtx[2], vtx[1]]; area = -area
    X, Y = tri_raster(V, sc)
    if len(X) == 0: return X, Y, np.zeros(0, np.int64), np.zeros(0, np.int64), (np.zeros(0, np.int64) if depth else None)
    prec = [v['precise'] for v in vs]
    k0 = anchor(V, sub, opt.rule, sc, prec)
    rq, rsh = area_rcp(area)
    ar = opt.arith
    Z = None
    if depth:
        kz = anchor(V, sub, depth_rule(opt.rule), sc, prec)
        Z = plane(V, [v['z'] for v in vs], kz, X, Y, rq, rsh, area, None, 65535)
    fog = plane(V, [v['fog'] for v in vs], k0, X, Y, rq, rsh, area, ar)
    if flat:
        col = np.full(len(X), last_rgba, np.int64)
    else:
        col = np.zeros(len(X), np.int64)
        spec = any(v['spec'] is not None for v in vs)
        sec = lambda v, i: chan(v['spec'], i) if v['spec'] is not None else 0
        for i in range(4):
            cv = [chan(v['rgba'], i) for v in vs]
            if spec and ar == 'sumcorners' and i < 3: cv = [c + sec(v, i) for c, v in zip(cv, vs)]
            col |= plane(V, cv, k0, X, Y, rq, rsh, area, ar) << (8 * i)
        if spec and ar != 'sumcorners':
            for i in range(3):
                s = plane(V, [chan(v['spec'], i) if v['spec'] is not None else 0 for v in vs],
                          k0, X, Y, rq, rsh, area, ar)
                ci = ((col >> (8 * i)) & 255)
                col = (col & ~(255 << (8 * i))) | (np.minimum(ci + s, 255) << (8 * i))
    return X, Y, col, fog, Z

def apply_fog(col, f, fogc):
    out = col & 0xFF000000
    for i in range(3):
        c = (col >> (8 * i)) & 255; fc = chan(fogc, i)
        out |= ((c * f + fc * (255 - f) + 255) >> 8) << (8 * i)
    return np.where(f >= 255, col, out)

# =========================================================================== lighting (ge.c light_vertex, the one case scene 95 uses)
def ge_pow(x, k):
    x = np.float32(x)
    if not x > 0: return np.float32(0)
    m, e = math.frexp(float(x))
    y = np.float32(k) * (np.float32(e - 1) + (np.float32(2.0) * np.float32(m) - np.float32(1.0)))
    if not y > -126: return np.float32(0)
    n = math.floor(y)
    return np.float32(math.ldexp(1.0 + float(np.float32(y - np.float32(n))), n))
def lit_mul(a, b): return ((2 * a + 1) * (2 * b + 1)) >> 10
def lit_byte(x):
    x = np.float32(x)
    if not x > 0: return 0
    q = np.float32(x * np.float32(256.0))
    return 255 if q >= 255 else int(q)

def light_spec_vertex(rgba, n, coef, spec_on):
    """Directional light (0,0,1), white diffuse and specular light, no ambient, colour
    material DIFFUSE|SPECULAR, separate specular: returns (primary, secondary or None)."""
    nn = np.array(n, np.float32)
    nl = np.float32(np.sqrt(np.float32(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2])))
    nn = (nn / nl).astype(np.float32)
    L = np.array([0, 0, 1], np.float32)
    ndl = np.float32(nn[0] * L[0] + nn[1] * L[1] + nn[2] * L[2])
    dfac = ndl if ndl > 0 else np.float32(0)
    sfac = np.float32(0)
    if ndl >= 0:
        H = np.array([0, 0, 2], np.float32); hl = np.float32(np.sqrt(np.float32(4)))
        H = (H / hl).astype(np.float32)
        ndh = np.float32(nn[0] * H[0] + nn[1] * H[1] + nn[2] * H[2])
        sfac = ge_pow(ndh, coef)
    vd, vsb = lit_byte(dfac), lit_byte(sfac)
    out = [0, 0, 0]; sec = [0, 0, 0]
    for k in range(3):
        vc = chan(rgba, k)
        out[k] = lit_mul(255, lit_mul(255, lit_mul(vd, lit_mul(255, vc))))
        sec[k] = lit_mul(255, lit_mul(255, lit_mul(vsb, lit_mul(255 if spec_on else 0, vc))))
    c = rgba & 0xFF000000
    s = 0
    for k in range(3):
        c |= min(out[k], 255) << (8 * k); s |= min(sec[k], 255) << (8 * k)
    return c, (s if s else None)

# =========================================================================== the mirror's GE
class Frame:
    def __init__(s, clear):
        s.px = np.full((H_, W_), clear & 0x00FFFFFF, np.int64)    # clear stencil 0 -> alpha byte 0
        s.cover = np.zeros((H_, W_), np.int32)                     # writes per pixel in this pass
        s.owner = np.full((H_, W_), -1, np.int32)                  # last triangle id written
        s.depth = np.zeros((H_, W_), np.int64)                     # scene_begin clears depth to 0
        s.dumpdepth = False

def through_z(z):
    """render.c: a through-mode corner's depth is its z as given, cut to an integer in 0..65535."""
    z = f32(z)
    return 0 if not z > 0 else (65535 if z >= 65535 else int(z))

class Mirror:
    """Runs a scene's op list.  opt picks the plane rule; everything else is psprecomp's.
    A rule is RULE[+ARITH][:fan|:nofan] (parse_rule): an arithmetic rival after '+', and
    ':nofan' (psprecomp's sw_draw, fans dropped) or ':fan' (fans drawn)."""
    def __init__(m, opt=None, ladder_exact=False, pc=None):
        m.opt = opt or Opt('cur')
        r, a, p = parse_rule(m.opt.rule, m.opt.arith)
        m.opt = Opt(r, a)
        if p is not None: pc = p
        m.ladder_exact = ladder_exact
        # psprecomp's own behaviour where it is not a plane rule: sw_draw dropped
        # through-mode GU_TRIANGLE_FAN until set 20's fix ("fans are assembled by the GE"
        # held only for transformed draws), leaving scene 88's fan tiles blank; 'z:fan',
        # fans as (first, previous, this), matches set 20's scene 88 on every pixel.
        m.pc = (m.opt.rule == 'cur') if pc is None else pc
    def reset(m, clear):
        m.F = Frame(clear)
        m.sc = (0, 0, 479, 271); m.flat = False
        m.atest = None; m.stencil = None; m.fog = None; m.depth = False
        m.P = m.V = m.W = P13.ident(); m.vp = (2048, 2048, 480, 272); m.off = (2048 - 240, 2048 - 136)
        m.light = None; m.tex = False
        m.tris = []          # per drawn triangle: dict(id, vtx, sc, X, Y)
        m.tid = 0
    # ---- vertices
    def vertex(m, v, vtype):
        if vtype & GU_TRANSFORM_2D:
            return dict(x=math.floor(f32(v['x']) * 16), y=math.floor(f32(v['y']) * 16), rgba=v['c'],
                        fog=255, spec=None, precise=0, z=through_z(v['z']))
        M = P13.wvp(m.P, m.V, m.W)
        p = [v['x'], v['y'], v['z'], 1.0]
        cx, cy, w = P13.dot4(M[0], p), P13.dot4(M[1], p), P13.dot4(M[3], p)
        nx, ny = P13.ge_over_w(cx, w), P13.ge_over_w(cy, w)
        sx = m.vp[0] - m.off[0] + (m.vp[2] / 2) * nx
        sy = m.vp[1] - m.off[1] - (m.vp[3] / 2) * ny
        assert sx * 16 == math.floor(sx * 16) and sy * 16 == math.floor(sy * 16), (sx, sy, v)
        fog = 255
        if m.fog:
            VW = P13.mat_mul(m.V, m.W)
            ez = P13.dot4(VW[2], p)
            f = f32(f32(m.fog[1] + ez) * m.fog[2])
            fog = 0 if f <= 0 else (min(255, int(f32(f * 256.0))) if f < 1 else 255)
        rgba, spec = v['c'], None
        if m.light is not None:
            rgba, spec = light_spec_vertex(v['c'], v['n'], m.light[1], m.light[0] == 1)
        return dict(x=int(sx * 16), y=int(sy * 16), rgba=rgba, fog=fog, spec=spec, precise=1, z=None)
    # ---- pixels
    def shade(m, X, Y, col, fog, tid, Z=None):
        F = m.F
        sc = m.sc
        ok = (X >= sc[0]) & (X <= sc[2]) & (Y >= sc[1]) & (Y <= sc[3])
        X, Y, col, fog = X[ok], Y[ok], col[ok], fog[ok]
        if Z is not None: Z = Z[ok]
        if m.atest:
            func, ref, mask = m.atest
            a = (col >> 24) & 255
            if func == GU_GEQUAL: p = (a & mask) >= (ref & mask)
            elif func == GU_EQUAL: p = (a & mask) == (ref & mask)
            else: p = np.ones(len(a), bool)
            X, Y, col, fog = X[p], Y[p], col[p], fog[p]
            if Z is not None: Z = Z[p]
        if m.depth and Z is not None: F.depth[Y, X] = Z      # depth test ALWAYS, writes on
        if m.fog: col = apply_fog(col, fog, m.fog[3])
        old = F.px[Y, X]
        if m.stencil == 'direct':      # analytic ladder: the stencil ends as the alpha plane
            new = col
        elif m.stencil:
            st = np.minimum(((old >> 24) & 255) + 1, 255)
            new = (col & 0x00FFFFFF) | (st << 24)
        else:
            new = (col & 0x00FFFFFF) | (old & 0xFF000000)
        F.px[Y, X] = new
        np.add.at(F.cover, (Y, X), 1)
        F.owner[Y, X] = tid
    def tri(m, vtx):
        dep = m.depth and all(v['z'] is not None for v in vtx)
        r = sw_tri(vtx, m.sc, m.flat, m.opt, dep)
        if r is None: return
        X, Y, col, fog, Z = r
        tid = m.tid; m.tid += 1
        m.tris.append(dict(id=tid, vtx=vtx, sc=m.sc, X=X, Y=Y, flat=m.flat, tex=m.tex, depth=dep))
        if m.tex:      # black texel REPLACE (RGB), then the secondary planes (render.c order)
            r2 = sw_tri([dict(v, rgba=v['rgba'] & 0xFF000000) for v in vtx], m.sc, m.flat, m.opt, dep)
            X, Y, col, fog, Z = r2
        m.shade(X, Y, col, fog, tid, Z)
    def point(m, v):
        x, y = v['x'] >> 4, v['y'] >> 4
        col = v['rgba'] & 0xFF000000 if m.tex else v['rgba']
        Z = np.array([v['z']], np.int64) if v['z'] is not None else None
        m.shade(np.array([x]), np.array([y]), np.array([col], np.int64), np.array([v['fog']]), -2, Z)
    def draw(m, prim, vtype, verts, idx):
        vv = [m.vertex(v, vtype) for v in verts]
        seq = [vv[i] for i in idx] if idx is not None else vv
        if prim == GU_POINTS:
            for v in seq: m.point(v)
        elif prim == GU_TRIANGLES:
            for i in range(0, len(seq) - 2, 3): m.tri(seq[i:i + 3])
        elif prim == GU_TRIANGLE_STRIP:
            for i in range(len(seq) - 2): m.tri(seq[i:i + 3])
        elif prim == GU_TRIANGLE_FAN:
            if m.pc and (vtype & GU_TRANSFORM_2D): return
            for i in range(2, len(seq)): m.tri([seq[0], seq[i - 1], seq[i]])
    def run(m, sc):
        frames = []
        for op in sc['ops']:
            k = op[0]
            if k == 'BEGIN': m.reset(op[2])
            elif k == 'SCISSOR': m.sc = tuple(op[1:5])
            elif k == 'SHADE': m.flat = op[1] == GU_FLAT
            elif k == 'MATS': m.P, m.V, m.W = op[1], op[2], op[3]
            elif k == 'VIEWPORT': m.vp = tuple(op[1:5])
            elif k == 'OFFSET': m.off = tuple(op[1:3])
            elif k == 'ALPHA': m.atest = tuple(op[2:5]) if op[1] else None
            elif k == 'STENCIL': m.stencil = op[1]
            elif k == 'FOG': m.fog = (op[1], f32(op[3]), f32(1.0 / (op[3] - op[2])), op[4]) if op[1] else None
            elif k == 'LIGHT': m.light = (op[1], op[2]) if op[1] else None
            elif k == 'TEX': m.tex = bool(op[1])
            elif k == 'DEPTH': m.depth = bool(op[1]) and op[2] == GU_ALWAYS
            elif k == 'DRAW': m.draw(op[1], op[2], op[3], op[4])
            elif k == 'LADDER':
                # draw once (alpha test off), then 'first'..'last' passes of GEQUAL r with
                # stencil INCR: the stencil (alpha byte) counts the r <= a, i.e. it is a.
                prim, vtype, verts, r0, r1 = op[1:6]
                m.atest = None; st = m.stencil
                m.stencil = 'direct' if not m.ladder_exact and (r0, r1) == (1, 255) else False
                m.draw(prim, vtype, verts, None)
                if m.ladder_exact or (r0, r1) != (1, 255):
                    m.stencil = True
                    for r in range(r0, r1 + 1):
                        m.atest = (GU_GEQUAL, r, 0xFF)
                        m.draw(prim, vtype, verts, None)
                m.atest = None; m.stencil = False
            elif k == 'END':
                m.F.dumpdepth = bool(op[1])
                frames.append((sc['num'], sc['name'], m.F.px.copy()))
        return m.F

# =========================================================================== scene builders
def P16(x): return int(round(x * 16))

def cv2d(x16, y16, c, z=0.0): return dict(x=x16 / 16.0, y=y16 / 16.0, z=float(z), c=c)
def rgba_of(ch):  # ch: list of 3 (r,g,b,a) tuples -> list of u32
    return [r | g << 8 | b << 16 | a << 24 for (r, g, b, a) in ch]

def wound(V):
    a, b, c = V
    ar = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    return ([V[0], V[2], V[1]], [0, 2, 1], -ar) if ar < 0 else (list(V), [0, 1, 2], ar)

class Geo:
    """A triangle copy prepared for colour scoring: wound corners, pixels, rcp, H1 corner."""
    def __init__(g, V, sc=(0, 0, 479, 271)):
        g.Vw, g.sub, g.area = wound(V)
        g.X, g.Y = tri_raster(g.Vw, sc)
        g.rq, g.rsh = area_rcp(g.area)
        g.kz = anchor(g.Vw, g.sub, 'z', sc, [0, 0, 0])
        g.kc = anchor(g.Vw, g.sub, 'cur', sc, [0, 0, 0])
    def vals(g, C, k, top=255):
        """C: (N, 3) candidate corner values in SUBMISSION order -> (N, P) floored values."""
        Cw = C[:, g.sub]
        a, b, c = g.Vw
        nx = (Cw[:, 1] - Cw[:, 0]) * (c[1] - a[1]) - (Cw[:, 2] - Cw[:, 0]) * (b[1] - a[1])
        ny = (Cw[:, 2] - Cw[:, 0]) * (b[0] - a[0]) - (Cw[:, 1] - Cw[:, 0]) * (c[0] - a[0])
        s = g.rsh - 14
        gx = (nx * g.rq) >> s; gy = (ny * g.rq) >> s
        xk, yk = g.Vw[k]
        acc = Cw[:, k:k + 1] * 16384 + gx[:, None] * (g.X[None, :] * 16 + 8 - xk) + gy[:, None] * (g.Y[None, :] * 16 + 8 - yk)
        return np.clip(acc >> 14, 0, top)

def choose_chan(geos, seed, lo=6, hi=249, ncand=48, top=255):
    """One channel's three corner values (submission order) for a set of copies that share
    them: ncand candidates from xorshift32(seed), each value lo + r % (hi-lo+1) drawn
    corner 0, 1, 2; score = min over copies and over the corners other than H1's of the
    pixels whose value changes when the plane starts there instead; the first best wins.
    top: the plane's clamp (255; 65535 for depth)."""
    r = XS(seed)
    C = np.array([[lo + r() % (hi - lo + 1) for _ in range(3)] for _ in range(ncand)], np.int64)
    score = corner_scores(geos, C, top)
    best = int(np.argmax(score))
    return [int(v) for v in C[best]], int(score[best])

def corner_scores(geos, C, top=255):
    """Per candidate row of C: min over copies and non-H1 corners of the pixels that change."""
    score = np.full(len(C), 1 << 30, np.int64)
    for g in geos:
        if len(g.X) == 0: continue
        ref = g.vals(C, g.kz, top)
        for k in range(3):
            if k == g.kz: continue
            score = np.minimum(score, (g.vals(C, k, top) != ref).sum(1))
    return score

def choose_rgba(geos, scene, item, chans=(0, 1, 2), alpha=0xFF):
    vals = {}; sc = {}
    for ch in chans:
        vals[ch], sc[ch] = choose_chan(geos, seed14(scene, item, ch))
    out = []
    for j in range(3):
        c = 0
        for ch in range(4):
            v = vals[ch][j] if ch in vals else (alpha if ch == 3 else 0)
            c |= v << (8 * ch)
        out.append(c)
    return out, sc

def mirror_rgba(c): return (c & 0xFF000000) | (~c & 0x00FFFFFF)

def tris_draw(tris, prim=GU_TRIANGLES, vtype=FMT_CV2D, zs=None):
    """zs: per triangle, its three through-mode z (else z = 0)."""
    verts = []
    for i, (V, cols) in enumerate(tris):
        Z = zs[i] if zs is not None else (0.0, 0.0, 0.0)
        for (x, y), c, z in zip(V, cols, Z): verts.append(cv2d(x, y, c, z))
    return ('DRAW', prim, vtype, verts, None)

def begin(): return ('BEGIN', GU_PSM_8888, 0xFF000000)

# ---------------------------------------------------------------- 83 anchorclock
def geo83():
    out = []
    base = [(21.0, 0.0), (17.0, 130.0), (20.0, 235.0)]
    for i in range(48):
        r, c = i // 8, i % 8
        cx, cy = 60 * c + 30, 45 * r + 22.5
        th = (i % 24) * 15.0 + 2.3
        V = []
        for rad, ang in base:
            a = math.radians(ang + th)
            dx, dy = rad * math.cos(a), rad * math.sin(a)
            if i >= 24: dx = -dx
            V.append((P16(cx + dx), P16(cy + dy)))
        out.append(V)
    return out

_cache = {}
def colours83():
    if '83' not in _cache:
        cols = []
        for i, V in enumerate(geo83()):
            c, _ = choose_rgba([Geo(V)], 0x83, i)
            cols.append(c)
        _cache['83'] = cols
    return _cache['83']

def sc83():
    return [begin(), tris_draw(list(zip(geo83(), colours83()))), ('END', 0)]

# ---------------------------------------------------------------- 84 anchorperm (8 shapes x 6 orders x plain/mirror)
SH84 = [  # 1/16 px inside a 38 x 32 px box (B's shapes): S0-S3 mid-left, S4-S7 the leftmost class
    [(30*16+3, 0*16+11), (0*16+13, 14*16+5), (36*16+7, 31*16+9)],
    [(36*16+5, 0*16+2), (2*16+9, 17*16+14), (36*16+5, 31*16+7)],     # vertical right long edge
    [(5*16+12, 0*16+6), (0*16+1, 25*16+3), (37*16+10, 31*16+13)],
    [(20*16+4, 0*16+9), (1*16+6, 9*16+1), (37*16+2, 30*16+15)],
    [(4*16+3, 0*16+11), (37*16+13, 12*16+5), (0*16+7, 31*16+9)],
    [(1*16+5, 0*16+2), (36*16+9, 15*16+14), (1*16+5, 31*16+7)],      # vertical left long edge
    [(0*16+3, 0*16+7), (37*16+11, 0*16+7), (20*16+9, 31*16+12)],     # flat top
    [(18*16+6, 0*16+4), (0*16+10, 31*16+13), (37*16+1, 31*16+13)],   # flat bottom
]
ORD = [(0, 1, 2), (1, 2, 0), (2, 0, 1), (0, 2, 1), (2, 1, 0), (1, 0, 2)]
def cell84(s, o, m):
    cell = (s * 6 + o) * 2 + m
    return 4 + (cell % 12) * 39, 2 + (cell // 12) * 34
def colours84():
    if '84' not in _cache:
        cols = []
        for s in range(8):
            cx, cy = cell84(s, 0, 0)
            V = [(x + cx * 16, y + cy * 16) for x, y in SH84[s]]
            c, _ = choose_rgba([Geo(V)], 0x84, s)
            cols.append(c)
        _cache['84'] = cols
    return _cache['84']
def tris84(alpha=None):
    """96 triangles: corner colours stay with the geometric corner; mirror set = 255 - c.
    alpha: None (0xFF) or per-shape (plain, mirror) alpha triples (scene 91)."""
    out = []
    for s in range(8):
        for o in range(6):
            for mm in range(2):
                cx, cy = cell84(s, o, mm)
                V = [(x + cx * 16, y + cy * 16) for x, y in SH84[s]]
                C = colours84()[s] if mm == 0 else [mirror_rgba(c) for c in colours84()[s]]
                if alpha is not None:
                    C = [(c & 0xFFFFFF) | alpha[s][mm][j] << 24 for j, c in enumerate(C)]
                out.append(([V[k] for k in ORD[o]], [C[k] for k in ORD[o]]))
    return out
def sc84(): return [begin(), tris_draw(tris84()), ('END', 0)]

# ---------------------------------------------------------------- 85 anchorties
def geo85():
    out = []
    for r in range(8):
        for c in range(8):
            ox, oy = 60 * c * 16, 34 * r * 16; k = c - 4
            X = lambda x: ox + x * 16; Y = lambda y: oy + y * 16
            V = [[(X(4), Y(4)), (X(52), Y(4) + k), (X(28), Y(30))],
                 [(X(4), Y(4)), (X(52), Y(4)), (X(4) + k, Y(30))],
                 [(X(4), Y(4)), (X(52), Y(4)), (X(52) + k, Y(30))],
                 [(X(28), Y(4)), (X(4), Y(30)), (X(52), Y(30) + k)],
                 [(X(4) + k, Y(4)), (X(4), Y(30)), (X(52), Y(30))],
                 [(X(52) + k, Y(4)), (X(4), Y(30)), (X(52), Y(30))],
                 [(X(30), Y(2)), (X(30) + k, Y(32)), (X(4), Y(17))],
                 [(X(30), Y(2)), (X(30) + k, Y(32)), (X(56), Y(17))]][r]
            out.append(V)
    return out
def sc85():
    if '85' not in _cache:
        _cache['85'] = [choose_rgba([Geo(V)], 0x85, i)[0] for i, V in enumerate(geo85())]
    return [begin(), tris_draw(list(zip(geo85(), _cache['85']))), ('END', 0)]

# ---------------------------------------------------------------- 86 anchorsub (256 sub-pixel offsets)
SL = [(22*16, 0), (0, 9*16), (26*16, 15*16)]       # mid-left, 26 x 15 px
SR = [(3*16, 0), (26*16, 5*16), (0, 15*16)]        # mid-right
def sub86(i):
    """Copy i's sub-pixel offset (dx, dy) in 1/16 px: every corner of the shape moves by it,
    and the shapes' corners are whole pixels, so it is also each corner's position & 15."""
    j = i % 128
    return j % 16, (j // 16) * 2 + ((j % 2) ^ (i >= 128))
def geo86():
    out = []
    for i in range(256):
        sh = SL if i < 128 else SR
        dx, dy = sub86(i)
        cx = 1 + (i % 16) * 30; cy = 1 + (i // 16) * 17
        out.append([(p[0] + cx * 16 + dx, p[1] + cy * 16 + dy) for p in sh])
    return out
def sc86():
    if '86' not in _cache:
        _cache['86'] = [choose_rgba([Geo(V)], 0x86, i)[0] for i, V in enumerate(geo86())]
    return [begin(), tris_draw(list(zip(geo86(), _cache['86']))), ('END', 0)]

# ---------------------------------------------------------------- 87 anchorscissor (per-triangle scissor)
# 16 triangles, one per 120x68 cell (row r, column c), each under its own scissor box
# (sx, sy)-(sx+107, sy+55), sx = 120c+6, sy = 68r+6.  Corners relative to (sx, sy) in px;
# ('A', v) is an absolute coordinate; +-1024 puts a corner where only the low ten bits of
# its pixel coordinate fall inside the box.  Mid-left (ML) triangles take H1's corner as
# the rightmost, mid-right (MR) as the leftmost; in every one but the last ML that corner
# lies outside the box with at least one other corner inside, so every scissor-inside
# rule (zins, zinsfull, zscreen10, zregion10) or psprecomp's leftmost-inside ('cur') has
# a different corner to choose wherever it calls H1's corner outside.  The names are
# computed from the geometry (describe87), not written by hand.
SPEC87 = [  # (row, col, class, corners)
    (0, 0, 'ML', [(150, 2), (8, 30), (90, 52)]),            # H1 right of the box, on screen
    (0, 1, 'ML', [(60, 2), (4, 30), (104, 90)]),            # H1 below the box, on screen
    (0, 2, 'ML', [(100, -300), (2, 40), (60, 54)]),         # H1 far above, off screen
    (0, 3, 'ML', [(('A', 600), 2), (4, 30), (70, 54)]),     # H1 at x = 600, off screen
    (1, 0, 'ML', [(100, -60), (5, 30), (70, 54)]),          # H1 above the box, on screen
    (1, 1, 'MR', [(-40, 2), (100, 30), (20, 54)]),          # H1 left of the box, on screen
    (1, 2, 'MR', [(2, -50), (100, 25), (40, 54)]),          # H1 above the box, on screen
    (1, 3, 'ML', [(100 + 1024, 3), (4, 30), (60, 54)]),     # H1 x - 1024 inside the box
    (2, 0, 'MR', [(30, 2), (100, 25), (2, 85)]),            # H1 below the box, on screen
    (2, 1, 'ML', [(150, -8), (-40, 25), (80, 50)]),         # two out: H1 right/above, one off screen
    (2, 2, 'MR', [(-30, -10), (140, 20), (50, 50)]),        # two out: H1 left/above, mid right
    (2, 3, 'ML', [(170, -30), (-50, 30), (90, 120)]),       # all three out of the box
    (3, 0, 'MR', [(('A', -100), 2), (100, 30), (40, 54)]),  # H1 at x = -100, off screen
    (3, 1, 'MR', [(30, 2), (106, 20), (-4, 330)]),          # H1 far below, off screen
    (3, 2, 'MR', [(5 - 1024, 2), (100, 30), (40, 54)]),     # H1 x + 1024 inside the box
    (3, 3, 'MR', [(30, 2), (100, 30), (2, 40 + 1024)]),     # H1 y - 1024 inside the box
]
def describe87(V, sc):
    """A name from the geometry: class, where H1's corner is, how many corners are in the box."""
    Vw, sub, _ = wound(V)
    o, fr = roles(Vw)
    kz = anchor(Vw, sub, 'z', sc, [0, 0, 0])
    def where(v):
        px, py = v[0] >> 4, v[1] >> 4
        if inside(v, 'scissorfull', sc, 0): return 'in the box'
        p = []
        if px < sc[0]: p.append('left of')
        if px > sc[2]: p.append('right of')
        if py < sc[1]: p.append('above')
        if py > sc[3]: p.append('below')
        s = ' and '.join(p) + ' the box'
        s += ', on screen' if (0 <= px <= 479 and 0 <= py <= 271) else f', off screen at ({px}, {py})'
        if inside(v, 'scissor10', sc, 0): s += ', low ten bits inside it'
        return s
    nin = sum(inside(v, 'scissorfull', sc, 0) for v in Vw)
    return f"{'ML' if fr else 'MR'}: H1 corner {where(Vw[kz])}; {nin} of 3 corners in the box"
def geo87():
    fr = [0.3125, 0.6875, 0.1875]
    out = []
    for i, (r, c, cls, cs) in enumerate(SPEC87):
        sx, sy = 120 * c + 6, 68 * r + 6
        sc = (sx, sy, sx + 107, sy + 55)
        V = []
        for j, (x, y) in enumerate(cs):
            ax = x[1] if isinstance(x, tuple) else sx + x
            ay = y[1] if isinstance(y, tuple) else sy + y
            V.append((P16(ax + fr[j]), P16(ay + fr[(j + 1) % 3])))
        name = describe87(V, sc)
        assert name.startswith(cls), (i, name)
        out.append((V, sc, name))
    return out
def colours87():
    if '87' not in _cache:
        _cache['87'] = [choose_rgba([Geo(V, sc)], 0x87, i)[0] for i, (V, sc, _) in enumerate(geo87())]
    return _cache['87']
def sc87():
    ops = [begin()]
    for (V, sc, _), C in zip(geo87(), colours87()):
        ops.append(('SCISSOR',) + sc)
        ops.append(tris_draw([(V, C)]))
    ops += [('SCISSOR', 0, 0, 479, 271), ('END', 0)]
    return ops

# ---------------------------------------------------------------- 97 scissordepth (87's depth twin)
def depth97():
    """Per triangle, three through-mode depths: choose_chan(seed14(0x97, i, 5)) over
    2000..63000 with the depth clamp, scored inside the triangle's box."""
    if '97' not in _cache:
        _cache['97'] = [choose_chan([Geo(V, sc)], seed14(0x97, i, 5), lo=2000, hi=63000, top=65535)[0]
                        for i, (V, sc, _) in enumerate(geo87())]
    return _cache['97']
def sc97():
    ops = [begin(), ('DEPTH', 1, GU_ALWAYS)]
    for (V, sc, _), C, Z in zip(geo87(), colours87(), depth97()):
        ops.append(('SCISSOR',) + sc)
        ops.append(tris_draw([(V, C)], zs=[Z]))
    ops += [('SCISSOR', 0, 0, 479, 271), ('DEPTH', 0, GU_ALWAYS), ('END', 1)]
    return ops

# ---------------------------------------------------------------- 88 primtypes
SH88 = [  # px relative to a 60 x 68 tile, corners a, b, c
    [(50.75, 2.1875), (57.4375, 64.8125), (3.3125, 35.5625)],     # mid-left
    [(4.5, 2.0625), (56.6875, 25.4375), (9.25, 65.9375)],         # mid-right
    [(55.0625, 2.25), (3.5, 20.375), (55.0625, 65.5)],            # mid-left, vertical right edge
]
# Row 3 (flat shading) is two half rows of 60x34 tiles holding the shape SH88F; corners
# a, b, c (S0's colours) and a fourth corner d (COL88D) where a draw makes two triangles.
SH88F = [(50.75, 1.1875), (57.4375, 32.8125), (3.3125, 17.5625)]    # a, b, c (mid-left)
D88S = (3.5, 33.25)       # strip's d: across edge b-c from a, so (b,c,d) does not overlap (a,b,c)
D88F = (4.0, 1.5)         # fan's d: across edge a-c from b, so (a,c,d) does not overlap (a,b,c)
COL88D = 0xFF3C9A5E
FLAT88 = [  # (half row, column, prim, vertices in memory, indices or None, provoking corner(s) by 'last vertex')
    (0, 0, GU_TRIANGLES, 'abc', None, 'c'), (0, 1, GU_TRIANGLES, 'bca', None, 'a'),
    (0, 2, GU_TRIANGLES, 'cab', None, 'b'), (0, 3, GU_TRIANGLES, 'acb', None, 'b'),
    (0, 4, GU_TRIANGLES, 'cba', None, 'a'), (0, 5, GU_TRIANGLES, 'bac', None, 'c'),
    (0, 6, GU_TRIANGLE_STRIP, 'abc', None, 'c'),            # strip triangle 0 (even)
    (0, 7, GU_TRIANGLE_STRIP, 'aabc', None, 'c'),           # strip triangle 1 (odd), 0 degenerate
    (1, 0, GU_TRIANGLE_STRIP, 'aaabc', None, 'c'),          # strip triangle 2 (even), 0-1 degenerate
    (1, 1, GU_TRIANGLE_STRIP, 'abcd', None, 'c/d'),         # strip (a,b,c) then (b,c,d), both drawn
    (1, 2, GU_TRIANGLE_FAN, 'abc', None, 'c'),              # fan triangle 0
    (1, 3, GU_TRIANGLE_FAN, 'abbc', None, 'c'),             # fan triangle 1, triangle 0 degenerate
    (1, 4, GU_TRIANGLE_FAN, 'abcd', None, 'c/d'),           # fan (a,b,c) then (a,c,d), both drawn
    (1, 5, GU_TRIANGLE_FAN, 'xcba', [3, 2, 1], 'c'),        # indexed fan, x never referenced
    (1, 6, GU_TRIANGLES, 'cba', [2, 1, 0], 'c'),            # indexed list
    (1, 7, GU_TRIANGLE_STRIP, 'dcba', [3, 2, 1, 0], 'c/d'), # indexed strip (a,b,c), (b,c,d)
]
def cols88():
    if '88' not in _cache:
        _cache['88'] = [choose_rgba([Geo([(P16(x), P16(y)) for x, y in SH88[s]])], 0x88, s)[0] for s in range(3)]
    return _cache['88']
def sc88():
    cols = cols88()
    ops = [begin()]
    for row in range(3):           # smooth: one shape a row, 8 ways of drawing it
        s = row
        for col in range(8):
            ox, oy = 60 * col, 68 * row
            P = [(P16(ox + x), P16(oy + y)) for x, y in SH88[s]]
            C = cols[s]
            if col < 6:
                o = ORD[col]
                ops.append(tris_draw([([P[k] for k in o], [C[k] for k in o])]))
            elif col == 6:         # strip a, a, b, c: (a,a,b) degenerate, then (a,b,c)
                ops.append(('DRAW', GU_TRIANGLE_STRIP, FMT_CV2D,
                            [cv2d(*P[k], C[k]) for k in (0, 0, 1, 2)], None))
            else:                  # indexed fan over [dummy, c, b, a], indices 3, 2, 1 -> a, b, c
                verts = [cv2d(ox * 16, oy * 16, 0xFF000000)] + [cv2d(*P[k], C[k]) for k in (2, 1, 0)]
                ops.append(('DRAW', GU_TRIANGLE_FAN, FMT_CV2D | GU_INDEX_16BIT, verts, [3, 2, 1]))
    ops.append(('SHADE', GU_FLAT))
    C = dict(zip('abc', cols[0])); C['d'] = COL88D; C['x'] = 0xFF000000
    for half, col, prim, mem, idx, _p in FLAT88:
        ox, oy = 60 * col, 204 + 34 * half
        P = {k: (P16(ox + x), P16(oy + y)) for k, (x, y) in zip('abc', SH88F)}
        P['d'] = (P16(ox + D88F[0]), P16(oy + D88F[1])) if prim == GU_TRIANGLE_FAN else \
                 (P16(ox + D88S[0]), P16(oy + D88S[1]))
        P['x'] = (ox * 16, oy * 16)
        verts = [cv2d(*P[k], C[k]) for k in mem]
        ops.append(('DRAW', prim, FMT_CV2D | (GU_INDEX_16BIT if idx else 0), verts, idx))
    ops += [('SHADE', GU_SMOOTH), ('END', 0)]
    return ops

# ---------------------------------------------------------------- 89 gradsteep (B's wedges and tiny triangles)
TH = [4, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64]
def tris89():
    tris = []
    cA, cB, cC = rgba_of([(0, 255, 40, 255), (128, 127, 90, 160), (255, 0, 215, 0)])
    for k, th in enumerate(TH):
        y = (2 + 10 * k) * 16 + 5
        A = (8*16+3, y); C = (8*16+3, y + th); B = (236*16+9, y + th // 2 + 48)
        tris.append(([A, B, C], [cA, cB, cC]))
        A2 = (471*16+13, y); C2 = (471*16+13, y + th); B2 = (244*16+7, y + th // 2 + 48)
        tris.append(([A2, C2, B2], [cA, cC, cB]))
    for j in range(9):
        for i in range(32):
            ox = (6 + i * 14) * 16 + (i * 5) % 16; oy = (128 + j * 15) * 16 + (j * 7 + i * 3) % 16
            w = 16 + ((i * 7 + j * 3) % 32); h = 16 + ((i * 11 + j * 5) % 32)
            if (i + j) % 2 == 0: V = [(ox, oy), (ox + w, oy + h // 3), (ox + w // 4, oy + h)]
            else: V = [(ox + w, oy), (ox, oy + h // 2), (ox + w - 3, oy + h)]
            tris.append((V, rgba_of([(255, 0, 255, 0), (0, 255, 0, 255), ((i * 37) & 255, (j * 53) & 255, 128, 64)])))
    return tris
def sc89(): return [begin(), tris_draw(tris89()), ('END', 0)]

# ---------------------------------------------------------------- 90 gradmid / 98 gradfar (B's scissored windows)
WIN = [(0, 0), (120, 0), (240, 0), (360, 0), (0, 136), (120, 136), (240, 136), (360, 136)]
def lim(v): return max(-2000, min(2000, v))
def far98():
    tris = []
    for w, (x0, y0) in enumerate(WIN):
        sc = (x0, y0, x0 + 119, y0 + 135)
        cx = x0 + 60 + 3 / 16; cy = y0 + 68 + 5 / 16
        f = lambda dx, dy: (P16(lim(cx + dx)), P16(lim(cy + dy)))
        V, C = [
            ([f(-1700, -1500), f(1500, -999.3125), f(1650, 1456.4375)], [(0, 0, 0, 0), (60, 200, 10, 128), (0, 0, 0, 0)]),
            ([f(-1650, -1456.4375), f(-1500, 1000.6875), f(1700, 1500)], [(255, 255, 255, 255), (195, 55, 245, 127), (255, 255, 255, 255)]),
            ([f(-200.25, -1900), f(-1980, 40.5), f(1980, 1700.125)], [(3, 250, 128, 10), (255, 0, 40, 240), (0, 255, 200, 100)]),
            ([f(200.25, -1900), f(1980, 40.5), f(-1980, 1700.125)], [(3, 250, 128, 10), (255, 0, 40, 240), (0, 255, 200, 100)]),
            ([f(30.5, -1990), f(-1900, 20.25), f(30.5, 1990)], [(250, 5, 90, 30), (10, 240, 170, 220), (128, 128, 0, 255)]),
            ([f(1000, -1200), f(1990, 600.5625), f(-1990, 1990)], [(0, 40, 255, 77), (255, 180, 0, 177), (90, 255, 30, 0)]),
            ([f(-30.5625, -66.8125), f(-59.4375, 40.125), f(58.9375, 66.9375)], [(255, 0, 77, 200), (0, 128, 255, 0), (140, 255, 0, 90)]),
            ([f(-55.875, -67.3125), f(59.6875, -20.4375), f(-50.0625, 67.0625)], [(255, 0, 77, 200), (0, 128, 255, 0), (140, 255, 0, 90)]),
        ][w]
        tris.append((V, rgba_of(C), sc))
    return tris
def mid90():
    out = []
    for V, C, sc in far98():
        cx = (sc[0] + 60) * 16 + 3; cy = (sc[1] + 68) * 16 + 5
        out.append(([(cx + (x - cx) // 4, cy + (y - cy) // 4) for x, y in V], C, sc))
    return out
def windows(tris):
    ops = [begin()]
    for V, C, sc in tris:
        ops.append(('SCISSOR',) + sc); ops.append(tris_draw([(V, C)]))
    return ops + [('SCISSOR', 0, 0, 479, 271), ('END', 0)]
def sc90(): return windows(mid90())
def sc98(): return windows(far98())

# ---------------------------------------------------------------- 91 alphaladder (84 with alpha planes)
def alpha91():
    """Per shape: plain set's alpha = its red corners; mirror set's alpha from its own search."""
    if '91' not in _cache:
        out = []
        for s in range(8):
            red = [chan(c, 0) for c in colours84()[s]]
            cx, cy = cell84(s, 0, 1)
            V = [(x + cx * 16, y + cy * 16) for x, y in SH84[s]]
            a, _ = choose_chan([Geo(V)], seed14(0x91, s, 3))
            out.append((red, a))
        _cache['91'] = out
    return _cache['91']
def sc91():
    d = tris_draw(tris84(alpha=alpha91()))
    return [begin(), ('LADDER', d[1], d[2], d[3], 1, 255), ('END', 0)]

# ---------------------------------------------------------------- 92 clock3d / 93 perspw / 94 fogplane / 95 spec
ID = P13.ident()
def mat_persp():   # clip x = X, clip y = Y, clip z = 0, clip w = -Z
    return [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 0, 0], [0, 0, -1.0, 0]]
VP3 = ('VIEWPORT', 2048, 2048, 512, 512)      # scale +256 / -256; offset as scene_begin
def model_xy(x16, y16, w=1):
    """Model X, Y that the identity transform (scale 256) puts on (x16, y16)/16 at clip w."""
    return w * (x16 - 3840) / 4096.0, w * (2176 - y16) / 4096.0
def cv3d(x16, y16, z, c, w=1):
    X, Y = model_xy(x16, y16, w)
    return dict(x=X, y=Y, z=z, c=c)
def sc92():
    verts = []
    for V, C in zip(geo83(), colours83()):
        for (x, y), c in zip(V, C): verts.append(cv3d(x, y, -0.5, c))
    return [begin(), ('MATS', ID, ID, ID), VP3, ('DRAW', GU_TRIANGLES, FMT_CV3D, verts, None), ('END', 0)]
WS = [(1, 2, 4), (4, 1, 2), (2, 4, 1)]
def sc93():
    verts = []
    for i, (V, C) in enumerate(zip(geo83(), colours83())):
        for j, ((x, y), c) in enumerate(zip(V, C)):
            w = WS[i % 3][j]
            verts.append(cv3d(x, y, -float(w), c, w))
    return [begin(), ('MATS', mat_persp(), ID, ID), VP3, ('DRAW', GU_TRIANGLES, FMT_CV3D, verts, None), ('END', 0)]
def fog94():
    if '94' not in _cache:
        _cache['94'] = [choose_chan([Geo(V)], seed14(0x94, i, 4))[0] for i, V in enumerate(geo83())]
    return _cache['94']
def fog_z(F): return (F + 0.5) / 256.0 - 1.0
def sc94():
    verts = []; pts = []
    for i, (V, Fs) in enumerate(zip(geo83(), fog94())):
        for (x, y), F in zip(V, Fs): verts.append(cv3d(x, y, fog_z(F), 0xFFFFFFFF))
        r, c = i // 8, i % 8
        for j, F in enumerate(Fs):     # the corner's fog as a point in the cell's top-left corner
            pts.append(cv3d((60 * c + 1 + 2 * j) * 16 + 8, (45 * r + 1) * 16 + 8, fog_z(F), 0xFFFFFFFF))
    return [begin(), ('MATS', ID, ID, ID), VP3, ('FOG', 1, 0.0, 1.0, 0x000000),
            ('DRAW', GU_TRIANGLES, FMT_CV3D, verts, None), ('DRAW', GU_POINTS, FMT_CV3D, pts, None),
            ('FOG', 0, 0.0, 1.0, 0), ('END', 0)]
SPEC95 = [  # screen corners (px) per triangle, normals by corner, colours (<= 0x7F a channel)
    ([(140.25, 2.5625), (150.6875, 86.4375), (4.3125, 47.8125)], [0xFF7F3F10, 0xFF20607F, 0xFF5A1F7A]),
    ([(306.0625, 2.25), (160.5, 30.375), (306.0625, 87.5)], [0xFF107F40, 0xFF7F2A55, 0xFF3C7F0F]),
    ([(320.5, 2.0625), (476.6875, 40.4375), (330.25, 87.9375)], [0xFF7F7F20, 0xFF207F7F, 0xFF7F207F]),
]
N95 = [(0.0, 0.0, 1.0), (0.6, 0.0, 0.8), (0.0, -0.6, 0.8)]
COEF95 = 2.0
def sc95():
    ops = [begin(), ('MATS', ID, ID, ID), VP3]
    verts = []
    for V, C in SPEC95:
        for (x, y), c, n in zip(V, C, N95):
            d = cv3d(P16(x), P16(y), -0.5, c); d['n'] = n; d['u'] = d['v'] = 0.0
            verts.append(d)
    for row, (mode, tex) in enumerate(((1, 0), (1, 1), (2, 0))):
        ops += [('OFFSET', 2048 - 240, 2048 - 136 - 90 * row), ('LIGHT', mode, COEF95), ('TEX', tex),
                ('DRAW', GU_TRIANGLES, FMT_TCNV3D, verts, None)]
        # every corner as a point, in a 4-px pitch row under the triangles: offset moves the
        # vertex by whole pixels, so its lighting is unchanged
        for t, (V, C) in enumerate(SPEC95):
            for j, (x, y) in enumerate(V):
                tx, ty = 8 + 4 * (3 * t + j) + 40 * row, 270
                dx, dy = tx - math.floor(x), ty - math.floor(y)
                ops += [('OFFSET', 2048 - 240 - dx, 2048 - 136 - dy),
                        ('DRAW', GU_POINTS, FMT_TCNV3D, [verts[3 * t + j]], None)]
    ops += [('TEX', 0), ('LIGHT', 0, 0.0), ('OFFSET', 2048 - 240, 2048 - 136), ('END', 0)]
    return ops

# ---------------------------------------------------------------- 96 fogw (94's hedge: fog where eye z = -w)
# Scene 93's projection (clip w = -Z, view and model identity, so eye z = Z = -w) with fog
# (near, far) = (FAR96 - 8, FAR96): f = (far - w) / 8, so a GE that took the fog depth from
# w instead of eye z would give the same corner bytes.  FAR96 = 1 + 200.5/32 puts every
# corner half-way between two fog bytes (f*256 = 200.5, 168.5, 104.5 for w = 1, 2, 4).
FAR96 = 7.265625
NEAR96 = FAR96 - 8.0
PERM3 = [(1, 2, 4), (1, 4, 2), (2, 1, 4), (2, 4, 1), (4, 1, 2), (4, 2, 1)]
def fog_of_w(w):
    """The mirror's vertex fog byte (Mirror.vertex) for eye z = -w under FOG(NEAR96, FAR96)."""
    f = f32(f32(f32(FAR96) + f32(-w)) * f32(1.0 / (FAR96 - NEAR96)))
    return 0 if f <= 0 else (min(255, int(f32(f * 256.0))) if f < 1 else 255)
def ws96():
    """Per triangle of scene 83's geometry, the w per corner: the first of the six orders of
    (1, 2, 4) whose fog plane best separates H1's corner from the others."""
    if '96' not in _cache:
        out = []
        for V in geo83():
            g = Geo(V)
            C = np.array([[fog_of_w(w) for w in p] for p in PERM3], np.int64)
            out.append(PERM3[int(np.argmax(corner_scores([g], C)))])
        _cache['96'] = out
    return _cache['96']
def sc96():
    verts = []; pts = []
    for i, (V, Ws) in enumerate(zip(geo83(), ws96())):
        for (x, y), w in zip(V, Ws): verts.append(cv3d(x, y, -float(w), 0xFFFFFFFF, w))
        r, c = i // 8, i % 8
        for j, w in enumerate(Ws):     # the corner's fog as a point in the cell's top-left corner
            pts.append(cv3d((60 * c + 1 + 2 * j) * 16 + 8, (45 * r + 1) * 16 + 8, -float(w), 0xFFFFFFFF, w))
    return [begin(), ('MATS', mat_persp(), ID, ID), VP3, ('FOG', 1, NEAR96, FAR96, 0x000000),
            ('DRAW', GU_TRIANGLES, FMT_CV3D, verts, None), ('DRAW', GU_POINTS, FMT_CV3D, pts, None),
            ('FOG', 0, 0.0, 1.0, 0), ('END', 0)]

SCENES = [  # (num, builder, dump name, step title)
    (83, sc83, 'anchorclock', 'colour planes: one scalene triangle at 48 orientations, through mode'),
    (84, sc84, 'anchorperm', 'colour planes: 8 shapes in 6 vertex orders, plain and mirrored colours'),
    (85, sc85, 'anchorties', 'colour planes: level and vertical corner pairs swept through the tie'),
    (86, sc86, 'anchorsub', 'colour planes: two shapes at 128 sub-pixel offsets each'),
    (87, sc87, 'anchorscissor', 'colour planes: corners outside a per-triangle scissor'),
    (88, sc88, 'primtypes', 'colour planes: list orders, strip, indexed fan; flat shading in 16 draws'),
    (89, sc89, 'gradsteep', 'colour planes: wedges down to 1/4 px and 288 tiny triangles'),
    (90, sc90, 'gradmid', 'colour planes: anchors up to 500 px outside a scissor window'),
    (91, sc91, 'alphaladder', 'colour planes: alpha read into the stencil by 255 alpha-test passes'),
    (92, sc92, 'anchorclock3d', 'colour planes: scene 83 through an identity 3D transform'),
    (93, sc93, 'perspw', 'colour planes: scene 83 with clip w 1, 2 and 4 at the corners'),
    (94, sc94, 'fogplane', 'colour planes: the fog coefficient plane, and its corners as points'),
    (95, sc95, 'specplane', 'colour planes: secondary colour, primary, and both, with corner points'),
    (96, sc96, 'fogw', 'colour planes: fog plane with eye z = -w (clip w 1, 2, 4), and its corners as points'),
    (97, sc97, 'scissordepth', 'colour planes: scene 87 again with corner depths; plus the full depth dump'),
    (98, sc98, 'gradfar', 'colour planes: anchors up to 2000 px outside a scissor window (last: range risk)'),
]
def scene(num):
    for n, b, name, title in SCENES:
        if n == num: return dict(num=n, name=name, title=title, ops=b())
    raise KeyError(num)
def all_scenes(): return [scene(n) for n, *_ in SCENES]

def render(sc, rule='cur', arith=None):
    """The mirror run of scene sc under rule (RULE[+ARITH][:fan|:nofan]) or rule + arith."""
    m = Mirror(Opt(rule, arith))
    m.run(sc)
    return m

# =========================================================================== stream emission
OPS = ['NOP', 'BEGIN', 'END', 'SCISSOR', 'SHADE', 'DRAW', 'MATS', 'VIEWPORT', 'OFFSET', 'ALPHA',
       'STENCIL', 'FOG', 'LADDER', 'LIGHT', 'TEX', 'SUMS', 'DEPTH',
       'BONE', 'MORPH', 'ZVIEW',                 # since geprobe 16 (skin16.py)
       'LGT', 'LMODE']                           # since geprobe 17 (lights17.py)
OP = {n: i for i, n in enumerate(OPS)}

def vbytes(vtype, verts):
    b = b''
    for v in verts:
        if vtype & GU_TEXTURE_32BITF: b += struct.pack('<ff', v['u'], v['v'])
        b += struct.pack('<I', v['c'])
        if vtype & GU_NORMAL_32BITF: b += struct.pack('<fff', *v['n'])
        b += struct.pack('<fff', v['x'], v['y'], v['z'])
    return b
def words(b):
    b = b + b'\0' * (-len(b) % 4)
    return list(struct.unpack(f'<{len(b) // 4}I', b))
def mem16(M):
    return [bf(M[r][c]) for c in range(4) for r in range(4)]

def emit_scene(sc):
    w = []; vcrc = 0; ndraw = 0; nvert = 0
    def op(name, args):
        w.append(OP[name] << 24 | len(args)); w.extend(a & 0xFFFFFFFF for a in args)
    body = []
    for o in sc['ops']:
        k = o[0]
        if k == 'BEGIN': body.append(('BEGIN', [o[1], o[2]]))
        elif k == 'END': body.append(('END', [o[1]]))
        elif k == 'SCISSOR': body.append(('SCISSOR', list(o[1:5])))
        elif k == 'SHADE': body.append(('SHADE', [o[1]]))
        elif k in ('DRAW', 'LADDER'):
            prim, vtype, verts = o[1], o[2], o[3]
            vb = vbytes(vtype, verts)
            idx = o[4] if k == 'DRAW' else None
            ib = struct.pack(f'<{len(idx)}H', *idx) if idx else b''
            cnt = len(idx) if idx else len(verts)
            vcrc = zlib.crc32(vb, vcrc); ndraw += 1; nvert += cnt
            if k == 'DRAW':
                body.append(('DRAW', [prim, vtype, cnt, len(vb), len(ib)] + words(vb) + words(ib)))
            else:
                body.append(('LADDER', [prim, vtype, cnt, len(vb), o[4], o[5]] + words(vb)))
        elif k == 'MATS': body.append(('MATS', [7] + mem16(o[1]) + mem16(o[2]) + mem16(o[3])))
        elif k == 'VIEWPORT': body.append(('VIEWPORT', list(o[1:5])))
        elif k == 'OFFSET': body.append(('OFFSET', list(o[1:3])))
        elif k == 'ALPHA': body.append(('ALPHA', list(o[1:5])))
        elif k == 'STENCIL': body.append(('STENCIL', [o[1]]))
        elif k == 'FOG': body.append(('FOG', [o[1], bf(o[2]), bf(o[3]), o[4]]))
        elif k == 'LIGHT': body.append(('LIGHT', [o[1], bf(o[2])]))
        elif k == 'TEX': body.append(('TEX', [o[1]]))
        elif k == 'DEPTH': body.append(('DEPTH', [o[1], o[2]]))
        else: raise ValueError(k)
    op(*body[0])
    op('SUMS', [ndraw, nvert, vcrc & 0xFFFFFFFF])
    for b in body[1:]: op(*b)
    return w, ndraw, nvert, vcrc & 0xFFFFFFFF

def emit(path):
    out = ['/* Generated by colour14.py emit: geprobe 14 scenes 83-98 (colour planes) as command',
           ' * streams for c14_run().  Do not edit; change colour14.py and run it again. */']
    for i, n in enumerate(OPS): out.append(f'#define C14_{n} {i}')
    steps = []
    for k, sc in enumerate(all_scenes()):
        w, nd, nv, vc = emit_scene(sc)
        crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
        steps.append((sc, len(w), nd, nv, vc, crc))
        out.append(f'\n/* scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
                   f' {len(w)} words, stream crc {crc:08X} */')
        out.append(f'static const w32 C14_S{k}[{len(w)}] = {{')
        for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
        out.append('};')
    out.append('\nstatic const struct c14_step { int scene; const char *name; const char *title; const w32 *s; int n; } C14_STEPS[] = {')
    for k, (sc, n, *_r) in enumerate(steps):
        out.append(f'    {{ {sc["num"]}, "{sc["name"]}", "{sc["title"]}", C14_S{k}, {n} }},')
    out.append('};\n#define C14_NSTEPS ((int)(sizeof C14_STEPS / sizeof C14_STEPS[0]))\n')
    open(path, 'w').write('\n'.join(out))
    return steps

# =========================================================================== verify the port on psprecomp's own frames
# verify needs a triangle log from an instrumented psprecomp run (one 'T ...' line per sw_tri call):
# set COLOUR14_TRILOG_DIR to the folder holding run/tri.txt.
MINER = os.environ.get('COLOUR14_TRILOG_DIR', '')
def trilog():
    T = {}; F = []
    for l in open(MINER + '/run/tri.txt'):
        if l[0] == 'T':
            parts = l.split('|'); h = parts[0].split()
            d = dict(flat=int(h[5]), sc=tuple(map(int, h[9:13])), v=[])
            for p in parts[1:]:
                f = p.split()
                d['v'].append(dict(x=int(f[0]), y=int(f[1]), rgba=int(f[2], 16), fog=int(f[3]),
                                   spec=int(f[4], 16) if int(f[5]) else None, precise=int(f[6])))
            T[int(h[1])] = d
        elif l[0] == 'F':
            _, i, n = l.split()
            if n.endswith('.raw'): F.append((int(i), os.path.basename(n)))
    return T, F

# What verify does not model, by scene: the triangle log gives sw_tri's output before the
# per-pixel stages, and only the last triangle to touch a pixel is kept.
VERIFY_NOTES = {10: 'dither', 11: 'blending', 15: 'fog applied after the plane (verify compares pre-fog colour)',
                18: 'depth-tested overlaps', 19: 'alpha/colour/stencil tests and logic op',
                22: 'patch line strips drawn over the triangles', 28: 'marker points drawn over the triangles',
                29: 'alpha/colour/stencil tests'}
VERIFY_DEFAULT = ('ge_39_gradients.raw', 'ge_46_gradsweep.raw', 'ge_47_gradsweep3d.raw')
def verify(names=VERIFY_DEFAULT):
    """names: dump names, or ('all',) for every 8888 dump of the miner's run that has
    triangles in the log.  Returns True when every listed scene without a VERIFY_NOTES
    entry matches on every pixel."""
    T, F = trilog()
    if tuple(names) == ('all',):
        names = []
        for _i, n in F:
            if n.endswith('_depth.raw') or n in names: continue
            p = MINER + '/run/ms/PSP/GAME/geprobe/' + n
            if os.path.exists(p) and os.path.getsize(p) == W_ * H_ * 4: names.append(n)
    ok = True
    for name in names:
        prev = 0
        for i, n in F:
            if n == name: lo, hi = prev, i; break
            if not n.endswith('_depth.raw'): prev = i
        fr = np.fromfile(MINER + '/run/ms/PSP/GAME/geprobe/' + name, np.uint32).reshape(272, 480).astype(np.int64)
        px = np.full((H_, W_), -1, np.int64)
        ntri = 0
        for tid in range(lo + 1, hi + 1):
            if tid not in T: continue
            t = T[tid]; ntri += 1
            r = sw_tri(t['v'], t['sc'], t['flat'], Opt('cur'))
            if r is None: continue
            X, Y, col, fog, _Z = r
            px[Y, X] = col & 0xFFFFFF
        if ntri == 0: continue
        drawn = px >= 0
        bad = drawn & (px != (fr & 0xFFFFFF))
        num = int(name[3:5])
        note = f'  (not modelled: {VERIFY_NOTES[num]})' if num in VERIFY_NOTES and bad.any() else ''
        print(f'{name}: {ntri} triangles, {int(drawn.sum())} px drawn by them, {int(bad.sum())} differ from psprecomp{note}')
        if num not in VERIFY_NOTES: ok &= not bad.any()
    return ok

# =========================================================================== checks
def edge_stats(t):
    """Through-mode triangle t (mirror tris entry): render.c's area (1/256 px^2), its pixels
    unclipped (area/256), the bounding box in px, and the largest |edge function| (1/256
    units) at a pixel centre of the scissor box and at the box corners of the triangle."""
    V, sub, area = wound([(v['x'], v['y']) for v in t['vtx']])
    xs = [p[0] for p in V]; ys = [p[1] for p in V]
    sc = t['sc']
    def emax(px, py):
        m = 0
        for p, q in ((V[1], V[2]), (V[2], V[0]), (V[0], V[1])):
            dx, dy = q[0] - p[0], q[1] - p[1]
            m = max(m, int(np.abs(dx * (py - q[1]) - dy * (px - q[0])).max()))
        return m
    cx = np.array([sc[0], sc[2], sc[0], sc[2]]) * 16 + 8; cy = np.array([sc[1], sc[1], sc[3], sc[3]]) * 16 + 8
    bx = np.array([min(xs), max(xs), min(xs), max(xs)]); by = np.array([min(ys), min(ys), max(ys), max(ys)])
    return dict(area_256=int(area), px_unclipped=int(area // 512), bbox_px=int((max(xs) - min(xs)) * (max(ys) - min(ys)) // 256),
                edge_max_in_window=emax(cx, cy), edge_max_at_bbox=emax(bx, by))

# The hardware's fill rate for an estimate of GE time: geprobe-s14 step 81 drew 100 sprites of
# 480x272 (13.06 M px) between two SIGNALs 35.6 ms apart.
FILL_PX_PER_MS = 480 * 272 * 100 / 35.6

def design_checks(scs=None, rules=None, ariths=None):
    rules = rules or [r for r in RULES if r != 'z']
    ariths = ariths or ARITH
    rep = {}
    for sc in scs or all_scenes():
        mz = render(sc, 'z')
        H1 = mz.F.px
        res = dict(name=sc['name'], tris=len(mz.tris))
        # coverage / overlap: count only the first (non-ladder) pass
        cov = mz.F.cover
        drawn = cov > 0
        res['px'] = int(drawn.sum())
        lad = any(o[0] == 'LADDER' for o in sc['ops'])
        res['overlap_px'] = int((cov > 1).sum())
        # corners on screen (scenes 87/90/97/98 place corners outside on purpose)
        offs = 0
        for t in mz.tris:
            for v in t['vtx']:
                if not (0 <= v['x'] < 480 * 16 and 0 <= v['y'] < 272 * 16): offs += 1
        res['corners_off_screen'] = offs
        # information: channels per pixel that are neither clamped nor constant
        R = H1 & 0xFFFFFF
        inf = 0
        for t in mz.tris:
            if t['flat'] or len(t['X']) == 0: continue
            X, Y = t['X'], t['Y']
            own = mz.F.owner[Y, X] == t['id']
            X, Y = X[own], Y[own]
            for i in range(3):
                vals = np.array([chan(v['rgba'], i) for v in t['vtx']])
                fogged = any(v['fog'] != 255 for v in t['vtx'])
                if vals.min() == vals.max() and not fogged: continue
                ch = (R[Y, X] >> (8 * i)) & 255
                inf += int(((ch > 0) & (ch < 255)).sum())
        res['informative_channels_per_px'] = round(inf / max(1, res['px']), 2)
        # separation
        sep = {}
        for r in rules:
            o = render(sc, r).F.px
            sep[r] = int((o != H1).sum())
        if sc['num'] == 88:
            o = Mirror(Opt('cur'), pc=False); o.run(sc)
            sep['cur_with_fans_drawn'] = int((o.F.px != H1).sum())
            o = Mirror(Opt('z'), pc=True); o.run(sc)
            fan = o.F.px != H1
            sep['fan_px_dropped_by_psprecomp'] = int(fan.sum())
            sep['cur_px_outside_the_fans'] = int(((render(sc, 'cur').F.px != H1) & ~fan).sum())
        for a in ariths:
            o = render(sc, 'z', a).F.px
            sep['z+' + a] = int((o != H1).sum())
        res['differs_from_H1'] = sep
        if mz.F.dumpdepth:
            res['depth_px'] = int((mz.F.depth > 0).sum())
            res['depth_differs_from_H1'] = {r: int((render(sc, r).F.depth != mz.F.depth).sum())
                                            for r in rules if depth_rule(r) != 'z' or r == 'cur'}
        # per triangle: smallest separation from H1 by any other corner (anchor resolvable?)
        mins = []; dmins = []
        for t in mz.tris:
            if t['flat'] or len(t['X']) == 0: continue
            g = Geo([(v['x'], v['y']) for v in t['vtx']], t['sc'])
            best = 1 << 30; dbest = 1 << 30
            for k in range(3):
                if k == g.kz: continue
                d = 0
                for i in range(4 if lad else 3):
                    C = np.array([[chan(v['rgba'], i) for v in t['vtx']]], np.int64)
                    if C.min() == C.max(): continue
                    d = max(d, int((g.vals(C, k) != g.vals(C, g.kz)).sum()))
                if any(v['fog'] != 255 for v in t['vtx']):
                    C = np.array([[v['fog'] for v in t['vtx']]], np.int64)
                    d = max(d, int((g.vals(C, k) != g.vals(C, g.kz)).sum()))
                best = min(best, d)
                if t.get('depth'):
                    C = np.array([[v['z'] for v in t['vtx']]], np.int64)
                    dbest = min(dbest, int((g.vals(C, k, 65535) != g.vals(C, g.kz, 65535)).sum()))
            mins.append(best)
            if t.get('depth'): dmins.append(dbest)
        res['tri_min_corner_sep'] = dict(min=int(min(mins)) if mins else None,
                                         median=float(np.median(mins)) if mins else None,
                                         zero=int(sum(1 for x in mins if x == 0)))
        if dmins:
            res['tri_min_corner_sep_depth'] = dict(min=int(min(dmins)), median=float(np.median(dmins)),
                                                   zero=int(sum(1 for x in dmins if x == 0)))
        # scissored scenes: which triangles each scissor rule moves off H1's corner
        if any(o[0] == 'SCISSOR' and tuple(o[1:5]) != (0, 0, 479, 271) for o in sc['ops']):
            per = []
            names = [n for _V, _s, n in geo87()] if sc['num'] in (87, 97) else None
            for t in mz.tris:
                Vw, sub, _ar = wound([(v['x'], v['y']) for v in t['vtx']])
                kz = anchor(Vw, sub, 'z', t['sc'], [0, 0, 0])
                moved = [r for r in ('cur', 'zins', 'zinsfull', 'zscreen10', 'zregion10')
                         if anchor(Vw, sub, r, t['sc'], [0, 0, 0]) != kz]
                d = dict(tri=t['id'], px=int(len(t['X'])), anchor_differs_under=moved)
                if names: d['name'] = names[t['id']]
                if sc['num'] in (90, 98): d.update(edge_stats(t))
                per.append(d)
            res['per_triangle'] = per
            res['triangles_separating'] = {r: sum(1 for d in per if r in d['anchor_differs_under'])
                                           for r in ('cur', 'zins', 'zinsfull', 'zscreen10', 'zregion10')}
            if sc['num'] == 98:
                un = sum(d['px_unclipped'] for d in per); bb = sum(d['bbox_px'] for d in per)
                res['ge_time_estimate_ms'] = dict(px_unclipped=un, bbox_px=bb,
                                                  if_whole_triangles_walked=round(un / FILL_PX_PER_MS, 1),
                                                  if_bounding_boxes_walked=round(bb / FILL_PX_PER_MS, 1),
                                                  if_scissor_windows_only=round(res['px'] / FILL_PX_PER_MS, 2))
        if lad:
            res['ge_time_estimate_ms'] = dict(px_per_pass=res['px'], passes=256,
                                              at_sprite_fill_rate=round(256 * res['px'] / FILL_PX_PER_MS, 1))
        rep[sc['num']] = res
    return rep

# =========================================================================== predicted frames / compare / decode
def depth_perm():
    """Screen (y, x) -> index into a `_depthfull.bin` dump (patch13.depth_full's permutation)."""
    y, x = np.mgrid[0:272, 0:480]
    l = 0x88000 + (y * 512 + x) * 2
    mid = (l >> 5) & 0x1F
    rot = ((mid << 1) | (mid >> 4)) & 0x1F
    p = ((l & ~(0x1F << 5)) | (rot << 5)) ^ 0x2040
    return (p - 0x88000) // 2

def predict(outdir, rule='cur'):
    """Predicted ge_NN_name.raw frames under a rule (e.g. cur, z, z:nofan, z+trunc, bias1),
    and for the scenes that dump depth a `_depthfull.bin` laid out as the probe dumps it."""
    parse_rule(rule)
    os.makedirs(outdir, exist_ok=True)
    for sc in all_scenes():
        F = render(sc, rule).F
        F.px.astype(np.uint32).tofile(os.path.join(outdir, f'ge_{sc["num"]:02d}_{sc["name"]}.raw'))
        if F.dumpdepth:
            raw = np.zeros(512 * 272, np.uint16)
            raw[depth_perm()] = F.depth.astype(np.uint16)
            raw.tofile(os.path.join(outdir, f'ge_{sc["num"]:02d}_{sc["name"]}_depthfull.bin'))

def load(d, sc):
    p = os.path.join(d, f'ge_{sc["num"]:02d}_{sc["name"]}.raw')
    if not os.path.exists(p): return None
    return np.fromfile(p, np.uint32).reshape(272, 480).astype(np.int64)

def load_depth(d, sc):
    p = os.path.join(d, f'ge_{sc["num"]:02d}_{sc["name"]}_depthfull.bin')
    if not os.path.exists(p): return None
    return np.fromfile(p, '<u2')[depth_perm()].astype(np.int64)

CMP_RULES = ('cur', 'z', 'z:nofan', 'zins', 'zinsfull', 'zscreen10', 'zregion10', 'left', 'zlower',
             'zswapT', 'zswapB', 'zswapTB', 'zpix', 'top', 'first')
CMP_ARITH = tuple('z+' + a for a in ARITH)
def compare(d, nums=None, rules=None):
    """Per scene and rule, the pixels of a dump that differ from the rule's prediction
    (32 bits in 91, else RGB), and for depth dumps the depth pixels.  By default the anchor
    rules (CMP_RULES) on one line and H1's corner under each arithmetic rival (CMP_ARITH)
    on a second, which ends with the rule(s) of all those with the fewest pixels off; with
    rules given, those alone.  Returns {scene: {rule: px}}."""
    lines = [('', CMP_RULES), (' arith', CMP_ARITH)] if rules is None else [('', tuple(rules))]
    for _l, rs in lines:
        for r in rs: parse_rule(r)
    out = {}
    for sc in all_scenes():
        if nums and sc['num'] not in nums: continue
        fr = load(d, sc)
        if fr is None: print(sc['num'], 'missing'); continue
        dep = load_depth(d, sc)
        mask = 0xFFFFFFFF if sc['num'] == 91 else 0xFFFFFF
        cnt = {}; dcnt = {}
        for tag, rs in lines:
            line = []; dline = []
            for r in rs:
                F = render(sc, r).F
                cnt[r] = int(((F.px & mask) != (fr & mask)).sum())
                line.append(f'{r}:{cnt[r]}')
                if F.dumpdepth and dep is not None and parse_rule(r)[1] is None:   # depth takes no arith rival
                    dcnt[r] = int((F.depth != dep).sum()); dline.append(f'{r}:{dcnt[r]}')
            if rules is None and tag:
                lo = min(cnt.values())
                line.append(f'| fewest px off ({lo}): ' + ' '.join(r for r in cnt if cnt[r] == lo))
            print(f'{sc["num"]} {sc["name"]}{tag}: ' + ' '.join(line))
            if dline: print(f'{sc["num"]} {sc["name"]}{tag} depth: ' + ' '.join(dline))
        out[sc['num']] = cnt
    return out

def cmp_dirs(a, b):
    """Two dump folders (e.g. a psprecomp run and a prediction) frame by frame, all 32 bits,
    and the depth of the scenes that dump it."""
    ok = True
    for sc in all_scenes():
        x, y = load(a, sc), load(b, sc)
        if x is None or y is None: print(sc['num'], sc['name'], 'missing'); ok = False; continue
        n = int((x != y).sum()); s = f'{sc["num"]} {sc["name"]}: {n} px differ (32 bit)'
        dx, dy = load_depth(a, sc), load_depth(b, sc)
        if dx is not None or dy is not None:
            nd = int((dx != dy).sum()) if dx is not None and dy is not None else -1
            s += f', depth {nd}'; ok &= nd == 0
        print(s); ok &= n == 0
    return ok

def fit_plane(g, cw, X, Y, vals, top=255):
    """The start constants (1/16384 units, at pixel (0,0)'s centre) that floored values
    `vals` at interior pixels (X, Y) allow for corner values cw (wound order), and which
    corners' predicted start lies in that interval.  None when no pixel is unclamped."""
    a, b, c = g.Vw
    nx = (cw[1] - cw[0]) * (c[1] - a[1]) - (cw[2] - cw[0]) * (b[1] - a[1])
    ny = (cw[2] - cw[0]) * (b[0] - a[0]) - (cw[1] - cw[0]) * (c[0] - a[0])
    s = g.rsh - 14; gx = (nx * g.rq) >> s; gy = (ny * g.rq) >> s
    G = gx * 16 * X + gy * 16 * Y
    u = (vals > 0) & (vals < top)
    if not u.any(): return None
    lo = int((vals[u] * 16384 - G[u]).max()); hi = int(((vals[u] + 1) * 16384 - G[u]).min())
    Cs = [cw[k] * 16384 + gx * (8 - g.Vw[k][0]) + gy * (8 - g.Vw[k][1]) for k in range(3)]
    fits = [k for k in range(3) if lo <= Cs[k] < hi]
    if lo >= hi: key = 'empty'
    elif fits == [g.kz] or (g.kz in fits and all(Cs[k] == Cs[g.kz] for k in fits)): key = 'H1'
    elif g.kz in fits: key = 'H1+other'
    elif fits: key = 'other'
    else: key = 'none'
    return key, lo, hi, Cs, fits

def refit_pm1(g, cw, X, Y, vals, top=255):
    """For a 'none' or 'empty' fit: does some corner value moved by -1, 0 or +1 make it fit?
    'H1 with corners+-1' / 'other with corners+-1' / '' (no)."""
    found = ''
    for d0 in (-1, 0, 1):
        for d1 in (-1, 0, 1):
            for d2 in (-1, 0, 1):
                if d0 == d1 == d2 == 0: continue
                r = fit_plane(g, [cw[0] + d0, cw[1] + d1, cw[2] + d2], X, Y, vals, top)
                if r is None: continue
                if r[0] in ('H1', 'H1+other'): return 'H1 with corners+-1'
                if r[0] == 'other': found = 'other with corners+-1'
    return found

# ---- decode's readouts of the arithmetic (H1d, H1e, H1g): what the spec's analysis plan does
# when a fit is not 'H1'.  All take the wound corner values cw and a corner k to start from.
def start_offsets(g, cw, k):
    """H1d: each start rival's start constant minus H1's from corner k (1/16384 units):
    centre0 -8(gx+gy), snap gx(xk%16-8) + gy(yk%16-8) (86: xk%16, yk%16 = the copy's
    (dx, dy)), bias1 1, bias16 16, biashalf 8192.  Gradients are H1's."""
    gx, gy = grads(g.Vw, cw, g.rq, g.rsh, g.area)
    xk, yk = g.Vw[k]
    return {'centre0': -8 * (gx + gy), 'snap': gx * ((xk & 15) - 8) + gy * ((yk & 15) - 8),
            'bias1': 1, 'bias16': 16, 'biashalf': 8192}

def arith_fits(g, cw, k, X, Y, vals, top, ok):
    """H1's plane and every PLANE_ARITH rival's, from corner k, at pixels (X, Y) (the
    triangle's own, edge pixels included: 89's slivers have no interior) against the dump
    where ok.  Returns (H1 reproduces every pixel, {rival: (px where it and H1 differ,
    verdict, it reproduces every pixel)}): verdict 'rival' / 'H1' / 'neither' as the dump
    matches one of the two on all of those pixels, '' when they never differ."""
    h = plane(g.Vw, cw, k, X, Y, g.rq, g.rsh, g.area, None, top)
    res = {}
    for a in PLANE_ARITH:
        p = plane(g.Vw, cw, k, X, Y, g.rq, g.rsh, g.area, a, top)
        D = ok & (p != h)
        n = int(D.sum())
        v = '' if not n else 'rival' if (vals[D] == p[D]).all() else 'H1' if (vals[D] == h[D]).all() else 'neither'
        res[a] = (n, v, bool((p[ok] == vals[ok]).all()))
    return bool((h[ok] == vals[ok]).all()), res

def clamp_wrap(g, cw, k, X, Y, vals, top, ok):
    """H1e clamp vs wrap (90/98's W0 and W1): at the pixels where H1's floored plane from
    corner k leaves 0..top, (pixels, dump == the clamp, dump == the value mod top+1)."""
    raw = plane(g.Vw, cw, k, X, Y, g.rq, g.rsh, g.area, None, top, clamp=False)
    o = ok & ((raw < 0) | (raw > top))
    return int(o.sum()), int((o & (vals == np.clip(raw, 0, top))).sum()), int((o & (vals == (raw & top))).sum())

def line_fits(g, cw, X, Y, vals, top, by):
    """Fit per row (by='row') or column ('col'): H1's form and gradient within each line of
    interior pixels alone, the start left free per line.  A row tests gx alone (gy*16Y is
    one constant along it), a column gy.  Returns (lines that fit, lines with two or more
    unclamped pixels)."""
    gx, gy = grads(g.Vw, cw, g.rq, g.rsh, g.area)
    u = (vals > 0) & (vals < top)
    if not u.any(): return 0, 0
    G = gx * 16 * X[u] + gy * 16 * Y[u]
    ks, inv, n = np.unique((Y if by == 'row' else X)[u], return_inverse=True, return_counts=True)
    lo = np.full(len(ks), -(1 << 62), np.int64); hi = np.full(len(ks), 1 << 62, np.int64)
    np.maximum.at(lo, inv, vals[u] * 16384 - G); np.minimum.at(hi, inv, (vals[u] + 1) * 16384 - G)
    m = n >= 2
    return int((lo < hi)[m].sum()), int(m.sum())

def grad_window(g, cw, X, Y, vals, top, R=3):
    """The integer gradient offsets (dgx, dgy), each within +-R of H1's (gx, gy), for which
    some start constant reproduces every unclamped interior pixel in H1's form."""
    gx, gy = grads(g.Vw, cw, g.rq, g.rsh, g.area)
    u = (vals > 0) & (vals < top)
    if not u.any(): return []
    X, Y, v = X[u] * 16, Y[u] * 16, vals[u] * 16384
    out = []
    for dgx in range(-R, R + 1):
        for dgy in range(-R, R + 1):
            G = (gx + dgx) * X + (gy + dgy) * Y
            if (v - G).max() < (v + 16384 - G).min(): out.append((dgx, dgy))
    return out

def residual_slope(g, cw, k, X, Y, vals, top):
    """Residual against distance from the anchor (89/90/98): the dump minus H1's plane from
    corner k at interior pixels where both are unclamped, fitted as c + ax*dx + ay*dy, with
    (dx, dy) the pixel centre's distance from the corner in 1/16 px.  Returns (pixels,
    pixels off, 16384*ax, 16384*ay): the gradient H1 misses in its own units (1/16384 a
    1/16 px; fractions are bits H1 lacks; None along an axis the pixels span by under
    4 px), or None with fewer than 3 such pixels.  A start rival shows here too, as a
    residual that does not grow with distance (the intercept, not reported)."""
    pred = plane(g.Vw, cw, k, X, Y, g.rq, g.rsh, g.area, None, top)
    u = (vals > 0) & (vals < top) & (pred > 0) & (pred < top)
    if u.sum() < 3: return None
    r = (vals - pred)[u]
    if not r.any(): return int(u.sum()), 0, 0.0, 0.0
    D = [(X[u] * 16 + 8 - g.Vw[k][0]).astype(float), (Y[u] * 16 + 8 - g.Vw[k][1]).astype(float)]
    use = [d.max() - d.min() >= 64 for d in D]      # an axis the pixels span by under 4 px says nothing
    A = np.stack([np.ones(len(r))] + [d for d, s in zip(D, use) if s], 1)
    sol = list(np.linalg.lstsq(A, r.astype(float), rcond=None)[0][1:])
    gx = float(sol.pop(0) * 16384) if use[0] else None
    gy = float(sol.pop(0) * 16384) if use[1] else None
    return int(u.sum()), int((r != 0).sum()), gx, gy

def row0_95(g, pw, sw, k, X, Y, vals, ok, arith=None):
    """H1g, scene 95 row 0 (primary plus secondary, no texture), one channel: H1's sum of the
    two floored planes (min(255, P + S), psprecomp) against one plane of the summed corner
    values, both from corner k (each plane in H1's arithmetic, or arith's).  Returns (pixels where they differ, verdict 'sum of planes'
    / 'plane of sums' / 'neither' / '' as the dump matches one on all of them, dump == sum
    of planes there, dump == plane of sums there)."""
    P = plane(g.Vw, pw, k, X, Y, g.rq, g.rsh, g.area, arith)
    S = plane(g.Vw, sw, k, X, Y, g.rq, g.rsh, g.area, arith)
    a = np.minimum(P + S, 255)
    b = plane(g.Vw, [p + s for p, s in zip(pw, sw)], k, X, Y, g.rq, g.rsh, g.area, arith)
    D = ok & (a != b)
    n = int(D.sum()); na = int((vals[D] == a[D]).sum()); nb = int((vals[D] == b[D]).sum())
    v = '' if not n else 'sum of planes' if na == n else 'plane of sums' if nb == n else 'neither'
    return n, v, na, nb

def point_px(fr, x, y): return int(fr[y, x])

def corner_values(scn, t, tri_index, fr, i):
    """The corner values (submission order) decode uses for triangle t's channel i, and
    where they came from.  Scenes 94 and 96 read each corner's fog byte from its point
    (R byte; white vertices, black fog); scene 95 reads row 2's primary and row 1's
    secondary from that row's points (row 1's from row 0 minus row 2 when the hardware
    leaves row-1 points black).  Everything else, and any point missing, from the mirror."""
    mir = None
    if scn in (94, 96): mir = [v['fog'] for v in t['vtx']]
    elif t['tex']: mir = [chan(v['spec'], i) if v['spec'] is not None else 0 for v in t['vtx']]
    else: mir = [chan(v['rgba'], i) for v in t['vtx']]
    if scn in (94, 96):
        r, c = tri_index // 8, tri_index % 8
        return [point_px(fr, 60 * c + 1 + 2 * j, 45 * r + 1) & 0xFF for j in range(3)], 'point', mir
    if scn == 95:
        row = tri_index // 3; tt = tri_index % 3
        pt = lambda rw, j: point_px(fr, 8 + 4 * (3 * tt + j) + 40 * rw, 270)
        if row == 2: return [chan(pt(2, j), i) for j in range(3)], 'point', mir
        if row == 1:
            p1 = [pt(1, j) for j in range(3)]
            if any(p & 0xFFFFFF for p in p1): return [chan(p, i) for p in p1], 'point', mir
            p0 = [pt(0, j) for j in range(3)]; p2 = [pt(2, j) for j in range(3)]
            if any((a ^ b) & 0xFFFFFF for a, b in zip(p0, p2)):
                return [max(0, chan(a, i) - chan(b, i)) for a, b in zip(p0, p2)], 'point0-2', mir
    return mir, 'mirror', mir

def decode(d, nums=None, verbose=False, use_points=True, quiet=False, alpha='a', rule='z'):
    """Per triangle and channel: the start-constant interval the dump allows (interior
    pixels, unclamped), and which corners' predicted start lies in it.  Corner values come
    from the scene's own corner points where it draws them (94, 95, 96); a 'none' or
    'empty' fit names the arithmetic rivals that reproduce the plane ('(arith: snap)'),
    else is retried with each corner value moved by +-1, which tells a corner byte one off
    from an anchor or arithmetic miss.  Scene 97 also decodes its depth planes.
    The arithmetic (H1d, H1e, H1g), per scene, every plane started from the corner the
    anchor rule `rule` picks (H1's by default; --rule=cur for psprecomp's frames; an
    arithmetic rival in it, --rule=z+trunc, is the arithmetic of 95's row-0 forms):
      * every colour, alpha and fog plane under H1's arithmetic and under each PLANE_ARITH
        rival, at the triangle's own pixels: how many planes H1 reproduces, and per rival
        the planes on which it and H1 differ, by which of the two the dump follows there;
      * clamp vs wrap at the pixels where H1's plane leaves 0..255 (90/98's W0 and W1);
      * scene 95 row 0: the sum of two floored planes (H1, psprecomp) against one plane of
        the summed corners, per channel (from another corner if neither fits from rule's);
      * for 'empty' fits, the fit per row (tests gx) and per column (gy); for any plane
        off H1, the residual against distance from the anchor (the missing gradient).
    -v prints, per plane off H1: the interval and its offset from H1's start, gx and gy,
    86's (dx, dy), each start rival's offset (and whether the interval holds it), the row
    and column fits, the integer gradients that fit, the residual slope and the rivals
    that reproduce the plane.
    alpha: how scene 91's alpha byte reads the alpha plane a, if selfcheck finds it is not
    a itself: '256-a' (GEQUAL passing when ref >= a), '255-a', or '2a' (the test-free first
    draw writing alpha; bytes of 255 then say nothing and are skipped).
    Returns {scene: (colour tally, depth tally, point sources, point corners != mirror,
    arithmetic dict)}."""
    out = {}
    for sc in all_scenes():
        if nums and sc['num'] not in nums: continue
        fr = load(d, sc)
        if fr is None: continue
        dep = load_depth(d, sc)
        m = render(sc, 'z')
        arule, aarith, _pc = parse_rule(rule)
        tally = {}; dtally = {}; ptdiff = 0; ptsrc = {}
        ar = dict(planes=0, H1=0, rivals={a: [0, 0, 0, 0, 0] for a in PLANE_ARITH}, clamp=[0, 0, 0],
                  row0={}, row0px=[0, 0, 0], lines=[0, 0, 0, 0], resid=[], fits={}, scene=sc['num'])
        def src(cv, s, mir):
            nonlocal ptdiff
            if s != 'mirror':
                ptsrc[s] = ptsrc.get(s, 0) + 1
                ptdiff += cv != mir
        for ti, t in enumerate(m.tris):
            if t['flat'] or len(t['X']) == 0: continue
            g = Geo([(v['x'], v['y']) for v in t['vtx']], t['sc'])
            ka = anchor(g.Vw, g.sub, arule, t['sc'], [t['vtx'][s]['precise'] for s in g.sub])
            own = m.F.owner[g.Y, g.X] == t['id']
            if not own.any(): continue
            Xo, Yo = g.X[own], g.Y[own]
            inner = np.ones(len(Xo), bool)
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    xx, yy = np.clip(Xo + dx, 0, 479), np.clip(Yo + dy, 0, 271)
                    inner &= m.F.owner[yy, xx] == t['id']
            Xi, Yi = Xo[inner], Yo[inner]
            nch = 4 if sc['num'] == 91 else 3
            spec = any(v['spec'] is not None for v in t['vtx'])
            planes = []
            if spec and not t['tex']:                 # row 0 of 95: primary + secondary
                tt = ti % 3
                for i in range(3):
                    pv, ps, pm = corner_values(sc['num'], t, 6 + tt, fr, i)
                    sv, ss, sm = corner_values(sc['num'], dict(t, tex=True), 3 + tt, fr, i)
                    if not use_points: pv, ps, sv, ss = pm, 'mirror', sm, 'mirror'
                    src(pv, ps, pm); src(sv, ss, sm)
                    vals = (fr[Yo, Xo] >> (8 * i)) & 255
                    for k in [ka] + [k for k in range(3) if k != ka]:     # rule's corner first
                        n, v, na, nb = row0_95(g, [pv[s] for s in g.sub], [sv[s] for s in g.sub], k, Xo, Yo,
                                               vals, np.ones(len(Xo), bool), aarith)
                        if v != 'neither': break
                    else: k = ka; n, v, na, nb = row0_95(g, [pv[s] for s in g.sub], [sv[s] for s in g.sub],
                                                          k, Xo, Yo, vals, np.ones(len(Xo), bool), aarith)
                    key = (v or 'same') + (f' (from corner {k}, not {arule}\'s)' if k != ka else '')
                    ar['row0'][key] = ar['row0'].get(key, 0) + 1
                    ar['row0px'][0] += n; ar['row0px'][1] += na; ar['row0px'][2] += nb
                    if verbose and key != 'sum of planes':
                        print(f"  tri {t['id']} ch {i} (row 0): {key}; of the {n} px where the two forms differ "
                              f"(from corner {k}), {na} read the sum of planes and {nb} the plane of sums; "
                              f"primary {pv} ({ps}), secondary {sv} ({ss}), {arule}'s corner {ka}")
            else:
                for i in range(nch):
                    cv, s, mir = corner_values(sc['num'], t, ti, fr, i)
                    if not use_points: cv, s = mir, 'mirror'
                    src(cv, s, mir)
                    vals = (fr[Yo, Xo] >> (8 * i)) & 255
                    ok = np.ones(len(Xo), bool)
                    if i == 3 and alpha != 'a':       # back to the plane; 0 marks a pixel unread
                        if alpha == '256-a': vals = np.where(vals == 255, 0, 256 - vals)
                        elif alpha == '255-a': vals = 255 - vals
                        elif alpha == '2a': vals = np.where(vals == 255, 0, vals // 2)
                        if alpha != '255-a': ok = vals != 0
                    planes.append((f'ch {i}', cv, vals, 255, tally, ok))
                    if sc['num'] in (94, 96): break     # R = G = B = the fog plane
            if t.get('depth') and dep is not None:
                planes.append(('depth', [v['z'] for v in t['vtx']], dep[Yo, Xo], 65535, dtally, None))
            for label, cv, vals, top, tl, ok in planes:
                if min(cv) == max(cv): continue
                cw = [cv[s] for s in g.sub]
                vi = vals[inner]
                r = fit_plane(g, cw, Xi, Yi, vi, top) if len(Xi) >= 3 else None
                key = None; kt = ka
                if r is not None: key, lo, hi, Cs, fits = r
                h1ok = True; afit = []
                if ok is not None:                    # colour, alpha, fog: the arithmetic
                    h1ok, res = arith_fits(g, cw, kt, Xo, Yo, vals, top, ok)
                    ar['planes'] += 1; ar['H1'] += h1ok
                    for a, (n, v, full) in res.items():
                        if n:
                            ar['rivals'][a][0] += 1; ar['rivals'][a][4] += n
                            ar['rivals'][a][('rival', 'H1', 'neither').index(v) + 1] += 1
                        if full and not h1ok: afit.append(a)
                    for a in afit: ar['fits'][a] = ar['fits'].get(a, 0) + 1
                    n, nc, nw = clamp_wrap(g, cw, kt, Xo, Yo, vals, top, ok)
                    ar['clamp'][0] += n; ar['clamp'][1] += nc; ar['clamp'][2] += nw
                rs = residual_slope(g, cw, kt, Xi, Yi, vi, top) if len(Xi) >= 3 else None
                if ok is not None and rs is not None and rs[1]: ar['resid'].append((t['id'], label) + rs)
                if key is None: continue
                if key == 'empty' and ok is not None:
                    rf, rn = line_fits(g, cw, Xi, Yi, vi, top, 'row'); cf, cn = line_fits(g, cw, Xi, Yi, vi, top, 'col')
                    ar['lines'][0] += rf; ar['lines'][1] += rn; ar['lines'][2] += cf; ar['lines'][3] += cn
                if key in ('none', 'empty'):
                    alt = f"arith: {', '.join(afit)}" if afit else refit_pm1(g, cw, Xi, Yi, vi, top)
                    if alt: key = f'{key} ({alt})'
                tl[key] = tl.get(key, 0) + 1
                if verbose and (key != 'H1' or not h1ok):
                    gx, gy = grads(g.Vw, cw, g.rq, g.rsh, g.area)
                    c0 = Cs[kt]
                    so = start_offsets(g, cw, kt)
                    sub = f", copy (dx, dy) = {sub86(t['id'])}" if sc['num'] == 86 else ''
                    print(f"  tri {t['id']} {label}: {key} interval [{lo},{hi}) width {hi - lo}, C_H1 {Cs[g.kz]} "
                          f"offset {lo - Cs[g.kz]}..{hi - 1 - Cs[g.kz]}, fits {fits} (H1 corner {g.kz})")
                    print(f"      gx {gx} gy {gy} (1/16384 per 1/16 px), corner {kt} at {g.Vw[kt]}{sub}; start "
                          f"rivals' offsets from corner {kt}'s start (interval {lo - c0}..{hi - 1 - c0}): "
                          + ' '.join(f"{a} {o}{' in' if lo <= c0 + o < hi else ''}" for a, o in so.items()))
                    rf, rn = line_fits(g, cw, Xi, Yi, vi, top, 'row'); cf, cn = line_fits(g, cw, Xi, Yi, vi, top, 'col')
                    gw = grad_window(g, cw, Xi, Yi, vi, top) if key.startswith('empty') else None
                    print(f"      rows fit {rf}/{rn}, columns fit {cf}/{cn}"
                          + (f"; integer gradient offsets (dgx, dgy) that fit: {gw or 'none within 3'}" if gw is not None else '')
                          + (f"; residual vs distance from corner {kt}: {rs[1]} of {rs[0]} px off, missing gradient "
                             f"gx {fmt_g(rs[2])} gy {fmt_g(rs[3])}" if rs and rs[1] else '')
                          + (f"; H1's arithmetic {'reproduces' if h1ok else 'misses'} the plane"
                             + (f", these rivals reproduce it: {afit}" if afit else '') if ok is not None else ''))
        extra = f'  corner values from points: {ptsrc}, {ptdiff} of them differ from the mirror' if ptsrc else ''
        if not quiet:
            print(sc['num'], sc['name'], tally, f'depth {dtally}' if dtally else '', extra)
            print_arith(ar)
        out[sc['num']] = (tally, dtally, ptsrc, int(ptdiff), ar)
    if not quiet and len(out) > 1:
        print('all scenes:'); print_arith(merge_arith([r[4] for r in out.values()]))
    return out

def fmt_g(v): return '-' if v is None else f'{v:+.2f}'

def merge_arith(ars):
    """decode's per-scene arithmetic dicts summed (the 'all scenes' lines)."""
    t = dict(planes=0, H1=0, rivals={a: [0, 0, 0, 0, 0] for a in PLANE_ARITH}, clamp=[0, 0, 0],
             row0={}, row0px=[0, 0, 0], lines=[0, 0, 0, 0], resid=[], fits={}, scene=None)
    for a in ars:
        t['planes'] += a['planes']; t['H1'] += a['H1']; t['resid'] += a['resid']
        for k in ('clamp', 'row0px', 'lines'): t[k] = [x + y for x, y in zip(t[k], a[k])]
        for r, v in a['rivals'].items(): t['rivals'][r] = [x + y for x, y in zip(t['rivals'][r], v)]
        for k in ('row0', 'fits'):
            for r, v in a[k].items(): t[k][r] = t[k].get(r, 0) + v
    return t

def print_arith(ar):
    """decode's arithmetic lines for one scene (see decode): the verdict first (the rivals
    the dump follows on every plane they decide; none means H1 wherever any rival
    differs from it), then per rival the planes it decides by which side the dump takes."""
    if not ar['planes'] and not ar['row0']: return
    if ar['planes']:
        dec = {a: v for a, v in ar['rivals'].items() if v[0]}
        und = [a for a, v in ar['rivals'].items() if not v[0]]
        held = [f'{a} ({v[0]})' for a, v in dec.items() if v[1] == v[0]]
        print(f"   arith: H1 reproduces {ar['H1']} of {ar['planes']} planes; rivals the dump follows on every "
              f"plane they decide: {', '.join(held) or 'none'}")
        print(f"   arith: planes each rival decides, dump follows rival/H1/neither: "
              + ' '.join(f'{a} {v[1]}/{v[2]}/{v[3]}' for a, v in dec.items())
              + (f"; never differs from H1 here: {' '.join(und)}" if und else ''))
        if ar['fits']: print(f"   arith: rivals reproducing planes H1 misses: {ar['fits']}")
    if ar['clamp'][0]:
        n, nc, nw = ar['clamp']
        print(f"   clamp vs wrap: {n} px where H1's plane leaves the range: clamp {nc}, wrap {nw}, neither {n - nc - nw}")
    if ar['row0']:
        n, na, nb = ar['row0px']
        print(f"   row 0 (primary + secondary, no texture), per channel: {ar['row0']}; of the {n} px where a sum "
              f"of floored planes and a plane of summed corners differ, {na} read the sum, {nb} the plane of sums")
    if ar['lines'][1] or ar['lines'][3]:
        print(f"   empty fits, per line: rows fit {ar['lines'][0]}/{ar['lines'][1]} (gx), "
              f"columns {ar['lines'][2]}/{ar['lines'][3]} (gy)")
    if ar['resid'] and ar.get('scene') in (89, 90, 98):      # far from the anchor: the gradient bits
        print(f"   residual vs distance from the anchor, missing gradient (1/16384 per 1/16 px) per plane "
              f"off H1: " + ' '.join(f'{t}/{l[3:]}:{fmt_g(gx)},{fmt_g(gy)}' for t, l, _n, _o, gx, gy in ar['resid']))

def arith_selftest(res, a):
    """selftest: decode's arithmetic readouts on frames predicted under z+a (a None: H1)."""
    A = [r[4] for r in res.values()]
    tot = lambda f: {k: sum(f(x).get(k, 0) for x in A) for x in A for k in f(x)}
    riv = {b: [sum(x['rivals'][b][j] for x in A) for j in range(5)] for b in PLANE_ARITH}
    row0, fits = tot(lambda x: x['row0']), tot(lambda x: x['fits'])
    cl = [sum(x['clamp'][j] for x in A) for j in range(3)]
    planes, h1 = sum(x['planes'] for x in A), sum(x['H1'] for x in A)
    msg = f'H1 reproduces {h1} of {planes} planes'
    if a is None:
        good = h1 == planes and all(v[2] == v[0] for v in riv.values()) and set(row0) == {'sum of planes'} \
            and cl[0] > 0 and cl[1] == cl[0]
        msg += (f'; every rival\'s deciding planes follow H1: {all(v[2] == v[0] for v in riv.values())}'
                f'; 95 row 0 {row0}; clamp {cl[1]} of {cl[0]} out-of-range px')
    elif a == 'sumcorners':
        good = h1 == planes and set(row0) == {'plane of sums'}
        msg += f'; 95 row 0 {row0}'
    else:
        dec, won, lost, nei = riv[a][:4]
        good = h1 < planes and dec > 0 and won == dec and fits.get(a, 0) == planes - h1
        msg += (f'; {a} decides {dec} planes, the dump follows it on {won}, H1 on {lost}, neither on {nei}; '
                f'planes H1 misses reproduced by {fits}')
        if a == 'wrap':
            good &= cl[0] > 0 and cl[2] == cl[0]; msg += f'; wrap {cl[2]} of {cl[0]} out-of-range px'
    print(f'decode on the z+{a} prediction: ' if a else 'decode on the z prediction, arithmetic: ', msg,
          '' if good else '  <- WRONG')
    return good

def selftest(tmp):
    """The decoder against frames whose answer is known: H1's own prediction (every fit
    'H1' or 'H1+other'; every arithmetic rival's deciding planes follow H1, 95's row 0
    reads the sum of planes, out-of-range pixels the clamp), psprecomp's current rule
    (fits 'other' where its corner is not H1's), the prediction under each arithmetic
    rival z+ARITH (the rival decides some planes and the dump follows it on all of them;
    the planes H1 misses are the ones the rival reproduces; z+wrap reads the wrap,
    z+sumcorners 95's row 0 as a plane of summed corners), and scene 94 with 12
    triangles' corner fog bytes made one higher than the mirror computes (the vertex z
    moved by 1/256, triangle and point alike): with the points decode still finds H1;
    with the mirror's corner values those fits fail and the +-1 retry names them.
    Prints the tallies; returns True when all of them behave."""
    ok = True
    for rule in ('z', 'cur'):
        dd = os.path.join(tmp, rule); predict(dd, rule)
        res = decode(dd, quiet=True)
        bad = {n: {k: v for k, v in r[0].items() if k not in ('H1', 'H1+other')} for n, r in res.items()}
        bad = {n: v for n, v in bad.items() if v}
        dbad = {n: {k: v for k, v in r[1].items() if k not in ('H1', 'H1+other')} for n, r in res.items() if r[1]}
        print(f'decode on the {rule} prediction: fits other than H1 by scene: {bad or "none"}; depth: {dbad}')
        if rule == 'z': ok &= not bad and not any(dbad.values()) and arith_selftest(res, None)
    for a in ARITH:
        dd = os.path.join(tmp, 'z+' + a); predict(dd, 'z+' + a)
        ok &= arith_selftest(decode(dd, quiet=True), a)
    sc = scene(94); bump = set(range(0, 48, 4)); F = fog94()
    ops = []
    for o in sc['ops']:
        if o[0] == 'DRAW':           # the triangles, then the points: corner j of triangle i at 3i+j in both
            vv = [dict(v) for v in o[3]]
            for i in bump: vv[3 * i]['z'] = fog_z(F[i][0] + 1)
            o = o[:3] + (vv,) + o[4:]
        ops.append(o)
    m = Mirror(Opt('z')); m.run(dict(sc, ops=ops))
    dd = os.path.join(tmp, 'bump94'); os.makedirs(dd, exist_ok=True)
    m.F.px.astype(np.uint32).tofile(os.path.join(dd, 'ge_94_fogplane.raw'))
    for up in (True, False):
        t, _dt, ps, pd, _ar = decode(dd, [94], quiet=True, use_points=up)[94]
        print(f'94 with 12 corner bytes one higher, corner values from the {"points" if up else "mirror"}: {t}'
              + (f' ({pd} point corners differ from the mirror)' if up else ''))
        if up: ok &= set(t) <= {'H1', 'H1+other'} and pd == 12
        else: ok &= any('corners+-1' in k for k in t)
    return ok

def selfcheck(d):
    """Checks that need no model: twins must be equal pixel for pixel."""
    L = {n: load(d, dict(num=n, name=name)) for n, _b, name, _t in SCENES}
    ok = True
    def rep(what, n):
        nonlocal ok
        ok &= n == 0
        print(f'{what}: {n}')
    rgb = 0xFFFFFF
    if L[83] is not None:
        for n in (92, 93):
            if L[n] is not None: rep(f'{n} == 83 (rgb px differing)', int(((L[n] ^ L[83]) & rgb != 0).sum()))
    if L[84] is not None:
        f = L[84] & rgb
        for s_ in range(8):
            for mm in range(2):
                cx, cy = cell84(s_, 0, mm); ref = f[cy:cy + 33, cx:cx + 39]
                for o in range(1, 6):
                    cx, cy = cell84(s_, o, mm)
                    n = int((f[cy:cy + 33, cx:cx + 39] != ref).sum())
                    if n: rep(f'84 shape {s_} set {mm}: order {o} vs order 0', n)
        print('84: the 6 orders of each shape and set compared (only differences listed)')
    if L[87] is not None and L[97] is not None:
        rep('97 rgb == 87 rgb (depth test ALWAYS leaves colour alone)', int(((L[97] ^ L[87]) & rgb != 0).sum()))
    if L[88] is not None:
        f = L[88] & rgb
        for row in range(3):
            ref = f[68 * row:68 * row + 68, 0:60]
            for col in range(1, 8):
                n = int((f[68 * row:68 * row + 68, 60 * col:60 * col + 60] != ref).sum())
                if n: rep(f'88 row {row} column {col} vs column 0', n)
        print('88 rows 0-2: columns compared (only differences listed)')
        C = dict(zip('abc', cols88()[0])); C['d'] = COL88D
        inv = {v & rgb: k for k, v in C.items()}
        got, want = [], []
        for half, col, prim, mem, idx, prov in FLAT88:
            blk = f[204 + 34 * half:238 + 34 * half, 60 * col:60 * col + 60]
            vals = sorted(set(int(v) for v in np.unique(blk) if v))
            got.append('/'.join(sorted(inv.get(v, f'{v:06X}') for v in vals)) or '-')
            want.append(prov)
        print('88 flat tiles (lists abc bca cab acb cba bac, strip abc, strip aabc | strip aaabc, strip abcd, '
              'fan abc, fan abbc, fan abcd, indexed fan, indexed list, indexed strip):')
        print('   provoking corner(s) found:   ', ' '.join(got))
        print('   last vertex of each triangle:', ' '.join(want))
    if L[91] is not None and L[84] is not None:
        rep('91 rgb == 84 rgb', int(((L[91] ^ L[84]) & rgb != 0).sum()))
        a = (L[91] >> 24) & 255; r = L[84] & 255
        plain = np.zeros_like(a, bool)
        for s_ in range(8):
            for o in range(6):
                cx, cy = cell84(s_, o, 0); plain[cy:cy + 33, cx:cx + 39] = True
        sel = plain & ((L[84] & rgb) != 0)
        n = int(((a != r) & sel).sum())
        rep('91 alpha == red on the plain cells', n)
        if n:   # what else the alpha byte could be, each still decodable
            alts = {'256 - red (GEQUAL passing when ref >= a; 255 at red 0)': np.where(r == 0, 255, 256 - r),
                    'min(255, 2 red) (the first draw writing alpha)': np.minimum(255, 2 * r),
                    '255 - red': 255 - r}
            for k, v in alts.items(): print(f'   alpha == {k}: {int(((a != v) & sel).sum())} px off')
    for n in (94, 96):      # every corner point of one fog value reads one byte (96: one per w)
        if L[n] is None: continue
        f = L[n] & 0xFF
        sc_ = scene(n); m = render(sc_, 'z')
        groups = {}
        for ti, t in enumerate(m.tris):
            r_, c_ = ti // 8, ti % 8
            for j, v in enumerate(t['vtx']):
                key = v['fog'] if n == 94 else ws96()[ti][j]
                groups.setdefault(key, set()).add(int(f[45 * r_ + 1, 60 * c_ + 1 + 2 * j]))
        bad = sum(len(s) - 1 for s in groups.values())
        rep(f'{n}: corner points of one ' + ('fog value' if n == 94 else 'w') + ' reading more than one byte', bad)
        if n == 96: print('   96 points by w:', {k: sorted(v) for k, v in sorted(groups.items())},
                          '(eye-z fog and w fog both predict', {w: fog_of_w(w) for w in (1, 2, 4)}, ')')
    return ok

# =========================================================================== the log's CRCs
def sums(dumpdir):
    """Each scene's SUMS line in DUMPDIR/geprobe.txt against this file's streams: the
    draws, vertices, vertex CRC and stream CRC must all match before any pixel is read."""
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for sc, n, nd, nv, vc, crc in emit(os.path.join(td, 'c14.inc')):
            m = re.search(rf'scene {sc["num"]}: colour planes.*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want
            ok &= good
            print(f'scene {sc["num"]} {sc["name"]}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

# =========================================================================== main
def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'verify':
        if not MINER:
            print('verify needs COLOUR14_TRILOG_DIR: a folder whose run/tri.txt is the triangle log of an '
                  'instrumented psprecomp run (one T line per sw_tri call)'); return 2
        return 0 if verify(tuple(argv[2:]) or VERIFY_DEFAULT) else 1
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'emit':
        path = argv[2] if len(argv) > 2 else os.path.join(HERE, 'c14_data.inc')
        for sc, n, nd, nv, vc, crc in emit(path):
            print(f'scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'check':
        nums = [int(a) for a in argv[2:]] or None
        scs = [scene(n) for n in nums] if nums else None
        rep = design_checks(scs)
        print(json.dumps(rep, indent=1))
        return 0
    if cmd == 'predict': predict(argv[2], argv[3] if len(argv) > 3 else 'cur'); return 0
    if cmd == 'compare':
        rs = [a for a in argv[3:] if not a.isdigit()]
        compare(argv[2], [int(a) for a in argv[3:] if a.isdigit()] or None, rs or None); return 0
    if cmd == 'cmp': return 0 if cmp_dirs(argv[2], argv[3]) else 1
    if cmd == 'decode':
        opt = dict(a[2:].split('=', 1) for a in argv[3:] if a.startswith('--') and '=' in a)
        decode(argv[2], [int(a) for a in argv[3:] if not a.startswith('-')] or None, '-v' in argv,
               alpha=opt.get('alpha', 'a'), rule=opt.get('rule', 'z'))
        return 0
    if cmd == 'selfcheck': return 0 if selfcheck(argv[2]) else 1
    if cmd == 'selftest': return 0 if selftest(argv[2]) else 1
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
