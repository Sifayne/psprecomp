#!/usr/bin/env python3
"""Derive integer weight divisors from our own magnified-translation experiment.

No emulator source or runtime implementation is an input. The recorded pixels
are independent output observations; cases.json describes only our stimuli.
"""
import collections
import json
import math
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parent
cases = json.loads((ROOT / 'cases.json').read_text())
pixels = collections.defaultdict(list)
for line in (ROOT / 'observed.txt').read_text().splitlines():
    kind, case, x, y, rgba = (int(word, 16) for word in line.split())
    if kind == 1:
        pixels[case].append((x, y, rgba))


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


# Fine steps span a pixel boundary without integer-format assumptions. This
# bounds the effective point bias; it does not prove a general raster rule.
lower, upper = -math.inf, math.inf
for case in cases:
    if case['group'] != 'subpixel_fine':
        continue
    screen = f32(32 + f32(16 * case['weights'][0]))
    x = pixels[case['id']][0][0]
    lower, upper = max(lower, x-screen), min(upper, x+1-screen)
assert lower < upper
print(f'Point bias interval: [{lower}, {upper}) pixels')
bias = lower

for fmt, bits in ((1, 8), (2, 16)):
    zoom_cases = [c for c in cases if c['group'] == 'zoom' and c['weight_format'] == fmt]
    zoom = 1 << (bits - 6)
    candidates = []
    for divisor in range(1, 1 << bits):
        matches = True
        for case in zoom_cases:
            weight = f32(case['weights'][0] / divisor)
            model = f32(weight * zoom)
            screen = f32(32 + f32(32 * f32(model + case['world_x'])))
            if math.floor(screen + bias) != pixels[case['id']][0][0]:
                matches = False
                break
        if matches:
            candidates.append(divisor)
    assert len(candidates) == 1, candidates
    print(f'Unsigned {bits}-bit weight divisor: {candidates[0]} (unique among 1..{(1 << bits)-1})')

# Walking-bit RGB observations independently establish bit placement and
# replication. Alpha is not written by these ordinary framebuffer draws.
for fmt, widths in ((4, (5, 6, 5)), (5, (5, 5, 5)), (6, (4, 4, 4))):
    checked = 0
    for case in cases:
        if case['group'] != 'color' or ((case['vertex_type'] >> 2) & 7) != fmt:
            continue
        raw = case['packed_color']
        shift, color = 0, 0
        for channel, width in enumerate(widths):
            component = (raw >> shift) & ((1 << width)-1)
            expanded = (component << (8-width)) | (component >> (2*width-8))
            color |= expanded << (8*channel)
            shift += width
        expected = [(32, 32, color)] if color else []
        assert pixels[case['id']] == expected, case['name']
        checked += 1
    print(f'Packed RGB format {fmt}: {checked} observed basis/pattern cases match')
