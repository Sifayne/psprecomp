#!/usr/bin/env python3
"""Fit the VFPU's piecewise-quadratic cores to vfpuprobe's hardware dumps.

usage: gencores.py RUNDIR [--v4 DIR] [--v5 DIR] [--out FILE] [--check] [--holdout]
                   [--ops a,b]

RUNDIR is a vfpuprobe v3 result folder with vfpu_core_<op>.bin, the run-1
sweep files vfpu_<op>.bin and vfpu_inputs.bin (fw660-run4 has all of them).
--v4 DIR adds a vfpuprobe v4 folder's vfpu_core4_vcos.bin and
vfpu_core4_vasin.bin (set 10, fw660-v4.txt steps 201-202): whole segments
of the sin and asin cores that v3's every-3rd dump left open. Everything in
the output comes from those files; nothing is computed from a math library
except starting points for a search whose answer the data decides.
src/vfpu_cores.h is this script's output for fw660-run4
(/mnt/project-files/hwresults/fw660-run4/vfpuprobe) and set 10's v4 run:

    gencores.py <fw660-run4>/vfpuprobe --v4 <set 10>/vfpuprobe-s10 \
        --check --out src/vfpu_cores.h

What the dumps show for vrcp, vexp2, vlog2 (x in [1,2)), vsqrt and vrsq:
the reduced argument X is 23 bits (for the roots, the 24-bit exponent-parity
and mantissa index with its last bit dropped); its top 7 bits pick one of
128 segments and the other 16 are u. Inside a segment the core computes,
at two bits below the 22 it keeps,

    Z = floor(D * u / 2^16) + V(|(u >> 6) - 512|)          Y = Z >> 2

a linear term in the full u, plus a term that depends only on the distance
of u's top ten bits from the segment's middle: a quadratic correction whose
values are close to c*d^2/2^17 for a 7-bit integer c, but not exactly any
formula tried. So V is kept as data: its value at the middle, and the steps
from one distance to the next (512 of them, 0 or 1 -- 0 to 2 for vrsq,
whose curve is steeper). A distance the samples leave open (the dumps step
u by 3 or 5, which aliases with some slopes) takes the value closest to a
quadratic through the rest, within the range the samples allow.

vsin/vcos (steps 193-194) and vasin (step 195) are the same core with two
differences. The result does not have a fixed exponent, so each segment
carries one: E, the exponent of the value at the segment's start. Z is at
2^(E-23) and the result is exactly Y * 2^(E-21), Y = Z >> 2, normalised:
fewer than 22 significant bits where the values fall below 2^E, 23 where
vasin's climb past 2^(E+1) (the low mantissa bit is then the only clear
one). The data show E as the finest grid a segment's results sit on, and
that is where it is read from. vasin's argument is x in 23-bit fixed point
(segment 0 has E = -9, one below segment 1's); vsin's is the complement of
the reduced quarter turn r, X = 2^23 - r, so the core is a cosine and the
segment holding r = k * 2^16 is the one ending there (its result is on that
segment's grid, not the next one's). X = 0, r = 2^23, gives Z = 2^24: 1.0.

vasin's V does not keep one direction: it rises with the distance except in
segments 116-120, 122 and 127, where it falls, and its steps reach 4 there.
They are stored as signed 4-bit fields.

v3 alone left 622 of the sin core's distances open, and 1,547 of vasin's,
in the 11 and 13 segments v4 dumps whole. With v4, every distance there is
pinned except where no argument's result depends on it (sin segment 0,
whose slope is so shallow that 210 distances stay a range wide; 6 in sin
segment 56, 3 and 5 in vasin's 98 and 112), so any value in range gives
every result. Of v3's picks, 601 of sin's and 1,485 of vasin's were
right.

For vlog2 of x >= 4 the hardware reuses the log2 core at lower precision
(src/vfpu.c, vfpu_log2). It needs the correction of a reduced coefficient,
which it looks up through VFPU_CORE_LOG2_BYCOEF, emitted after the tables:
a log2 segment with that coefficient or, for 44, 40 and 32, a sqrt one.
The correction is one function of the coefficient across the cores whose
curve bends the same way (log2, sqrt and the cosine core; rcp, exp2, rsq
and vasin share another), which is why sqrt's serves.

--v5 DIR adds a vfpuprobe v5 folder's vfpu_core5_vlog2.bin (set 11,
fw660-v5.txt step 204): every x in [1,2) of the 9 log2 segments v3's dump
left open. 40 distances stay open in segments 55-57, whose slope is so
near 2 per u that no x in [1,2) shows V's low bit at some distances; two
of them, V(0) of 56 and V(512) of 57, set C2, and the x >= 4 results
(the sweep, v4's step 203 and v5's step 205) settle them: 88 in both.
With all three folders the script output is src/vfpu_cores.h:

    gencores.py <set 11>/vfpuprobe-s11 --v4 <set 11> --v5 <set 11> \
        --check --out src/vfpu_cores.h

(a v5 folder holds the v3 and v4 dumps too, byte for byte the same).

--check reports how many dump and sweep samples the fitted tables
reproduce. --holdout fits on the dumps alone and checks the sweeps, which
the fit has not seen.
"""
import argparse
import os
import sys

