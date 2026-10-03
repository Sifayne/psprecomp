#!/usr/bin/env python3
"""geprobe 22 (scenes 139-140): morphed colours and texture coordinates.

Set 17 (skin16.py) read morphed positions and weights whole: one accumulator (ge_acc),
set 0 first, each term the 16-bit morph weight times the 16-bit value. Colours and
texture coordinates are morphed too, and psprecomp blends both in float32 (ge.c
read_mvert: colour channels summed and truncated, texture coordinates after their scale
and offset). Nothing has read either. psprecomp also takes only 8888 vertex colours in
3D, whatever the format (the rest read as the material colour), so the 16-bit formats'
own expansion is read here too. Every point goes to clip (0, 0, 0, 1) through a zero
projection and its offset places it, as in 115-124.

  139 mrcol   lighting off, untextured: each point's colour is its morphed vertex colour.
              Batches: plain 5650 / 5551 / 4444 (one set: the expansion), 8888 morphed
              over 2 to 8 sets with weights summing to one, to more or less than one,
              negative, and at sums a hair either side of a half or a whole code; the
              16-bit formats morphed over 2 to 4 sets
  140 mruv    textured with a 256 x 256 texture whose texel (s, t) holds s in red and t
              in green (main.c c22_texture), nearest, repeating, replacing: a point's
              colour names the texel its coordinates fall in. Each point is drawn three
              times, at texture scale 1, 2^8 and 2^12 (the TEXC op), so its coordinate
              reads to 2^-8, 2^-16 and 2^-20 of a unit, the scale-2^8 and 2^12 draws offset
              by the whole texels the coarser ones read. Batches: plain (one set; float,
              u16 and u8) at chosen boundaries to read the scale-and-offset path itself;
              float, u16 and u8 coordinates morphed over 2 to 4 sets, plainly and
              cancelling (sets near each other, weights of opposite sign); with an
              offset and weights summing to other than one (does the offset get morphed?)

The rules:
  colour: cur (float32 sum, truncated, clamped), floor, round (exact sum), H (ge_acc on
          the channel, then floored), H8 (ge_acc, the channel as value/255), raw16 (a
          16-bit format's fields morphed before they are widened)
  coords: cur (psprecomp: scale and offset first, then a float32 blend), H (ge_acc on the
          raw coordinates, then the scale and offset as ge_mul/ge_sum), F (a float32 blend
          of the raw coordinates, then scale and offset in float32)

    morph22.py emit [OUT.inc]        write c22m_data.inc
    morph22.py sums DUMPDIR          the logged CRCs against these streams
    morph22.py compare DUMPDIR       readings matching per rule and batch
    morph22.py check                 design checks: how often the rules part

Set 23 (fw 6.60): colours are H with the sign dropped before the clamp (floor(|acc|), 255
at most: a sum of -34.004 reads 34), all 13,200 channels, the 16-bit formats widened by
repeating their top bits; coordinates are H, with the texel coordinate cut toward zero
to 1/16 texel before it is floored (-61.004 texels reads texel -61), all 15,000
readings. psprecomp now does both (ge.c read_mvert, uv_to_texels; render.c
nearest_texel).
"""
import sys, os, math, struct, zlib, re
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour14 as C
import patch13 as P13
import skin16 as S16
from colour14 import XS, f32, bf
from lights17 import f24, GU_POINTS, P0, PITCH, ACROSS

