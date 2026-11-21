#!/usr/bin/env python3
"""Draws the Universal Weather Node app icon (PWA / home screen / favicon).

Motif: sun behind a cloud, with radio waves (the node receives the station
on 868 MHz) - white on the dashboard's teal accent colour. Drawn with
Pillow at 8x and downsampled, so no SVG renderer is needed.

  .venv-img/bin/python tools/make_icon.py [out_dir]   (default: pictures/icon)

Writes icon-512.png, icon-192.png, apple-touch-icon.png (180, opaque),
icon-maskable-512.png (motif inside the 80 % safe zone, full-bleed
background) and favicon-32.png.
"""
import math
import os
import sys

from PIL import Image, ImageDraw

TEAL_TOP = (15, 157, 148)      # flat dashboard accent #0f9d94 (a gradient bands in a 64-colour PNG)
TEAL_BOT = (15, 157, 148)
WHITE = (255, 255, 255, 255)
SUN = (255, 214, 92, 255)
WAVE = (255, 255, 255, 255)
SS = 8                         # supersampling


def background(size, rounded):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    grad = Image.new("RGBA", (size, size))
    gd = ImageDraw.Draw(grad)
    for y in range(size):
        t = y / (size - 1)
        c = tuple(int(TEAL_TOP[i] + (TEAL_BOT[i] - TEAL_TOP[i]) * t) for i in range(3)) + (255,)
        gd.line([(0, y), (size, y)], fill=c)
    mask = Image.new("L", (size, size), 0)
    md = ImageDraw.Draw(mask)
    if rounded:
        md.rounded_rectangle([0, 0, size - 1, size - 1], radius=int(size * 0.22), fill=255)
    else:
        md.rectangle([0, 0, size, size], fill=255)
    img.paste(grad, (0, 0), mask)
    return img


def motif(d, size, scale):
    """Draws the motif centred, `scale` = fraction of the canvas it may use."""
    s = size * scale
    ox = (size - s) / 2
    oy = (size - s) / 2

    def P(x, y):                      # motif coordinates 0..1 -> pixels
        return (ox + x * s, oy + y * s)

    def R(r):
        return r * s

    # sun (upper left, partly hidden by the cloud) with rays
    cx, cy = P(0.36, 0.40)
    r = R(0.17)
    for k in range(8):
        a = math.radians(k * 45 - 22.5)
        x1, y1 = cx + math.cos(a) * r * 1.35, cy + math.sin(a) * r * 1.35
        x2, y2 = cx + math.cos(a) * r * 1.75, cy + math.sin(a) * r * 1.75
        d.line([(x1, y1), (x2, y2)], fill=SUN, width=int(R(0.045)))
        d.ellipse([x2 - R(0.0225), y2 - R(0.0225), x2 + R(0.0225), y2 + R(0.0225)], fill=SUN)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=SUN)

    # radio waves (upper right): three arcs around a small dot = the node listening
    wx, wy = P(0.74, 0.30)
    dot = R(0.035)
    d.ellipse([wx - dot, wy - dot, wx + dot, wy + dot], fill=WAVE)
    for i, rr in enumerate((0.09, 0.15, 0.21)):
        rad = R(rr)
        d.arc([wx - rad, wy - rad, wx + rad, wy + rad], start=-150, end=-30,
              fill=WAVE, width=int(R(0.038)))

    # cloud (bottom), drawn on top of the sun
    def circ(x, y, rr):
        X, Y = P(x, y)
        q = R(rr)
        d.ellipse([X - q, Y - q, X + q, Y + q], fill=WHITE)
    circ(0.30, 0.66, 0.15)
    circ(0.50, 0.56, 0.20)
    circ(0.70, 0.66, 0.15)
    x0, y0 = P(0.30, 0.66)
    x1, y1 = P(0.70, 0.81)
    d.rectangle([x0, y0, x1, y1], fill=WHITE)
    # flat cloud base with rounded ends
    bx0, by = P(0.15, 0.81)
    bx1, _ = P(0.85, 0.81)
    d.rounded_rectangle([bx0, by - R(0.15), bx1, by], radius=int(R(0.075)), fill=WHITE)


def render(size, rounded=True, scale=0.78, opaque=False):
    big = size * SS
    img = background(big, rounded and not opaque)
    if opaque:
        bg = Image.new("RGBA", (big, big), TEAL_BOT + (255,))
        bg.alpha_composite(img)
        img = bg
    motif(ImageDraw.Draw(img), big, scale)
    out = img.resize((size, size), Image.LANCZOS)
    return out.convert("RGB") if opaque else out


def save(img, path):
    """Palette PNG (64 colours, no dither): ~5x smaller, which matters because
    the icons are embedded in the firmware."""
    img.convert("RGBA").quantize(colors=64, method=Image.FASTOCTREE, dither=Image.NONE).save(path, optimize=True)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "pictures", "icon")
    os.makedirs(out, exist_ok=True)
    save(render(512), os.path.join(out, "icon-512.png"))
    save(render(192), os.path.join(out, "icon-192.png"))
    # iOS rounds the corners itself and wants an opaque square
    save(render(180, rounded=False, opaque=True), os.path.join(out, "apple-touch-icon.png"))
    # Android adaptive icons crop to a circle/shape: motif within the 80 % safe zone
    save(render(512, rounded=False, scale=0.62, opaque=True), os.path.join(out, "icon-maskable-512.png"))
    save(render(32, scale=0.9), os.path.join(out, "favicon-32.png"))
    for f in sorted(os.listdir(out)):
        print(f, os.path.getsize(os.path.join(out, f)), "bytes")


if __name__ == "__main__":
    main()