import numpy as np

SEGBITS, UBITS, GSHIFT, F = 7, 16, 6, 2
NG = 1 << (UBITS - GSHIFT)          # 1024 groups
HALF = NG // 2                       # distances 0..512


def f32(bits):
    with np.errstate(invalid='ignore'):          # NaN inputs stay NaN
        return np.asarray(bits, dtype=np.uint32).view(np.float32).astype(np.float64)


def load(rundir, name):
    return np.fromfile(os.path.join(rundir, name), dtype='<u4')


# ---- samples: (X, Y) with Y the 22 bits the float shows, Z >> 2 == Y ------

def sig22(out_bits):
    """the output's 24-bit significand, top 22 bits"""
    return (((out_bits & 0x7FFFFF) | 0x800000) >> 2).astype(np.int64)


def normal(bits):
    e = (bits >> 23) & 0xFF
    return (e != 0) & (e != 255)


def samples_rcp(rundir, sweeps=True):
    core = load(rundir, 'vfpu_core_vrcp.bin')
    xin = (0x3F800000 + 3 * np.arange(len(core), dtype=np.int64)).astype(np.uint32)
    X = [xin & 0x7FFFFF]; Y = [sig22(core)]
    if sweeps:
        inp = load(rundir, 'vfpu_inputs.bin')
        for op in ('vrcp', 'vnrcp'):
            out = load(rundir, 'vfpu_%s.bin' % op)
            ok = normal(inp) & normal(out)
            X.append(inp[ok] & 0x7FFFFF); Y.append(sig22(out[ok]))
    X = np.concatenate(X).astype(np.int64); Y = np.concatenate(Y)
    keep = X != 0                    # 1/1 is a power of two, outside the binade
    return X[keep], Y[keep]


def samples_exp2(rundir, sweeps=True):
    core = load(rundir, 'vfpu_core_vexp2.bin')
    n0 = 2796203
    x0 = (0x3F800000 + 3 * np.arange(n0, dtype=np.int64)).astype(np.uint32)
    X = [x0 & 0x7FFFFF]; Y = [sig22(core[:n0])]
    # x = 0.5 + i*2^-24: the argument keeps 23 fraction bits, truncated
    i = np.arange(len(core) - n0, dtype=np.int64)
    X.append((0x400000 + (i >> 1)) & 0x7FFFFF); Y.append(sig22(core[n0:]))
    if sweeps:
        inp = load(rundir, 'vfpu_inputs.bin')
        # vrexp2(x) is vexp2(-x)
        for name, flip in (('vfpu_vexp2.bin', 0), ('vfpu_vrexp2.bin', 0x80000000)):
            out = load(rundir, name)
            xb = inp ^ np.uint32(flip)
            ok = normal(out) & normal(inp) & (np.abs(f32(xb)) < 64)
            X.append(exp2_arg(xb[ok])); Y.append(sig22(out[ok]))
    return np.concatenate(X).astype(np.int64), np.concatenate(Y)


def exp2_arg(xb):
    """|x| on a 23-bit fraction grid, truncated; a negative x takes the
    fraction's ones' complement (so 2^x = 2^(-1-n) * core(~frac))"""
    e = ((xb >> 23) & 0xFF).astype(np.int64) - 127
    m24 = (xb & 0x7FFFFF).astype(np.int64) | 0x800000
    fx = np.where(e >= 0, m24 << np.clip(e, 0, 7), m24 >> np.clip(-e, 0, 40))
    return np.where(xb >> 31 != 0, ~fx, fx) & 0x7FFFFF


# vfpuprobe v5's whole log2 segments (main.c CORE5_LOG2): every x in [1,2).
CORE5_LOG2_SEGS = [8, 9, 10, 11, 12, 55, 56, 57, 79]


