#!/usr/bin/env python3
"""Create controlled vertex bytes and matrix inputs; no expected output model.

SDK format flags/order define the experiments. Natural member alignment is
an explicit input-layout hypothesis, tested by the observed raster output.
The poison padding and varied widths expose incorrect interpretation.
"""
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parent
cases = []


def fbits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def palette(translations=None):
    translations = translations or [(0.5, 0, 0)] * 8
    return [fbits(v) for xyz in translations
            for v in (1, 0, 0, 0, 1, 0, 0, 0, 1, *xyz)]


def vertex(weights, wf, pos, pf=3, tf=0, cf=7, nf=0, color=0xff40a0e0, minimum_alignment=1, normal=(0, 0, .5), packed_color=None):
    raw = bytearray()
    alignment = 1

    def field(fmt, values):
        nonlocal alignment
        width = struct.calcsize('<' + fmt)
        boundary = max(width, minimum_alignment)
        alignment = max(alignment, boundary)
        raw.extend(b'\xcc' * ((-len(raw)) % boundary))
        raw.extend(struct.pack('<' + fmt * len(values), *values))

    field({1: 'B', 2: 'H', 3: 'f'}[wf], weights)
    if tf:
        field({1: 'B', 2: 'H', 3: 'f'}[tf], (0, 0))
    if cf:
        packed = packed_color if packed_color is not None else {4: 0xf81f, 5: 0xfc1f, 6: 0xff0f, 7: color}[cf]
        field('I' if cf == 7 else 'H', (packed,))
    if nf:
        scale = {1: 128, 2: 32768, 3: 1}[nf]
        field({1: 'b', 2: 'h', 3: 'f'}[nf],
              [int(v*scale) if nf != 3 else v for v in normal])
    field({1: 'b', 2: 'h', 3: 'f'}[pf], pos)
    raw.extend(b'\xcc' * ((-len(raw)) % alignment))
    return raw


def add(name, weights, wf, positions, pf=3, tf=0, cf=7, nf=0, through=0,
        bones=None, minimum_alignment=1, group='layout', world_x=0,
        normal=(0, 0, .5), lighting=0, packed_color=None, primitive=0):
    data = b''.join(vertex(weights, wf, pos, pf, tf, cf, nf, color,
                           minimum_alignment, normal, packed_color)
                    for pos, color in zip(positions, (0xff40a0e0, 0xff80d020, 0xffe02080)))
    assert len(data) <= 256 and 1 <= len(weights) <= 8
    vtype = tf | (cf << 2) | (nf << 5) | (pf << 7) | (wf << 9) | ((len(weights)-1) << 14) | (through << 23)
    cases.append(dict(id=100+len(cases), name=name, group=group, weights=weights,
                      weight_format=wf, vertex_type=vtype, positions=positions,
                      count=len(positions), bytes=list(data), bones=bones or palette(),
                      world_x=world_x, normal=normal, lighting=lighting,
                      packed_color=packed_color, primitive=primitive))


# Input powers and adjacent values find normalization boundaries without
# embedding a denominator in the probe or its oracle.
for wf, bits in ((1, 8), (2, 16)):
    values = sorted({0, (1 << bits)-1} |
                    {v for bit in range(bits) for v in ((1 << bit)-1, 1 << bit, (1 << bit)+1)
                     if 0 <= v < (1 << bits)})
    for value in values:
        add(f'weight{bits}_{value}', [value], wf, [(0, 0, 0)], group='normalization')
for value in (-0.5, 0, 0.125, 0.5, 1, 1.5):
    add(f'weight_float_{value}', [value], 3, [(0, 0, 0)], group='normalization')

# Magnify the small differences near each integer format's midpoint using
# a large bone translation and an opposing world translation. This separates
# adjacent possible divisors that an ordinary 64-pixel viewport cannot resolve.
for wf, bits in ((1, 8), (2, 16)):
    midpoint = 1 << (bits-1)
    zoom = 1 << (bits-6)
    for value in range(midpoint-8, midpoint+9):
        add(f'weight{bits}_zoom_{value}', [value], wf, [(0, 0, 0)],
            bones=palette([(zoom, 0, 0)]*8), world_x=-zoom, group='zoom')

# Probe subpixel conversion separately using floats around one, without
# embedding any integer-weight normalization or raster rounding rule.
offsets = {0} | {sign*((1 << bit)+delta) for bit in range(19)
                 for sign in (-1, 1) for delta in (-1, 0, 1)}
for offset in sorted(offsets):
    value = struct.unpack('<f', struct.pack('<I', 0x3f800000+offset))[0]
    add(f'float_boundary_{offset}', [value], 3, [(0, 0, 0)], group='subpixel')

