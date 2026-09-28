#!/usr/bin/env python3
"""Fit the VFPU's piecewise-quadratic cores to vfpuprobe's hardware dumps.

usage: gencores.py RUNDIR [--out FILE] [--check] [--holdout] [--ops a,b]

RUNDIR is a vfpuprobe v3 result folder with vfpu_core_<op>.bin, the run-1
sweep files vfpu_<op>.bin and vfpu_inputs.bin (fw660-run4 has all of them).
Everything in the output comes from those files; nothing is computed from a
math library except starting points for a search whose answer the data
decides. src/vfpu_cores.h is this script's output for fw660-run4:

    gencores.py /mnt/project-files/hwresults/fw660-run4/vfpuprobe \
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


def samples_log2(rundir, sweeps=True):
    core = load(rundir, 'vfpu_core_vlog2.bin')
    n0 = 2796203
    x0 = (0x3F800000 + 3 * np.arange(n0, dtype=np.int64)).astype(np.uint32)
    y = f32(core[:n0])
    X = [x0 & 0x7FFFFF]; Y = [np.round(y * 2.0 ** 22).astype(np.int64)]
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


OPS = {'rcp': samples_rcp, 'exp2': samples_exp2, 'log2': samples_log2,
       'sqrt': lambda d, sweeps=True: samples_root(d, 'vsqrt', sweeps),
       'rsq': lambda d, sweeps=True: samples_root(d, 'vrsq', sweeps)}


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

def emit(name, tables, f):
    """per segment: D, V(0), and the steps V(k+1) - V(k), k = 0..511, as
    BITS-bit fields packed from the low end of each word"""
    sgns = {t[2] for t in tables}
    assert len(sgns) == 1, 'mixed step signs'
    sgn = sgns.pop()
    steps = [(t[1][1:] - t[1][:-1]) * sgn for t in tables]
    top = max(int(st.max()) for st in steps)
    assert min(int(st.min()) for st in steps) >= 0
    bits = 1 if top <= 1 else 2
    assert top < (1 << bits)
    nw = HALF * bits // 32
    N = name.upper()
    f.write('/* %s: %d segments; V moves by %+d times each step. */\n' % (name, len(tables), sgn))
    f.write('#define VFPU_CORE_%s_SGN %d\n' % (N, sgn))
    f.write('#define VFPU_CORE_%s_BITS %d\n' % (N, bits))
    f.write('static const int32_t VFPU_CORE_%s_DV[%d][2] = {\n' % (N, len(tables)))
    for i in range(0, len(tables), 4):
        f.write('   ' + ''.join(' {%d,%d},' % (t[0], int(t[1][0])) for t in tables[i:i + 4]) + '\n')
    f.write('};\n')
    f.write('static const uint32_t VFPU_CORE_%s_STEPS[%d][%d] = {\n' % (N, len(tables), nw))
    for st in steps:
        words = [0] * nw
        for i, v in enumerate(st):
            b = i * bits
            words[b >> 5] |= int(v) << (b & 31)
        f.write('    {')
        for j in range(0, nw, 8):
            f.write(('' if j == 0 else '\n     ') + ','.join('0x%08X' % w for w in words[j:j + 8]) + ',')
        f.write('},\n')
    f.write('};\n\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rundir')
    ap.add_argument('--out')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--holdout', action='store_true')
    ap.add_argument('--ops', default=','.join(OPS))
    a = ap.parse_args()
    fitted = {}
    for name in a.ops.split(','):
        get = OPS[name]
        X, Y = get(a.rundir, sweeps=not a.holdout)
        t = fit(X, Y)
        fitted[name] = t
        amb = sum(x[3] for x in t)
        if a.check or a.holdout:
            Xa, Ya = get(a.rundir, sweeps=True)
            p = predict(t, Xa)
            print('%-5s fitted on %d samples, %d distances open; all samples %d/%d exact'
                  % (name, len(X), amb, int((p == Ya).sum()), len(Ya)))
    if a.out:
        with open(a.out, 'w') as f:
            f.write('/* Generated by tools/hwprobe/vfpuprobe/gencores.py from vfpuprobe v3\n'
                    ' * core dumps and run-1 sweeps (fw 6.60). Do not edit. */\n\n')
            for name, t in fitted.items():
                emit(name, t, f)


if __name__ == '__main__':
    main()