CLEAR = 0xFF0000FF                 # red: a point not drawn (the coordinate texture's blue is 0x80)
ID4 = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
COLFMT = {'5650': 4, '5551': 5, '4444': 6, '8888': 7}
TEXFMT = {'u8': 1, 'u16': 2, 'float': 3}
GU_VERTEX_32BITF = 3 << 7
def GU_VERTICES(n): return ((n - 1) & 7) << 18
ge_acc = S16.ge_acc
def c16(v): return P13.ge_cut(v)
def gsum(*terms): return P13.ge_sum([P13.ge_mul(a, b) for a, b in terms])
def slot(i): return 1 + PITCH * (i % ACROSS), 1 + PITCH * (i // ACROSS)

# =========================================================================== vertices
def pack16(fmt, c):
    """An 8888 colour's channels (r, g, b, a in 0..255) packed into a 16-bit format's fields
    (the fields themselves: c is the field values here)."""
    r, g, b, a = c
    if fmt == '5650': return r | g << 5 | b << 11
    if fmt == '5551': return r | g << 5 | b << 10 | a << 15
    if fmt == '4444': return r | g << 4 | b << 8 | a << 12
    raise ValueError(fmt)
FIELD_BITS = {'5650': (5, 6, 5, 0), '5551': (5, 5, 5, 1), '4444': (4, 4, 4, 4), '8888': (8, 8, 8, 8)}
def widen(bits, v):
    """A field widened to 8 bits by repeating its top bits (an alpha bit to 0 or 255)."""
    if bits == 8: return v
    if bits == 0: return 255
    if bits == 1: return 255 if v else 0
    x = v << (8 - bits)
    return x | (x >> bits) if bits >= 4 else x
def colour_value(fmt, fields):
    """The four channels a vertex colour stands for."""
    return [widen(b, f) for b, f in zip(FIELD_BITS[fmt], fields)]

def vrecord(colfmt=None, colour=None, texfmt=None, uv=None):
    """One vertex set: texture, colour, position (float, all 0), each aligned to its size."""
    b = b''
    def align(n):
        nonlocal b
        b += b'\0' * (-len(b) % n)
    if texfmt:
        if texfmt == 'u8': b += bytes(uv)
        elif texfmt == 'u16': align(2); b += struct.pack('<2H', *uv)
        else: align(4); b += struct.pack('<2f', *uv)
    if colfmt:
        if colfmt == '8888': align(4); b += struct.pack('<I', colour[0] | colour[1] << 8 | colour[2] << 16 | colour[3] << 24)
        else: align(2); b += struct.pack('<H', pack16(colfmt, colour))
    align(4); b += struct.pack('<3f', 0.0, 0.0, 0.0)
    return b

def vtype_of(colfmt, texfmt, nsets):
    vt = GU_VERTEX_32BITF | GU_VERTICES(nsets)
    if colfmt: vt |= COLFMT[colfmt] << 2
    if texfmt: vt |= TEXFMT[texfmt]
    return vt

# =========================================================================== scene 139: colours
def colour_rule(rule, pt):
    """The four channels a morphed (or plain) point is drawn in, under rule."""
    fmt, sets, mws = pt['fmt'], pt['sets'], pt['mws']
    bits = FIELD_BITS[fmt]
    if len(sets) == 1:
        return colour_value(fmt, sets[0])
    out = []
    for ch in range(4):
        if rule == 'raw16' and fmt != '8888':
            v = sum(mw * s[ch] for mw, s in zip(mws, sets))
            f = max(0, min((1 << bits[ch]) - 1, math.floor(v))) if bits[ch] else 0
            out.append(widen(bits[ch], f)); continue
        vals = [colour_value(fmt, s)[ch] for s in sets]
        if rule == 'cur':
            acc = f32(0.0)
            for mw, v in zip(mws, vals): acc = f32(acc + f32(f32(mw) * f32(v)))
            x = int(acc) if acc > -1 else 0     # (int) truncates toward zero, then the clamp
        elif rule in ('floor', 'round', 'raw16'):
            e = sum(mw * v for mw, v in zip(mws, vals))
            x = math.floor(e + (0.5 if rule == 'round' else 0.0))
        elif rule == 'H':
            acc = 0.0
            for mw, v in zip(mws, vals): acc = ge_acc(acc, c16(mw) * v)
            x = math.floor(acc)
        elif rule == 'H8':
            acc = 0.0
            for mw, v in zip(mws, vals): acc = ge_acc(acc, c16(c16(mw) * (v / 255.0)))
            x = math.floor(acc * 255.0)
        else: raise ValueError(rule)
        out.append(max(0, min(255, x)))
    return out
COL_RULES = ['cur', 'floor', 'round', 'H', 'H8', 'raw16']

def rfields(r, fmt):
    return [r() % (1 << b) if b else 0 for b in FIELD_BITS[fmt]]

def col_point(r, batch):
    if batch.startswith('plain'):
        fmt = batch[5:]
        return dict(batch=batch, fmt=fmt, sets=[rfields(r, fmt)], mws=None)
    if batch in ('sum1', 'over', 'neg', 'many'):
        fmt = '8888'
        k = {'sum1': 2 + r() % 2, 'over': 2 + r() % 3, 'neg': 2 + r() % 2, 'many': 5 + r() % 4}[batch]
        sets = [rfields(r, fmt) for _ in range(k)]
        if batch == 'sum1':
            ws = [(r() % 65536) / 65536 for _ in range(k - 1)]
            ws = [w / max(1.0, sum(ws)) for w in ws]; ws.append(1.0 - sum(ws))
        elif batch == 'over': ws = [0.2 + (r() % 1000) / 1000 for _ in range(k)]
        elif batch == 'neg': ws = [1.0 + (r() % 1000) / 1000] + [-(r() % 1000) / 1000 for _ in range(k - 1)]
        else: ws = [(r() % 1000) / 1000 / k * 2 for _ in range(k)]
        return dict(batch=batch, fmt=fmt, sets=sets, mws=[f24(w) for w in ws])
    if batch == 'edge':
        # a channel's sum a hair from a half or whole code: c0 (1 - w) + c1 w with w near
        # a half, quarter or eighth, the 16-bit weight's last bits around it
        fmt = '8888'
        base = (0.5, 0.25, 0.75, 0.125, 0.375)[r() % 5]
        d = ((r() % 9) - 4) * 2.0 ** -(14 + r() % 3)
        w1 = f24(base + d); w0 = f24(1.0 - w1)
        sets = [rfields(r, fmt), rfields(r, fmt)]
        return dict(batch=batch, fmt=fmt, sets=sets, mws=[w0, w1])
    if batch.startswith('m'):
        fmt = batch[1:]
        k = 2 + r() % 3
        sets = [rfields(r, fmt) for _ in range(k)]
        ws = [(r() % 65536) / 65536 for _ in range(k)]
        s = sum(ws); ws = [w / s * (0.8 + (r() % 400) / 1000) for w in ws]
        return dict(batch=batch, fmt=fmt, sets=sets, mws=[f24(w) for w in ws])
    raise ValueError(batch)
COL_BATCHES = ['plain5650', 'plain5551', 'plain4444', 'sum1', 'over', 'neg', 'many', 'edge',
               'm5650', 'm5551', 'm4444']
COL_PER = {'plain5650': 200, 'plain5551': 200, 'plain4444': 200, 'sum1': 600, 'over': 400, 'neg': 400,
           'many': 400, 'edge': 800, 'm5650': 400, 'm5551': 400, 'm4444': 400}

_col = None
def col_points():
    global _col
    if _col is None:
        r = XS(0x13900001)
        _col = [col_point(r, b) for b in COL_BATCHES for _ in range(COL_PER[b])]
    return _col

# =========================================================================== scene 140: coordinates
SCALES = (1.0, 256.0, 4096.0)
def uv_value(fmt, raw):
    if fmt == 'u8': return raw / 128.0
    if fmt == 'u16': return raw / 32768.0
    return raw

def uv_rule(rule, pt, axis):
    """The coordinate (in units, before the texture's scale and offset) and the function
    that takes it through scale su and offset ou to texels, under rule."""
    fmt, sets, mws = pt['fmt'], pt['sets'], pt['mws']
    vals = [uv_value(fmt, s[axis]) for s in sets]
    if len(sets) == 1: return vals[0]
    if rule == 'H':
        acc = 0.0
        for mw, v in zip(mws, vals): acc = ge_acc(acc, c16(mw) * c16(v))
        return acc
    acc = f32(0.0)
    for mw, v in zip(mws, vals): acc = f32(acc + f32(f32(mw) * f32(v)))
    return acc

def texel(rule, pt, axis, su, ou):
    """The texel a point reads along axis at scale su and offset ou (units), mod 256."""
    fmt, sets, mws = pt['fmt'], pt['sets'], pt['mws']
    if rule == 'cur':
        # psprecomp: each set scaled and offset, in texels, then blended in float32
        per = [f32(f32(f32(f32(uv_value(fmt, s[axis])) * f32(su)) + f32(ou)) * 256.0) for s in sets]
        if len(sets) == 1: t = per[0]
        else:
            t = f32(0.0)
            for mw, v in zip(mws, per): t = f32(t + f32(f32(mw) * v))
        return math.floor(t) % 256
    u = uv_rule(rule, pt, axis)
    if rule == 'H':
        t = gsum((u, su), (ou, 1.0)) * 256.0
    else:
        t = f32(f32(f32(u) * f32(su)) + f32(ou)) * 256.0
    return math.floor(t) % 256
UV_RULES = ['H', 'cur', 'F']

def uv_exact(pt, axis):
    vals = [uv_value(pt['fmt'], s[axis]) for s in pt['sets']]
    return vals[0] if len(vals) == 1 else sum(mw * v for mw, v in zip(pt['mws'], vals))

def raw_of(fmt, u):
    if fmt == 'u8': return max(0, min(255, int(round(u * 128))))
    if fmt == 'u16': return max(0, min(65535, int(round(u * 32768))))
    return f32(u)

def uv_point(r, batch):
    if batch.startswith('plain'):
        fmt = batch[5:]
        # a coordinate on, or a hair either side of, a 2^-8, 2^-16 or 2^-20 boundary
        g = (2.0 ** -8, 2.0 ** -16, 2.0 ** -20)[r() % 3]
        k = 1 + r() % 4000
        if fmt == 'float':
            d = ((r() % 5) - 2) * 2.0 ** -(22 + r() % 3)
            uv = [f32(min(0.999, (k * g) % 1.0 + d)) for _ in range(2)]
            uv[1] = f32(((k * 7 + 3) * g) % 1.0 + d)
        elif fmt == 'u16': uv = [r() % 32768, r() % 32768]
        else: uv = [r() % 128, r() % 128]
        return dict(batch=batch, fmt=fmt, sets=[[abs(x) for x in uv]], mws=None, off=0.0)
    if batch in ('float', 'u16', 'u8', 'cancel', 'cancel16', 'cancel8', 'offset'):
        fmt = {'float': 'float', 'u16': 'u16', 'u8': 'u8', 'cancel': 'float', 'cancel16': 'u16',
               'cancel8': 'u8', 'offset': 'float'}[batch]
        k = 2 + r() % 3
        for _ in range(1000):
            if batch.startswith('cancel'):
                big = (r() % 6) if fmt == 'float' else 0
                base = [(0.5 + (r() % 1000) / 1000) * 2.0 ** big for _ in range(2)]
                sets = []
                for s in range(k):
                    sets.append([raw_of(fmt, b + ((r() % 2001) - 1000) / 1000 * 2.0 ** -(6 + r() % 4)) for b in base])
                ws = [0.5 + (r() % 1000) / 1000 for _ in range(k - 1)]
                ws.append(-sum(ws) + ((r() % 1000) / 1000) * 2.0 ** -(4 + r() % 4))   # sums to a little
            else:
                sets = [[raw_of(fmt, (r() % 10000) / 10000 * (1.9 if fmt != 'float' else 1.0)) for _ in range(2)] for _ in range(k)]
                ws = [(r() % 65536) / 65536 for _ in range(k)]
                s = sum(ws); tot = 1.0 if batch != 'offset' else (0.6 + (r() % 800) / 1000)
                ws = [w / s * tot for w in ws]
            pt = dict(batch=batch, fmt=fmt, sets=sets, mws=[f24(w) for w in ws],
                      off=(f24(((r() % 200) - 100) / 64.0) if batch == 'offset' else 0.0))
            if all(0.0 <= uv_exact(pt, a) < 4.0 for a in (0, 1)): return pt
        raise RuntimeError(batch)
    raise ValueError(batch)
UV_BATCHES = ['plainfloat', 'plainu16', 'plainu8', 'float', 'u16', 'u8', 'cancel', 'cancel16', 'cancel8', 'offset']
UV_PER = {'plainfloat': 300, 'plainu16': 150, 'plainu8': 150, 'float': 250, 'u16': 250, 'u8': 250,
          'cancel': 400, 'cancel16': 250, 'cancel8': 250, 'offset': 250}

_uv = None
def uv_points():
    global _uv
    if _uv is None:
        r = XS(0x14000001)
        _uv = [uv_point(r, b) for b in UV_BATCHES for _ in range(UV_PER[b])]
    return _uv

_draws = None
def uv_draws():
    global _draws
    if _draws is None: _draws = _uv_draws()
    return _draws
def _uv_draws():
    """(point index, scale index, su, ou_u, ou_v): every point at the three scales. At
    scale 1 the offset is the point's own (0 but in the offset batch); at 2^8 and 2^12 it
    takes away the whole units the scaled exact coordinate holds, so the texel read is
    the coordinate's next 8 bits and the texture coordinate stays under 256 texels."""
    out = []
    for si, su in enumerate(SCALES):
        for i, pt in enumerate(uv_points()):
            if si == 0: out.append((i, si, su, pt['off'], pt['off'])); continue
            ou, ov = (f24(-math.floor(uv_exact(pt, a) * su)) for a in (0, 1))
            out.append((i, si, su, ou, ov))
    return out

# =========================================================================== emission
def scene_ops_col():
    ops = [('BEGIN', 3, CLEAR), ('MATS', P0, ID4, ID4)]
    lastm = None
    for i, pt in enumerate(col_points()):
        x, y = slot(i)
        ops.append(('OFFSET', 2048 - x, 2048 - y))
        if pt['mws'] and tuple(pt['mws']) != lastm:
            ops.append(('MORPH', tuple(pt['mws']) + (0.0,) * (8 - len(pt['mws'])))); lastm = tuple(pt['mws'])
        vb = b''.join(vrecord(pt['fmt'], s) for s in pt['sets'])
        ops.append(('RAW', GU_POINTS, vtype_of(pt['fmt'], None, len(pt['sets'])), 1, vb))
    ops += [('MORPH', (1.0,) + (0.0,) * 7), ('OFFSET', 2048 - 240, 2048 - 136), ('END', 0)]
    return ops

def scene_ops_uv():
    ops = [('BEGIN', 3, CLEAR), ('MATS', P0, ID4, ID4)]
    lastm = None; lastt = None
    n = len(uv_points())
    for k, (i, si, su, ou, ov) in enumerate(uv_draws()):
        pt = uv_points()[i]
        x, y = slot(si * n + i)
        ops.append(('OFFSET', 2048 - x, 2048 - y))
        t = (su, ou, ov)
        if t != lastt: ops.append(('TEXC', 1, su, su, ou, ov)); lastt = t
        if pt['mws'] and tuple(pt['mws']) != lastm:
            ops.append(('MORPH', tuple(pt['mws']) + (0.0,) * (8 - len(pt['mws'])))); lastm = tuple(pt['mws'])
        vb = b''.join(vrecord('8888', (255, 255, 255, 255), pt['fmt'], s) for s in pt['sets'])
        ops.append(('RAW', GU_POINTS, vtype_of('8888', pt['fmt'], len(pt['sets'])), 1, vb))
    ops += [('TEXC', 0, 1.0, 1.0, 0.0, 0.0), ('MORPH', (1.0,) + (0.0,) * 7), ('OFFSET', 2048 - 240, 2048 - 136), ('END', 0)]
    return ops

SCENES = [(139, 'mrcol', 'morphed colours, and 16-bit vertex colours', scene_ops_col),
          (140, 'mruv', 'morphed texture coordinates, read through a coordinate texture', scene_ops_uv)]

def emit_ops(ops):
    OP = C.OP
    w = []; vcrc = 0; nd = 0; nv = 0
    def op(name, args):
        w.append(OP[name] << 24 | len(args)); w.extend(a & 0xFFFFFFFF for a in args)
    body = []
    for o in ops:
        k = o[0]
        if k == 'BEGIN': body.append(('BEGIN', [o[1], o[2]]))
        elif k == 'END': body.append(('END', [o[1]]))
        elif k == 'MATS': body.append(('MATS', [7] + C.mem16(o[1]) + C.mem16(o[2]) + C.mem16(o[3])))
        elif k == 'MORPH': body.append(('MORPH', [bf(x) for x in o[1]]))
        elif k == 'OFFSET': body.append(('OFFSET', [o[1], o[2]]))
        elif k == 'TEXC': body.append(('TEXC', [o[1]] + [bf(f32(x)) for x in o[2:6]]))
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
    out = ['/* Generated by morph22.py emit: geprobe 22 scenes 139-140 (morphed colours and',
           ' * texture coordinates) as command streams for c14_run() (colour14.py\'s format, with',
           ' * TEXC).  Do not edit; change morph22.py and run it again. */']
    steps = []
    for k, (n, name, title, fn) in enumerate(SCENES):
        w, nd, nv, vc = emit_ops(fn())
        crc = zlib.crc32(struct.pack(f'<{len(w)}I', *w)) & 0xFFFFFFFF
        steps.append((dict(num=n, name=name, title=title), len(w), nd, nv, vc, crc))
        out.append(f'\n/* scene {n} {name}: {nd} draws, {nv} vertices, vertex crc {vc:08X},'
                   f' {len(w)} words, stream crc {crc:08X} */')
        out.append(f'static const w32 C22M_S{k}[{len(w)}] = {{')
        for i in range(0, len(w), 8): out.append(' ' + ' '.join(f'0x{x:08X}u,' for x in w[i:i + 8]))
        out.append('};')
    out.append('\nstatic const struct c14_step C22M_STEPS[] = {')
    for k, (sc, n, *_r) in enumerate(steps):
        out.append(f'    {{ {sc["num"]}, "{sc["name"]}", "{sc["title"]}", C22M_S{k}, {n} }},')
    out.append('};\n#define C22M_NSTEPS ((int)(sizeof C22M_STEPS / sizeof C22M_STEPS[0]))\n')
    text = '\n'.join(out)
    if not os.path.exists(path) or open(path).read() != text: open(path, 'w').write(text)
    return steps

def sums(dumpdir):
    import tempfile
    log = open(os.path.join(dumpdir, 'geprobe.txt')).read()
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for sc, n, nd, nv, vc, crc in emit(os.path.join(td, 'c22m.inc')):
            m = re.search(rf'scene {sc["num"]}: .*?\n\s+(\d+) draws, (\d+) vertices, '
                          rf'vertex crc ([0-9A-F]+), stream crc ([0-9A-F]+)', log)
            want = (str(nd), str(nv), f'{vc:08X}', f'{crc:08X}')
            good = bool(m) and m.groups() == want; ok &= good
            print(f'scene {sc["num"]} {sc["name"]}: ' + ('log MATCHES' if good else
                  f'log DIFFERS: {m.groups() if m else "no SUMS line"} against {want}'))
    print('all CRCs match' if ok else 'CRC PROBLEMS: do not read the pixels')
    return ok

def frame(dumpdir, n, name):
    return np.fromfile(os.path.join(dumpdir, f'ge_{n}_{name}.raw'), '<u4').reshape(272, 480)

def compare(dumpdir):
    F = frame(dumpdir, 139, 'mrcol')
    pts = col_points()
    print('139 mrcol: channels matching (r, g, b) per rule')
    for b in COL_BATCHES:
        idx = [i for i, p in enumerate(pts) if p['batch'] == b]
        hw = {i: [(int(F[slot(i)[1], slot(i)[0]]) >> (8 * c)) & 0xFF for c in range(3)] for i in idx}
        res = []
        for rule in COL_RULES:
            ok = sum(1 for i in idx for c in range(3) if colour_rule(rule, pts[i])[c] == hw[i][c])
            res.append(f'{rule} {ok}')
        print(f'  {b:10s} {len(idx)} points: ' + ', '.join(res))
    F = frame(dumpdir, 140, 'mruv')
    pts = uv_points(); n = len(pts)
    print('140 mruv: texels matching (s and t) per rule and scale')
    for b in UV_BATCHES:
        idx = [i for i, p in enumerate(pts) if p['batch'] == b]
        res = []
        for rule in UV_RULES:
            per = []
            for si, su in enumerate(SCALES):
                ok = 0
                for i in idx:
                    d = uv_draws()[si * n + i]
                    x, y = slot(si * n + i)
                    c = int(F[y, x])
                    for a, ou in ((0, d[3]), (1, d[4])):
                        ok += texel(rule, pts[i], a, su, ou) == (c >> (8 * a)) & 0xFF
                per.append(ok)
            res.append(f'{rule} {per}')
        print(f'  {b:10s} {len(idx)} points: ' + ', '.join(res))

def check():
    pts = col_points()
    print(f'139: {len(pts)} points')
    for b in COL_BATCHES:
        idx = [i for i, p in enumerate(pts) if p['batch'] == b]
        base = {i: colour_rule('cur', pts[i]) for i in idx}
        parts = []
        for rule in COL_RULES[1:]:
            d = sum(1 for i in idx for c in range(3) if colour_rule(rule, pts[i])[c] != base[i][c])
            parts.append(f'{rule} {d}')
        print(f'  {b:10s}: channels where cur parts from ' + ', '.join(parts))
    pts = uv_points(); n = len(pts)
    print(f'140: {n} points, {3 * n} draws')
    for b in UV_BATCHES:
        idx = [i for i, p in enumerate(pts) if p['batch'] == b]
        parts = []
        for rule in UV_RULES[1:]:
            per = []
            for si, su in enumerate(SCALES):
                d = 0
                for i in idx:
                    dr = uv_draws()[si * n + i]
                    for a, ou in ((0, dr[3]), (1, dr[4])):
                        d += texel(rule, pts[i], a, su, ou) != texel('H', pts[i], a, su, ou)
                per.append(d)
            parts.append(f'{rule} {per}')
        print(f'  {b:10s}: texels where H parts from ' + ', '.join(parts))
    print(f'slots used: {3 * n} of {ACROSS * 135} (uv), {len(col_points())} (colour)')

def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'emit':
        for sc, n, nd, nv, vc, crc in emit(argv[2] if len(argv) > 2 else os.path.join(HERE, 'c22m_data.inc')):
            print(f'scene {sc["num"]} {sc["name"]}: {nd} draws, {nv} vertices, vertex crc {vc:08X}, {n} words, stream crc {crc:08X}')
        return 0
    if cmd == 'sums': return 0 if sums(argv[2]) else 1
    if cmd == 'compare': compare(argv[2]); return 0
    if cmd == 'check': check(); return 0
    print(__doc__); return 2

if __name__ == '__main__':
    sys.exit(main(sys.argv))