def samples_log2(rundir, sweeps=True, v5=None):
    core = load(rundir, 'vfpu_core_vlog2.bin')
    n0 = 2796203
    x0 = (0x3F800000 + 3 * np.arange(n0, dtype=np.int64)).astype(np.uint32)
    y = f32(core[:n0])
    X = [x0 & 0x7FFFFF]; Y = [np.round(y * 2.0 ** 22).astype(np.int64)]
    if v5:
        out = load(v5, 'vfpu_core5_vlog2.bin')
        x5 = np.concatenate([(s << UBITS) + np.arange(1 << UBITS, dtype=np.int64)
                             for s in CORE5_LOG2_SEGS])
        assert len(out) == len(x5), 'vfpu_core5_vlog2.bin: %d words, want %d' % (len(out), len(x5))
        X.append(x5); Y.append(np.round(f32(out) * 2.0 ** 22).astype(np.int64))
    if sweeps:
        inp = load(rundir, 'vfpu_inputs.bin')
        out = load(rundir, 'vfpu_vlog2.bin')
        ex = ((inp >> 23) & 0xFF).astype(np.int64) - 127
        ok = normal(inp) & normal(out) & (inp < 0x80000000) & (ex == 0)
        X.append(inp[ok] & 0x7FFFFF); Y.append(np.round(f32(out[ok]) * 2.0 ** 22).astype(np.int64))
    return np.concatenate(X).astype(np.int64), np.concatenate(Y)


def samples_root(rundir, op, sweeps=True):
    """vsqrt and vrsq: the argument is the 24-bit (exponent parity, mantissa)
    index of x in [1, 4) with its last bit dropped"""
    core = load(rundir, 'vfpu_core_%s.bin' % op)
    idx = 5 * np.arange(len(core), dtype=np.int64)
    X = [idx >> 1]; Y = [sig22(core)]; E = [(core >> 23) & 0xFF]
    if sweeps:
        inp = load(rundir, 'vfpu_inputs.bin')
        out = load(rundir, 'vfpu_%s.bin' % op)
        ok = normal(inp) & normal(out) & (inp < 0x80000000)
        ex = ((inp[ok] >> 23) & 0xFF).astype(np.int64)
        i = (((ex - 127) & 1) << 23) | (inp[ok] & 0x7FFFFF).astype(np.int64)
        X.append(i >> 1); Y.append(sig22(out[ok]))
        # the output's exponent relative to the one the index gives
        E.append((((out[ok] >> 23) & 0xFF).astype(np.int64)
                  - ((ex - 127) >> 1) * (1 if op == 'vsqrt' else -1)))
    X = np.concatenate(X).astype(np.int64); Y = np.concatenate(Y)
    E = np.concatenate(E).astype(np.int64)
    # vrsq of an exact power of four is a power of two, one binade up
    want = 127 if op == 'vsqrt' else 126
    keep = E == want
    return X[keep], Y[keep]


def seg_exps(X, out):
    """per segment, E such that the finest grid its results sit on is 2^(E-21)"""
    e = ((out >> 23) & 0xFF).astype(np.int64) - 127
    m = ((out & 0x7FFFFF) | 0x800000).astype(np.int64)
    tz = np.zeros_like(m)
    for b in range(24):
        tz += ((m & ((1 << (b + 1)) - 1)) == 0)
    E = np.full(1 << SEGBITS, 1 << 20, dtype=np.int64)
    np.minimum.at(E, X >> UBITS, e - 23 + tz)
    return E + 21


def seg_y(X, out, E):
    """Y with out = Y * 2^(E-21) for X's segment; every sample must be on its grid"""
    e = ((out >> 23) & 0xFF).astype(np.int64) - 127
    m = ((out & 0x7FFFFF) | 0x800000).astype(np.int64)
    sh = e - E[X >> UBITS] - 2
    Y = np.where(sh >= 0, m << np.clip(sh, 0, 40), m >> np.clip(-sh, 0, 40))
    exact = np.where(sh >= 0, True, (Y << np.clip(-sh, 0, 40)) == m)
    if not exact.all():
        raise ValueError('%d results off their segment\'s grid' % int((~exact).sum()))
    return Y


def quarter_fixed(xb):
    """|x| in quarter turns, 25-bit fixed point (2 quadrant bits), truncated;
    exponents 2^33..2^40 shift by e-127-32"""
    e = ((xb >> 23) & 0xFF).astype(np.int64)
    m24 = (xb & 0x7FFFFF).astype(np.int64) | 0x800000
    sh = e - 127
    x = np.where(sh < 0, np.where(sh > -32, m24 >> np.clip(-sh, 0, 31), 0),
                 np.where(sh <= 32, m24 << np.clip(sh, 0, 32),
                          np.where(sh < 64, m24 << np.clip(sh - 32, 0, 31), 0)))
    return np.where(e == 0, 0, x) & ((1 << 25) - 1)


