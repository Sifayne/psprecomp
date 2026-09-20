#!/usr/bin/env python3
"""Derive texture, transfer, mask and clear facts from our recorded pixel probe.

The stimuli are the ones probe.c builds: texel i of a 32-wide layout is
0xff40YYXX, palette entry i is 0xffII(255-I)II, transfer word i of a 64-wide
layout is 0xaa00YYXX. Every rule below is checked against recorded output
only; no emulator source or runtime implementation is an input.
"""
from pathlib import Path

root = Path(__file__).resolve().parent
pixels, words, extra, summary = {}, {}, {}, {}
for line in (root / 'observed.txt').read_text().splitlines():
    kind, case, a, b, c = (int(w, 16) for w in line.split())
    if kind == 0: summary[case] = (a, b, c)
    elif kind == 1: pixels.setdefault(case, {})[a] = b
    elif kind == 2: words.setdefault(case, {})[a] = b
    elif kind == 4: extra.setdefault(case, {})[a] = b
assert all(s[0] == 0 for s in summary.values()), 'every list completed'


def texel(i): return 0xff400000 | (i & 31) | (((i >> 5) & 0xff) << 8)
def entry(i): return 0xff000000 | (i << 16) | ((255 - i) << 8) | i
def src(i): return 0xaa000000 | (i & 63) | ((i >> 6) << 8)
def rgb(v): return v & 0xffffff
def window(case, w, h): return [[pixels[case][y * 64 + x] for x in range(w)] for y in range(h)]
def same(a, b): return all(rgb(p) == rgb(q) for ra, rb in zip(window(a, 8, 8), window(b, 8, 8)) for p, q in zip(ra, rb))
def expect(case, f, w=8, h=8):
    for y in range(h):
        for x in range(w):
            assert rgb(pixels[case][y * 64 + x]) == rgb(f(x, y)), (case, x, y, hex(pixels[case][y * 64 + x]), hex(f(x, y)))


# --- texture address, stride and size registers ---------------------------
expect(100, lambda x, y: texel(y * 32 + x))            # the SDK encoding samples texel (x, y)
for case in (101, 102, 103): assert same(case, 100)    # bytes above the address nibble are ignored
assert same(104, 100)                                   # the low four address bits are ignored
expect(105, lambda x, y: texel(y * 32 + x + 4))        # +16 bytes is four 8888 texels
assert same(106, 100)                                   # stride bit 11 is ignored
expect(107, lambda x, y: texel(y * 48 + x))            # the stride is honoured in texels
expect(108, lambda x, y: texel(y * 40 + x))
assert same(109, 100)                                   # size exponent bit 4 is ignored
limits = [L for L in (256, 512, 1024, 2048)
          if all(rgb(pixels[110][i]) == rgb(texel((600 + i) % L)) for i in range(16))
          and all(rgb(pixels[111][i]) == rgb(texel(min(600 + i, L - 1))) for i in range(16))]
assert limits == [512], limits                          # a declared 1024 samples as 512, repeat masks, clamp holds
TEX_MAX_SIZE = limits[0]

