#!/usr/bin/env python3
"""geprobe 22 (scene 134): what set 22 left open about tall long edges.

Set 22 (edges21.py) found that the far pixel of a long edge's group of four takes the
near one's decision only when its centre lies within the triangle's x extent, min x <=
centre < max x, and that of a level triangle's two full-height edges only the left one
does. Two things it could not tell: every level triangle came left corner first, so "the
left edge" and "the edge from the first corner given" fit alike; and no far pixel sat
exactly on min x (one sat on max x). Scene 134 asks both, in edges20's 220 windows:

  windows 0-143    level triangles (scene 129's flat_tri) in all six vertex orders, top
                   and bottom level, each edge read through its own windows
  windows 144-219  tall triangles whose long edge leans 2 to 7 pixels and whose extreme
                   corner (min x with the inside to the right, max x with it to the left)
                   sits d = -2 .. +2 sixteenths from the centre of a far pixel the window
                   holds, so the copy is decided by the extent's boundary: at d = 0 the
                   centre is exactly on it

The rules (rule_mask): gate 'ge' (min x <= centre < max x, psprecomp's), 'gt' (both
strict), 'le' (both inclusive); pick 'left' (psprecomp's), 'first' or 'last' (the edge
from the level corner given first or last), 'both', 'right'.

    edges22.py emit [OUT.inc]        write c22e_data.inc (colour14's stream format)
    edges22.py sums DUMPDIR          the logged CRCs against these streams
    edges22.py compare DUMPDIR       windows matching each rule, per case
    edges22.py check                 design checks: one edge a window, where rules part

Set 23 (fw 6.60): 'g9' with 'left' fits every window -- the copy when min x <= 16 x + 9
<= max x (the centre plus a sixteenth, both ends in: a centre one sixteenth short of min x
copies, one on max x does not), and the left edge whatever corner comes first. It fits
scenes 125-129's 1100 windows too.
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
import edges21 as E21
from colour14 import XS
from edges20 import window, E, crossing, exact_mask, draws, frame, WIN, COLS, ROWS, LIM

def P16(v): return int(round(v * 16))
ORD = [(0, 1, 2), (1, 2, 0), (2, 0, 1), (0, 2, 1), (2, 1, 0), (1, 0, 2)]
NFLAT = 144

def flat_items(r):
    """Windows 0-143: level triangles, (top/bottom, edge, order) cycling over 24 cases."""
    cases = [(tl, which, o) for tl in (True, False) for which in (0, 1) for o in range(6)]
    out = []
    for k in range(NFLAT):
        tl, which, o = cases[k % len(cases)]
        sc = window(k)
        for _ in range(400):
            H = 2800 + r() % 1100
            width = 400 + r() % 1600
            V = E21.flat_tri(sc, r, H, tl, 0, which, width)        # [left corner, right corner, apex]
            if any(abs(c) > LIM * 16 for p in V for c in p): continue
            if crossing(V, sc) != (1, True): continue
            out.append((sc, [V[i] for i in ORD[o]], dict(kind='flat', top=tl, which=which, order=o))); break
        else: raise RuntimeError(f'window {k}')
    return out

DS = (-2, -1, 0, 1, 2)
def extent_tri(sc, r, d, inside_right, extreme_bottom):
    """A tall triangle whose long edge's extreme corner (its min x with the inside to the
    right, max x with it to the left) lies d sixteenths inside of a far pixel's centre in
    the window -- centre - min x = d, or max x - centre = d -- and whose edge is 1 to 2.5
    pixels inside of that corner's x along the window's rows."""
    H = 2760 + r() % 440
    f = 0.35 + (r() % 250) / 1000                 # the window's fraction of the way from the corner
    u = 1.0 + (r() % 1500) / 1000                 # the edge's x at the window, from the corner, px
    L = u / f                                      # its lean
    xmid = sc[0] + 6 + r() % 6
    if inside_right:
        X0 = (xmid & ~3)                           # far pixel: the group's first
        ext = 16 * X0 + 8 - d                      # min x
        sgn = 1
    else:
        X0 = (xmid & ~3) + 3                       # far pixel: the group's last
        ext = 16 * X0 + 8 + d                      # max x
        sgn = -1
    yc = (sc[1] + WIN // 2) * 16 + 8 + (r() % 16) - 8
    yc_px = yc / 16
    if extreme_bottom:
        A = (ext, P16(yc_px + f * H))                                   # the corner, at the bottom
        B = (ext + sgn * P16(L), P16(yc_px - (1 - f) * H))              # the top
    else:
        A = (ext, P16(yc_px - f * H))
        B = (ext + sgn * P16(L), P16(yc_px + (1 - f) * H))
    cx = ext + sgn * P16(1200 + r() % 600)
    cy = P16(yc_px + ((r() % 600) - 300) / 1000 * H)
    return [A, B, (cx, cy)]

def extent_items(r):
    out = []
    cases = [(d, ir, eb) for ir in (True, False) for eb in (True, False) for d in DS]
    for k in range(NFLAT, COLS * ROWS):
        d, ir, eb = cases[(k - NFLAT) % len(cases)]
        sc = window(k)
        for _ in range(400):
            V = extent_tri(sc, r, d, ir, eb)
            if any(abs(c) > LIM * 16 for p in V for c in p): continue
            if crossing(V, sc) != (1, True): continue
            if not parts(sc, V): continue
            o = r() % 6
            out.append((sc, [V[i] for i in ORD[o]], dict(kind='extent', d=d, inside_right=ir, bottom=eb, order=o))); break
        else: raise RuntimeError(f'window {k}')
    return out

def parts(sc, V):
    """True when the window holds rows where the copy decides: the far pixel at the
    extreme corner exactly outside and its group's near pixel inside."""
    return int((rule_mask(sc, V, gate='open') != rule_mask(sc, V, gate='none')).sum()) >= 3

