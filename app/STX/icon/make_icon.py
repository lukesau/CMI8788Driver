"""Pixel-art app icon from a photo of the card (white background).

  python3 -m venv /tmp/iconenv && /tmp/iconenv/bin/pip install pillow
  /tmp/iconenv/bin/python make_icon.py card-photo.png
      -> stx_art.png (the 74x48 pixel grid), stx_icon_1024.png (master)

Then build AppIcon.icns from the master (nearest-neighbour for 256 px and up,
smooth for the tiny sizes) with iconutil; see the commit that added it.
"""
from collections import deque
from PIL import Image, ImageEnhance

import sys
SRC, W, COLORS = sys.argv[1] if len(sys.argv) > 1 else "stx_photo.png", 72, 28

im = Image.open(SRC).convert("RGBA")
w, h = im.size
px = im.load()

# 1. Remove the white background: flood fill from the border through near-white.
def bg(p): return p[0] > 228 and p[1] > 228 and p[2] > 228
seen = bytearray(w * h)
q = deque((x, y) for x in range(w) for y in (0, h - 1)) + deque((x, y) for y in range(h) for x in (0, w - 1))
while q:
    x, y = q.popleft()
    i = y * w + x
    if seen[i] or not bg(px[x, y]):
        continue
    seen[i] = 1
    px[x, y] = (0, 0, 0, 0)
    for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
        if 0 <= nx < w and 0 <= ny < h and not seen[ny * w + nx]:
            q.append((nx, ny))
im = im.crop(im.getbbox())

# 2. Pixelate: box-filter down to a W-pixel-wide grid, hard alpha.
H = round(im.height * W / im.width)
small = im.resize((W, H), Image.BOX)
alpha = small.split()[3].point(lambda a: 255 if a > 140 else 0)

# 3. Flat palette, no dithering.
# Boost saturation so gold, red and blue parts keep their colour in the palette.
vivid = ImageEnhance.Color(small.convert("RGB")).enhance(1.6)
vivid = ImageEnhance.Contrast(vivid).enhance(1.1)
rgb = vivid.quantize(colors=COLORS, method=Image.Quantize.FASTOCTREE,
                     dither=Image.Dither.NONE).convert("RGB")
art = rgb.convert("RGBA")
art.putalpha(alpha)

# 4. One-pixel dark outline around the card's silhouette.
art = art.crop((-1, -1, W + 1, H + 1))   # transparent 1px margin
a = art.load()
outline = (24, 22, 22, 255)
pts = []
for y in range(art.height):
    for x in range(art.width):
        if a[x, y][3] == 0 and any(0 <= x + dx < art.width and 0 <= y + dy < art.height and
                                   a[x + dx, y + dy][3] == 255 and a[x + dx, y + dy] != outline
                                   for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
            pts.append((x, y))
for p in pts:
    a[p] = outline
art.save("stx_art.png")
print("art grid", art.size)

# 5. Icon master: nearest-neighbour upscale, centered on a 1024 canvas.
scale = 1000 // art.width
big = art.resize((art.width * scale, art.height * scale), Image.NEAREST)
master = Image.new("RGBA", (1024, 1024), (0, 0, 0, 0))
master.paste(big, ((1024 - big.width) // 2, (1024 - big.height) // 2))
master.save("stx_icon_1024.png")
print("scale", scale, "->", big.size)