def trig_arg(xb, cosine):
    """the core's argument 2^23 - r, r the quarter turn reflected into
    [0, 2^23], and whether the result is negated"""
    x = quarter_fixed(xb)
    if cosine:
        x = (x + (1 << 23)) & ((1 << 25) - 1)
    q = x >> 23
    r = np.where(q & 1, (1 << 23) - (x & 0x7FFFFF), x & 0x7FFFFF)
    neg = (q >= 2) ^ ((xb >> 31 != 0) & (not cosine))
    return (1 << 23) - r, neg


# vfpuprobe v4's whole segments (main.c CORES4): every X of each, in order.
CORE4_SEGS = {'vcos': [0, 11, 17, 34, 35, 36, 56, 81, 82, 83, 84],
              'vasin': [10, 34, 35, 36, 37, 38, 39, 40, 73, 82, 98, 112, 119]}


def core4(v4dir, op):
    """(X, out) of vfpu_core4_<op>.bin: vcos and vasin of x = X * 2^-23,
    which for vcos is the shared cosine core at X itself"""
    out = load(v4dir, 'vfpu_core4_%s.bin' % op).astype(np.int64)
    X = np.concatenate([(s << UBITS) + np.arange(1 << UBITS, dtype=np.int64)
                        for s in CORE4_SEGS[op]])
    assert len(out) == len(X), 'vfpu_core4_%s.bin: %d words, want %d' % (op, len(out), len(X))
    return X, out


def samples_sin(rundir, sweeps=True, v4=None):
    """vsin's dump is r = 3k, then x = 1 + 31k*2^-23 (r = 2^23 - 31k), then
    x = 0.5 + i*2^-24 (r = 2^22 + i/2); vcos's dump repeats the second part
    and its own strip is r = 2^22 - i/2. v4 adds whole segments of the core
    by vcos. Sweeps: vsin, vcos and vnsin."""
    core = load(rundir, 'vfpu_core_vsin.bin').astype(np.int64)
    cos = load(rundir, 'vfpu_core_vcos.bin').astype(np.int64)
    n0, n1 = 2796203, 270601
    k = np.arange(n0, dtype=np.int64); i1 = np.arange(n1, dtype=np.int64)
    i2 = np.arange(len(core) - n0 - n1, dtype=np.int64)
    r = [3 * k, (1 << 23) - 31 * i1, (1 << 22) + (i2 >> 1), (1 << 22) - (i2 >> 1)]
    X = [(1 << 23) - v for v in r]
    O = [core[:n0], core[n0:n0 + n1], core[n0 + n1:], cos[n1:]]
    if v4:
        x4, o4 = core4(v4, 'vcos')
        X.append(x4); O.append(o4)
    ndump = len(X)
    if sweeps:
        inp = load(rundir, 'vfpu_inputs.bin').astype(np.int64)
        for name, cosine in (('vfpu_vsin.bin', 0), ('vfpu_vcos.bin', 1), ('vfpu_vnsin.bin', 0)):
            out = load(rundir, name).astype(np.int64)
            ok = normal(inp) & normal(out)
            x, _ = trig_arg(inp[ok], cosine)
            X.append(x); O.append(out[ok] & 0x7FFFFFFF)
    return core_samples(X, O, ndump, (1 << 23) - 1)


def core_samples(X, O, ndump, xmax):
    """(X, Y, E) for the cores with an exponent per segment; E is read off
    the first ndump parts (the dumps), and r = 0 (vsin) or x = 0 (vasin),
    zeros rather than the core, are dropped"""
    X = [np.asarray(x, dtype=np.int64) for x in X]
    keep = [(x >= 0) & (x <= xmax) & normal(o) for x, o in zip(X, O)]
    X = [x[k] for x, k in zip(X, keep)]; O = [o[k] for o, k in zip(O, keep)]
    E = seg_exps(np.concatenate(X[:ndump]), np.concatenate(O[:ndump]))
    X = np.concatenate(X); O = np.concatenate(O)
    return X, seg_y(X, O, E), E


def samples_asin(rundir, sweeps=True, v4=None):
    """the dump is x = 3k * 2^-23, then x = 0.5 + i*2^-24 (X = 2^22 + i/2);
    v4 adds whole segments"""
    core = load(rundir, 'vfpu_core_vasin.bin').astype(np.int64)
    n0 = 2796203
    i2 = np.arange(len(core) - n0, dtype=np.int64)
    X = [3 * np.arange(n0, dtype=np.int64), (1 << 22) + (i2 >> 1)]
    O = [core[:n0], core[n0:]]
    if v4:
        x4, o4 = core4(v4, 'vasin')
        X.append(x4); O.append(o4)
    ndump = len(X)
    if sweeps:
        inp = load(rundir, 'vfpu_inputs.bin').astype(np.int64)
        out = load(rundir, 'vfpu_vasin.bin').astype(np.int64)
        ok = normal(inp) & normal(out) & ((inp & 0x7FFFFFFF) < 0x3F800000)
        e = (inp[ok] >> 23) & 0xFF
        m24 = (inp[ok] & 0x7FFFFF) | 0x800000
        X.append(np.where(127 - e < 32, m24 >> np.clip(127 - e, 1, 31), 0))
        O.append(out[ok] & 0x7FFFFFFF)
    X, Y, E = core_samples(X, O, ndump, (1 << 23) - 1)
    keep = X > 0
    return X[keep], Y[keep], E