_items = None
def items(num=134):
    global _items
    if _items is None:
        r = XS(0x13400001)
        _items = flat_items(r) + extent_items(r)
    return [(sc, V) for sc, V, _ in _items]
def meta(): items(); return [m for _, _, m in _items]

SCENES = [(134, None, 'edgeorder', 'tall long edges: level corners in every order, far pixels on the extent')]
def scene(num):
    n, _, name, title = SCENES[0]
    if num != n: raise KeyError(num)
    return dict(num=n, name=name, title=title, ops=draws(items()))
def all_scenes(): return [scene(134)]

# =========================================================================== the rules
def rule_mask(sc, V, gate='ge', pick='left'):
    """sw_tri's long-edge rule with the extent gate's ends (gate) and the choice between two
    full-height edges (pick) as parameters; V in the order given."""
    order = list(V)
    a, b, c = V
    mnx = min(p[0] for p in V); mxx = max(p[0] for p in V)
    if E(c, a, b) < 0: b, c = c, b
    ys = [a[1], b[1], c[1]]; H = max(ys) - min(ys)
    def tl(dx, dy): return dy < 0 or (dy == 0 and dx > 0)
    edges = [(b, c), (c, a), (a, b)]
    lifted = []
    if 3 * H >= 2**17:
        for k, (s, t) in enumerate(edges):
            if abs(t[1] - s[1]) == H: lifted.append(k)
        if len(lifted) > 1 and pick != 'both':
            if pick in ('left', 'right'):
                want = -H if pick == 'left' else H
                lifted = [k for k in lifted if edges[k][1][1] - edges[k][0][1] == want]
            else:
                ytop, ybot = min(ys), max(ys)
                # the level corners: the two sharing the y that only one vertex lacks
                lvl = [p for p in order if sum(1 for q in order if q[1] == p[1]) == 2]
                corner = lvl[0] if pick == 'first' else lvl[-1]
                lifted = [k for k in lifted if corner in edges[k]]
    def gated(cx):
        if gate == 'g9': return mnx <= cx + 1 <= mxx        # set 23: the centre plus a sixteenth, both ends in
        if gate == 'ge': return mnx <= cx < mxx
        if gate == 'gt': return mnx < cx < mxx
        if gate == 'le': return mnx <= cx <= mxx
        if gate == 'open': return True
        if gate == 'none': return False
        raise ValueError(gate)
    m = np.zeros((WIN, WIN), bool)
    for yy in range(WIN):
        for xx in range(WIN):
            x, y = sc[0] + xx, sc[1] + yy
            ok = True
            for k, (s, t) in enumerate(edges):
                dx, dy = t[0] - s[0], t[1] - s[1]
                px = x
                if k in lifted:
                    blk = x & ~3; near, far = (blk, blk + 3) if dy > 0 else (blk + 3, blk)
                    if x == far and gated(far * 16 + 8): px = near
                e = E((px * 16 + 8, y * 16 + 8), s, t)
                if not (e > 0 or (e == 0 and tl(dx, dy))): ok = False; break
            m[yy, xx] = ok
    return m