translations = [((i % 4 - 1.5)/8, (i//4 - 0.5)/8, 0) for i in range(8)]
for wf in (1, 2, 3):
    # Unit values here are experimental stimuli; their interpretation is
    # checked by the preceding independent normalization observations.
    unit = {1: 128, 2: 32768, 3: 1.0}[wf]
    for count in range(1, 9):
        weights = [0]*count
        weights[-1] = unit
        add(f'last_bone_fmt{wf}_count{count}', weights, wf,
            [(-0.25, 0.25, 0), (0, 0.25, 0), (0.25, 0.25, 0)],
            bones=palette(translations), group='palette')
    add(f'unnormalized_mix_fmt{wf}', [unit, unit], wf,
        [(0, 0, 0), (0.125, 0, 0), (0.25, 0, 0)],
        bones=palette(translations), group='palette')
    for through in (0, 1):
        for pf in (1, 2, 3):
            for tf, cf, nf in ((0, 0, 1), (1, 4, 1), (2, 5, 2), (3, 7, 3), (0, 6, 0)):
                if through:
                    positions = [(10, 10, 0), (20, 10, 0), (30, 10, 0)]
                else:
                    scale = {1: 128, 2: 32768, 3: 1}[pf]
                    positions = [(v*scale, 0.25*scale, 0) for v in (-0.25, 0, 0.25)]
                    if pf != 3:
                        positions = [tuple(int(x) for x in pos) for pos in positions]
                add(f'layout_w{wf}_p{pf}_t{tf}_c{cf}_n{nf}_through{through}',
                    [unit, 0, 0], wf, positions, pf, tf, cf, nf, through,
                    bones=palette(translations))

# Packed color basis probes resolve channel placement and expansion.
for cf in (4, 5, 6):
    for raw in sorted({0, 0xffff, 0x5555, 0xaaaa} | {1 << bit for bit in range(16)}):
        add(f'color{cf}_{raw:04x}', [1.0], 3, [(0,0,0)], cf=cf,
            bones=palette([(0,0,0)]*8), packed_color=raw, group='color')

# Refine the independently observed transition between pixels 47 and 48.
for step in range(65):
    add(f'float_fine_{step}', [1-step/16384], 3, [(0, 0, 0)], group='subpixel_fine')

# A directional +Z light reports how the palette transforms the normal.
# Translation varies independently, and a second matrix rotates +X to +Z.
for wf in (1, 2, 3):
    unit = {1: 128, 2: 32768, 3: 1.0}[wf]
    for nf in (1, 2, 3):
        for mode in range(5):
            bones = palette([(0, 0, 0)]*8)
            if mode == 1:
                bones[9:12] = [fbits(v) for v in (.25, -.25, .5)]
            if mode >= 2:
                bones[12:24] = [fbits(v) for v in (0,0,1, 0,1,0, -1,0,0, 0,0,0)]
            weights = [unit] if mode < 2 else [0, unit] if mode == 2 else [unit//2 if wf != 3 else .5]*2
            normal = (0,0,.5) if mode < 2 else (.5,0,0) if mode < 4 else (0,0,-.5)
            add(f'normal_w{wf}_n{nf}_mode{mode}', weights, wf, [(0,0,0)],
                nf=nf, bones=bones, lighting=1, normal=normal, group='normal')

# Perspective triangles select the host's model-transform backend. All z=0
# makes W=1 while still exercising that branch. Constant color isolates coverage.
# Quarter/eighth-pixel offsets avoid samples exactly on an edge. The earlier
# integer-aligned capture exposed a separate edge-ownership discrepancy; these
# cases measure the weighted transform without assuming either edge-fill rule.
for wf in (1, 2, 3):
    unit = {1: 128, 2: 32768, 3: 1.0}[wf]
    for mode in range(3):
        bones = palette([(0,0,0)]*8)
        bones[12:24] = [fbits(v) for v in (0,1,0, -1,0,0, 0,0,1, .25,0,0)]
        weights = [unit,0] if mode==0 else [0,unit] if mode==1 else [unit,unit]
        add(f'triangle_w{wf}_mode{mode}', weights, wf,
            [(-.25+1/128,.25+1/256,0),(.25+1/128,.25+1/256,0),(-.25+1/128,-.25+1/256,0)],
            bones=bones, packed_color=0xff0080ff, primitive=3, group='triangle')

lines = ['/* Generated by make_cases.py: original probe inputs, no oracle. */',
         'typedef struct { u32 id,type,count; unsigned char bytes[256]; u32 bones[96]; u32 world_x,lighting,primitive; } VertexCase;',
         'static const VertexCase vertex_cases[] __attribute__((aligned(16)))={']
for case in cases:
    data = ','.join(f'0x{x:02x}' for x in case['bytes'])
    bones = ','.join(f'0x{x:08x}' for x in case['bones'])
    lines += [f'{{{case["id"]},0x{case["vertex_type"]:08x},{case["count"]},{{{data}}},{{{bones}}},0x{fbits(case["world_x"]):08x},{case["lighting"]},{case["primitive"]}}},']
lines += ['};', f'#define VERTEX_CASE_COUNT {len(cases)}']
(ROOT/'cases.h').write_text('\n'.join(lines)+'\n')
(ROOT/'cases.json').write_text(json.dumps(cases, indent=2)+'\n')
print(f'Wrote {len(cases)} controlled input cases')
