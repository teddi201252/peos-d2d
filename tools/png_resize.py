#!/usr/bin/env python3
"""A PNG made smaller, with nothing but Python's standard library (where
macOS's sips is not there: Linux, WSL).

    python3 tools/png_resize.py in.png out.png WIDTH HEIGHT

Reads 8-bit PNGs (grey, grey+alpha, RGB, RGBA, palette; not interlaced),
averages the source pixels each output pixel covers (alpha-weighted, so
transparent edges do not darken), writes an RGBA PNG.
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, palette, trns = 8, [], None, None
    w = h = depth = ctype = interlace = None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if depth != 8 or interlace:
        raise ValueError("only 8-bit, non-interlaced PNGs")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b"".join(idat))
    stride = w * channels
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 255
        rows.append(line)
        prev = line
    rgba = []
    for line in rows:
        out = []
        for x in range(w):
            px = line[x * channels:(x + 1) * channels]
            if ctype == 0:
                out.append((px[0], px[0], px[0], 255))
            elif ctype == 4:
                out.append((px[0], px[0], px[0], px[1]))
            elif ctype == 2:
                out.append((px[0], px[1], px[2], 255))
            elif ctype == 6:
                out.append(tuple(px))
            else:
                r, g, b = palette[px[0]]
                a = trns[px[0]] if trns and px[0] < len(trns) else 255
                out.append((r, g, b, a))
        rgba.append(out)
    return w, h, rgba


def resize(w, h, rgba, nw, nh):
    out = []
    for oy in range(nh):
        y0, y1 = oy * h / nh, (oy + 1) * h / nh
        row = []
        for ox in range(nw):
            x0, x1 = ox * w / nw, (ox + 1) * w / nw
            acc = [0.0, 0.0, 0.0, 0.0]
            area = 0.0
            for sy in range(int(y0), min(h, int(y1 + 0.999999))):
                wy = min(y1, sy + 1) - max(y0, sy)
                if wy <= 0:
                    continue
                for sx in range(int(x0), min(w, int(x1 + 0.999999))):
                    wx = min(x1, sx + 1) - max(x0, sx)
                    if wx <= 0:
                        continue
                    r, g, b, a = rgba[sy][sx]
                    k = wx * wy
                    acc[0] += r * a * k
                    acc[1] += g * a * k
                    acc[2] += b * a * k
                    acc[3] += a * k
                    area += k
            alpha = acc[3] / area if area else 0
            if acc[3]:
                px = (round(acc[0] / acc[3]), round(acc[1] / acc[3]), round(acc[2] / acc[3]), round(alpha))
            else:
                px = (0, 0, 0, 0)
            row.append(px)
        out.append(row)
    return out


def write_png(path, w, h, rgba):
    raw = bytearray()
    for row in rgba:
        raw.append(0)
        for px in row:
            raw.extend(px)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    if len(sys.argv) != 5:
        print("usage: png_resize.py in.png out.png WIDTH HEIGHT", file=sys.stderr)
        return 2
    w, h, rgba = read_png(sys.argv[1])
    nw, nh = int(sys.argv[3]), int(sys.argv[4])
    write_png(sys.argv[2], nw, nh, resize(w, h, rgba, nw, nh))
    return 0


if __name__ == "__main__":
    sys.exit(main())
