#!/usr/bin/env python3
"""Cut res/mactab.ico from the source frames in res/icon/.

This used to draw the icon procedurally, because there was not one. There is
now, so this reads it instead. See res/icon/README.md for where those frames
come from and why the set is the shape it is.

Four of the seven .ico frames have a source rendered at exactly that size. The
other three are reduced from a larger one by a whole number, which is the only
kind of reduction a box filter does without argument: every output pixel is the
average of a whole number of input pixels, with nothing weighted fractionally.

Entries at 64px and below are stored as classic BMP (BITMAPINFOHEADER + BGRA XOR
data + 1bpp AND mask), because some Windows shell surfaces still mis-handle
PNG-compressed entries at small sizes. The 256px entry is PNG, which is the
documented Vista+ path and keeps the file small.

The sources must be 8 bit sRGB. A .ico frame carries no colour management, so
the shell reads its bytes as sRGB whatever they were tagged as, and mixing
colour spaces across frames makes the icon change colour as the shell picks
between them. res/icon/README.md has the conversion command.

Stdlib only. Run from anywhere:  python3 tools/make_icon.py
"""

import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "res", "icon")
OUT = os.path.join(HERE, "..", "res", "mactab.ico")

# (frame size, source file, reduction factor). A factor of 1 means the source
# was rendered at this size and is used as it is.
FRAMES = [
    (16,  "mactab-16.png",  1),
    (20,  "mactab-40.png",  2),
    (24,  "mactab-192.png", 8),
    (32,  "mactab-32.png",  1),
    (48,  "mactab-192.png", 4),
    (64,  "mactab-64.png",  1),
    (256, "mactab-256.png", 1),
]


def read_png(path):
    """Decode an 8 bit RGBA PNG to (size, [(r, g, b, a), ...]) top-down."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("%s is not a PNG" % path)

    pos = 8
    idat = b""
    width = height = depth = ctype = None

    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype != 6 or interlace != 0:
                raise SystemExit(
                    "%s must be 8 bit RGBA and not interlaced (got depth %d, "
                    "colour type %d, interlace %d). See res/icon/README.md."
                    % (path, depth, ctype, interlace))
            if width != height:
                raise SystemExit("%s is %dx%d, expected a square" % (path, width, height))
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
        pos += 12 + length

    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    prev = bytearray(stride)
    p = 0

    for _ in range(height):
        filt = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if filt == 1:
                line[i] = (line[i] + a) & 255
            elif filt == 2:
                line[i] = (line[i] + b) & 255
            elif filt == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif filt == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 255
            elif filt != 0:
                raise SystemExit("%s uses PNG filter %d" % (path, filt))
        out += line
        prev = line

    px = [tuple(out[i:i + 4]) for i in range(0, len(out), 4)]
    return width, px


def reduce_by(px, size, factor):
    """Box filter by a whole factor, averaging premultiplied so that a colour
    behind a transparent pixel cannot leak into the result."""
    if factor == 1:
        return px

    n = size // factor
    area = factor * factor
    out = []

    for j in range(n):
        for i in range(n):
            r = g = b = a = 0
            for dy in range(factor):
                row = (j * factor + dy) * size + i * factor
                for dx in range(factor):
                    sr, sg, sb, sa = px[row + dx]
                    r += sr * sa
                    g += sg * sa
                    b += sb * sa
                    a += sa
            a //= area
            if a == 0:
                out.append((0, 0, 0, 0))
                continue

            # Unpremultiply. The sums hold colour times alpha, so the averaged
            # product divided by the averaged alpha gives the colour back.
            # Clamped, because rounding must not lift a channel above its own
            # alpha and come back brighter than anything in the source.
            out.append((min(255, ((r // area) + a // 2) // a),
                        min(255, ((g // area) + a // 2) // a),
                        min(255, ((b // area) + a // 2) // a),
                        a))
    return out


def encode_bmp_entry(size, px):
    """Classic icon BMP: header, bottom-up BGRA, then a 1bpp AND mask."""
    header = struct.pack(
        "<IiiHHIIiiII",
        40,            # biSize
        size,          # biWidth
        size * 2,      # biHeight (XOR + AND)
        1,             # biPlanes
        32,            # biBitCount
        0,             # biCompression = BI_RGB
        0,             # biSizeImage
        0, 0,          # resolution
        0, 0,          # palette
    )

    xor = bytearray()
    for y in range(size - 1, -1, -1):  # bottom-up
        for x in range(size):
            r, g, b, a = px[y * size + x]
            xor += bytes((b, g, r, a))

    # AND mask: 1bpp, rows padded to 4-byte boundaries. With a real alpha
    # channel present this is ignored by modern Windows, but the format
    # requires it and older code paths still read it.
    row_bytes = ((size + 31) // 32) * 4
    and_mask = bytearray()
    for y in range(size - 1, -1, -1):
        row = bytearray(row_bytes)
        for x in range(size):
            if px[y * size + x][3] < 128:
                row[x // 8] |= 0x80 >> (x % 8)
        and_mask += row

    return bytes(header) + bytes(xor) + bytes(and_mask)


def encode_png_entry(size, px):
    """Minimal RGBA PNG."""
    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter type 0 (None)
        for x in range(size):
            raw += bytes(px[y * size + x])

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def main():
    cache = {}
    entries = []

    for size, source, factor in FRAMES:
        if source not in cache:
            cache[source] = read_png(os.path.join(SRC, source))
        src_size, px = cache[source]

        if src_size != size * factor:
            raise SystemExit(
                "%s is %dpx, but frame %d wants %d at a factor of %d"
                % (source, src_size, size, size * factor, factor))

        frame = reduce_by(px, src_size, factor)
        blob = encode_png_entry(size, frame) if size >= 256 else encode_bmp_entry(size, frame)
        entries.append((size, blob))
        print("  %3d  from %-16s %s"
              % (size, source, "as exported" if factor == 1 else "reduced by %d" % factor))

    # ICONDIR + ICONDIRENTRY table, then the image blobs.
    offset = 6 + 16 * len(entries)
    directory = struct.pack("<HHH", 0, 1, len(entries))
    for size, blob in entries:
        directory += struct.pack(
            "<BBBBHHII",
            0 if size >= 256 else size,   # 0 means 256
            0 if size >= 256 else size,
            0,                            # palette colours
            0,                            # reserved
            1,                            # colour planes
            32,                           # bits per pixel
            len(blob),
            offset,
        )
        offset += len(blob)

    with open(OUT, "wb") as f:
        f.write(directory)
        for _, blob in entries:
            f.write(blob)

    print("wrote %s (%d bytes, %d entries)"
          % (os.path.normpath(OUT), offset, len(entries)))


if __name__ == "__main__":
    main()