OPS = {'rcp': lambda d, sweeps=True, v4=None, v5=None: samples_rcp(d, sweeps),
       'exp2': lambda d, sweeps=True, v4=None, v5=None: samples_exp2(d, sweeps),
       'log2': lambda d, sweeps=True, v4=None, v5=None: samples_log2(d, sweeps, v5),
       'sqrt': lambda d, sweeps=True, v4=None, v5=None: samples_root(d, 'vsqrt', sweeps),
       'rsq': lambda d, sweeps=True, v4=None, v5=None: samples_root(d, 'vrsq', sweeps),
       'sin': lambda d, sweeps=True, v4=None, v5=None: samples_sin(d, sweeps, v4),
       'asin': lambda d, sweeps=True, v4=None, v5=None: samples_asin(d, sweeps, v4)}


# ---- the fit ------------------------------------------------------------------

def intervals(u, Y, D):
    """V(k) for k = |g - 512| lies in [vl, vh]; +-inf where no sample says"""
    T1 = (D * u) >> UBITS
    # Z = T1 + V in [Y << F, (Y + 1) << F)  ->  V in [Y*4 - T1, Y*4 + 3 - T1]
    lo = (Y << F) - T1; hi = ((Y + 1) << F) - 1 - T1
    k = np.abs((u >> GSHIFT) - HALF)
    vl = np.full(HALF + 1, -(1 << 60), dtype=np.int64)
    vh = np.full(HALF + 1, 1 << 60, dtype=np.int64)
    np.maximum.at(vl, k, lo)
    np.minimum.at(vh, k, hi)
    return vl, vh


def resolve(vl, vh):
    """one V per distance: inside [vl, vh], never turning back, steps no
    larger than the curve needs, closest to a quadratic through the
    distances the samples pin down"""
    k = np.arange(HALF + 1, dtype=np.float64)
    known = (vl == vh)
    if known.sum() < 8:
        raise ValueError('too few pinned distances')
    A = np.stack([np.ones(known.sum()), k[known] ** 2], 1)
    coef, *_ = np.linalg.lstsq(A, vl[known].astype(np.float64), rcond=None)
    target = coef[0] + coef[1] * k * k
    sgn = 1 if coef[1] >= 0 else -1
    maxstep = max(1, int(np.ceil(np.abs(np.diff(target)).max())))
    # dynamic programme over k; the state is V(k), a handful of values each
    lo = np.maximum(vl, np.floor(target).astype(np.int64) - 40)
    hi = np.minimum(vh, np.ceil(target).astype(np.int64) + 40)
    if (lo > hi).any():
        raise ValueError('empty interval at distance %d' % int(np.argmax(lo > hi)))
    prev = {int(v): ((v - target[0]) ** 2, None) for v in range(lo[0], hi[0] + 1)}
    back = [prev]
    for i in range(1, HALF + 1):
        cur = {}
        for v in range(lo[i], hi[i] + 1):
            best = None
            for step in range(maxstep + 1):
                p = prev.get(v - sgn * step)
                if p is not None and (best is None or p[0] < best[0]):
                    best = (p[0], v - sgn * step)
            if best is not None:
                cur[v] = (best[0] + (v - target[i]) ** 2, best[1])
        if not cur:
            raise ValueError('no monotone path at distance %d' % i)
        back.append(cur)
        prev = cur
    v = min(prev, key=lambda z: prev[z][0])
    out = [v]
    for i in range(HALF, 0, -1):
        v = back[i][v][1]
        out.append(v)
    return np.array(out[::-1], dtype=np.int64), sgn


