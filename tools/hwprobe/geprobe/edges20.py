#!/usr/bin/env python3
"""geprobe 20 (scenes 125-127): where a long edge's pixels fall.

Scene 98's window 1 is the last triangle psprecomp covers differently from the PSP: a
through-mode triangle with corners 1500-1700 pixels outside its 120 x 136 scissor window.
Along its long edge, a right boundary stepping 47303/16 pixels down for 53600/16 across,
the PSP adds one pixel a row, and always the last of an aligned group of four (x = 3 mod
4): the one in the group the edge crosses, whenever that group's first pixel is inside.
The pixels between follow the exact edge. The same edge mirrored, as window 0's left
boundary, and every ordinary triangle so far are exact. One edge cannot say what sets it
off: the edge's steps (its x and y extents), how far the corners are, which side of the
triangle it bounds, its slope, or where the groups of four start. These scenes ask.

Each scene is 220 small windows (20 x 20 pixels, scissored, the window's corner 0-3
pixels into its 24-pixel cell, so x and y take every alignment mod 4), each crossed by
exactly one edge of one flat white triangle on black: the edge through a random
sub-pixel point of the window, the third corner far on the inside so that no other edge
reaches it. Coverage reads straight off the frame.
  125 edgerand    any direction, edges 32 to 3900 pixels long, third corner 100-2000 out
  126 edgelong    edges 2000 to 3900 pixels long only, any direction
  127 edgeninety  scene 98's window 1 triangle itself at all 16 alignments, in all six
                  vertex orders, and mirrored (its long edge a left boundary), and the
                  same with the triangle shifted a sixteenth at a time

    edges20.py emit [OUT.inc]        write c20_data.inc (colour14's stream format)
    edges20.py sums DUMPDIR          the logged CRCs against these streams
    edges20.py compare DUMPDIR       per scene: windows and pixels off psprecomp's exact
                                     coverage, and how the extra or missing pixels sit
    edges20.py check                 design checks: every window crossed by one edge only
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
from colour14 import XS, GU_TRIANGLES, FMT_CV2D, GU_FLAT, GU_SMOOTH, begin, cv2d

CELL, WIN = 24, 20
COLS, ROWS = 20, 11
WHITE = 0xFFFFFFFF
LIM = 2000.0                  # every corner within +-2000 pixels, as scene 98's

def window(k):
    """Window k's scissor (x0, y0, x1, y1): its cell, shifted (k mod 4, k / 4 mod 4)."""
    i, j = k % COLS, k // COLS
    ax, ay = k % 4, (k // 4) % 4
    x0, y0 = CELL * i + ax, CELL * j + ay
    return (x0, y0, x0 + WIN - 1, y0 + WIN - 1)

def E(p, a, b):
    """Edge function of a->b at p (1/16 units), positive to the left of a->b (y down)."""
    return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])

def only_edge(V, sc):
    """True when, of V's three edges, only V[0]->V[1] changes sign over the window's
    pixel centres (the other two keep every centre on the triangle's side)."""
    a, b, c = V
    area = E(c, a, b)
    sgn = 1 if area > 0 else -1
    xs = [sc[0] * 16 + 8, sc[2] * 16 + 8]; ys = [sc[1] * 16 + 8, sc[3] * 16 + 8]
    corners = [(x, y) for x in xs for y in ys]
    for (p, q) in ((b, c), (c, a)):
        vals = [sgn * E(r, p, q) for r in corners]
        if min(vals) <= 0: return False
    vals = [sgn * E(r, a, b) for r in corners]
    return min(vals) < 0 < max(vals)

def crossing(V, sc):
    """How many of V's edges change sign over the window's pixel centres, and whether the
    window lies wholly on the triangle's side of every other edge."""
    a, b, c = V
    sgn = 1 if E(c, a, b) > 0 else -1
    xs = [sc[0] * 16 + 8, sc[2] * 16 + 8]; ys = [sc[1] * 16 + 8, sc[3] * 16 + 8]
    corners = [(x, y) for x in xs for y in ys]
    n = 0; inside = True
    for (p, q) in ((a, b), (b, c), (c, a)):
        vals = [sgn * E(r, p, q) for r in corners]
        if min(vals) < 0 < max(vals): n += 1
        elif max(vals) <= 0: inside = False
    return n, inside

def P16(v): return int(round(v * 16))

