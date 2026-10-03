#!/usr/bin/env python3
"""geprobe 22 (scene 138): skinned and morphed normals, read through lighting.

Set 17 (skin16.py) read skinned and morphed positions whole: one accumulator (ge_acc),
skinning bone by bone over the translation, x, y and z terms, morphing set by set before
skinning. Normals were never read: psprecomp still skins them in float32 through the
bones' 3x3 parts (ge.c skin_mvert) and morphs them in float32 (read_mvert). Lighting is
settled (lights19.py rule 'ge'), so a normal's direction reads off a diffuse byte -- and
through bones whose entries are large and cancel on the normal, as scene 122 did through
world matrices, the 16-bit cuts move the direction by many codes. Every point goes to
clip (0, 0, 0, 1) through a zero projection and its offset places it, as in 115-124;
world and view are identity, light 0 directional and white, diffuse only.

Batches (BATCHES):
  one       one bone R1 diag(2^m, 2^-m, 1) R2, m 4..7, the normal on R2's shrunk axis;
            weights 1 and others (float)
  pair      two bones B and -B + 2^-m E, weights 1/2 and 1/2 or 0.3/0.7: the terms cancel
            between bones
  four      four bones, two cancelling pairs
  u8, u16   one or two bones, weights 8 and 16 bits
  trans     bones with a translation 2^2..2^6 and an ordinary normal: does the translation
            reach the normal (as it would were the normal skinned as a position)?
  morph     two to four morph sets whose normals cancel, no bones
  mskin     two morph sets of normal and weights, then one or two bones

The rules (normal_of), each followed by the settled lighting:
  cur     psprecomp: float32, sum_i w_i (B_i3 n), and morphing sum_s mw_s n_s (the
          weights of a morphed skinned vertex by the accumulator, as positions)
  H       set 17's accumulator: per axis, bone by bone, the x, y, z terms
          ((w_i B_i[c][k]) cut) n[k] cut, into ge_acc; morphing as for positions
  Ht      H with the translation term too, the normal's w taken as 1
  Hk      H with the terms in k-major order (each k over the bones)
  A       per bone one GE row sum (dot3), times the weight, chained with ge_acc
  M       the bones' 3x3 blended first (entry by entry, ge_acc), then one row sum

    norms22.py emit [OUT.inc]        write c22n_data.inc
    norms22.py sums DUMPDIR          the logged CRCs against these streams
    norms22.py compare DUMPDIR [rule..]   bytes matching per rule and batch
    norms22.py check                 design checks: how often the rules part
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
import patch13 as P13
import lights17 as L17
import lights19 as L19
import skin16 as S16
from colour14 import XS, f32, bf
from lights17 import f24, Light, GU_DIRECTIONAL, GU_DIFFUSE, GU_POINTS, CLEAR, slot, P0

GU_NORMAL_32BITF, GU_VERTEX_32BITF = 3 << 5, 3 << 7
GU_WEIGHT = {'u8': 1 << 9, 'u16': 2 << 9, 'float': 3 << 9}
def GU_WEIGHTS(n): return ((n - 1) & 7) << 14
def GU_VERTICES(n): return ((n - 1) & 7) << 18
ID4 = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
RULES = ['H', 'cur', 'Ht', 'Hk', 'A', 'M']
BATCHES = ['one', 'pair', 'four', 'u8', 'u16', 'trans', 'morph', 'mskin']
PER_BATCH = 300

def c16(v): return P13.ge_cut(v)
ge_acc = S16.ge_acc

# =========================================================================== the rules
def skin_normal(rule, bones, ws, n):
    """A skinned normal (3 floats) under rule; bones as GE rows (B[c][k]), ws the weights'
    values (8/16-bit already scaled)."""
    if rule == 'cur':
        out = [f32(0.0)] * 3
        for B, w in zip(bones, ws):
            t = [f32(f32(f32(f32(B[c][0]) * f32(n[0])) + f32(f32(B[c][1]) * f32(n[1]))) + f32(f32(B[c][2]) * f32(n[2])))
                 for c in range(3)]
            out = [f32(out[c] + f32(f32(w) * t[c])) for c in range(3)]
        return out
    v4 = [c16(n[0]), c16(n[1]), c16(n[2]), 1.0]
    out = []
    for c in range(3):
        acc = 0.0
        if rule in ('H', 'Ht'):
            ks = (3, 0, 1, 2) if rule == 'Ht' else (0, 1, 2)
            for B, w in zip(bones, ws):
                for k in ks: acc = ge_acc(acc, c16(c16(c16(w) * c16(B[c][k])) * v4[k]))
        elif rule == 'Hk':
            for k in (0, 1, 2):
                for B, w in zip(bones, ws): acc = ge_acc(acc, c16(c16(c16(w) * c16(B[c][k])) * v4[k]))
        elif rule == 'A':
            for B, w in zip(bones, ws):
                acc = ge_acc(acc, P13.ge_sum([P13.ge_mul(w, P13.ge_sum([P13.ge_mul(B[c][k], v4[k]) for k in range(3)]))]))
        elif rule == 'M':
            row = []
            for k in range(3):
                e = 0.0
                for B, w in zip(bones, ws): e = ge_acc(e, c16(c16(w) * c16(B[c][k])))
                row.append(e)
            acc = P13.ge_sum([P13.ge_mul(row[k], v4[k]) for k in range(3)])
        else: raise ValueError(rule)
        out.append(acc)
    return out

def morph_vec(rule, mws, vecs):
    """sum_s mw_s v_s: float32 (cur) or the accumulator (the rest)."""
    if rule == 'cur':
        out = [f32(0.0)] * len(vecs[0])
        for mw, v in zip(mws, vecs): out = [f32(o + f32(f32(mw) * f32(x))) for o, x in zip(out, v)]
        return out
    out = [0.0] * len(vecs[0])
    for mw, v in zip(mws, vecs): out = [ge_acc(o, c16(mw) * c16(x)) for o, x in zip(out, v)]
    return out

def normal_of(rule, pt):
    sets, bones = pt['sets'], pt['bones']
    if len(sets) > 1:
        mws = pt['mws']
        n = morph_vec(rule, mws, [s['n'] for s in sets])
        # psprecomp morphs a skinned vertex's weights with the accumulator already
        ws = morph_vec('H' if rule == 'cur' else rule, mws, [s['w'] for s in sets]) if bones else None
    else:
        n = sets[0]['n']; ws = sets[0]['w'] if bones else None
    if bones:
        wv = [S16.wval(pt['fmt'], x) for x in ws] if pt['fmt'] != 'float' and len(sets) == 1 else ws
        n = skin_normal(rule, bones, wv, n)
    return n

def byte_of(rule, pt):
    n = normal_of(rule, pt)
    return L19.ge_byte_world(ID4, ID4, dict(L=pt['L'], v=(0.0, 0.0, 0.0), n=tuple(n)))

# =========================================================================== geometry
def rot(r): return L19.rot(r)
def mm3(a, b): return L19.mm3(a, b)
def tr(M): return [[M[j][i] for j in range(3)] for i in range(3)]
def mv(M, v): return [sum(M[i][j] * v[j] for j in range(3)) for i in range(3)]
def unit(v): return L17.unit(v)
def rnd(r): return r() / 2**31 - 1.0
def bone_of(M3, t=(0.0, 0.0, 0.0)):
    """A bone (GE rows, the w row 0 0 0 1) with every entry as the command's 24-bit float."""
    return [[f24(M3[i][0]), f24(M3[i][1]), f24(M3[i][2]), f24(t[i])] for i in range(3)] + [[0.0, 0.0, 0.0, 1.0]]