def fit(X, Y):
    seg = X >> UBITS
    tables = []
    for s in range(1 << SEGBITS):
        sel = seg == s
        u = X[sel] & ((1 << UBITS) - 1); y = Y[sel]
        c = np.polyfit(u / 2.0 ** UBITS, (y << F).astype(np.float64) + 2, 2)
        D0 = int(round(c[1] + c[0]))
        # Every linear term the samples allow, most constrained first; the
        # first whose V can be monotone wins. More than one can fit where the
        # dump's stride of 3 aliases with the slope.
        cands = []
        for D in range(D0 - 64, D0 + 65):
            vl, vh = intervals(u, y, D)
            if (vh >= vl).all():
                cands.append((int((vh - vl).sum()), D, vl, vh))
        cands.sort(key=lambda t: t[0])
        for _, D, vl, vh in cands:
            try:
                V, sgn = resolve(vl, vh)
            except ValueError:
                continue
            tables.append((D, V, sgn, int((vl != vh).sum()), len(cands)))
            break
        else:
            raise ValueError('segment %d: no linear term fits (%d candidates)' % (s, len(cands)))
    return tables


def predict(tables, X):
    seg = X >> UBITS; u = X & ((1 << UBITS) - 1)
    k = np.abs((u >> GSHIFT) - HALF)
    D = np.array([t[0] for t in tables], dtype=np.int64)[seg]
    V = np.stack([t[1] for t in tables])[seg, k]
    return ((D * u >> UBITS) + V) >> F


# ---- C output -----------------------------------------------------------------

def emit(name, tables, f, exps=None):
    """per segment: D, V(0), and the steps V(k+1) - V(k), k = 0..511, as
    BITS-bit fields packed from the low end of each word; 4-bit fields,
    used where the direction changes from segment to segment, are signed.
    With exps, also each segment's exponent E."""
    sgns = {t[2] for t in tables}
    sgn = sgns.pop() if len(sgns) == 1 else 1
    steps = [(t[1][1:] - t[1][:-1]) * sgn for t in tables]
    top = max(int(np.abs(st).max()) for st in steps)
    if min(int(st.min()) for st in steps) < 0:
        bits = 4
        assert top < 8
    else:
        bits = 1 if top <= 1 else 2
        assert top < (1 << bits)
    nw = HALF * bits // 32
    N = name.upper()
    if bits == 4:
        f.write('/* %s: %d segments; each step moves V by its signed field. */\n' % (name, len(tables)))
    else:
        f.write('/* %s: %d segments; V moves by %+d times each step. */\n' % (name, len(tables), sgn))
    f.write('#define VFPU_CORE_%s_SGN %d\n' % (N, sgn))
    f.write('#define VFPU_CORE_%s_BITS %d\n' % (N, bits))
    if exps is not None:
        f.write('static const int8_t VFPU_CORE_%s_EXP[%d] = {\n' % (N, len(tables)))
        for i in range(0, len(tables), 16):
            f.write('    ' + ' '.join('%d,' % int(x) for x in exps[i:i + 16]) + '\n')
        f.write('};\n')
    f.write('static const int32_t VFPU_CORE_%s_DV[%d][2] = {\n' % (N, len(tables)))
    for i in range(0, len(tables), 4):
        f.write('   ' + ''.join(' {%d,%d},' % (t[0], int(t[1][0])) for t in tables[i:i + 4]) + '\n')
    f.write('};\n')
    f.write('static const uint32_t VFPU_CORE_%s_STEPS[%d][%d] = {\n' % (N, len(tables), nw))
    for st in steps:
        words = [0] * nw
        for i, v in enumerate(st):
            b = i * bits
            words[b >> 5] |= (int(v) & ((1 << bits) - 1)) << (b & 31)
        f.write('    {')
        for j in range(0, nw, 8):
            f.write(('' if j == 0 else '\n     ') + ','.join('0x%08X' % w for w in words[j:j + 8]) + ',')
        f.write('},\n')
    f.write('};\n\n')


# ---- vlog2 of x >= 4 -----------------------------------------------------------

# The x >= 4 dumps: v4's step 203 (main.c CORES4) and v5's step 205
# (CORE5_LOG2W), as (first input, stride, count).
CORE4_LOG2 = ([(e, 255, 32897) for e in (0x40800000, 0x41800000, 0x43800000, 0x47800000,
                                        0x4F800000, 0x5F800000)]
              + [(e, 2040, 4113) for e in (0x41000000, 0x43000000, 0x47000000, 0x4F000000,
                                          0x5F000000, 0x7F000000)]
              + [(e, 1, 2048) for e in (0x40800000, 0x40C07C00, 0x4F800000, 0x4FC07C00)])
CORE5_LOG2W = [(((127 + ex) << 23) | (s << UBITS), 3, 21846) for ex, s in
               [(2, 9), (2, 10), (2, 56), (2, 57), (4, 9), (4, 10), (4, 56), (4, 57),
                (8, 9), (8, 10), (8, 56), (8, 57), (2, 124), (2, 53), (2, 8), (4, 124),
                (4, 50), (4, 53), (4, 6), (4, 8), (8, 124), (8, 3), (8, 6), (16, 100),
                (16, 124), (16, 0), (16, 6)]]


