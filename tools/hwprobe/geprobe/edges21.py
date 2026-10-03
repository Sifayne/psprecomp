#!/usr/bin/env python3
"""geprobe 21 (scenes 128-129): the two things set 21 left open about tall long edges.

Set 21 (edges20.py) found that along a triangle's long edge (top corner to bottom
corner), once 3 x the height in sixteenths reaches 2^17, the pixel of each aligned group
of four farthest from the inside takes the nearest's decision. Every slanted long edge it
posed leaned 185 pixels or more, and scene 98's window 4, a vertical one, is exact, so
between the two is unmeasured; and a triangle level at its top or bottom has two edges of
full height, which no window posed. render.c sw_tri gives the rule to every long edge with
any lean and to both full-height edges. These scenes ask:

  128 edgelean   tall triangles (2800-3900 pixels, and a few at 2700-2760 either side of
                 the threshold) whose long edge leans 0, 1/16, 1/8 ... 184 pixels either
                 way, the inside on either side, read through 20 x 20 windows at every
                 alignment mod 4
  129 edgeflat   tall triangles level at the top or the bottom (two full-height edges),
                 each edge read through its own windows; and near-level ones, the two top
                 (or bottom) corners 1/16, 1/2, 1 and 4 pixels apart, to say which counts

    edges21.py emit [OUT.inc]        write c21_data.inc (colour14's stream format)
    edges21.py sums DUMPDIR          the logged CRCs against these streams
    edges21.py compare DUMPDIR [-v]  windows off exact coverage and off psprecomp's rule,
                                     per lean and per case
    edges21.py check                 design checks
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
import edges20 as E20
from colour14 import XS
from edges20 import window, E, crossing, exact_mask, draws, frame, WIN, COLS, ROWS, LIM

def P16(v): return int(round(v * 16))

LEANS = [0, 1 / 16, 2 / 16, 4 / 16, 8 / 16, 1, 1.5, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 160, 184]

def tall(sc, r, H, lean, inside_left, top_frac):
    """A triangle whose long edge A (top) -> B (bottom) is H tall, leans `lean` pixels
    (signed) and crosses window sc's middle at fraction top_frac of its height; the third
    corner 1200-1800 pixels to the inside, level with a random height in between."""
    wx, wy = sc[0] + WIN / 2 + (r() % 64) / 64 - 0.5, sc[1] + WIN / 2 + (r() % 64) / 64 - 0.5
    ytop = wy - H * top_frac; ybot = ytop + H
    xtop = wx - lean * top_frac; xbot = xtop + lean
    side = -1 if inside_left else 1
    cx = wx + side * (1200 + r() % 600); cy = ytop + H * (0.2 + (r() % 600) / 1000)
    return [(P16(xtop), P16(ytop)), (P16(xbot), P16(ybot)), (P16(cx), P16(cy))]

def items128():
    r = XS(0x12800001)
    out = []
    combos = [(lean * sg, il) for lean in LEANS for sg in ((1,) if lean == 0 else (1, -1)) for il in (True, False)]
    k = 0
    while k < COLS * ROWS:
        lean, il = combos[k % len(combos)]
        sc = window(k)
        for _ in range(200):
            if k % 10 == 9: H = 2700 + (r() % 61)             # either side of 2730.67
            else: H = 2800 + r() % 1100
            V = tall(sc, r, H, lean, il, 0.25 + (r() % 500) / 1000)
            if any(abs(c) > LIM * 16 for p in V for c in p): continue
            if crossing(V, sc) != (1, True): continue
            out.append((sc, V)); break
        else: raise RuntimeError(f'window {k}')
        k += 1
    return out

def flat_tri(sc, r, H, top_level, gap, which, width):
    """Level at the top (top_level) or the bottom: two corners `width` pixels apart in x and
    `gap` sixteenths apart in y, the apex H away and up to 600 pixels to either side. The
    window sits on edge `which` (0: from the left level corner to the apex, 1: from the
    right), a fraction 0.3-0.7 of the way along."""
    wx, wy = sc[0] + WIN / 2 + (r() % 64) / 64 - 0.5, sc[1] + WIN / 2 + (r() % 64) / 64 - 0.5
    f = 0.3 + (r() % 400) / 1000                    # how far from the level corners the window is
    lean = (r() % 1200) - 600
    yl = wy - H * f if top_level else wy + H * f    # the level corners' y
    ya = yl + H if top_level else yl - H            # the apex's
    ex = width / 2
    # edge from the chosen level corner (cx -+ ex, yl) to the apex (cx + lean, ya) through (wx, wy)
    cx = wx - f * lean - (1 - f) * (-ex if which == 0 else ex)
    return [(P16(cx - ex), P16(yl)), (P16(cx + ex), P16(yl + gap / 16)), (P16(cx + lean), P16(ya))]

def items129():
    r = XS(0x12900001)
    out = []
    cases = [(tl, gap, which) for tl in (True, False) for gap in (0, 0, 0, 0, 1, -1, 8, -8, 16, -16, 64, -64)
             for which in (0, 1)]
    k = 0
    while k < COLS * ROWS:
        tl, gap, which = cases[k % len(cases)]
        sc = window(k)
        for _ in range(400):
            H = 2800 + r() % 1100
            width = 400 + r() % 1600
            V = flat_tri(sc, r, H, tl, gap, which, width)
            if any(abs(c) > LIM * 16 for p in V for c in p): continue
            if crossing(V, sc) != (1, True): continue
            out.append((sc, V)); break
        else: raise RuntimeError(f'window {k}')
        k += 1
    return out

SCENES = [
    (128, items128, 'edgelean', 'tall long edges: leans 0 to 184 pixels'),
    (129, items129, 'edgeflat', 'tall long edges: level tops and bottoms, two full-height edges'),
]
_cache = {}
def items(num):
    if num not in _cache:
        for n, fn, name, title in SCENES:
            if n == num: _cache[num] = fn()
    return _cache[num]
def scene(num):
    for n, fn, name, title in SCENES:
        if n == num: return dict(num=n, name=name, title=title, ops=draws(items(n)))
    raise KeyError(num)
def all_scenes(): return [scene(n) for n, *_ in SCENES]

# =========================================================================== the rules
def rule_mask(sc, V, lean_min=1, both=True, bbox=True):
    """psprecomp's sw_tri rule (lean_min = smallest |dx| in sixteenths that takes it;
    both: two full-height edges both take it; bbox: no pixel outside the triangle's
    bounding box, floor(min/16) .. floor((max + 15)/16), which sw_tri never visits --
    a far pixel can lie there when the long edge leans less than a pixel)."""
    a, b, c = V
    bx0 = min(p[0] for p in V) >> 4; bx1 = (max(p[0] for p in V) + 15) >> 4
    if E(c, a, b) < 0: b, c = c, b
    ys = [a[1], b[1], c[1]]; H = max(ys) - min(ys)
    def tl(dx, dy): return dy < 0 or (dy == 0 and dx > 0)
    edges = [(b, c), (c, a), (a, b)]
    lifted = []
    if 3 * H >= 2**17:
        for k, (s, t) in enumerate(edges):
            dx, dy = t[0] - s[0], t[1] - s[1]
            if abs(dy) == H and abs(dx) >= lean_min: lifted.append(k)
        if not both and len(lifted) > 1: lifted = lifted[:1]
    m = np.zeros((WIN, WIN), bool)
    for yy in range(WIN):
        for xx in range(WIN):
            x, y = sc[0] + xx, sc[1] + yy
            ok = not bbox or bx0 <= x <= bx1
            for k, (s, t) in enumerate(edges):
                if not ok: break
                dx, dy = t[0] - s[0], t[1] - s[1]
                px = x
                if k in lifted:
                    blk = x & ~3; near, far = (blk, blk + 3) if dy > 0 else (blk + 3, blk)
                    if x == far: px = near
                e = E((px * 16 + 8, y * 16 + 8), s, t)
                if not (e > 0 or (e == 0 and tl(dx, dy))): ok = False; break
            m[yy, xx] = ok
    return m

# =========================================================================== emit, sums, compare
def emit(path):
    out = ['/* Generated by edges21.py emit: geprobe 21 scenes 128-129 (tall long edges) as',
           ' * command streams for c14_run() (colour14.py\'s format).  Do not edit; change',
           ' * edges21.py and run it again. */']
    steps = []
    for k, sc in enumerate(all_scenes()):
        w, nd, nv, vc = C.emit_scene(sc)
        crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
        steps.append((sc, len(w), nd, nv, vc, crc))
        out.append(f'\n/* scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
                   f' {len(w)} words, stream crc {crc:08X} */')
        out.append(f'static const w32 C21_S{k}[{len(w)}] = {{')
        for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
        out.append('};')
    out.append('\nstatic const struct c14_step C21_STEPS[] = {')
    for k, (sc, n, *_r) in enumerate(steps):
        out.append(f'    {{ {sc["num"]}, "{sc["name"]}", "{sc["title"]}", C21_S{k}, {n} }},')
    out.append('};\n#define C21_NSTEPS ((int)(sizeof C21_STEPS / sizeof C21_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text: open(path, 'w').write(text)
    return steps

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for sc, n, nd, nv, vc, crc in emit(os.path.join(td, 'c21.inc')):
            m = re.search(rf'scene {sc["num"]}: tall long edges.*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want; ok &= good
            print(f'scene {sc["num"]} {sc["name"]}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

def lean_of(V):
    """(height, the long edge's |dx| in sixteenths, how many edges are full height)."""
    ys = [p[1] for p in V]; H = max(ys) - min(ys)
    full = [(V[i], V[(i + 1) % 3]) for i in range(3) if abs(V[(i + 1) % 3][1] - V[i][1]) == H]
    return H, min(abs(t[0] - s[0]) for s, t in full), len(full)

def compare(dumpdir, verbose=False):
    for n, fn, name, title in SCENES:
        F = frame(dumpdir, n, name)
        by = {}
        for k, (sc, V) in enumerate(items(n)):
            hw = F[sc[1]:sc[3] + 1, sc[0]:sc[2] + 1] != 0
            H, ldx, nfull = lean_of(V)
            key = (round(ldx / 16, 4), nfull) if n == 128 else (nfull, 'top' if k % 48 < 24 else 'bottom', 'edge %d' % (k % 2))
            st = by.setdefault(key, {'n': 0, 'exact': 0, 'rule': 0, 'nobbox': 0, 'lean0': 0, 'one': 0, 'extra': 0, 'H': []})
            st['n'] += 1; st['H'].append(H / 16)
            st['exact'] += (exact_mask(sc, V) == hw).all()
            st['rule'] += (rule_mask(sc, V) == hw).all()
            st['nobbox'] += (rule_mask(sc, V, bbox=False) == hw).all()
            st['lean0'] += (rule_mask(sc, V, lean_min=0) == hw).all()
            st['one'] += (rule_mask(sc, V, both=False) == hw).all()
            st['extra'] += int((hw & ~exact_mask(sc, V)).sum())
        print(f'{n} {name}:')
        for key, st in sorted(by.items()):
            print(f'   {key}: {st["n"]} windows, matching exact {st["exact"]}, psprecomp\'s rule {st["rule"]} '
                  f'(unclipped {st["nobbox"]}, vertical too {st["lean0"]}, one full edge only {st["one"]}), '
                  f'extra pixels {st["extra"]}, heights {min(st["H"]):.0f}-{max(st["H"]):.0f}')

def check():
    for n, fn, name, title in SCENES:
        its = items(n)
        bad = sum(1 for sc, V in its if crossing(V, sc) != (1, True))
        far = max(max(abs(c) for p in V for c in p) for sc, V in its) / 16
        hs = [lean_of(V)[0] / 16 for sc, V in its]
        nf = sum(1 for sc, V in its if lean_of(V)[2] == 2)
        diff = sum(1 for sc, V in its if (exact_mask(sc, V) != rule_mask(sc, V)).any())
        print(f'{n} {name}: {len(its)} windows, {bad} crossed by more than one edge, farthest corner {far:.0f} px, '
              f'heights {min(hs):.0f}-{max(hs):.0f}, {nf} with two full-height edges, {diff} where the rule changes coverage')

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        for sc, n, nd, nv, vc, crc in emit(argv[2] if len(argv) > 2 else os.path.join(HERE, 'c21_data.inc')):
            print(f'scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'compare': compare(argv[2], '-v' in argv); return 0
    if cmd == 'check': check(); return 0
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