RULES = [('g9', 'left'), ('ge', 'left'), ('ge', 'first'), ('ge', 'last'), ('ge', 'right'), ('ge', 'both'),
         ('gt', 'left'), ('le', 'left')]

# =========================================================================== emit, sums, compare
def emit(path):
    out = ['/* Generated by edges22.py emit: geprobe 22 scene 134 (tall long edges) as a',
           ' * command stream for c14_run() (colour14.py\'s format).  Do not edit; change',
           ' * edges22.py and run it again. */']
    steps = []
    for k, sc in enumerate(all_scenes()):
        w, nd, nv, vc = C.emit_scene(sc)
        crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
        steps.append((sc, len(w), nd, nv, vc, crc))
        out.append(f'\n/* scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
                   f' {len(w)} words, stream crc {crc:08X} */')
        out.append(f'static const w32 C22E_S{k}[{len(w)}] = {{')
        for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
        out.append('};')
    out.append('\nstatic const struct c14_step C22E_STEPS[] = {')
    for k, (sc, n, *_r) in enumerate(steps):
        out.append(f'    {{ {sc["num"]}, "{sc["name"]}", "{sc["title"]}", C22E_S{k}, {n} }},')
    out.append('};\n#define C22E_NSTEPS ((int)(sizeof C22E_STEPS / sizeof C22E_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text: open(path, 'w').write(text)
    return steps

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for sc, n, nd, nv, vc, crc in emit(os.path.join(td, 'c22e.inc')):
            m = re.search(rf'scene {sc["num"]}: .*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want; ok &= good
            print(f'scene {sc["num"]} {sc["name"]}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

def case_key(m):
    if m['kind'] == 'flat':
        return ('flat', 'top' if m['top'] else 'bottom', 'edge %d' % m['which'], 'order %s' % (ORD[m['order']],))
    return ('extent', 'inside right' if m['inside_right'] else 'inside left',
            'corner at bottom' if m['bottom'] else 'corner at top', 'd %+d' % m['d'])

def compare(dumpdir, verbose=False):
    F = frame(dumpdir, 134, 'edgeorder')
    by = {}
    for (sc, V), m in zip(items(), meta()):
        hw = F[sc[1]:sc[3] + 1, sc[0]:sc[2] + 1] != 0
        st = by.setdefault(case_key(m), {'n': 0, **{r: 0 for r in RULES}, 'exact': 0})
        st['n'] += 1
        st['exact'] += (exact_mask(sc, V) == hw).all()
        for rule in RULES: st[rule] += (rule_mask(sc, V, *rule) == hw).all()
    tot = {r: 0 for r in RULES}; n = 0
    for key, st in sorted(by.items()):
        n += st['n']
        for r in RULES: tot[r] += st[r]
        if verbose: print('  ', key, st['n'], 'exact', st['exact'], ' '.join(f'{g}/{p} {st[(g, p)]}' for g, p in RULES))
    print(f'134 edgeorder: {n} windows; matching ' + ', '.join(f'{g}/{p} {tot[(g, p)]}' for g, p in RULES))

def check():
    its = items(); ms = meta()
    bad = sum(1 for sc, V in its if crossing(V, sc) != (1, True))
    far = max(max(abs(c) for p in V for c in p) for sc, V in its) / 16
    print(f'134: {len(its)} windows, {bad} crossed by more than one edge, farthest corner {far:.0f} px')
    base = [rule_mask(sc, V) for sc, V in its]
    for rule in RULES[1:]:
        diff = sum(1 for (sc, V), b in zip(its, base) if (rule_mask(sc, V, *rule) != b).any())
        print(f'   {rule[0]}/{rule[1]} parts from ge/left in {diff} windows')
    ext = [(sc, V, m) for (sc, V), m in zip(its, ms) if m['kind'] == 'extent']
    for d in DS:
        sel = [(sc, V) for sc, V, m in ext if m['d'] == d]
        moved = sum(int((rule_mask(sc, V, 'open') != rule_mask(sc, V, 'none')).sum()) for sc, V in sel)
        bnd = sum(int((rule_mask(sc, V, 'le') != rule_mask(sc, V, 'gt')).sum()) for sc, V in sel)
        print(f'   extent d {d:+d}: {len(sel)} windows, {moved} pixels the copy decides, {bnd} the boundary\'s ends')

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        for sc, n, nd, nv, vc, crc in emit(argv[2] if len(argv) > 2 else os.path.join(HERE, 'c22e_data.inc')):
            print(f'scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'compare': compare(argv[2], '-v' in argv); return 0
    if cmd == 'check': check(); return 0
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
