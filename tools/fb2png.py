#!/usr/bin/env python3
"""Convert a raw 8-bit grayscale framebuffer dump (Y8, 1072x1448, stride 1072) to PNG.
Usage: fb2png.py dump.raw out.png [width height stride]"""
import sys, zlib, struct
src, dst = sys.argv[1], sys.argv[2]
w = int(sys.argv[3]) if len(sys.argv) > 3 else 1072
h = int(sys.argv[4]) if len(sys.argv) > 4 else 1448
stride = int(sys.argv[5]) if len(sys.argv) > 5 else w
data = open(src, 'rb').read()
rows = b''.join(b'\x00' + data[y*stride:y*stride+w] for y in range(h))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b'')
open(dst, 'wb').write(png)
print(dst, w, h)
