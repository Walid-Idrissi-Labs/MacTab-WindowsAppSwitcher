#!/usr/bin/env python3
"""Generate res/mactab.ico — the tray/app icon.

The mark is a squircle inside a squircle, which is the whole point of the
project: Apple's continuous-corner shape rather than a plain rounded rect.

Icon entries at 48px and below are stored as classic BMP (BITMAPINFOHEADER +
BGRA XOR data + 1bpp AND mask), because some Windows shell surfaces still
mis-handle PNG-compressed entries at small sizes. The 256px entry is PNG, which
is the documented Vista+ path and keeps the file small.

Stdlib only. Run from the repo root:  python3 tools/make_icon.py
"""

import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(__file__), "..", "res", "mactab.ico")
SIZES = [16, 20, 24, 32, 48, 64, 256]

# Superellipse exponent. n=5 is the usual close approximation of Apple's
# continuous-corner curve; n=4 is visibly rounder, n=6 visibly boxier.
SQUIRCLE_N = 5.0

# Vertical gradient for the outer squircle (top RGB -> bottom RGB).
GRAD_TOP = (94, 137, 245)
GRAD_BOTTOM = (58, 88, 205)

# The inner "app tile" glyph.
TILE_COLOR = (255, 255, 255)
TILE_SCALE = 0.46      # fraction of the canvas
TILE_ALPHA = 0.94

SS = 4  # supersampling factor per axis (4x4 = 16 samples per pixel)


def squircle_coverage(px, py, size, half_extent, n=SQUIRCLE_N):
    """Fractional coverage of pixel (px, py) by a centred superellipse.

    half_extent is the shape's half-width in pixels. Computed by point-sampling
    an SS x SS grid inside the pixel, which is plenty for static art.
    """
    cx = cy = size / 2.0
    hit = 0
    for sy in range(SS):
        for sx in range(SS):
            x = px + (sx + 0.5) / SS - cx
            y = py + (sy + 0.5) / SS - cy
            # |x/a|^n + |y/a|^n <= 1
            v = (abs(x) / half_extent) ** n + (abs(y) / half_extent) ** n
            if v <= 1.0:
                hit += 1
    return hit / float(SS * SS)


def lerp(a, b, t):
    return a + (b - a) * t


def render_rgba(size):
    """Render the icon at `size` as a flat list of (r, g, b, a) top-down."""
    px = [None] * (size * size)

    # Outer squircle fills the canvas with a hair of padding so the
    # antialiased edge is never clipped.
    outer_half = (size / 2.0) * 0.96
    inner_half = (size / 2.0) * TILE_SCALE

    for y in range(size):
        t = y / max(1.0, size - 1.0)
        gr = int(round(lerp(GRAD_TOP[0], GRAD_BOTTOM[0], t)))
        gg = int(round(lerp(GRAD_TOP[1], GRAD_BOTTOM[1], t)))
        gb = int(round(lerp(GRAD_TOP[2], GRAD_BOTTOM[2], t)))

        for x in range(size):
            outer = squircle_coverage(x, y, size, outer_half)
            if outer <= 0.0:
                px[y * size + x] = (0, 0, 0, 0)
                continue

            inner = squircle_coverage(x, y, size, inner_half) * TILE_ALPHA

            # Composite the white tile over the gradient, then apply the
            # outer shape as the overall alpha.
            r = int(round(lerp(gr, TILE_COLOR[0], inner)))
            g = int(round(lerp(gg, TILE_COLOR[1], inner)))
            b = int(round(lerp(gb, TILE_COLOR[2], inner)))
            a = int(round(outer * 255))
            px[y * size + x] = (r, g, b, a)

    return px


def encode_bmp_entry(size, px):
    """Classic icon BMP: header, bottom-up BGRA, then a 1bpp AND mask."""
    # BITMAPINFOHEADER with doubled height (XOR image + AND mask).
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
            r, g, b, a = px[y * size + x]
            raw += bytes((r, g, b, a))

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
    entries = []
    for size in SIZES:
        px = render_rgba(size)
        blob = encode_png_entry(size, px) if size >= 256 else encode_bmp_entry(size, px)
        entries.append((size, blob))

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

    os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(directory)
        for _, blob in entries:
            f.write(blob)

    print("wrote %s (%d bytes, %d entries)"
          % (os.path.normpath(OUT), offset, len(entries)))


if __name__ == "__main__":
    main()