def cancelling(r, m):
    R1, R2 = rot(r), rot(r)
    return mm3(mm3(R1, [[2.0 ** m, 0, 0], [0, 2.0 ** -m, 0], [0, 0, 1]]), R2), R1, R2
def light_near(r, n):
    """A directional light 20-60 degrees off the normal's exact direction."""
    return Light(GU_DIRECTIONAL, GU_DIFFUSE, L19.V3(L19.near(r, unit(n), 60.0)))
def wraw(fmt, w):
    """The stored weight for value w: u8 / 128, u16 / 32768, or the float."""
    if fmt == 'u8': return max(0, min(255, int(round(w * 128))))
    if fmt == 'u16': return max(0, min(65535, int(round(w * 32768))))
    return f32(w)

def exact_skin(bones, ws, n):
    return [sum(w * sum(B[c][k] * n[k] for k in range(3)) for B, w in zip(bones, ws)) for c in range(3)]

def make(r, batch):
    """One point: dict(batch, fmt, bones, sets [{n, w}], mws, L)."""
    fmt = 'float'; mws = None
    if batch == 'one':
        m = 4 + r() % 4
        A, R1, R2 = cancelling(r, m)
        a_, c_ = rnd(r), rnd(r)
        n = mv(tr(R2), [2.0 ** (-2 * m) * a_, 1.0, 2.0 ** -m * c_])
        w = (1.0, 0.5, 0.75, 1.25, 0.3)[r() % 5]
        bones = [bone_of(A)]; sets = [dict(n=[f32(x) for x in n], w=[f32(w)])]
    elif batch in ('pair', 'u8', 'u16'):
        fmt = 'float' if batch == 'pair' else batch
        m = 4 + r() % 4
        B = [[rnd(r) * 2.0 ** m for _ in range(3)] for _ in range(3)]
        Ee = [[rnd(r) for _ in range(3)] for _ in range(3)]
        w1 = (0.5, 0.3, 0.75, 0.25)[r() % 4]; w2 = 1.0 - w1
        # w1 B1 + w2 B2 = 2^-m E: B2 = (2^-m E - w1 B1) / w2
        B2 = [[(2.0 ** -m * Ee[i][j] - w1 * B[i][j]) / w2 for j in range(3)] for i in range(3)]
        bones = [bone_of(B), bone_of(B2)]
        n = unit([rnd(r), rnd(r), rnd(r)])
        ws = [wraw(fmt, w1), wraw(fmt, w2)]
        if batch == 'u8' and r() & 1:          # one bone, the weight 8 bits
            A, R1, R2 = cancelling(r, m)
            n = mv(tr(R2), [2.0 ** (-2 * m) * rnd(r), 1.0, 2.0 ** -m * rnd(r)])
            bones = [bone_of(A)]; ws = [wraw(fmt, (0.5, 0.75, 1.0, 1.5)[r() % 4])]
        sets = [dict(n=[f32(x) for x in n], w=ws)]
    elif batch == 'four':
        m = 4 + r() % 4
        bones = []; ws = []
        for _ in range(2):
            B = [[rnd(r) * 2.0 ** m for _ in range(3)] for _ in range(3)]
            Ee = [[rnd(r) for _ in range(3)] for _ in range(3)]
            w1 = (0.25, 0.2, 0.3)[r() % 3]; w2 = 0.5 - w1
            B2 = [[(2.0 ** -m * Ee[i][j] - w1 * B[i][j]) / w2 for j in range(3)] for i in range(3)]
            bones += [bone_of(B), bone_of(B2)]; ws += [w1, w2]
        n = unit([rnd(r), rnd(r), rnd(r)])
        sets = [dict(n=[f32(x) for x in n], w=[f32(w) for w in ws])]
    elif batch == 'trans':
        R = rot(r); s = 2.0 ** ((r() % 5) - 2)
        t = [rnd(r) * 2.0 ** (2 + r() % 5) for _ in range(3)]
        bones = [bone_of([[s * x for x in row] for row in R], t)]
        n = unit([rnd(r), rnd(r), rnd(r)])
        sets = [dict(n=[f32(x) for x in n], w=[f32((1.0, 0.75, 0.5)[r() % 3])])]
    elif batch == 'morph':
        k = 2 + r() % 3
        m = 8 + r() % 6
        mws = [f24((0.5, 0.25, 0.75, 1.0, 0.3)[r() % 5]) for _ in range(k)]
        vecs = [[rnd(r) for _ in range(3)] for _ in range(k - 1)]
        part = [sum(mw * v[c] for mw, v in zip(mws, vecs)) for c in range(3)]
        res = [2.0 ** -m * rnd(r) for _ in range(3)]
        last = [(res[c] - part[c]) / mws[-1] for c in range(3)]     # the sets nearly cancel
        bones = []
        sets = [dict(n=[f32(x) for x in v], w=None) for v in vecs + [last]]
    elif batch == 'mskin':
        m = 4 + r() % 4
        A, R1, R2 = cancelling(r, m)
        mws = [f24((0.5, 0.25, 0.75)[r() % 3])]; mws.append(f24(1.0 - mws[0]))
        n0 = mv(tr(R2), [2.0 ** (-2 * m) * rnd(r), 1.0, 2.0 ** -m * rnd(r)])
        n1 = mv(tr(R2), [2.0 ** (-2 * m) * rnd(r), 1.0, 2.0 ** -m * rnd(r)])
        bones = [bone_of(A)]
        if r() & 1:
            Bb = [[rnd(r) for _ in range(3)] for _ in range(3)]
            bones.append(bone_of(Bb))
            w0 = [f32(0.9), f32(0.1)]; w1 = [f32(0.7), f32(0.3)]
        else:
            w0 = [f32(1.0)]; w1 = [f32(0.6)]
        sets = [dict(n=[f32(x) for x in n0], w=w0), dict(n=[f32(x) for x in n1], w=w1)]
    else: raise ValueError(batch)
    pt = dict(batch=batch, fmt=fmt, bones=bones, sets=sets, mws=mws)
    nx = normal_exact(pt)
    pt['L'] = light_near(r, nx)
    return pt

