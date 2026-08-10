# Measure a switcher's glass material out of a screenshot.
#
#   python3 tools/measure/measure.py <screenshot.png>
#
# Written for Apple's Cmd-Tab panel, and it works just as well on MacTab's own,
# which is the point: the same script measuring both is the only definition of
# "1:1" available to a project that cannot run its own output.
#
# What it needs from the image: the panel as a horizontal band with some of the
# desktop visible above and below it, and a wallpaper with real structure. A flat
# wallpaper measures nothing.
#
# The panel is a horizontal band over a photo of a building whose fins run
# diagonally, so a row just inside the panel and a row just outside it see the
# SAME structure, shifted sideways by the diagonal slope. That is what makes a
# per-pixel comparison possible at all.
#
# Method: find the slope by correlating rows outside the panel, use it to align
# an outside row with an inside row, then fit the blur sigma and the affine
# transfer that together turn one into the other.
import math
from png import read_png, luma, relsat

import sys
PATH = sys.argv[1] if len(sys.argv) > 1 else 'reference.png'
W, H, PX = read_png(PATH)

# Rows and columns to work in. Defaults are for the 2000x474 reference; pass
# them on the command line for anything else.
TOP_IN = int(sys.argv[2]) if len(sys.argv) > 2 else 76     # first row inside
X0     = int(sys.argv[3]) if len(sys.argv) > 3 else 260    # clear of the ends
X1     = int(sys.argv[4]) if len(sys.argv) > 4 else 1880

def row(y, chan=None):
    if chan is None:
        return [luma(PX[y][x]) for x in range(W)]
    return [float(PX[y][x][chan]) for x in range(W)]

def best_shift(a, b, lo=-60, hi=60):
    # Shift b against a over [X0, X1], least squares after removing means.
    best, bestErr = 0, None
    for s in range(lo, hi + 1):
        n = 0; err = 0.0
        for x in range(X0, X1):
            if 0 <= x + s < W:
                d = a[x] - b[x + s]; err += d * d; n += 1
        if n and (bestErr is None or err / n < bestErr):
            bestErr, best = err / n, s
    return best, bestErr

def blur1d(v, sigma):
    if sigma <= 0.05: return v[:]
    r = int(math.ceil(sigma * 3))
    k = [math.exp(-(i * i) / (2 * sigma * sigma)) for i in range(-r, r + 1)]
    t = sum(k); k = [q / t for q in k]
    out = []
    for x in range(len(v)):
        s = 0.0
        for i, kk in enumerate(k):
            j = min(len(v) - 1, max(0, x + i - r))
            s += v[j] * kk
        out.append(s)
    return out

def affine_fit(src, dst, shift):
    # dst[x] ~ a * src[x + shift] + b
    n = 0; sx = sy = sxx = sxy = 0.0
    for x in range(X0, X1):
        j = x + shift
        if not (0 <= j < W): continue
        u, v = src[j], dst[x]
        sx += u; sy += v; sxx += u * u; sxy += u * v; n += 1
    den = n * sxx - sx * sx
    if abs(den) < 1e-9: return 1.0, 0.0, 1e9
    a = (n * sxy - sx * sy) / den
    b = (sy - a * sx) / n
    res = 0.0
    for x in range(X0, X1):
        j = x + shift
        if not (0 <= j < W): continue
        d = dst[x] - (a * src[j] + b)
        res += d * d
    return a, b, math.sqrt(res / n)

# --- the diagonal slope, measured outside the panel -------------------------
o1, o2 = row(20), row(60)
s, _ = best_shift(o2, o1)          # o1 shifted to match o2, over 40 rows
slope = s / 40.0
print('diagonal slope dx/dy = %.3f  (%.1f deg from vertical)'
      % (slope, math.degrees(math.atan(slope))))

# --- blur and transfer, top edge -------------------------------------------
print()
print('%-6s %-6s %6s %8s %7s %7s %7s' % ('out y', 'in y', 'shift', 'sigma_x', 'sigma', 'gain', 'bias'))

results = []
for (yo, yi) in [(60, 82), (60, 96), (55, 88), (50, 90), (64, 78)]:
    src, dst = row(yo), row(yi)
    shift = int(round(slope * (yi - yo)))
    # search sigma_x
    best = None
    sx = 0.5
    while sx <= 40.0:
        a, b, res = affine_fit(blur1d(src, sx), dst, shift)
        if best is None or res < best[2]: best = (sx, (a, b), res)
        sx += 0.5
    sigx, (gain, bias), res = best
    sigma = sigx / math.sqrt(1 + slope * slope)
    results.append((sigx, sigma, gain, bias))
    print('%-6d %-6d %6d %8.1f %7.1f %7.3f %7.1f   (residual %.1f)'
          % (yo, yi, shift, sigx, sigma, gain, bias, res))

print()
print('median sigma  %.1f' % sorted(r[1] for r in results)[len(results)//2])
print('median gain   %.3f' % sorted(r[2] for r in results)[len(results)//2])
print('median bias   %.1f/255 = %.3f' % (sorted(r[3] for r in results)[len(results)//2],
                                         sorted(r[3] for r in results)[len(results)//2] / 255.0))

# --- per channel ------------------------------------------------------------
print()
sig = sorted(r[0] for r in results)[len(results)//2]
for ch, name in enumerate('RGB'):
    src, dst = row(60, ch), row(90, ch)
    a, b, res = affine_fit(blur1d(src, sig), dst, int(round(slope * 30)))
    print('%s: gain %.3f  bias %6.1f (%.3f)   residual %.1f' % (name, a, b, b / 255.0, res))

# --- saturation and mean luma, in and out -----------------------------------
def stats(y0, y1, x0=X0, x1=X1):
    n = 0; L = 0.0; S = 0.0
    for y in range(y0, y1):
        for x in range(x0, x1, 3):
            c = PX[y][x]; L += luma(c); S += relsat(c); n += 1
    return L / n, S / n

lo, so = stats(20, 66)
li, si = stats(80, 130)
print()
print('outside  luma %.1f (%.3f)  rel.sat %.3f' % (lo, lo / 255.0, so))
print('inside   luma %.1f (%.3f)  rel.sat %.3f' % (li, li / 255.0, si))
print('sat ratio %.3f' % (si / so))
