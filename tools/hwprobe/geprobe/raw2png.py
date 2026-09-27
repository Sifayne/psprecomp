#!/usr/bin/env python3
"""Convert geprobe's raw framebuffer dumps to PNG.

    raw2png.py ge_06_dither_bayer_5650.raw [more.raw ...]

Each file is 480x272, rows packed. The pixel format comes from the size (4
bytes per pixel is 8888) and, for 16-bit files, from the name: _5650, _5551,
_4444, or _depth (shown as grey). 16-bit files with none of those in the name
are read as 5650. Alpha is dropped. Standard library only."""
import struct, sys, zlib

W, H = 480, 272

def rgb(data, fmt):
    out = bytearray()
    if fmt == "8888":
        for i in range(0, len(data), 4):
            out += data[i:i + 3]
        return out
    for (v,) in struct.iter_unpack("<H", data):
        if fmt == "5650":
            r, g, b = v & 31, (v >> 5) & 63, v >> 11
            out += bytes((r * 255 // 31, g * 255 // 63, b * 255 // 31))
        elif fmt == "5551":
            r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
            out += bytes((r * 255 // 31, g * 255 // 31, b * 255 // 31))
        elif fmt == "4444":
            r, g, b = v & 15, (v >> 4) & 15, (v >> 8) & 15
            out += bytes((r * 17, g * 17, b * 17))
        else:  # depth
            out += bytes((v >> 8,) * 3)
    return out

def png(path, pixels):
    raw = b"".join(b"\0" + bytes(pixels[y * W * 3:(y + 1) * W * 3]) for y in range(H))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))

for name in sys.argv[1:]:
    data = open(name, "rb").read()
    if len(data) == W * H * 4:
        fmt = "8888"
    else:
        fmt = next((f for f in ("5551", "4444", "depth") if f in name), "5650")
    png(name[:-4] + ".png", rgb(data, fmt))
    print(name, fmt)