def make_tri(r, sc, Lmin, Lmax, cmin=100.0, cmax=2000.0):
    """One triangle whose edge V[0]->V[1] alone crosses window sc."""
    for _ in range(1000):
        qx = sc[0] + 2 + (r() % ((WIN - 4) * 16)) / 16
        qy = sc[1] + 2 + (r() % ((WIN - 4) * 16)) / 16
        th = (r() % 3600000) / 3600000 * 2 * math.pi
        ux, uy = math.cos(th), math.sin(th)
        L = Lmin * (Lmax / Lmin) ** ((r() % 10000) / 10000)
        f = (r() % 1000) / 1000
        A = (qx - ux * L * f, qy - uy * L * f); B = (qx + ux * L * (1 - f), qy + uy * L * (1 - f))
        side = 1 if r() & 1 else -1
        dc = cmin * (cmax / cmin) ** ((r() % 10000) / 10000)
        g = (r() % 1000) / 1000
        Cp = (A[0] + (B[0] - A[0]) * g - side * uy * dc, A[1] + (B[1] - A[1]) * g + side * ux * dc)
        V = [A, B, Cp]
        if any(abs(c) > LIM for p in V for c in p): continue
        V16 = [(P16(x), P16(y)) for x, y in V]
        if E(V16[2], V16[0], V16[1]) == 0 or not only_edge(V16, sc): continue
        return V16
    raise RuntimeError('no triangle for window %s' % (sc,))

def draws(items):
    """Ops for a list of (scissor, three 1/16 corners) drawn flat white."""
    ops = [begin(), ('SHADE', GU_FLAT)]
    for sc, V in items:
        ops.append(('SCISSOR',) + sc)
        ops.append(('DRAW', GU_TRIANGLES, FMT_CV2D, [cv2d(x, y, WHITE) for x, y in V], None))
    return ops + [('SCISSOR', 0, 0, 479, 271), ('SHADE', GU_SMOOTH), ('END', 0)]

def items125():
    r = XS(0x12500001)
    return [(window(k), make_tri(r, window(k), 32.0, 3900.0)) for k in range(COLS * ROWS)]
def items126():
    r = XS(0x12600001)
    return [(window(k), make_tri(r, window(k), 2000.0, 3900.0)) for k in range(COLS * ROWS)]

