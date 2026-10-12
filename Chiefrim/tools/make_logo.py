#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws Chiefrim's logo (art/logo/chiefrim-logo.svg) after Halo's.

Chrome lettering in the Halo font (art/logo/Halo.ttf, outlined, so the SVG
needs no font), a blue bloom, and the blueprint work behind: the reclaimer
symbol (art/logo/reclaimer.svg), a ticked reticle and type guides.

Needs fontTools (a build tool; nothing of it is in the output).

Usage: tools/make_logo.py [out.svg]
"""

import math
import re
import sys
from pathlib import Path

from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

ART = Path(__file__).resolve().parent.parent / "art" / "logo"

TEXT = "CHIEFRIM"
W, H = 1860, 620
CAP_H = 118          # cap height of the lettering, px
BASELINE = 362
TRACK = 30           # extra space between letters, px
CX, CY = W / 2, 294  # centre of the reticle
RY = 300             # the reclaimer's centre (on CX), behind the lettering
R_RECL = 64          # the reclaimer's outer radius, px


def number(v):
    return f"{v:.2f}".rstrip("0").rstrip(".")


def lettering():
    """the text as one SVG path, and its left, right and top edges"""
    font = TTFont(ART / "Halo.ttf")
    glyphs, cmap = font.getGlyphSet(), font.getBestCmap()

    def bounds(name):
        pen = BoundsPen(glyphs)
        glyphs[name].draw(pen)
        return pen.bounds

    scale = CAP_H / bounds(cmap[ord("H")])[3]
    names = [cmap[ord(c)] for c in TEXT]
    spans = [(b[0], b[2]) for b in map(bounds, names)]
    total = sum((xmax - xmin) * scale for xmin, xmax in spans) + TRACK * (len(names) - 1)
    left = x = (W - total) / 2

    paths = []
    for name, (xmin, xmax) in zip(names, spans):
        pen = SVGPathPen(glyphs, ntos=number)
        glyphs[name].draw(TransformPen(pen, (scale, 0, 0, -scale, x - xmin * scale, BASELINE)))
        paths.append(pen.getCommands())
        x += (xmax - xmin) * scale + TRACK
    return " ".join(paths), left, left + total, BASELINE - CAP_H


def reclaimer():
    """the reclaimer's path: centred on (468.75, 468.75), radius 375"""
    d = re.search(r'\sd="([^"]+)"', (ART / "reclaimer.svg").read_text()).group(1)
    if not re.fullmatch(r"[0-9a-zA-Z.,\s-]+", d):
        sys.exit("make_logo: reclaimer.svg's path has unexpected characters")
    return d


def blueprint(left, right, top):
    """the thin blue construction lines behind the lettering"""
    out = []

    def circle(x, y, r, w=1.0, o=0.55):
        out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r:.1f}" stroke-width="{w}" opacity="{o}"/>')

    def line(x1, y1, x2, y2, w=1.0, o=0.5):
        out.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke-width="{w}" opacity="{o}"/>')

    def ticks(x, y, r, n, length, start=0.0, w=1.2, o=0.6):
        for i in range(n):
            a = start + 2 * math.pi * i / n
            line(x + r * math.cos(a), y + r * math.sin(a),
                 x + (r + length) * math.cos(a), y + (r + length) * math.sin(a), w, o)

    # the reticle
    circle(CX, CY, 250, 1.1, 0.55)
    circle(CX, CY, 232, 0.8, 0.35)
    circle(CX, CY, 122, 1.0, 0.5)
    ticks(CX, CY, 250, 24, 18, start=math.pi / 24, w=1.4, o=0.55)
    ticks(CX, CY, 122, 12, -14, w=1.1, o=0.45)
    # paired tick marks on the axes
    for side in (-1, 1):
        for d in (-6, 6):
            line(CX + d, CY + side * 268, CX + d, CY + side * 286, 1.3, 0.6)
            line(CX + side * 268, CY + d, CX + side * 286, CY + d, 1.3, 0.6)
    line(CX, CY - 300, CX, CY + 300, 0.8, 0.35)
    line(CX - 330, CY, CX + 330, CY, 0.8, 0.35)
    # offset circles left and right
    circle(CX - 330, CY - 40, 92, 1.0, 0.45)
    circle(CX - 330, CY - 40, 60, 0.8, 0.3)
    ticks(CX - 330, CY - 40, 92, 16, 10, w=1.0, o=0.4)
    circle(CX + 360, CY + 70, 58, 1.0, 0.45)
    circle(CX + 360, CY + 70, 30, 0.8, 0.3)
    circle(CX + 560, CY - 30, 34, 0.8, 0.3)
    circle(CX - 560, CY + 50, 26, 0.8, 0.3)
    # type guides across the lettering
    for y, o in ((top, 0.55), (top + CAP_H * 0.5, 0.35), (BASELINE, 0.55), (BASELINE + 14, 0.3), (top - 14, 0.3)):
        line(left - 50, y, right + 50, y, 0.9, o)
    for x in (left - 20, left + 30, right - 30, right + 20, CX - 140, CX + 140):
        line(x, top - 40, x, BASELINE + 40, 0.7, 0.3)
    # dimension marks under the lettering
    for x in range(int(left), int(right), 120):
        line(x, BASELINE + 26, x, BASELINE + 36, 0.8, 0.35)
    return "\n    ".join(out)