# --- swizzle: find the block shape that maps every pixel of case 112 ---------
def swizzled(x, y, bw, bh, row_bytes=128):
    bx = x * 4
    block = ((y // bh) * (row_bytes // bw) + bx // bw) * (bw * bh)
    return (block + (y % bh) * bw + bx % bw) // 4
shapes = [(bw, bh) for bw in (8, 16, 32, 64) for bh in (2, 4, 8, 16, 32)
          if all(rgb(pixels[112][y * 64 + x]) == rgb(texel(swizzled(x, y, bw, bh)))
                 for y in range(16) for x in range(16))]
assert shapes == [(16, 8)], shapes
SWIZZLE_BYTES, SWIZZLE_ROWS = shapes[0]
assert same(113, 100) and same(114, 100)                # only TEX_MODE bit 0 swizzles

# --- 16-bit texel and palette formats: high bits replicated downward ----------
def expand(p, fmt):
    if fmt == 0: r, g, b = p & 31, (p >> 5) & 63, (p >> 11) & 31; return ((b << 3 | b >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (r << 3 | r >> 2)
    if fmt == 1: r, g, b = p & 31, (p >> 5) & 31, (p >> 10) & 31; return ((b << 3 | b >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (r << 3 | r >> 2)
    r, g, b = p & 15, (p >> 4) & 15, (p >> 8) & 15; return ((b * 17) << 16) | ((g * 17) << 8) | r * 17
def half(i): return (texel(i // 2) >> (16 * (i & 1))) & 0xffff
for case, fmt in ((115, 0), (116, 1), (117, 2)):
    expect(case, lambda x, y, fmt=fmt: expand(half(y * 32 + x), fmt))

# --- palette indexing: ((raw >> shift) & mask) | start * 16 ------------------
def byte(i): return (texel(i // 4) >> (8 * (i & 3))) & 0xff
def nibble(i): return (byte(i // 2) >> (4 * (i & 1))) & 0xf
def index(raw, shift, mask, start): return ((raw >> shift) & mask) | (start << 4)
expect(118, lambda x, y: entry(index(byte(y * 32 + x), 0, 0xff, 0)))
expect(119, lambda x, y: entry(index(byte(y * 32 + x), 0, 0x0f, 0)))
expect(120, lambda x, y: entry(index(byte(y * 32 + x), 4, 0xff, 0)))
expect(121, lambda x, y: entry(index(byte(y * 32 + x), 0, 0xff, 1)))
expect(122, lambda x, y: entry(index(nibble(y * 32 + x), 0, 0xff, 0)))
expect(123, lambda x, y: entry(index(half(y * 32 + x), 0, 0xff, 0)))
expect(124, lambda x, y: entry(index(texel(y * 32 + x), 0, 0xff, 0)))
expect(125, lambda x, y: entry(index(texel(y * 32 + x), 8, 0xff, 0)))
def entry16(i): return (entry(i // 2) >> (16 * (i & 1))) & 0xffff
for case, fmt in ((126, 0), (127, 1), (128, 2)):
    expect(case, lambda x, y, fmt=fmt: expand(entry16(byte(y * 32 + x)), fmt))

# --- modulation: texel * (vertex + vertex >> 7), doubled before the shift ------
def modulate(t, c, double):
    return [min(255, ((((t >> s) & 255) * (((c >> s) & 255) + (((c >> s) & 255) >> 7)) * (2 if double else 1)) >> 8)) << s for s in (0, 8, 16)]
def packed(parts): return sum(parts)
expect(129, lambda x, y: packed(modulate(texel(y * 32 + x), 0x80c08040, 0)))
expect(134, lambda x, y: packed(modulate(texel(y * 32 + x), 0x80c08040, 1)))
expect(141, lambda x, y: packed(modulate(entry(byte(y * 32 + x)), 0xffffffff, 0)), 4, 1)
expect(142, lambda x, y: packed(modulate(entry(byte(y * 32 + x)), 0x80808080, 0)), 4, 1)
expect(130, lambda x, y: texel(y * 32 + x))            # decal with an opaque texel is the texel
expect(133, lambda x, y: texel(y * 32 + x))            # replace, RGB

# --- wrap and nearest magnification ----------------------------------------
expect(135, lambda x, y: texel(y * 32 + min(24 + x, 31)), 16, 2)
expect(136, lambda x, y: texel(y * 32 + ((24 + x) & 31)), 16, 2)
expect(138, lambda x, y: texel((y // 2) * 32 + x // 2))

# --- colour masks: a set bit keeps the framebuffer's bit at 0xE8, not 0xD8 ----
assert all(pixels[139][i] == 0x99c0ffc0 for i in range(4)), [hex(pixels[139][i]) for i in range(4)]
assert all(pixels[140][i] == 0x99ffffff for i in range(4))
assert all(pixels[143][i] == 0x99fefdfc for i in range(4))
MASK_COLOR, MASK_ALPHA = 0xe8, 0xe9

# --- block transfers ---------------------------------------------------------
def region(case, f, w=16, h=8):
    for y in range(h):
        for x in range(w):
            assert words[case][y * 64 + x] == f(x, y), (case, x, y, hex(words[case][y * 64 + x]), hex(f(x, y)))
def halves(i): return (src(i // 2) >> (16 * (i & 1))) & 0xffff
def pair(i): return halves(i) | (halves(i + 1) << 16)
region(200, lambda x, y: src((4 + y) * 64 + 4 + x))                      # 32-bit, positions in pixels
region(203, lambda x, y: src((4 + y) * 64 + 4 + x))                      # bit 0 selects 32-bit
# 16-bit rows are 64 halfwords apart, so the 64-word window sees every second
# destination row; positions and strides count halfwords.
region(201, lambda x, y: pair((4 + 2 * y) * 64 + 4 + 2 * x) if x < 8 else 0)
region(202, lambda x, y: pair((4 + 2 * y) * 64 + 4 + 2 * x) if x < 8 else 0)  # bit 1 does not select 32-bit
region(214, lambda x, y: pair(2 * y * 64 + 1 + 2 * x) if x < 8 else 0)         # an odd 16-bit source position
region(204, lambda x, y: src((y - 3) * 64 + x - 2) if x >= 2 and y >= 3 else 0)
region(205, lambda x, y: src((4 + y) * 64 + 4 + x))                      # stride bit 11 ignored
region(207, lambda x, y: src((4 + y) * 64 + 4 + x))                      # stride low three bits ignored
region(208, lambda x, y: src((4 + x // 8) * 64 + 4 + x % 8) if y == 0 else 0)  # stride 8
region(206, lambda x, y: src(19 * 64 + 4 + x) if y == 0 else 0)          # stride 1032 -> zero: rows coincide
region(217, lambda x, y: src(4 + x))                                     # zero source stride rereads one line
region(209, lambda x, y: src(y * 64 + x))                                # the low four address bits are ignored
region(210, lambda x, y: src(y * 64 + 4 + x))                            # +16 bytes is four words
region(211, lambda x, y: 0)                                              # high byte 0x18: no memory, nothing written
region(212, lambda x, y: src((4 + y) * 64 + 4 + x))                      # high byte 0x88 reaches the same memory
region(216, lambda x, y: src((4 + y) * 64 + 4 + x))                      # and so does 0x48
region(213, lambda x, y: src((4 + y) * 64 + 4 + x) if x < 17 and y < 3 else 0, 16, 8)
for case, stride, honoured in ((218, 0x400, True), (219, 0x3f8, True), (220, 0x7f8, False), (221, 0x404, True)):
    at = stride & 0x7f8
    if honoured:
        assert all(words[case][x] == src(4 * 64 + 4 + x) and extra[case][at + x] == src(5 * 64 + 4 + x) for x in range(16)), case
    else:
        assert all(words[case][x] == src(5 * 64 + 4 + x) and extra[case][at + x] == 0 for x in range(16)), case
XFER_STRIDE_MASK, XFER_STRIDE_LIMIT = 0x7f8, 0x400

# --- clear mode and the depth test --------------------------------------------
def clear(case, value, w=8, h=2): assert all(pixels[case][y * 64 + x] == value for y in range(h) for x in range(w)), (case, hex(pixels[case][0]))
clear(300, 0x44101010)   # a draw at 0x7000 fails GEQUAL against the clear's 0x8000; stencil untouched
clear(301, 0x4400ff00)   # 0x9000 passes; an ordinary draw leaves the alpha byte
clear(302, 0x44202020)   # a colour-only clear does not write depth
clear(303, 0x4400ff00)   # a depth-only clear does
clear(320, 0x44101010)   # and leaves colour alone
clear(304, 0x77404040)   # the stencil bit writes the alpha byte
clear(305, 0x4400ff00)   # a clear under a NEVER test still writes depth
clear(321, 0x44505050)   # and colour
clear(306, 0x4400ff00)   # a clear with depth writes masked off still writes depth
clear(307, 0x44707070)   # a colour-only clear under ALWAYS with writes on does not
clear(308, 0x440000ff, 4, 2)   # an ordinary draw's alpha is not written
passes = {0: '', 1: 'lmr', 2: 'm', 3: 'lr', 4: 'l', 5: 'lm', 6: 'r', 7: 'mr'}
colours = {'l': 0x000000ff, 'm': 0x0000ff00, 'r': 0x00ff0000}
for func, seen in passes.items():
    for slot, key in enumerate('lmr'):
        want = colours[key] if key in seen else 0x00101010
        assert all(pixels[309 + func][slot * 4 + x] == want for x in range(4)), (func, key)
clear(317, 0x0000ff00, 4, 1)   # test disabled: NEVER does not apply
clear(318, 0x0000ff00, 4, 1)   # depth writes masked: the second draw still passes
clear(319, 0x000000ff, 4, 1)   # depth written: it fails

lines = ['/* Generated by tests/provenance/pixels/derive.py from our probe stdout. */',
         f'#define GE_TEX_MAX_SIZE {TEX_MAX_SIZE}',
         f'#define GE_SWIZZLE_BLOCK_BYTES {SWIZZLE_BYTES}',
         f'#define GE_SWIZZLE_BLOCK_ROWS {SWIZZLE_ROWS}',
         f'#define GE_XFER_STRIDE_MASK 0x{XFER_STRIDE_MASK:x}',
         '/* The largest tested stride still honoured; 0x7f8 collapsed to zero. */',
         f'#define GE_XFER_STRIDE_LIMIT 0x{XFER_STRIDE_LIMIT:x}',
         f'#define GE_MASKRGB 0x{MASK_COLOR:X}',
         f'#define GE_MASKALPHA 0x{MASK_ALPHA:X}']
(root.parents[2] / 'src/hle/ge_observed.h').write_text('\n'.join(lines) + '\n')
print('\n'.join(lines[1:]))
print(f'{len(summary)} cases checked')