def normal_exact(pt):
    sets, bones = pt['sets'], pt['bones']
    def wv(ws): return [S16.wval(pt['fmt'], x) for x in ws] if pt['fmt'] != 'float' else ws
    if len(sets) > 1:
        n = [sum(mw * s['n'][c] for mw, s in zip(pt['mws'], sets)) for c in range(3)]
        ws = [sum(mw * s['w'][i] for mw, s in zip(pt['mws'], sets)) for i in range(len(sets[0]['w']))] if bones else None
    else:
        n = sets[0]['n']; ws = wv(sets[0]['w']) if bones else None
    return exact_skin(bones, ws, n) if bones else n

_S = None
def points():
    global _S
    if _S is None:
        r = XS(0x13800001)
        _S = []
        for b in BATCHES:
            n = 0
            while n < PER_BATCH:
                pt = make(r, b)
                if L19.ge_byte_world(ID4, ID4, dict(L=pt['L'], v=(0.0, 0.0, 0.0), n=tuple(normal_exact(pt)))) < 20: continue
                _S.append(pt); n += 1
    return _S

# =========================================================================== emission
def vtype_of(pt):
    vt = GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_VERTICES(len(pt['sets']))
    if pt['bones']: vt |= GU_WEIGHTS(len(pt['bones'])) | GU_WEIGHT[pt['fmt']]
    return vt