def logo():
    text, left, right, top = lettering()
    k = R_RECL / 375
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}">
  <title>Chiefrim</title>
  <defs>
    <!-- chrome: bright top, cool steel-blue bottom, sharp horizon at mid -->
    <linearGradient id="chrome" gradientUnits="userSpaceOnUse" x1="0" y1="{top}" x2="0" y2="{BASELINE}">
      <stop offset="0" stop-color="#ffffff"/>
      <stop offset="0.28" stop-color="#f4f7ff"/>
      <stop offset="0.47" stop-color="#c9d4ee"/>
      <stop offset="0.5" stop-color="#7f93c4"/>
      <stop offset="0.62" stop-color="#9aabd6"/>
      <stop offset="0.82" stop-color="#5a6d9f"/>
      <stop offset="1" stop-color="#2a3866"/>
    </linearGradient>
    <linearGradient id="sheen" gradientUnits="userSpaceOnUse" x1="{left}" y1="0" x2="{right}" y2="0">
      <stop offset="0" stop-color="#ffffff" stop-opacity="0"/>
      <stop offset="0.3" stop-color="#ffffff" stop-opacity="0.25"/>
      <stop offset="0.5" stop-color="#ffffff" stop-opacity="0"/>
      <stop offset="0.75" stop-color="#ffffff" stop-opacity="0.2"/>
      <stop offset="1" stop-color="#ffffff" stop-opacity="0"/>
    </linearGradient>
    <clipPath id="letters"><path d="{text}"/></clipPath>
    <filter id="glow" x="-20%" y="-80%" width="140%" height="260%">
      <feGaussianBlur stdDeviation="9 26"/>
    </filter>
    <filter id="halo" x="-20%" y="-80%" width="140%" height="260%">
      <feGaussianBlur stdDeviation="4"/>
    </filter>
    <filter id="soft" x="-10%" y="-10%" width="120%" height="120%">
      <feGaussianBlur stdDeviation="0.6"/>
    </filter>
  </defs>

  <!-- blueprint construction work, with the reclaimer outlined at its heart -->
  <g fill="none" stroke="#3d6fd8" filter="url(#soft)">
    {blueprint(left, right, top)}
    <path transform="translate({CX:.2f} {RY}) scale({k:.5f}) translate(-468.75 -468.75)" d="{reclaimer()}" stroke-width="{1.0 / k:.2f}" opacity="0.5"/>
  </g>

  <!-- blue bloom behind the lettering -->
  <path d="{text}" fill="#2f63ff" opacity="1" filter="url(#glow)"/>
  <path d="{text}" fill="#8fb4ff" opacity="0.7" filter="url(#halo)"/>

  <!-- chrome lettering -->
  <path d="{text}" fill="url(#chrome)" stroke="#0d1a3d" stroke-width="0.6" paint-order="stroke"/>
  <g clip-path="url(#letters)">
    <rect x="{left}" y="{top}" width="{right - left}" height="{CAP_H}" fill="url(#sheen)"/>
    <!-- a construction line etched into the metal -->
    <line x1="{left}" y1="{top + CAP_H * 0.5:.1f}" x2="{right}" y2="{top + CAP_H * 0.5:.1f}" stroke="#3a5aa8" stroke-width="1" opacity="0.55"/>
    <line x1="{left}" y1="{top + 2.5:.1f}" x2="{right}" y2="{top + 2.5:.1f}" stroke="#ffffff" stroke-width="3" opacity="0.9"/>
  </g>
</svg>
'''


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ART / "chiefrim-logo.svg"
    out.write_text(logo())
    print(f"make_logo: wrote {out}")