def items127():
    """Scene 98's window 1 triangle, its long edge (corners 0 and 2) put through each
    window's middle at the window's alignment, in each vertex order, mirrored or not, and
    walked through the pixel a few sixteenths at a time; each window takes the first
    variant that keeps every corner within +-2000 pixels."""
    (V98, _c, sc98) = C.far98()[1]
    cx98, cy98 = (sc98[0] + 60) * 16 + 3, (sc98[1] + 68) * 16 + 5
    rel = [(x - cx98, y - cy98) for x, y in V98]
    orders = [(0, 1, 2), (1, 2, 0), (2, 0, 1), (0, 2, 1), (2, 1, 0), (1, 0, 2)]
    out = []
    for k in range(COLS * ROWS):
        sc = window(k)
        wx, wy = (sc[0] + WIN / 2) * 16, (sc[1] + WIN / 2) * 16
        for attempt in range(12):
            mirror = (k // 6 + attempt) % 2
            o = orders[(k + attempt // 2) % 6]
            R = [(-x, y) for x, y in rel] if mirror else rel
            a, b = R[0], R[2]
            ex, ey = b[0] - a[0], b[1] - a[1]
            t = -(a[0] * ex + a[1] * ey) / (ex * ex + ey * ey)
            px, py = a[0] + ex * t, a[1] + ey * t
            sx = int(round(wx - px)) + (k * 3) % 16
            sy = int(round(wy - py)) + (k * 5) % 16
            V0 = [(R[i][0] + sx, R[i][1] + sy) for i in o]
            # slide along the long edge (keeps it through the window) to bring the corners in
            L = math.hypot(ex, ey); ux, uy = ex / L, ey / L
            best = None
            for st in range(-160, 161):
                d = st * 16 * 8                                  # 8-pixel steps, whole sixteenths
                V = [(x + int(round(ux * d)), y + int(round(uy * d))) for x, y in V0]
                far = max(abs(c) for p in V for c in p)
                if best is None or far < best[0]: best = (far, V)
            V = best[1]
            if best[0] > LIM * 16: continue
            if crossing(V, sc) != (1, True): continue
            out.append((sc, V)); break
        else:
            raise RuntimeError(f'window {k}: no variant fits')
    return out

SCENES = [
    (125, items125, 'edgerand', 'long edges: any direction, 32 to 3900 pixels, one edge a window'),
    (126, items126, 'edgelong', 'long edges: 2000 to 3900 pixels, any direction'),
    (127, items127, 'edgeninety', "long edges: scene 98's window 1 at every alignment, order and mirrored"),
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

# =========================================================================== emit, sums
def emit(path):
    out = ['/* Generated by edges20.py emit: geprobe 20 scenes 125-127 (long edges) as command',
           ' * streams for c14_run() (colour14.py\'s format).  Do not edit; change edges20.py and',
           ' * run it again. */']
    steps = []
    for k, sc in enumerate(all_scenes()):
        w, nd, nv, vc = C.emit_scene(sc)
        crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
        steps.append((sc, len(w), nd, nv, vc, crc))
        out.append(f'\n/* scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
                   f' {len(w)} words, stream crc {crc:08X} */')
        out.append(f'static const w32 C20_S{k}[{len(w)}] = {{')
        for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
        out.append('};')
    out.append('\nstatic const struct c14_step C20_STEPS[] = {')
    for k, (sc, n, *_r) in enumerate(steps):
        out.append(f'    {{ {sc["num"]}, "{sc["name"]}", "{sc["title"]}", C20_S{k}, {n} }},')
    out.append('};\n#define C20_NSTEPS ((int)(sizeof C20_STEPS / sizeof C20_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text: open(path, 'w').write(text)
    return steps

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for sc, n, nd, nv, vc, crc in emit(os.path.join(td, 'c20.inc')):
            m = re.search(rf'scene {sc["num"]}: long edges.*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want; ok &= good
            print(f'scene {sc["num"]} {sc["name"]}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

# =========================================================================== exact coverage, reading a dump
def exact_mask(sc, V):
    """psprecomp's coverage (render.c sw_tri: exact edge functions at pixel centres, the
    top-left rule for ties) over window sc."""
    a, b, c = V
    if E(c, a, b) < 0: b, c = c, b
    def tl(dx, dy): return dy < 0 or (dy == 0 and dx > 0)
    edges = [(b, c), (c, a), (a, b)]
    m = np.zeros((WIN, WIN), bool)
    for yy in range(WIN):
        for xx in range(WIN):
            p = ((sc[0] + xx) * 16 + 8, (sc[1] + yy) * 16 + 8)
            ok = True
            for (s, t) in edges:
                e = E(p, s, t)
                dx, dy = t[0] - s[0], t[1] - s[1]
                if not (e > 0 or (e == 0 and tl(dx, dy))): ok = False; break
            m[yy, xx] = ok
    return m

def frame(dumpdir, num, name):
    return np.fromfile(os.path.join(dumpdir, f'ge_{num}_{name}.raw'), '<u4').reshape(272, 480) & 0xFFFFFF

def compare(dumpdir, verbose=False):
    for n, fn, name, title in SCENES:
        F = frame(dumpdir, n, name)
        bad_w = 0; extra = 0; miss = 0; xmod = np.zeros(4, int); ymod = np.zeros(4, int)
        rows = []
        for k, (sc, V) in enumerate(items(n)):
            hw = F[sc[1]:sc[3] + 1, sc[0]:sc[2] + 1] != 0
            ex = exact_mask(sc, V)
            d_ex = hw & ~ex; d_mi = ex & ~hw
            if d_ex.any() or d_mi.any():
                bad_w += 1; extra += int(d_ex.sum()); miss += int(d_mi.sum())
                for yy, xx in np.argwhere(d_ex | d_mi):
                    xmod[(sc[0] + xx) % 4] += 1; ymod[(sc[1] + yy) % 4] += 1
                a, b = V[0], V[1]
                rows.append((k, sc, int(d_ex.sum()), int(d_mi.sum()), (b[0] - a[0]) / 16, (b[1] - a[1]) / 16))
        print(f'{n} {name}: {len(items(n))} windows, {bad_w} off psprecomp\'s exact coverage '
              f'({extra} extra pixels, {miss} missing); x mod 4 of those {[int(v) for v in xmod]}, y mod 4 {[int(v) for v in ymod]}')
        if verbose:
            for r_ in rows[:40]: print('   window %d %s: +%d -%d, edge dx %.1f dy %.1f' % r_)

def check():
    for n, fn, name, title in SCENES:
        its = items(n)
        bad = sum(1 for sc, V in its if crossing(V, sc) != (1, True))
        lens = [math.hypot((V[1][0] - V[0][0]) / 16, (V[1][1] - V[0][1]) / 16) for sc, V in its]
        far = max(max(abs(c) for p in V for c in p) for sc, V in its) / 16
        px = sum(int(exact_mask(sc, V).sum()) for sc, V in its)
        print(f'{n} {name}: {len(its)} windows, {bad} with more than the one edge crossing; edge length '
              f'{min(lens):.0f}-{max(lens):.0f} px; farthest corner {far:.0f} px; {px} pixels covered')

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        for sc, n, nd, nv, vc, crc in emit(argv[2] if len(argv) > 2 else os.path.join(HERE, 'c20_data.inc')):
            print(f'scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'compare': compare(argv[2], '-v' in argv); return 0
    if cmd == 'check': check(); return 0
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