def vbytes(pt):
    b = b''
    for s in pt['sets']:
        rec = b''
        if pt['bones']:
            ws = s['w']
            if pt['fmt'] == 'u8': rec += bytes(ws)
            elif pt['fmt'] == 'u16': rec += struct.pack(f'<{len(ws)}H', *ws)
            else: rec += struct.pack(f'<{len(ws)}f', *ws)
            rec += b'\0' * (-len(rec) % 4)
        rec += struct.pack('<fff', *s['n']) + struct.pack('<fff', 0.0, 0.0, 0.0)
        b += rec
    return b

def scene_ops():
    ops = [('BEGIN', 3, CLEAR), ('LMODE', 1), ('MATS', P0, ID4, ID4)]
    cur = {}
    def state(key, val, op):
        if cur.get(key) != val: ops.append(op); cur[key] = val
    for i, pt in enumerate(points()):
        x, y = slot(i)
        ops.append(('OFFSET', 2048 - x, 2048 - y))
        state('L', pt['L'].key(), ('LGT', pt['L'].words()))
        for k, B in enumerate(pt['bones']): state(f'bone{k}', str(B), ('BONE', k, B))
        if pt['mws']: state('morph', tuple(pt['mws']), ('MORPH', tuple(pt['mws']) + (0.0,) * (8 - len(pt['mws']))))
        elif len(pt['sets']) == 1: pass
        ops.append(('RAW', GU_POINTS, vtype_of(pt), 1, vbytes(pt)))
    ops += [('LMODE', 0), ('OFFSET', 2048 - 240, 2048 - 136), ('END', 0)]
    return ops

