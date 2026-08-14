# Icon sources

The frames `res/mactab.ico` is cut from. Named by their real pixel size, which
is the only thing that matters here; the design tool they came out of numbered
them by point size and scale factor, which meant nothing on Windows.

Drawn in Figma, composed and exported with Icon Composer.

## Which file feeds which frame

| .ico frame | source          | how                |
|-----------:|-----------------|--------------------|
|         16 | `mactab-16.png` | as exported        |
|         20 | `mactab-40.png` | reduced by 2       |
|         24 | `mactab-192.png`| reduced by 8       |
|         32 | `mactab-32.png` | as exported        |
|         48 | `mactab-192.png`| reduced by 4       |
|         64 | `mactab-64.png` | as exported        |
|        256 | `mactab-256.png`| as exported        |

Four of the seven are rendered at their final size rather than resampled to it,
which is worth having: the export at 16 carries about 11% partially transparent
pixels against 0.2% in the 1024, so it is antialiased as a 16 pixel drawing
rather than averaged down from a large one. Side by side the difference is
obvious, the four tiles in the bar stay separate instead of smearing together.

The other three are reduced by exact whole numbers, which is the one case where
a box filter is unarguable: every output pixel is the average of a whole number
of input pixels with no fractional weighting. That is why 24 and 48 come off the
192 rather than the 1024, and why 20 comes off the 40.

`mactab-1024.png` feeds no frame. It is the master, kept so the set can be
re-cut without going back to the design tool.

## Colour

All of these are 8 bit sRGB. They did not arrive that way.

The export gave 8 bit sRGB at 16, 32 and 40 pixels, and 16 bit Display P3 at 64
and above. A `.ico` frame carries no colour management at all, so Windows reads
whatever bytes are in it as sRGB. Embedding the P3 ones untouched would have
left the icon shifting colour as the shell picked different frames: measured
against the 32 pixel export, the untouched P3 was out by 9 levels of red on
average and 40 at worst, which drops to about 2 once converted.

So anything replacing these has to be converted to sRGB first. On a Mac:

    sips --matchTo "/System/Library/ColorSync/Profiles/sRGB Profile.icc" \
         in.png --out out.png

The 16 bit depth is thrown away in the same pass, since an `.ico` cannot carry
it and it was costing 2.3 MB in the 1024 alone.