def samples_log2_wide(rundir, v4=None, v5=None):
    """(x, vlog2 x) for x >= 4: the run-1 sweep and, given, v4's step 203
    and v5's step 205"""
    inp = load(rundir, 'vfpu_inputs.bin').astype(np.int64)
    out = load(rundir, 'vfpu_vlog2.bin').astype(np.int64)
    e = ((inp >> 23) & 0xFF) - 127
    ok = normal(inp) & normal(out) & (inp < 0x80000000) & (e >= 2)
    xs = [inp[ok]]; os_ = [out[ok]]
    for d, name, segs in ((v4, 'vfpu_core4_vlog2.bin', CORE4_LOG2),
                          (v5, 'vfpu_core5_vlog2w.bin', CORE5_LOG2W)):
        if d:
            o = load(d, name).astype(np.int64)
            x = np.concatenate([b + s * np.arange(c, dtype=np.int64) for b, s, c in segs])
            assert len(o) == len(x), '%s: %d words, want %d' % (name, len(o), len(x))
            xs.append(x); os_.append(o)
    return np.concatenate(xs), np.concatenate(os_)


def concave_sources(log2, sqrt):
    """C2 -> (core, segment) of the correction for a coefficient C2 =
    V(0) - V(512): the log2 core's segment with that C2 if it has one, else
    the sqrt core's, the one with fewest open distances first"""
    src = {}
    for name, tables in (('log2', log2), ('sqrt', sqrt)):
        for s in sorted(range(len(tables)), key=lambda i: tables[i][3]):
            V = tables[s][1]
            src.setdefault(int(V[0] - V[HALF]), (name, s))
    return src


def correction_table(log2, sqrt):
    """R[C2, k] = V(0) - V(k) from concave_sources; 0 for C2 = 0, -1 where
    no segment has C2"""
    R = np.full((256, HALF + 1), -1, dtype=np.int64)
    R[0] = 0
    for c2, (name, s) in concave_sources(log2, sqrt).items():
        if 0 < c2 < 256:
            V = (log2 if name == 'log2' else sqrt)[s][1]
            R[c2] = V[0] - V
    return R


def log2_wide(log2, R, xb):
    """vlog2 of x >= 4 (src/vfpu.c, log2_wide): the log2 core with D's low
    t + 1 bits cleared, t the bit length of the exponent less one, and the
    correction of C2 cut the same way, rounded to 2^t; 23 bits kept"""
    e = ((xb >> 23) & 0xFF).astype(np.int64) - 127
    m = (xb & 0x7FFFFF).astype(np.int64)
    t = np.frexp(e.astype(np.float64))[1] - 1
    a = t + 1
    seg = m >> UBITS; u = m & ((1 << UBITS) - 1); k = np.abs((u >> GSHIFT) - HALF)
    D = np.array([x[0] for x in log2], dtype=np.int64)[seg]
    V = np.stack([x[1] for x in log2])
    v0 = V[seg, 0]; c2 = v0 - V[seg, HALF]; c2t = (c2 >> a) << a
    r = R[c2t, k]
    assert (r >= 0).all(), 'a cut C2 with no correction: %s' % sorted(set(c2t[r < 0].tolist()))
    tot = ((e << 24) + ((((D >> a) << a) * u) >> 16)
           + (((v0 - (c2 - c2t)) >> t) << t) - (((r + (1 << t) - 1) >> t) << t))
    nb = np.frexp(tot.astype(np.float64))[1]
    sig = tot >> (nb - 23)
    return ((nb - 25 + 127) << 23) | ((sig << 1) & 0x7FFFFF)


def settle_log2_knots(log2, sqrt, X, Y, xw, ow):
    """Where the [1,2) results leave V(0) or V(512) open -- segments 55-57,
    whose slope is so near 2 per u that Y never shows V's low bit in the
    middle or at the knot -- they still decide C2, which x >= 4 reads: take
    the values in range that reproduce the most x >= 4 results, the fitted
    ones on a tie. Returns the segments changed."""
    changed = []
    seg_w = (xw & 0x7FFFFF) >> UBITS
    for s in range(len(log2)):
        sel = (X >> UBITS) == s
        D, V = log2[s][0], log2[s][1]
        vl, vh = intervals(X[sel] & ((1 << UBITS) - 1), Y[sel], D)
        if vl[0] == vh[0] and vl[HALF] == vh[HALF]:
            continue
        w = seg_w == s
        best = None
        for v0 in range(int(vl[0]), int(vh[0]) + 1):
            for vk in range(int(vl[HALF]), int(vh[HALF]) + 1):
                V2 = V.copy(); V2[0] = v0; V2[HALF] = vk
                trial = list(log2); trial[s] = (D, V2) + tuple(log2[s][2:])
                miss = int((log2_wide(trial, correction_table(trial, sqrt), xw[w]) != ow[w]).sum())
                key = (miss, abs(v0 - int(V[0])) + abs(vk - int(V[HALF])))
                if best is None or key < best[0]:
                    best = (key, V2)
        if (best[1] != V).any():
            log2[s] = (D, best[1]) + tuple(log2[s][2:])
            changed.append((s, int(V[0] - V[HALF]), int(best[1][0] - best[1][HALF])))
    return changed