SCENES = [(138, 'sknorm', 'skinned and morphed normals, read through lighting')]
def emit_scene():
    OP = C.OP
    w = []; vcrc = 0; nd = 0; nv = 0
    def op(name, args):
        w.append(OP[name] << 24 | len(args)); w.extend(a & 0xFFFFFFFF for a in args)
    body = []
    for o in scene_ops():
        k = o[0]
        if k == 'BEGIN': body.append(('BEGIN', [o[1], o[2]]))
        elif k == 'END': body.append(('END', [o[1]]))
        elif k == 'MATS': body.append(('MATS', [7] + C.mem16(o[1]) + C.mem16(o[2]) + C.mem16(o[3])))
        elif k == 'LMODE': body.append(('LMODE', [o[1]]))
        elif k == 'LGT': body.append(('LGT', o[1]))
        elif k == 'BONE': body.append(('BONE', [o[1]] + S16.mem16(o[2])))
        elif k == 'MORPH': body.append(('MORPH', [bf(x) for x in o[1]]))
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

def emit(path):
    out = ['/* Generated by norms22.py emit: geprobe 22 scene 138 (skinned and morphed normals)',
           ' * as a command stream for c14_run() (colour14.py\'s format).  Do not edit; change',
           ' * norms22.py and run it again. */']
    n, name, title = SCENES[0]
    w, nd, nv, vc = emit_scene()
    crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
    out.append(f'\n/* scene {n} {name}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
               f' {len(w)} words, stream crc {crc:08X} */')
    out.append(f'static const w32 C22N_S0[{len(w)}] = {{')
    for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
    out.append('};')
    out.append('\nstatic const struct c14_step C22N_STEPS[] = {')
    out.append(f'    {{ {n}, "{name}", "{title}", C22N_S0, {len(w)} }},')
    out.append('};\n#define C22N_NSTEPS ((int)(sizeof C22N_STEPS / sizeof C22N_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text: open(path, 'w').write(text)
    return [(dict(num=n, name=name, title=title), len(w), nd, nv, vc, crc)]

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for sc, n, nd, nv, vc, crc in emit(os.path.join(td, 'c22n.inc')):
            m = re.search(rf'scene {sc["num"]}: .*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want; ok &= good
            print(f'scene {sc["num"]} {sc["name"]}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

def read(dumpdir):
    F = np.fromfile(os.path.join(dumpdir, 'ge_138_sknorm.raw'), '<u4').reshape(272, 480)
    out = []
    for i in range(len(points())):
        x, y = slot(i)
        c = int(F[y, x]) & 0xFFFFFF
        out.append(-1 if c == (CLEAR & 0xFFFFFF) else (c >> 8) & 0xFF)
    return out

def compare(dumpdir, rules=None):
    rules = rules or RULES
    hw = read(dumpdir)
    print(f'138 sknorm: {sum(1 for v in hw if v < 0)} of {len(hw)} points not drawn')
    for b in BATCHES:
        idx = [i for i, pt in enumerate(points()) if pt['batch'] == b]
        line = []
        for rule in rules:
            ok = sum(1 for i in idx if byte_of(rule, points()[i]) == hw[i])
            line.append(f'{rule} {ok}')
        print(f'  {b:6s} {len(idx)} points: ' + ', '.join(line))

def check():
    pts = points()
    base = [byte_of('H', pt) for pt in pts]
    print(f'138: {len(pts)} points; H bytes {min(base)}-{max(base)}')
    for b in BATCHES:
        idx = [i for i, pt in enumerate(pts) if pt['batch'] == b]
        parts = []
        for rule in RULES[1:]:
            d = sum(1 for i in idx if byte_of(rule, pts[i]) != base[i])
            parts.append(f'{rule} {d}')
        print(f'  {b:6s}: points where H parts from ' + ', '.join(parts))

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        for sc, n, nd, nv, vc, crc in emit(argv[2] if len(argv) > 2 else os.path.join(HERE, 'c22n_data.inc')):
            print(f'scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'compare': compare(argv[2], argv[3:] or None); return 0
    if cmd == 'check': check(); return 0
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