def emit_log2_coef(log2, sqrt, f):
    """vlog2 for x >= 4 takes the quadratic correction of the coefficient
    C2 = V(0) - V(512) with its low bits cleared. That correction is one
    function of C2 for the cores whose curve bends down (log2, sqrt and the
    cosine core: equal C2, equal V(0) - V(k) in all of them), so it comes
    from the log2 core's segment with that C2 or, where none has it, the
    sqrt core's. Every C2 x >= 4 can ask for has one but 0, whose
    correction is 0."""
    src = concave_sources(log2, sqrt)
    by = [-1] * 256
    for c2, (name, s) in src.items():
        if 0 < c2 < 256:
            by[c2] = s if name == 'log2' else 128 + s
    for x in log2:
        c2 = int(x[1][0] - x[1][HALF])
        for t in range(1, 7):
            c2t = (c2 >> (t + 1)) << (t + 1)
            assert c2t == 0 or by[c2t] >= 0, 'cut C2 %d has no segment' % c2t
    f.write('/* log2 for x >= 4: for each C2 = V(0) - V(512), the segment whose\n'
            ' * V(0) - V(k) is its correction: the log2 core\'s, or 128 + the sqrt\n'
            ' * core\'s; -1 where none is needed. */\n')
    f.write('static const int16_t VFPU_CORE_LOG2_BYCOEF[256] = {\n')
    for i in range(0, 256, 16):
        f.write('    ' + ' '.join('%d,' % v for v in by[i:i + 16]) + '\n')
    f.write('};\n\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rundir')
    ap.add_argument('--v4', help='a vfpuprobe v4 folder: its vfpu_core4_*.bin')
    ap.add_argument('--v5', help='a vfpuprobe v5 folder: its vfpu_core5_vlog2.bin')
    ap.add_argument('--out')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--holdout', action='store_true')
    ap.add_argument('--ops', default=','.join(OPS))
    a = ap.parse_args()
    fitted = {}
    for name in a.ops.split(','):
        get = OPS[name]
        X, Y, *E = get(a.rundir, sweeps=not a.holdout, v4=a.v4, v5=a.v5)
        t = fit(X, Y)
        fitted[name] = (t, E[0] if E else None)
        amb = sum(x[3] for x in t)
        if a.check or a.holdout:
            Xa, Ya, *_ = get(a.rundir, sweeps=True, v4=a.v4, v5=a.v5)
            p = predict(t, Xa)
            print('%-5s fitted on %d samples, %d distances open; all samples %d/%d exact'
                  % (name, len(X), amb, int((p == Ya).sum()), len(Ya)))
    if 'log2' in fitted and 'sqrt' in fitted:
        log2 = fitted['log2'][0]; sqrt = fitted['sqrt'][0]
        xw, ow = samples_log2_wide(a.rundir, a.v4, a.v5)
        X, Y = OPS['log2'](a.rundir, sweeps=True, v5=a.v5)
        for s, old, new in settle_log2_knots(log2, sqrt, X, Y, xw, ow):
            print('log2  segment %d: C2 %d -> %d, as x >= 4 has it' % (s, old, new))
        if a.check:
            p = log2_wide(log2, correction_table(log2, sqrt), xw)
            print('log2  x >= 4: %d/%d exact' % (int((p == ow).sum()), len(ow)))
    if a.out:
        with open(a.out, 'w') as f:
            f.write('/* Generated by tools/hwprobe/vfpuprobe/gencores.py from vfpuprobe v3\n'
                    ' * core dumps and run-1 sweeps%s (fw 6.60). Do not edit. */\n\n'
                    % ((' and v4\'s and v5\'s whole segments' if a.v5 else ' and v4\'s whole segments')
                       if a.v4 else ''))
            for name, (t, E) in fitted.items():
                emit(name, t, f, E)
            if 'log2' in fitted and 'sqrt' in fitted:
                emit_log2_coef(fitted['log2'][0], fitted['sqrt'][0], f)


if __name__ == '__main__':
    main()
